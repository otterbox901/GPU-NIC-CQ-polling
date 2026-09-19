# Documentation

| document | read it for |
|---|---|
| [`../README.md`](../README.md) | what `gnp` is, building it, running it on a NIC |
| [`design.md`](design.md) | the owner-bit protocol, publication ordering, memory placement, the AF_XDP producer, multi-queue, and the next steps |
| this file | measured results, where the latency goes, how to read the run summary |
| [`../testing/README.md`](../testing/README.md) | how every result here was produced, and how to reproduce it |
| [`bench/`](bench/) | raw benchmark data (`results.csv`) and the machine it came from (`machine.txt`) |
| [`img/`](img/) | the charts below, light and dark |

## Status

| component | state | evidence |
|---|---|---|
| ring protocol | verified | `test_ring` |
| GPU and CPU pollers | verified, 1–N queues | `run_sim.sh` sweep, benchmarks below |
| XDP program + AF_XDP ingest | verified on veth, generic XDP | 10,000/10,000 packets, 0 gaps ([reference run](../testing/README.md#3-real-ingest-path-pcap-replay-over-veth)) |
| native XDP on a physical NIC | **not yet tested** | needs a native-XDP NIC ([checklist](../README.md#run-on-a-nic)) |
| `on_packet()` | placeholder, empty | [`packet_handler.hpp`](../include/gnp/packet_handler.hpp) |

## GPU polling vs CPU polling

All numbers in this section come from the **simulated NIC** (`gnp_sim`). It
generates arrivals at a precise rate on any machine, so the two pollers can be
compared without network noise. The poll loop and the ring are the same code
`gnp` runs on real traffic.

Both backends run the **same poll loop over the same owner-bit ring**. They
differ only in where the loop runs and which memory it reads:

| | GPU poller ([`poll_kernel.cu`](../src/gpu/poll_kernel.cu)) | CPU fallback ([`poll_cpu.cpp`](../src/gpu/poll_cpu.cpp)) |
|---|---|---|
| poll loop runs on | an SM: one single-thread block per queue | a host core: one spinning thread per queue |
| ring it reads | device memory, filled by DMA over PCIe | host memory, written directly by the producer |
| **host cores spent polling** (5 queues) | **0.00** | **4.96** |
| mean detection latency (5 queues × 50 kpps) | 6.0 µs | 1.16 µs |
| … of which H2D flush | 5.7 µs | none |
| poll loop period (1 queue) | 1.3 µs | 0.002 µs |
| unpaced throughput (5 queues) | 14.3 Mpps | 75.8 Mpps |

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/host-cpu-dark.svg">
  <img src="img/host-cpu.svg" width="720" alt="Host CPU spent on polling, 1 to 5 queues: the GPU poller uses 0 cores at every queue count; the CPU fallback uses 0.99, 1.99, 2.98, 3.96 and 4.96 cores.">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/latency-dark.svg">
  <img src="img/latency.svg" width="720" alt="Mean detection latency at 50 kpps per queue, 1 to 5 queues: GPU poller 5.7, 6.4, 6.1, 5.9, 6.0 microseconds; CPU fallback 0.10, 0.12, 0.15, 0.55, 1.16 microseconds.">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/throughput-dark.svg">
  <img src="img/throughput.svg" width="720" alt="Unpaced throughput with 64-entry rings, 1 to 5 queues: GPU poller 3.0, 6.0, 8.9, 11.6, 14.3 Mpps; CPU fallback 22.5, 41.1, 59.0, 65.9, 75.8 Mpps. Both scale roughly linearly.">
</picture>

**How to read these charts:**

- **Host CPU is the result that matters, and the simulator doesn't affect it.**
  A spinning poll thread burns a full core whether packets arrive or not, so
  the CPU fallback costs exactly one core per queue. The GPU poller costs **no
  host CPU at all**. Its price is an SM block slot per queue instead.
- **Latency and throughput favour the CPU fallback, and almost all of the
  latency gap is the host → device copy.** The CPU poller reads the ring the
  producer writes, so no descriptor ever crosses PCIe. The GPU path pays a DMA
  hop per batch (host staging → device ring), and that hop is nearly all of its
  latency (see [below](#where-the-gpu-latency-goes)). The AF_XDP path has the
  same hop, because AF_XDP delivers into host memory. It goes away only when
  the NIC writes straight into GPU memory (GPUDirect / DOCA).
- **The GPU latency is steady.** It stays at about 6 µs from 1 to 5 queues,
  and both backends' throughput scales roughly linearly with the queue count.

Setup: GTX 1650 (sm_75, 16 SMs) and an i7-9750H (12 hardware threads). Each
point is the median of 5 runs. Every run delivered every packet with zero gaps.
The raw data is in [`bench/results.csv`](bench/results.csv).

<details>
<summary>Data table</summary>

| queues | host cores polling (GPU / CPU) | mean latency µs (GPU / CPU) | unpaced Mpps (GPU / CPU) |
|---|---|---|---|
| 1 | 0.00 / 0.99 | 5.7 / 0.10 | 3.0 / 22.5 |
| 2 | 0.00 / 1.99 | 6.4 / 0.12 | 6.0 / 41.1 |
| 3 | 0.00 / 2.98 | 6.1 / 0.15 | 8.9 / 59.0 |
| 4 | 0.00 / 3.96 | 5.9 / 0.55 | 11.6 / 65.9 |
| 5 | 0.00 / 4.96 | 6.0 / 1.16 | 14.3 / 75.8 |

Host CPU is measured per thread from `/proc`, excluding the simulated
producers, which cost one core per queue on both backends. Latency and host
CPU come from runs at 50 kpps per queue for 2 s. Throughput comes from
unpaced runs on 64-entry rings for 1 s.
</details>

## Where the GPU latency goes

The GPU poller's 6 µs against the CPU fallback's 0.1–1 µs looked like a verdict
on GPU polling. It isn't one. The detection latency (producer publishes a CQE →
poller observes it) has two parts on the GPU path, and only one belongs to the
poller:

1. **H2D flush.** The producer writes CQEs into pinned host staging, and
   `backend_flush_descs` DMAs them into the device ring. This happens for
   **every batch, all run long**. A NIC writing into GPU memory directly
   would remove this step.
2. **Poll reaction.** Once the data lands in the device ring, the poller has to
   notice it. This is the part that measures GPU polling.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/latency-split-dark.svg">
  <img src="img/latency-split.svg" width="720" alt="Mean GPU detection latency beside the sampled H2D flush time at 50 kpps per queue, 1 to 5 queues: GPU mean 5.46, 5.58, 5.73, 6.02, 6.04 microseconds; H2D flush 5.14, 5.35, 5.49, 5.61, 5.72 microseconds; CPU fallback mean 0.10, 0.12, 0.15, 0.55, 1.16 microseconds.">
</picture>

**The flush is 5.1–5.7 µs of a 5.5–6.0 µs mean.** What's left, 0.1–0.4 µs,
is smaller than the measurement error (see below), so subtraction can't
resolve it. A direct measurement can: an idle poll iteration takes **1.3 µs**
(`poll loop period`), so data that lands mid-iteration waits about 0.65 µs on
average and 1.3 µs at worst. That figure comes from the poller's own counters,
not from a clock comparison. Without the copy, GPU polling reacts in the same
order of magnitude as the CPU fallback's 0.1–1 µs.

The 1.3 µs iteration is itself a finding. On the idle path the kernel reads
`stop_flag`, which lives in host-mapped memory, so every empty poll pays a PCIe
round trip. Checking it less often should shorten the loop. That change is
untested.

| queues | GPU mean µs | GPU H2D flush µs | CPU mean µs |
|---|---|---|---|
| 1 | 5.46 | 5.14 | 0.10 |
| 2 | 5.58 | 5.35 | 0.12 |
| 3 | 5.73 | 5.49 | 0.15 |
| 4 | 6.02 | 5.61 | 0.55 |
| 5 | 6.04 | 5.72 | 1.16 |

<details>
<summary>How we got here: measuring the flush without distorting it</summary>

- **"Pre-load the ring before starting the timer": rejected.** The copy happens
  per batch for the whole run, not once at setup, so pre-loading only removes
  the first batch. It also leaves the ring already full when the poll loop
  starts, so "detection" becomes instant and the latency stops measuring
  anything.
- **"Pause the timer during the copy": rejected.** The copy engine and the
  poll loop run at the same time on different engines. The poll loop never
  waits on a copy in sequence, so there is nothing to pause around.
- **Split the metric instead.** Time the flush separately and report it next
  to the unchanged total. The first version timed the flush with a CUDA event
  pair and got it wrong: the flush came out longer than the whole latency
  (10 µs of flush inside a 5 µs total). A standalone test on the same GPU
  found the cause: recording events around a 32-byte copy more than doubled
  the time until a spinning kernel saw the data (3.5 → 8 µs). The measurement
  was slowing down the thing it measured.
- **What ships:** 1 in 32 flushes is timed with a host clock around
  `cudaStreamSynchronize`. This leaves the copy alone, but it reads about
  1.4 µs high, because the host needs time to wake after the copy completes.
  The mean latency also carries the ~2 µs clock-offset error. So the
  `outside H2D flush` residual is only good to about ±2 µs, and it can go
  negative.
- **Opt-in, because it still perturbs heavy load.** The sampled sync stalls
  its producer. At 50 kpps that made no difference, but at 2 × 200 kpps it
  throttled the copy backlog and cut mean latency from ~11 µs to ~7 µs. So
  timing only runs with `--copy-timing`, and `bench.py` gets those numbers
  from a separate `copy` scenario. Every other figure on this page comes from
  runs without it.
</details>

## Multi-queue latency

Holding total load at 400 kpps and splitting it over more queues cuts mean
latency:

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/multiqueue-latency-dark.svg">
  <img src="img/multiqueue-latency.svg" width="720" alt="GPU poller at 400 kpps total: mean detection latency 18.9 microseconds with 1 queue, 11.4 with 2, 7.2 with 4 and 6.6 with 8.">
</picture>

Single-queue runs are the noisiest (14.6–29.1 µs over 5 runs). At 400 kpps,
one producer thread feeding one copy stream is the bottleneck, and 4 or 8
queues spread that work out. [`design.md`](design.md#multi-queue) has the
reasoning and the rejected alternatives.

## What Nsight Systems shows

An Nsight Systems trace of one queue at 100 kpps for 1 s (GTX 1650) confirms
the design from the outside:

- **The host does no polling.** The main thread spends the whole run in a
  single 1 s `nanosleep`. `gnp_poll_kernel` is one launch that runs for the
  full second. The only busy host thread is the simulated NIC, `gnp-prod-0`.
- **Pacing is exact.** 100,006 H2D copies of 32 B each, at 100,004 per second.
  The median gap between copies is 9.95 µs against a 10 µs target.
- **Each flush costs about 3.3 µs.** That is about 2 µs in the host
  `cudaMemcpyAsync` call and 1.3 µs of DMA at the median. The copy engine is
  busy 15% of the run.
- **The latency spikes come from the host scheduler, not the GPU.** The worst
  gaps between copies (1.45 ms, 413 µs and 126 µs) line up with `gnp-prod-0`
  being scheduled off its core.

The trace (`profiles/gnp_q1.nsys-rep`) is local only: `*.nsys-rep` is
gitignored. To make a new one:

```bash
nsys profile -o profiles/gnp_q1 ./build/dev/testing/gnp_sim --pps 100000 --duration 1000
nsys stats profiles/gnp_q1.nsys-rep
```

## Reading the run summary

`gnp` and `gnp_sim` print the same summary.

**Correctness.** `packet-id gaps` must be 0, and `descriptors published` must
equal `packets observed`. A gap means the poller mis-sequenced the ring. A
shortfall (`NOT observed`) means it stopped before draining. For `gnp`, the
checks in the [main README](../README.md#read-the-result) apply too.

**Producer section.**
- `ring-full stalls` counts times the producer waited because the poller was
  behind.
- `frames dropped` (AF_XDP only, shown when non-zero) counts frames too large
  for `--size`, or malformed.
- `achieved rate` divides by the whole run, so for `gnp` it understates
  bursty traffic.

**Consumer section.**
- `idle spins per packet` is how many times the poller read a descriptor and
  found nothing, per useful packet. At low rates it is large by construction:
  that is the cost this design accepts in exchange for leaving the CPU alone.
- `poll loop period` (single queue) is poller active time divided by loop
  iterations. A descriptor that lands mid-iteration waits about half a period,
  so this is the reaction time without any cross-clock comparison.
- `service time` is active time per packet.

**Latency section.**
- Latency runs from `post_ns` (host clock at publish) to the poller seeing the
  descriptor.
- On CUDA, the GPU's `%globaltimer` is translated onto the host clock with a
  calibrated offset, good to about ±2 µs. The large `cross-clock offset
  applied` value is only the difference between the two clocks' epochs.
- `clamped to zero` counts samples that went negative because of that error.
- With `--copy-timing`, `H2D flush, submit->done` and `outside H2D flush` split
  the mean as described [above](#where-the-gpu-latency-goes).

**Multi-queue.** The top sections are the aggregate, with counters summed and
latency pooled. A per-queue table follows. Check it: a pooled mean can hide one
slow queue.
