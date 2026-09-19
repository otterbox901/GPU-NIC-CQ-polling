# GPU NIC-CQ Polling (GNP)

Kernel-bypass UDP receive with completion-queue polling moved off the CPU and
onto GPU SMs.

Packets arrive through **AF_XDP**. An eBPF/XDP program on the NIC redirects one
UDP port into a userspace socket before the kernel network stack ever sees the
frame. An RX thread copies each UDP payload into a payload arena and publishes
a 32-byte `CompletionDesc` into a `CompletionRing` that lives in GPU memory. A
persistent CUDA kernel polls that ring from an SM and hands every packet to
`gnp::on_packet()`, the function where real processing goes (it is an empty
placeholder today). The main thread allocates, launches, sleeps and reports.

| document | contents |
|---|---|
| **this file** | what it is, how to build it, how to run it on a NIC |
| [`testing/README.md`](testing/README.md) | unit tests, the simulated NIC, pcap replay, benchmarks |
| [`docs/README.md`](docs/README.md) | measured results, latency analysis, and an index of the design docs |
| [`docs/design.md`](docs/design.md) | the protocol and the reasoning behind every design choice |

```mermaid
flowchart LR
    subgraph NIC["NIC"]
        XDP["XDP program<br/>udp_redirect.bpf.c<br/>UDP dport == --port ?"]
    end
    subgraph HOST["Host CPU"]
        UMEM[("AF_XDP UMEM<br/>(raw frames)")]
        RX["RX thread (xdp_ingest.cpp)<br/>parse, copy payload,<br/>ring_publish()"]
        ARENA[("payload arena")]
        STAGE[("pinned staging ring")]
        MAIN["main thread<br/>allocate, launch, sleep, report"]
        UMEM --> RX
        RX -- "UDP payload" --> ARENA
        RX -- "CompletionDesc,<br/>owner bit last" --> STAGE
    end
    subgraph GPU["GPU"]
        CQ[("device completion ring<br/>(GDDR)")]
        POLL["persistent poller<br/>on_packet(desc, payload)"]
        CQ -- "spin: one load +<br/>owner-bit compare" --> POLL
    end
    XDP -- "XDP_REDIRECT<br/>(bypasses the network stack)" --> UMEM
    XDP -. "any other traffic:<br/>XDP_PASS" .-> STACK["kernel network stack"]
    STAGE -- "DMA copy engine" --> CQ
    POLL -. "consumed watermark<br/>(back-pressure)" .-> RX
    MAIN -. "launch once,<br/>stop at the end" .-> POLL
```

## How a packet gets through

1. **Attach.** `gnp` loads `udp_redirect.bpf.o`, writes `--port` and
   `--nic-queue` into its `config_map`, and attaches it to `--iface`. It uses
   native (driver) XDP when the NIC supports it and generic (SKB) XDP otherwise.
2. **Open the socket.** One AF_XDP socket is bound to NIC RX queue
   `--nic-queue` (default 0) and registered in the program's `xsks_map`. A 16 MiB
   UMEM (4096 × 4 KiB frames) is handed to the kernel through the fill ring.
3. **Filter at the driver.** For every incoming frame, the XDP program checks
   Ethernet → IPv4 → UDP with bounds checks. An unfragmented datagram to the
   capture port is redirected into the socket. Anything else (other ports, TCP,
   ARP, IPv6, fragments) gets `XDP_PASS` and reaches the normal stack, so SSH
   and everything else on the interface keep working.
4. **Ingest.** The RX thread (`gnp-xdp-rx`) takes up to 64 descriptors from
   the RX ring at a time. For each frame it re-validates the headers, copies the
   UDP payload into arena slot `ring_slot(idx) × --size`, and calls
   `ring_publish()`. That fills `payload_offset`, `byte_len`, `packet_id` and
   `post_ns`, issues a full fence, and writes the owner bit last. The UMEM frame
   goes straight back on the fill ring.
5. **Move to the GPU.** After each batch, the new descriptors are DMA'd from
   pinned staging into the device ring, owner bit last per slot.
6. **Poll.** The persistent kernel spins on one load plus an owner-bit compare.
   When a descriptor is ready it reads the fields, calls
   `on_packet(desc, payload)`, updates the counters, and publishes its consumed
   watermark every 64 packets so the RX thread knows which slots are free.
7. **Stop and report.** After `--duration`, the RX thread is joined, the XDP
   program is detached, and the socket is closed. The XDP program's and the
   kernel's counters are printed. The poller drains up to the final published
   count before it retires, then the summary is printed.

## Build

### 1. Install the dependencies

