# gpu-nic-poll — design

## Problem

In a conventional kernel-bypass receive path the CPU burns a core spinning on a
completion queue. Every arrival costs a poll loop iteration on the host, and the
data then has to be copied or mapped to wherever it is actually consumed.

If the consumer is a GPU, all of that is wasted motion. The NIC can DMA straight
into GPU memory (GPUDirect / PeerDirect), and the completion ring can live there
too — so an SM can do the polling itself and the CPU can stay idle.

This prototype builds the polling half of that. Packets reach it through AF_XDP
(kernel bypass into host memory), and a software producer in `testing/` stands
in for the NIC during development.

## Structure

```
producer                    ring (GPU memory)              consumer
─────────────────────       ────────────────────────       ─────────────────
xdp_ingest.cpp        ──►   CompletionRing            ◄──  poll_kernel.cu
(AF_XDP RX thread)          capacity × 32 B CQE            persistent kernel
                            + RingControl                  1 thread per queue
testing/sim/          ──►   + payload arena           ◄──  (poll_cpu.cpp when
sim_inject.cpp (dev)                                        nvcc is absent)
                                                            └─ on_packet()

        × N queues (--queues N): each row above is replicated per queue,
          and no queue shares memory, a stream or a thread with another.
```

The ring is the seam. Swapping the simulator for real AF_XDP ingest did not
touch the consumer: both producers publish through the same `ring_publish()`
(`src/host/ring.cpp`), and the pollers cannot tell them apart.

### The AF_XDP producer

`src/bpf/udp_redirect.bpf.c` runs at the NIC driver's XDP hook. It parses
Ethernet/IPv4/UDP and redirects unfragmented datagrams to one destination port
into `xsks_map[rx_queue_index]`. Everything else is passed to the kernel stack.
The map is keyed by RX queue because AF_XDP only accepts frames that arrived on
the queue its socket is bound to. A matching packet on a queue with no socket
falls back to `XDP_PASS` instead of being dropped.

`src/host/xdp_ingest.cpp` owns the UMEM (4096 × 4 KiB frames), the fill ring
and one socket. Its RX thread re-validates each frame, copies the UDP payload
into the arena slot for the next ring index, publishes the descriptor, and
recycles the frame onto the fill ring within the same batch. The copy exists
because UMEM frames must go back to the kernel promptly, while ring slots are
held until the poller consumes them. Decoupling the two lets a slow consumer
back up into the ring instead of starving the NIC of frames.

## Arrival detection: the owner bit

The consumer cannot be told that a packet arrived — there is no doorbell it can
receive and no interrupt it can take. So arrival has to be inferable from the
descriptor memory alone.

Real NIC completion queues solve this with an **owner (phase) bit**, and this
prototype copies that design:

- `CompletionDesc::status` bit 0 is the owner bit.
- The producer flips the expected value every time it wraps the ring.
- At index `i` the consumer expects `((i / capacity) & 1) ^ 1`.
- A zeroed ring therefore reads as empty for the whole first pass — pass 0
  expects owner `1`, and freshly allocated memory holds `0`.

The payoff is that the consumer never needs a producer index. It reads one
32-bit word, compares one bit, and knows whether slot `i` holds a new entry. No
shared counter, no atomics, no host round trip.

`ring_slot`, `ring_expected_owner` and `desc_ready` are `GNP_HD` inline
functions in `ring.hpp`, so the kernel, the CPU fallback and the unit tests all
run the *same* index math. A protocol bug cannot hide in one copy.

## Publication ordering

The producer must not let the owner bit become visible before the payload
fields. `ring_publish()` writes `payload_offset`, `byte_len`, `packet_id`,
`post_ns`, then a barrier, then `status`.

The barrier is `memory_order_seq_cst`, not `release`, and that is deliberate. On
x86 a release fence is only a compiler barrier — correct for write-back memory,
but these stores may land in write-combining PCIe-mapped memory, where the CPU
reorders freely. A seq_cst fence emits a real `mfence` and drains the WC
buffers.

On the consumer side, `__threadfence_system()` after the `status` load keeps the
payload loads from floating above it.

## Memory placement

The CUDA backend keeps **two** copies of the completion ring:

1. **Host staging** (`cudaHostAlloc` mapped) — the producer (AF_XDP RX thread or
   simulator) publishes here.
2. **Device CQ** (`cudaMalloc`) — the persistent poller reads here, so status
   loads hit GDDR instead of bouncing over PCIe every spin.

