# Testing

Everything that verifies `gnp` lives here. Nothing in this directory is part of
the production build: it is compiled only with `GNP_BUILD_TESTING=ON`, which the
`dev`, `dev-cpu` and `debug` presets set. See the [main README](../README.md)
for building `gnp` itself.

| level | tool | needs | proves |
|---|---|---|---|
| 1. protocol | `test_ring` (ctest) | nothing | owner-bit ring logic, wrap-around, queue independence |
| 2. pollers | `gnp_sim` + `run_sim.sh` | a build | the GPU/CPU pollers drain every packet, in order, under load |
| 3. ingest | `gnp` + veth + `replay_pcap.py` | root, libxdp | XDP → AF_XDP → ring → poller works on real frames |
| 4. hardware | `gnp` + NIC + a sender | root, a second machine | the same, on a physical NIC |
| perf | `bench.py` + `plot_bench.py` | CUDA and CPU builds | host CPU, throughput and H2D flush ([results](../docs/README.md)) |

## Build

```bash
cmake --preset dev     && cmake --build --preset dev       # CUDA poller (if nvcc found)
cmake --preset dev-cpu && cmake --build --preset dev-cpu   # CPU poller
```

This produces, per preset:

```
build/<preset>/gnp                   the production receiver (same as the prod build)
build/<preset>/testing/gnp_sim       the pollers fed by a simulated NIC
build/<preset>/testing/test_ring     ring-protocol unit tests
```

## 1. Unit tests

```bash
ctest --preset dev        # or dev-cpu
```

`unit/test_ring.cpp` drives `gnp::ring_publish()`, the same publish function the
AF_XDP path uses, against rings in plain host memory. The index math in
`ring.hpp` is the code the kernel runs, so no GPU is needed. It covers:
- the 32-byte descriptor layout
- slot and owner-bit math
- an empty ring on zeroed memory
- publish/consume across many wraps
- filling the ring and draining it
- rings not interfering with each other
- multi-queue stats aggregation

## 2. The simulated NIC and the correctness sweep

`gnp_sim` runs the same pollers as `gnp`, fed by one host thread per queue that
makes up packets at a controlled rate (`sim/sim_inject.cpp`). It publishes
through the same `ring_publish()`. It needs no network, no root and no libbpf,
and it supports many queues. This is how the protocol was proven before real
ingest existed.

```bash
testing/scripts/run_sim.sh ./build/dev/testing/gnp_sim       # ~20 s
testing/scripts/run_sim.sh ./build/dev-cpu/testing/gnp_sim
```

The sweep covers paced, unpaced, bursty, jumbo, near-idle, fixed-count and
multi-queue runs, plus rejection of too many queues. A run passes only with
zero `packet-id gaps` and `descriptors published` == `packets observed`. The
sweep ends with `sweep passed`, or `FAIL` lines. Saved sweeps are in
[`results/`](results/).

Running `gnp_sim` by hand:

```bash
./build/dev/testing/gnp_sim --verbose                    # 1024-entry ring, 100 kpps, 2 s
./build/dev/testing/gnp_sim --pps 0 --ring 64            # unpaced, exercises back-pressure
./build/dev/testing/gnp_sim --packets 50000 --burst 16   # fixed count, bursty arrivals
./build/dev/testing/gnp_sim --queues 4 --pps 100000      # 4 independent queues, 400 kpps total
```

It accepts `--queues --ring --size --duration --backoff --copy-timing --verbose`
like `gnp`, plus:

| flag | meaning | default |
|---|---|---|
| `--pps R` | injection rate **per queue**, `0` = unpaced | 100000 |
| `--packets N` | stop after N packets **per queue**, `0` = use duration | 0 |
| `--burst N` | descriptors published back-to-back | 1 |

Multi-queue notes:

- **Resident-poller limit.** Pollers are persistent, so all of them must be
  resident at once, or the late ones never start and the run hangs. The limit
  is max-resident-blocks-per-SM × SM count (256 on a 16-SM GTX 1650). A higher
  `--queues` is refused before anything is allocated.
- **`--pps` is per queue.** `--queues 4 --pps 100000` is 400 kpps in total, so
  compare it with `--queues 1 --pps 400000`.
- **Simulator limit.** Each simulated producer busy-spins on a host thread.
  Past one per hardware thread, the results measure host scheduling, and the
  program warns.

With more than one queue, the summary shows the aggregate under the usual
labels, followed by a per-queue table. Check that too: a pooled mean can hide
one slow queue.

```
  per-queue breakdown
  queue   published    observed   gaps   stalls   idle/pkt    us/pkt lat avg us lat max us    neg
  --------------------------------------------------
  0          100012      100012      0        0      7.341    10.002     15.917     45.906      0
  1          100007      100007      0        0      7.343    10.003     15.696     80.548      0
```