| dependency | needed for | Fedora | Debian / Ubuntu |
|---|---|---|---|
| CMake ≥ 3.24, g++ (C++17), pkg-config | everything | `cmake gcc-c++ pkgconf` | `cmake g++ pkg-config` |
| libbpf, libxdp, kernel headers | AF_XDP ingest | `libbpf-devel libxdp-devel kernel-headers` | `libbpf-dev libxdp-dev linux-libc-dev` |
| clang, llvm | compiling the XDP program | `clang llvm` | `clang llvm` |
| CUDA toolkit | GPU poller (optional) | see [below](#installing-cuda-fedora) | `nvidia-cuda-toolkit` |

```bash
sudo dnf install cmake gcc-c++ pkgconf clang llvm libbpf-devel libxdp-devel kernel-headers
```

Without CUDA, the poller runs on a host thread instead of an SM. Without
libbpf, libxdp or clang, `gnp` still builds, but it has no packet source and
says so at startup.

### 2. Configure and build

```bash
cmake --preset prod
cmake --build --preset prod
```

The configure log states what was found. Check this line:

```
-- gnp: AF_XDP ingest enabled (libbpf 1.6.3, libxdp 1.6.3, /usr/bin/clang)
-- gnp: cuda=ON xdp=ON testing=OFF
```

- `cuda=ON`: `nvcc` and a host compiler it accepts were found, so the poller is
  the persistent kernel (`src/gpu/poll_kernel.cu`). With `OFF`, it is
  `src/gpu/poll_cpu.cpp`.
- `xdp=ON`: the AF_XDP ingest is compiled in. With `OFF`, the warning above it
  names what's missing.
- `testing=OFF`: nothing from `testing/` is compiled or linked.

The output is two files:

```
build/prod/gnp                 the receiver
build/prod/udp_redirect.bpf.o  the XDP program, loaded by gnp at startup
```

Keep them together, or pass `--bpf-obj PATH`.

### 3. Presets

| preset | builds | use it for |
|---|---|---|
| `prod` | `gnp` | real traffic |
| `dev` | `gnp`, `gnp_sim`, `test_ring` | development, benchmarks, CI ([testing/README.md](testing/README.md)) |
| `dev-cpu` | same as `dev`, CPU poller forced | fast protocol iteration |
| `debug` | same as `dev`, `-O0 -g` | debugging |

The underlying options are `GNP_ENABLE_CUDA` (ON), `GNP_ENABLE_XDP` (ON) and
`GNP_BUILD_TESTING` (OFF). The first two auto-detect, and turning one OFF
skips the detection.

### Installing CUDA (Fedora)

Only the NVIDIA driver is required at runtime, but `nvcc` is needed to build the
kernel:

```bash
sudo dnf install cuda-toolkit
```

Two things commonly bite on a current Fedora:

- **Host compiler too new.** `nvcc` rejects host compilers it does not
  recognise, and Fedora's GCC runs ahead of every CUDA release.
  `cmake/gnp_cuda.cmake` handles this automatically:
  - It finds `nvcc` on `PATH`, via `CUDACXX`/`CUDA_PATH`, or in `/usr/local/cuda`.
  - It probes it with the default `g++`, then every installed versioned `g++-N`
    from newest to oldest, and keeps the first pairing `nvcc` accepts.
  - The configure log shows each probe (`gnp: probing ...`).

  If none is accepted, install a supported one (e.g. `sudo dnf install gcc14-c++`)
  or pass `-DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-14`. An explicit choice is the
  only one tried. A failed probe is re-run on the next configure rather than
  cached, so you never need to delete a build directory after fixing the toolchain.
- **Display-GPU watchdog.** If the GPU also drives your desktop, a persistent
  kernel will freeze the display for as long as it runs and may be killed by the
  X/Wayland watchdog after a few seconds. Keep `--duration` short, or run on a
  GPU that is not driving a display.

## Run on a NIC

### Pick the hardware

The NIC's driver decides whether this is real kernel bypass:

- **Native XDP** runs in the driver before any skb is allocated. Supported by
  Intel `ice`, `i40e`, `ixgbe`, `igb`, `igc`, Mellanox `mlx5`, `virtio_net` and
  others. `ice`, `i40e` and `mlx5` also support zero-copy AF_XDP.
- **Generic XDP** works on any interface (Wi-Fi, USB Ethernet, veth), but runs
  after skb allocation. It is correct but not a bypass.

Check the driver with `ethtool -i <iface>`. The GPU should be in the same
machine as the NIC.

### Prepare the interface

```bash
# multi-queue NICs spread flows over RX queues; put the capture port on queue 0
sudo ethtool -N enp1s0 flow-type udp4 dst-port 9000 action 0
# or collapse to a single queue
sudo ethtool -L enp1s0 combined 1
```

AF_XDP only sees packets on the queue its socket is bound to. Matching packets
that arrive on another queue are passed to the kernel, not dropped, but `gnp`
won't see them. The exit report says when this happens.

### Start the receiver

```bash
sudo ./build/prod/gnp --iface enp1s0 --port 9000 --duration 30000
```

| flag | meaning | default |
|---|---|---|
| `--iface NAME` | interface to attach the XDP program to | required |
| `--port N` | destination UDP port to capture | required |
| `--nic-queue N` | NIC RX queue the AF_XDP socket binds to | 0 |
| `--skb-mode` | force generic XDP | native, falling back to generic |
| `--bpf-obj PATH` | XDP program object | `udp_redirect.bpf.o` next to `gnp` |
| `--ring N` | entries in the completion ring, power of two | 1024 |
| `--size B` | arena bytes per slot = largest UDP payload kept | 1024 |
| `--duration MS` | how long to capture | 2000 |
| `--backoff NS` | relax the poll loop when idle, `0` = pure spin | 0 |
| `--copy-timing` | time 1 in 32 H2D flushes (CUDA) | off |
| `--verbose` | device, allocation and clock-offset details | off |

- **Privileges.** Loading XDP and opening AF_XDP sockets needs root. Instead of
  `sudo` you can grant capabilities once:
  `sudo setcap cap_net_admin,cap_net_raw,cap_bpf,cap_perfmon,cap_ipc_lock+ep build/prod/gnp`.
  `CAP_IPC_LOCK` is needed because the UMEM is locked memory.
- **Native first.** Without `--skb-mode`, `gnp` tries native XDP and prints
  `falling back to generic (SKB) mode` if the driver can't do it.
- **One XDP program per interface.** `gnp` refuses to replace an existing one.
  Check with `ip link show enp1s0` (look for `prog/xdp`). If a killed run left
  the program attached, remove it with `sudo ip link set enp1s0 xdp off` (or
  `xdpgeneric off`).
- **v1 scope.** One UDP port, one RX queue, IPv4, `--queues 1`.

Send traffic from another machine while it runs. To replay a capture with
tcpreplay, see
[testing/README.md](testing/README.md#4-real-nic-tcpreplay-from-a-second-machine).

### Read the result

`gnp` prints two diagnostic lines before the summary:

```
[gnp] AF_XDP kernel counters: rx_dropped=0 rx_invalid=0 rx_ring_full=0 fill_ring_empty=0
[gnp] XDP program: 10000 frames to UDP port 9000, 10000 of them on RX queue 0
```

| symptom | meaning |
|---|---|
| `XDP program: 0 frames` | no traffic reached the interface while `gnp` was attached (wrong port, wrong IP/MAC, or sent outside the window) |
| frames on queue 0 < frames total | RSS put some on other queues; steer the port (above) |
| `rx_ring_full` or `fill_ring_empty` > 0 | the RX thread fell behind and the kernel dropped frames |
| `frames dropped` > 0 in the summary | a payload was larger than `--size`, or a frame was malformed |
| `ring-full stalls` > 0 | the poller fell behind and the RX thread waited for free slots |

A clean run has `descriptors published` == `packets observed` == frames on the
bound queue, `packet-id gaps` == 0, and every counter above at zero. The other
summary fields are explained in
[docs/README.md](docs/README.md#reading-the-run-summary).

## Where packet processing goes

[`include/gnp/packet_handler.hpp`](include/gnp/packet_handler.hpp):

```cpp
GNP_HD GNP_FORCEINLINE void on_packet(const CompletionDesc& desc, const uint8_t* payload) {
    (void)desc;
    (void)payload;
}
```

Both pollers call it once per packet, in ring order, right after the descriptor
is observed. `desc` carries `byte_len`, `packet_id`, `post_ns` and
`payload_offset`. `payload` points at the UDP payload on the CPU poller. It is
`nullptr` on the CUDA poller, because the payload arena is host memory the
kernel can't read. A GPU-resident arena is a listed next step in
[`docs/design.md`](docs/design.md#next-steps-roughly-in-order).

It runs on the poll loop's critical path, so whatever you put there sets the
per-packet budget. Slow work should be handed off (for example, to the rest
of the warp on the GPU) rather than done inline.

## Layout

```
include/gnp/
  common.hpp          RunConfig, clocks, GNP_HD, GNP_FORCEINLINE, GNP_CUDA_CHECK
  ring.hpp            CompletionDesc / CompletionRing / RingControl, owner-bit
                      math, IngestStats, ring_publish()
  packet_handler.hpp  on_packet(): the per-packet processing hook (empty)
  xdp_ingest.hpp      AF_XDP ingest lifecycle
  cli.hpp             flags shared by gnp and gnp_sim
  metrics.hpp         PollStats, report()
  gpu_poll.hpp        backend interface, PollQueue, Session / QueueSession
src/bpf/
  udp_redirect.bpf.c  XDP program: one UDP port -> AF_XDP socket, rest XDP_PASS
src/host/
  main.cpp            gnp: attach, poll, capture, report
  xdp_ingest.cpp      BPF load/attach, UMEM and socket, RX thread
  ring.cpp            ring_publish(): the owner-bit publish protocol
  cli.cpp             shared flag parsing
  setup.cpp           allocation, teardown, queue-limit check, host_now_ns()
  metrics.cpp         aggregation and end-of-run summary
src/gpu/
  poll_kernel.cu      the persistent pollers (<<<queues, 1>>>) and residency limit
  utils.cu            device probe, shared allocation, copy streams, clock calibration
  poll_cpu.cpp        CPU fallback (one thread per queue), built only when nvcc is missing
cmake/
  gnp_cuda.cmake      finds a working nvcc + host compiler pair
  gnp_xdp.cmake       finds libbpf, libxdp and clang
testing/              simulator, unit tests, scripts; see testing/README.md
docs/                 results and design; see docs/README.md
```
