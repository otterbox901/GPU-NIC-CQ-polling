# Documentation

| document | read it for |
|---|---|
| [`../README.md`](../README.md) | what `gnp` is, building it, running it on a NIC |
| [`design.md`](design.md) | the owner-bit protocol, publication ordering, memory placement, the AF_XDP producer, multi-queue, and the next steps |
| this file | measured results and how to read the run summary |
| [`../testing/README.md`](../testing/README.md) | how every result here was produced, and how to reproduce it |
| [`bench/`](bench/) | the machine the measurements came from (`machine.txt`) |
| [`img/`](img/) | the charts below, light and dark |

## Status

| component | state | evidence |
|---|---|---|
| ring protocol | verified | `test_ring` |
| ring poller (CUDA) | verified, 1–N queues | `run_sim.sh` sweep, benchmarks below |
| AF_XDP ingest, generic XDP on veth | verified | 10,000/10,000 packets, 0 gaps ([reference run](../testing/README.md#3-real-ingest-path-pcap-replay-over-veth)) |
| AF_XDP ingest, generic XDP on a physical NIC | verified | 9,999/9,999 packets, 0 gaps, USB Ethernet ([procedure](../testing/README.md#4-real-nic-tcpreplay-from-a-second-machine)) |
| native (driver) XDP on a physical NIC | **not yet tested** | needs a native-XDP NIC ([checklist](../README.md#run-on-a-nic)) |
| GPU-direct receive (DOCA GPUNetIO) | **built, never run** | compiles and links against DOCA 3.5 and refuses cleanly with no NIC; needs a ConnectX-6 Dx or newer ([how](../README.md#building-the-gpu-direct-path)) |
| publish -> observe latency | **not measured** | needs a host/GPU time base this hardware does not provide ([why](#why-latency-is-not-reported)) |
| `on_packet()` | placeholder, empty | [`packet_handler.hpp`](../include/gnp/packet_handler.hpp) |

## GPU polling vs CPU polling

All numbers in this section come from the **simulated NIC** (`gnp_sim`). It
generates arrivals at a precise rate on any machine, so the two pollers can be
compared without network noise. The poll loop and the ring are the same code
`gnp` runs on real traffic.

> **Historical.** These numbers compare the CUDA ring poller against a CPU
> fallback poller that has since been removed, when the project moved to
> GPU-direct receive. They are kept because they are the measured result that
> justified moving polling off the CPU, but they cannot be reproduced from this
> tree: the CPU backend is gone, and the raw CSV they came from was overwritten
> by a later contended run and has been dropped rather than left contradicting
> the charts. `testing/scripts/bench.py` now measures the GPU path only.

Both backends ran the **same poll loop over the same owner-bit ring**. They
differed only in where the loop ran and which memory it read:

| | GPU poller ([`poll_kernel.cu`](../src/gpu/poll_kernel.cu)) | CPU fallback (removed) |
|---|---|---|
| poll loop runs on | an SM: one single-thread block per queue | a host core: one spinning thread per queue |
| ring it reads | device memory, filled by DMA over PCIe | host memory, written directly by the producer |
| **host cores spent polling** (5 queues) | **0.00** | **4.96** |
| H2D flush per batch (5 queues × 50 kpps) | 5.7 µs | none |
| poll loop period (1 queue) | 1.3 µs | 0.002 µs |
| unpaced throughput (5 queues) | 14.3 Mpps | 75.8 Mpps |

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/host-cpu-dark.svg">
  <img src="img/host-cpu.svg" width="720" alt="Host CPU spent on polling, 1 to 5 queues: the GPU poller uses 0 cores at every queue count; the CPU fallback uses 0.99, 1.99, 2.98, 3.96 and 4.96 cores.">
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
- **Throughput favours the CPU fallback, and the gap is the host → device
  copy.** The CPU poller reads the ring the producer writes, so no descriptor
  ever crosses PCIe. The GPU path pays a DMA hop per batch (host staging →
  device ring), measured at 5.1–5.7 µs per flush. The AF_XDP path has the same
  hop, because AF_XDP delivers into host memory. It goes away only when the NIC
  writes straight into GPU memory (GPUDirect / DOCA).
- **Both backends scale roughly linearly** with the queue count.

Setup: GTX 1650 (sm_75, 16 SMs) and an i7-9750H (12 hardware threads). Each
point is the median of 5 runs. Every run delivered every packet with zero gaps.
The raw CSV behind these figures was not retained (see the note above), so the
charts and the table below are the record.

<details>
<summary>Data table</summary>

| queues | host cores polling (GPU / CPU) | unpaced Mpps (GPU / CPU) | GPU H2D flush µs |
|---|---|---|---|
| 1 | 0.00 / 0.99 | 3.0 / 22.5 | 5.14 |
| 2 | 0.00 / 1.99 | 6.0 / 41.1 | 5.35 |
| 3 | 0.00 / 2.98 | 8.9 / 59.0 | 5.49 |
| 4 | 0.00 / 3.96 | 11.6 / 65.9 | 5.61 |
| 5 | 0.00 / 4.96 | 14.3 / 75.8 | 5.72 |

Host CPU is measured per thread from `/proc`, excluding the simulated
producers, which cost one core per queue on both backends. Host CPU and the
flush come from runs at 50 kpps per queue for 2 s. Throughput comes from
unpaced runs on 64-entry rings for 1 s.
</details>

## Why latency is not reported

The obvious question about a polling design is how long it takes to notice a
packet. `gnp` does not answer it, on purpose.

Measuring publish → observe means comparing a host timestamp (`post_ns`, stamped
by the producer from `steady_clock`) with a GPU timestamp (`%globaltimer`, read
by the poller). Those two clocks share neither an epoch nor a rate, so the
comparison needs a calibrated offset, and it is only as good as that offset.

On this hardware it is not good enough to publish:

- A ping-pong calibration over 4096 round trips pins the offset to **±0.8–1.1 µs**
  — a PCIe round trip is ~2.7 µs, and that is the floor of the method.
- The offset then **moves during the run**. Measured drift ranged from 2 to
  326 ppm across runs on a GTX 1650, i.e. the offset goes stale by tens to
  thousands of µs over a few seconds. A calibration taken while the GPU's clocks
  were still ramping contradicted itself outright.
- The drift is not a fixed rate that could be corrected out: its apparent sign
  and size vary run to run (+5.1, +1.9, +1.7, −23.7, −326 ppm), where a real
  crystal pair would hold a fraction of a ppm.

So a publish → observe figure on this machine is dominated by the clock, not by
the poller. An earlier version of this code reported one anyway, and produced a
confident `mean 0.146 us` on a real-NIC run where the clock error had pushed 624
of 625 samples negative. Rather than publish a number with an error bar wider
than the number, the measurement was removed.

**What is reported instead**, all of it from one clock or from counters:

- **`poll loop period`** — poller active time ÷ loop iterations, **1.3 µs** on a
  GTX 1650. A descriptor that lands mid-iteration waits about half that on
  average, one period at worst. This is the direct measure of how fast the
  poller reacts, and it needs no second clock.
- **`H2D flush, submit->done`** (`--copy-timing`) — **5.1–5.7 µs** per batch,
  timed with the host clock on both ends of the copy. This is the PCIe hop the
  GPU path pays and the CPU path does not.
- Counts, `packet-id gaps`, bytes and stalls, none of which involve a clock.

Together those bound the picture: the flush dominates, and the poller's own
reaction is on the order of a microsecond. What is missing is the single
end-to-end number, and getting it would need a time base this GPU does not
provide — GPU persistence mode and locked clocks, calibration repeated *during*
the run, or a scheme that stays entirely in device time.

The 1.3 µs iteration is itself a finding. On the idle path the kernel reads
`stop_flag`, which lives in host-mapped memory, so every empty poll pays a PCIe
round trip. Checking it less often should shorten the loop. That change is
untested.

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
- **The stalls come from the host scheduler, not the GPU.** The worst gaps
  between copies (1.45 ms, 413 µs and 126 µs) line up with `gnp-prod-0` being
  scheduled off its core.

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

**Multi-queue.** The top sections are the aggregate, with counters summed. A
per-queue table follows. Check it: a pooled mean can hide one
slow queue.
