#!/usr/bin/env python3
"""Benchmark the GPU poller against the CPU fallback and record the results.

Runs the same workloads on both builds and writes one CSV row per run:

  paced    50 kpps per queue, 1-5 queues: host CPU spent polling
           (kept under the simulator's ~0.45 Mpps paced-flush ceiling)
  unpaced  no pacing, 64-entry rings, 1-5 queues: throughput
  copy     GPU only, same as paced plus sampled H2D flush timing

Host CPU is measured per thread from /proc: producers are named gnp-prod-N,
CPU-fallback pollers gnp-poll-N, everything else (main thread, CUDA driver
threads) is "other". The GPU poller runs on the GPU, so on that backend the
polling cost shows up as poll + other = ~0.

Usage:
  testing/scripts/bench.py [--gpu build/dev/testing/gnp_sim] [--cpu build/dev-cpu/testing/gnp_sim] [--runs 5]
Then render the charts with testing/scripts/plot_bench.py.
"""

import argparse
import csv
import os
import platform
import re
import subprocess
import sys
import time
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLK_TCK = os.sysconf("SC_CLK_TCK")

FIELDS = [
    "scenario", "backend", "queues", "pps_per_queue", "ring", "run",
    "published", "observed", "gaps", "achieved_mpps", "copy_us", "poll_period_us",
    "cores_poll", "cores_prod", "cores_other",
]

PATTERNS = {
    "published": r"descriptors published\s+(\d+)",
    "observed": r"packets observed\s+(\d+)",
    "gaps": r"packet-id gaps\s+(\d+)",
    "achieved_mpps": r"achieved rate\s+([\d.]+) Mpps",
    # CUDA only: the sampled H2D flush time. Measured with one clock on the host
    # side of the copy, so unlike a publish -> observe latency it needs no
    # comparison against the GPU's clock.
    "copy_us": r"H2D flush, submit->done\s+([\d.]+) us",
    # Single-queue runs only.
    "poll_period_us": r"poll loop period\s+([\d.]+) us",
}


def thread_ticks(pid):
    """CPU ticks (user + system) per thread class for a running process."""
    ticks = {"poll": 0, "prod": 0, "other": 0}
    try:
        tasks = os.listdir(f"/proc/{pid}/task")
    except FileNotFoundError:
        return None
    for tid in tasks:
        try:
            with open(f"/proc/{pid}/task/{tid}/comm") as f:
                comm = f.read().strip()
            with open(f"/proc/{pid}/task/{tid}/stat") as f:
                # comm may contain spaces; fields resume after the last ')'.
                rest = f.read().rsplit(")", 1)[1].split()
        except (FileNotFoundError, ProcessLookupError):
            continue
        used = int(rest[11]) + int(rest[12])  # utime + stime
        cls = "poll" if comm.startswith("gnp-poll") else (
            "prod" if comm.startswith("gnp-prod") else "other")
        ticks[cls] += used
    return ticks


def run_once(binary, queues, pps, ring, duration_ms, extra=()):
    args = [binary, "--queues", str(queues), "--pps", str(pps), "--ring", str(ring),
            "--duration", str(duration_ms), *extra]
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    # Steady state begins once the producer threads exist (pollers launch first).
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            names = [open(f"/proc/{proc.pid}/task/{t}/comm").read()
                     for t in os.listdir(f"/proc/{proc.pid}/task")]
        except (FileNotFoundError, ProcessLookupError):
            names = []
        if sum(n.startswith("gnp-prod") for n in names) == queues or proc.poll() is not None:
            break
        time.sleep(0.005)

    # Sample the middle 80% of the run.
    time.sleep(duration_ms * 0.1 / 1000)
    t1, s1 = time.monotonic(), thread_ticks(proc.pid)
    time.sleep(duration_ms * 0.8 / 1000)
    t2, s2 = time.monotonic(), thread_ticks(proc.pid)

    out, _ = proc.communicate(timeout=120)
    if proc.returncode != 0:
        sys.exit(f"{' '.join(args)} failed ({proc.returncode}):\n{out}")

    row = {}
    for key, pat in PATTERNS.items():
        m = re.search(pat, out, re.MULTILINE)
        row[key] = m.group(1) if m else ""
    if s1 and s2:
        wall = t2 - t1
        for cls in ("poll", "prod", "other"):
            row[f"cores_{cls}"] = f"{(s2[cls] - s1[cls]) / CLK_TCK / wall:.3f}"
    return row


def machine_info(gpu_bin):
    cpu = "unknown CPU"
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    gpu = "unknown GPU"
    try:
        gpu = subprocess.run(["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"],
                             capture_output=True, text=True, timeout=10).stdout.strip() or gpu
    except (OSError, subprocess.TimeoutExpired):
        pass
    try:
        gpu_bin = Path(gpu_bin).resolve().relative_to(ROOT)
    except ValueError:
        pass
    return (f"date: {date.today().isoformat()}\n"
            f"gpu: {gpu}\n"
            f"cpu: {cpu} ({os.cpu_count()} hardware threads)\n"
            f"kernel: {platform.release()}\n"
            f"gpu binary: {gpu_bin}\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gpu", default=str(ROOT / "build/dev/testing/gnp_sim"))
    ap.add_argument("--cpu", default=str(ROOT / "build/dev-cpu/testing/gnp_sim"))
    ap.add_argument("--runs", type=int, default=5)
    args = ap.parse_args()

    for b in (args.gpu, args.cpu):
        if not os.access(b, os.X_OK):
            sys.exit(f"missing binary: {b} (build the dev and dev-cpu presets first)")

    # Each CPU-fallback queue costs two spinning threads; stay within the host.
    max_q = min(5, max(1, (os.cpu_count() or 2) // 2 - 1))
    queue_counts = list(range(1, max_q + 1))

    plan = []
    for q in queue_counts:
        for backend, binary in (("gpu", args.gpu), ("cpu", args.cpu)):
            plan.append(("paced", backend, binary, q, 50000, 1024, 2000))
            plan.append(("unpaced", backend, binary, q, 0, 64, 1000))
    # Same as "paced" on the GPU, plus sampled H2D flush timing. Kept separate:
    # the sampling stalls the producer, so the other scenarios run without it.
    for q in queue_counts:
        plan.append(("copy", "gpu", args.gpu, q, 50000, 1024, 2000))

    # plot_bench.py reads this exact path, so it is fixed rather than a flag.
    out_dir = ROOT / "docs/bench"
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / "results.csv"
    bad = 0
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        total = len(plan) * args.runs
        n = 0
        for scenario, backend, binary, q, pps, ring, dur in plan:
            extra = ("--copy-timing",) if scenario == "copy" else ()
            for r in range(args.runs):
                n += 1
                print(f"[{n}/{total}] {scenario:8s} {backend} queues={q} pps/q={pps} ring={ring}",
                      flush=True)
                row = run_once(binary, q, pps, ring, dur, extra)
                if row["published"] != row["observed"] or row["gaps"] != "0":
                    bad += 1
                    print(f"  WARNING: published={row['published']} observed={row['observed']} "
                          f"gaps={row['gaps']}", flush=True)
                row.update(scenario=scenario, backend=backend, queues=q, pps_per_queue=pps,
                           ring=ring, run=r)
                w.writerow(row)

    (out_dir / "machine.txt").write_text(machine_info(args.gpu))
    print(f"wrote {csv_path}" + (f" ({bad} run(s) dropped packets!)" if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