`backend_flush_descs()` pushes completed CQEs across with the **DMA copy
engine** (`cudaMemcpyAsync`), two-phase per slot (payload fields, then owner
bit). A compute flush kernel was tried and abandoned: the persistent poller
starves other kernels on this GPU, whereas the copy engine runs concurrently.
Unpaced runs batch 32 CQEs per flush; paced runs flush every burst.

Flushes go on a per-queue copy stream; see "Multi-queue" below for why.

Control/stats stay in pinned mapped memory (both sides read/write them). The
payload arena is ordinary host memory — the poller never inspects packet bytes.

`cudaMallocManaged` with a GPU-preferred location was tried first and abandoned:
bidirectional touch thrash-migrates pages, which showed up as multi-millisecond
stalls before a descriptor was observed. A real NIC will DMA straight into the
device CQ and the flush step disappears; the poller does not change.

Device-side CQ loads use `ld.acquire.gpu` on the owner bit so payload fields
cannot float above the status store.

## Why one thread per queue

A completion queue is consumed strictly in order, so exactly one lane can own
its head; extra lanes on the same queue would contend on the same descriptor and
buy nothing. `gnp_poll_kernel` therefore gives each queue a block of exactly one
thread. Width comes from **more queues**, not more lanes per queue — see
"Multi-queue". Payload *processing* is the other place width belongs, and that
is still out of scope — see "Next steps".

## Multi-queue

`--queues N` runs N completion rings side by side, the way a multi-queue NIC
uses RSS to spread flows over independent Rx queues. Each queue has its own
ring, `RingControl`, `PollStats`, payload arena, producer thread, copy stream
and poller. Nothing is shared, so no queue ever waits on another, and each
poller runs *exactly* the single-queue loop.

### Alternatives rejected

- **Several lanes polling one ring.** Lanes would have to claim slots through a
  shared counter or a CAS on the owner bit. That is true sharing — every claim
  serialises on one cache line — and it breaks the in-order cursor that gap
  detection and the drain protocol rely on.
- **Padding descriptors apart.** Padding cures *false* sharing between
  independent writers. The claim counter above is *true* sharing, which padding
  cannot fix; and spreading CQEs over separate cache lines would undo the
  coalesced loads a tight 32-byte layout gets.

### Launch: one grid, block b owns queue b

The host builds one `PollQueue {ring, ctrl, stats}` per queue, copies the array
to device memory once, and launches a single `<<<N, 1>>>` grid. Block `b` reads
`queues[b]` at entry and never touches the array again, so the hot loop is
byte-for-byte the single-queue loop and `N = 1` costs nothing extra. One launch
(rather than N) means one stream, one sync and no partial-launch states.

### The residency limit (deadlock hazard)

The pollers are persistent: a block only exits when told to stop. CUDA does not
preempt a resident block to make room for a queued one, so if the grid has more
blocks than the GPU can hold **at the same time**, the extra blocks never start —
and `backend_wait_poller()` hangs forever.

`backend_max_queues()` computes the true limit as
`cudaOccupancyMaxActiveBlocksPerMultiprocessor(kernel, 1 thread, 0 smem) × SM count`.
For this tiny kernel the binding constraint is the per-SM resident-block cap
(16 on sm_75), not registers — a GTX 1650 with 16 SMs allows 256 queues.
`session_create()` rejects a larger `--queues` **before allocating anything**, and
`backend_launch_poller()` re-checks, because the failure mode is a hang, not an
error.

Up to the SM count, the block scheduler places one poller per SM. Past it,
pollers share an SM's warp schedulers. That is still correct, but each spinning
poller takes issue slots from its neighbours.

### Copy streams (simulation only)

The simulated NIC flushes host staging to the device ring with `cudaMemcpyAsync`.
Measured on a GTX 1650 with a 12-thread host:

- One copy stream shared by all queues serialises every producer's flushes:
  8 queues × 100 kpps delivered about 0.1 Mpps in total.
- One stream per queue removes that, and it stayed the best arrangement up to
  32 queues when this was measured against 2–16 shared streams.
- The streams must exist **before** the pollers launch. Per-thread default
  streams, which the driver creates lazily on first use (mid-run, from producer
  threads), were unreliable: at 32–128 queues most flushes did not execute while
  the pollers ran and only drained once the pollers' watchdog fired.

So `backend_setup_copy()` creates `min(N, 32)` non-blocking streams up front,
and queue `q` uses stream `q % 32`. This is a property of the software producer:
a real NIC DMAs into the device CQ itself and has no copy streams.

### Stopping N queues

- The drain protocol runs per queue: store that queue's `publish_limit`, fence,
  then raise its `stop_flag`. There is no ordering requirement *across* queues.