`gnp` reports no publish -> observe latency; see
[docs/README.md](../docs/README.md#why-latency-is-not-reported) for why. Read
`us/pkt` and the aggregate `poll loop period` instead.

## 3. Real ingest path: pcap replay over veth

`scripts/replay_pcap.py` has two subcommands:
- `make` writes a sample UDP capture.
- `send` replays the IPv4/UDP packets of any pcap onto an interface. It can
  rewrite the destination MAC, IP and port so a capture from elsewhere reaches
  `gnp`.

Frames go out at layer 2 on one raw socket, so they arrive at the receiving
interface's XDP hook like traffic from a wire. It needs scapy
(`sudo dnf install python3-scapy`). Python sends one packet per syscall, which
is fine for correctness but is not a load test.

Without a spare NIC, use a veth pair: two virtual interfaces joined by a
virtual cable. It has no native XDP, so run `gnp` with `--skb-mode`.

```bash
sudo ip link add veth0 type veth peer name veth1
sudo ip addr add 10.99.0.1/24 dev veth0 && sudo ip addr add 10.99.0.2/24 dev veth1
sudo ip link set veth0 up && sudo ip link set veth1 up
testing/scripts/replay_pcap.py make --out udp.pcap --count 10000 --size 512

# terminal A
sudo ./build/prod/gnp --iface veth0 --port 9000 --skb-mode --duration 30000

# terminal B: only after A prints "capturing UDP port 9000 ..."
sudo testing/scripts/replay_pcap.py send --pcap udp.pcap --iface veth1 \
    --dst-mac "$(cat /sys/class/net/veth0/address)" --dst-ip 10.99.0.1 --dst-port 9000 --pps 5000

sudo ip link del veth0   # cleanup
```

Start the sender only while `gnp` is capturing. Frames sent outside the window
go to the kernel stack, and the report shows `XDP program: 0 frames`.

Any second device works as the sender. A phone tethered over USB is the least
trouble: it appears as an ordinary interface (`ipheth`, e.g. `eth0`), and a few
lines of Python in a terminal app send to it, so no capture or extra tool is
needed. Run `gnp` against that interface with `--skb-mode` exactly as above.

**Reference run** (2026-09-18, GTX 1650, CUDA poller, generic XDP on veth):

```
[gnp] AF_XDP kernel counters: rx_dropped=0 rx_invalid=0 rx_ring_full=0 fill_ring_empty=0
[gnp] XDP program: 10000 frames to UDP port 9000, 10000 of them on RX queue 0
  descriptors published                 10000
  packets observed                      10000
  bytes observed                      5120000
  packet-id gaps                            0
```

Every frame the replay sent was matched by XDP, published, and observed by the
GPU, with 512 payload bytes each. `achieved rate` in that run reads 0.000 Mpps
because it divides by the whole 30 s window, not the 2 s of traffic.

## 4. Real NIC: tcpreplay from a second machine

Prepare the receiver as described in the
[main README](../README.md#run-on-a-nic): check the driver, steer the port, and
run without `--skb-mode`. Then, on the sender:

```bash
sudo dnf install tcpreplay

# address the frames to the receiver
tcprewrite --infile=udp.pcap --outfile=udp-rw.pcap \
    --enet-dmac=<receiver MAC> --dstipmap=0.0.0.0/0:<receiver IP>/32 \
    --portmap=<original dport>:9000 --fixcsum

sudo tcpreplay --intf1=<sender iface> --pps=10000 udp-rw.pcap
```

1. Start at `--pps 10000` and check the pass criteria below.
2. Raise `--pps` (and finally use `--topspeed`) until `rx_ring_full`,
   `fill_ring_empty` or `ring-full stalls` goes non-zero. That point is the
   capture ceiling for this NIC, ring size and poller.

**Pass criteria** for any replay:
- `XDP program:` reports as many frames on the bound queue as were sent.
- `descriptors published` == `packets observed` == that count.
- `packet-id gaps` == 0 and `frames dropped` absent.
- All four AF_XDP kernel counters are 0.

## Benchmarks

```bash
testing/scripts/bench.py        # ~5 min; runs both dev builds, writes docs/bench/results.csv
testing/scripts/plot_bench.py   # docs/bench/results.csv -> docs/img/*.svg, prints the tables
```

`bench.py` runs the `gnp_sim` binaries from `build/dev` and `build/dev-cpu`
(change them with `--gpu` / `--cpu`). It records host CPU per thread from
`/proc`, and uses the `gnp-prod-*` thread names to leave out the simulated
producers. Copy timing (`--copy-timing`) runs only in its own `copy` scenario,
because it disturbs heavily loaded runs. The results and their analysis are in
[docs/README.md](../docs/README.md).

## Layout

```
CMakeLists.txt          gnp_sim, test_ring + ctest
include/gnp_testing/
  sim.hpp               sim_start / sim_request_stop / sim_stop
sim/
  sim_inject.cpp        the simulated NIC: paced, bursty or unpaced, one thread per queue
  main_sim.cpp          gnp_sim: same orchestration as gnp, simulator instead of AF_XDP
unit/
  test_ring.cpp         ring-protocol unit tests
scripts/
  run_sim.sh            correctness sweep over gnp_sim
  replay_pcap.py        make a UDP pcap / replay one onto an interface
  bench.py              GPU vs CPU benchmark -> docs/bench/results.csv
  plot_bench.py         results.csv -> docs/img/*.svg (stdlib only)
results/
  cpu_sweep.txt         saved run_sim.sh output, CPU poller
  gpu_sweep.txt         saved run_sim.sh output, CUDA poller
```
