#!/usr/bin/env bash
# Sweep the simulator across a few rates and ring sizes.
#
# Correctness gate: every run must report zero packet-id gaps and must observe
# exactly as many packets as the producer published.

set -euo pipefail

BIN="${1:-./build/sim/gnp}"
if [[ ! -x "$BIN" ]]; then
    echo "usage: $0 [path-to-gnp]   (built binary not found at '$BIN')" >&2
    exit 1
fi

fail=0

run() {
    local desc="$1"; shift
    local out
    out="$("$BIN" "$@" 2>&1)"

    local published observed gaps
    published=$(awk '/descriptors published/ {print $3}' <<<"$out")
    observed=$(awk  '/packets observed/      {print $3}' <<<"$out")
    gaps=$(awk      '/packet-id gaps/        {print $3}' <<<"$out")
    mean=$(awk      '/^  mean/               {print $2}' <<<"$out")

    if [[ "$published" == "$observed" && "$gaps" == "0" ]]; then
        printf 'ok    %-34s %10s pkts  mean %8s us\n' "$desc" "$observed" "$mean"
    else
        printf 'FAIL  %-34s published=%s observed=%s gaps=%s\n' \
               "$desc" "$published" "$observed" "$gaps"
        fail=1
    fi
}

run "100 kpps, ring 1024"    --pps 100000  --duration 1000
run "1 Mpps, ring 1024"      --pps 1000000 --duration 1000
run "unpaced, ring 1024"     --pps 0       --duration 1000
run "unpaced, ring 64"       --pps 0       --duration 1000 --ring 64
run "bursty, 16 per burst"   --pps 500000  --duration 1000 --burst 16
run "fixed 200k packets"     --pps 0       --duration 3000 --packets 200000
run "jumbo payload 9000 B"   --pps 200000  --duration 1000 --size 9000
run "near-idle, 1 pps"       --pps 1       --duration 500

# Multi-queue: the summary's top sections are the aggregate over all queues, so
# the same parsing applies. Any gap on any queue shows up in the total.
run "2 queues, 100 kpps each"   --queues 2 --pps 100000 --duration 1000
run "4 queues, 100 kpps each"   --queues 4 --pps 100000 --duration 1000
run "4 queues, unpaced, ring 64" --queues 4 --pps 0     --duration 1000 --ring 64
run "4 queues, bursty"          --queues 4 --pps 50000  --duration 1000 --burst 16
run "3 queues, fixed 50k each"  --queues 3 --pps 0      --duration 3000 --packets 50000

# A queue count past the backend's limit must be refused up front. On CUDA the
# alternative is launching pollers that can never all be resident: a hang.
if out="$(timeout 20 "$BIN" --queues 100000 --duration 100 2>&1)"; then
    printf 'FAIL  %-34s accepted an impossible queue count\n' "over-limit --queues rejected"
    fail=1
elif [[ $? -eq 124 ]]; then
    printf 'FAIL  %-34s hung instead of refusing\n' "over-limit --queues rejected"
    fail=1
elif ! grep -q "exceeds this backend's limit" <<<"$out"; then
    printf 'FAIL  %-34s failed for the wrong reason: %s\n' "over-limit --queues rejected" "$out"
    fail=1
else
    printf 'ok    %-34s\n' "over-limit --queues rejected"
fi

echo
if (( fail )); then
    echo "sweep FAILED"
    exit 1
fi
echo "sweep passed"