- Producers are all **signalled before any is joined** (`sim_request_stop`, then
  `sim_stop`). Stopping them one at a time let the rest keep publishing: at
  256 queues a "1 s" run published 1.5 M packets over 9 s, long enough for the
  pollers' watchdog to fire before they drained.

### Ids, pacing, reporting

- Packet ids are per queue, starting at 0. A global id would need a shared
  atomic counter in the producers — the same contention this design avoids.
  Gap detection is per poller, so it works unchanged.
- `--pps` and `--packets` are **per queue**, so total offered load is
  `N × pps`. To see what parallelism buys, hold total load constant:
  compare `--queues 4 --pps 100000` with `--queues 1 --pps 400000`, not with
  `--queues 1 --pps 100000`.
- The report's top sections are the aggregate, under the same labels as a
  single-queue run: counters summed, and the producer window is the union.
  `testing/scripts/run_sim.sh` therefore parses N-queue runs unchanged. A per-queue
  table follows, and it matters: an aggregate can hide one straggling queue.
  For `N = 1` the output is identical to the pre-multi-queue format.
- `service time` is omitted from the aggregate: pollers run concurrently, so
  wall time divided by total packets is no single poller's service time. The
  per-queue table reports the real figure.

### Measured (GTX 1650, sm_75, 16 SMs; i7-9750H, 12 threads)

All numbers come from `testing/scripts/bench.py` (raw data in
[`bench/results.csv`](bench/results.csv)). Each is the median of 5 runs, and
every run delivered every packet with zero gaps. `testing/scripts/plot_bench.py`
redraws the README charts from the same file.

Unpaced, 64-entry rings: throughput scales with the queue count.

| queues | delivered |
|---|---|
| 1 | 3.0 Mpps |
| 2 | 6.0 Mpps |
| 3 | 8.9 Mpps |
| 4 | 11.6 Mpps |
| 5 | 14.3 Mpps |

At 400 kpps through a single queue, one producer thread submitting ~400k small
copies a second to one stream is the bottleneck, and the run-to-run spread is
wide. That is a property of the simulated NIC rather than of the poller;
spreading the same total over 4–8 queues steadies it.

### GPU poller vs CPU fallback

The same benchmark runs the CPU fallback: the identical loop on one host thread
per queue. [`docs/README.md`](README.md#gpu-polling-vs-cpu-polling) charts the comparison. In short:

- **Host CPU spent polling:** 0.00 cores for the GPU poller at every queue
  count, against one full core per queue for the CPU fallback. This is measured
  per thread from `/proc`, using the `gnp-poll-N` and `gnp-prod-N` thread names,
  with the producers excluded.
- **Throughput** favours the CPU fallback: up to 76 Mpps against 14 Mpps on the
  GPU. That is because the simulator writes the CPU poller's ring directly, with
  no PCIe crossing, while the GPU path copies every batch over PCIe. A real NIC
  puts both paths behind PCIe DMA, so this comparison flatters the CPU. The
  host-CPU result does not depend on the simulator.
- **The PCIe copy dominates the GPU path.** With `--copy-timing`, the sampled
  H2D flush is 5.1–5.7 µs per batch, while the poller's own reaction is one
  `poll loop period` at worst (1.3 µs per idle iteration, so ~0.65 µs on
  average). See "The two figures that do get measured" below.

### Limits

- **Paced flush ceiling.** Paced runs issue one `cudaMemcpyAsync` per packet,
  and on this machine that caps out near 0.45 Mpps in total whatever `N` is.
  Past it, rings fill and the run measures the copy backlog, not the pollers.
  Unpaced runs batch 32 CQEs per copy and do not hit this ceiling.
- **Host threads.** Every simulated producer busy-spins. With more producers
  than hardware threads they preempt each other (and CUDA's internal locks), and
  the results reflect host scheduling. The program warns when this happens.
  The CPU backend needs two spinning threads per queue.
- **SM budget.** Each poller holds a whole block slot for one spinning thread.
  The residency limit allows hundreds of queues, but in any real deployment `N`
  should stay well below the SM count. Every SM running a poller is denied to
  whatever compute the GPU is there to do.

## Stopping

`RingControl::publish_limit` and `stop_flag` are host→device. After the producer
quiesces, the host stores the final produced count into `publish_limit`, fences,
then raises `stop_flag`. The kernel checks these **only on the idle path**, and
only retires once `idx >= publish_limit`. That closes the race where `stop`
becomes visible before the last CQE status bit — which previously showed up as
`published == observed + 1` on small rings.

There is also a hard `max_run_ns` ceiling (requested duration + 5 s) checked
every 1024 idle spins and every 64 packets, so a crashed host cannot wedge the
GPU indefinitely.

`RingControl::consumed` goes the other way, device→host, updated every 64
entries. The producer uses it for back-pressure. It is deliberately stale — a
lagging watermark only ever makes the producer more conservative.

## Timing, and what is not measured

Two clocks exist and they are never compared. The host stamps every descriptor's
`post_ns` with `steady_clock`; the kernel reads `%globaltimer` only for
device-side intervals (how long the poller ran, when to retire). There is no
publish → observe latency in the summary, because on this hardware the
comparison those two clocks would need is not accurate enough to report: the
offset can be pinned to about ±1 µs, but it then drifts by 2 to 326 ppm over a
run, with a sign and size that vary run to run. The reasoning, the measurements
and what it would take to do properly are in
[`../docs/README.md`](README.md#why-latency-is-not-reported).

What is measured instead needs no second clock: the poll loop period from the
poller's own counters, the H2D flush timed on the host at both ends, and the
counters.

### The two figures that do get measured

The copy engine and the poll loop run concurrently, so there is no point where
the poller "waits for the copy" that a timer could pause around. Each is
measured on its own instead.

- **Flush time (`--copy-timing`, CUDA only).** `backend_flush_descs` times 1 in
  32 calls per queue: `host_now_ns()` before the `cudaMemcpyAsync` calls and
  again after `cudaStreamSynchronize`. The figure includes queueing behind the
  stream's earlier flushes, which the CQEs really do wait through. Timing state
  is one slot per queue, touched only by that queue's producer thread, and is
  reported through `backend_copy_stats` into `SimStats`. The CPU backend's copy
  fields stay zero.
- **Poll reaction (always on).** `poll loop period` = poller active time ÷ loop
  iterations. A CQE that lands mid-iteration waits for the next status load,
  about half a period on average. This needs no clock offset.

Both are honest on their own terms, and neither is subtracted from the other:
the flush reads ~1.4 µs high (host wake-up after the sync), and in unpaced runs
a CQE also waits for its 32-entry batch to fill before the flush starts.

Rejected approaches:

- **CUDA event pair around the copy.** On the GTX 1650, recording events
  around a 32-byte H2D copy more than doubled the time until a spinning kernel
  saw the data (3.5 → 8 µs), and the pair read 5.7 µs for a ~3 µs copy: the
  measurement changed what it was measuring.
- **Always-on sampling.** The sampled sync stalls the producer, which throttles
  the copy backlog near saturation and changes the flush time it reports. At
  50 kpps there was no measurable effect. Hence opt-in, so ordinary runs are
  undisturbed.
- **Timing every flush** would put a sync on every batch.
- **A non-blocking event pool**, drained later, wouldn't stall the producer,
  but events slow the copy itself on this GPU (see above).

The 1.3 µs idle iteration comes from the idle path reading `stop_flag` in
host-mapped memory, which is a PCIe round trip per empty poll. Checking it
every N idle spins should cut the reaction time. That change is untested.

## Non-goals for v1

No DPDK, no kernel module, no userspace network stack, no RSS hashing (queues
are fed by independent producers, not by a flow hash), no payload processing
(`on_packet()` is an empty hook), no reliability layer. Real ingest covers one
UDP port on one NIC RX queue, IPv4 only.

## Next steps, roughly in order

1. **Payload processing.** Give `on_packet()` (`include/gnp/packet_handler.hpp`)
   a body. For width, keep lane 0 as the CQ head and hand descriptors to the
   rest of the warp for checksum/parse work.
2. **GPU-visible payloads.** The arena is host memory, so the CUDA poller calls
   `on_packet()` with a null payload. Receiving straight into GPU memory
   (DOCA GPUNetIO / GDAKI, or GPUDirect RDMA) fixes that and also removes the
   RX thread's copy and the H2D descriptor flush.
3. **Multi-queue real ingest.** One AF_XDP socket per NIC RX queue, each
   feeding its own ring. The pollers and `xsks_map` already support it; the
   ingest and `main.cpp` are what's limited to one queue.
4. **Occupancy honesty.** A persistent kernel holds an SM forever, and with
   multi-queue it can hold many. Measure what that costs a co-resident compute
   kernel.

Done: **multi-queue** (one ring per block, `N` rings polled independently) —
see "Multi-queue".

Done: **real ingest, v1** — AF_XDP + an XDP redirect on one UDP port replaced
the simulator as `gnp`'s producer. The simulator moved to `testing/`.
