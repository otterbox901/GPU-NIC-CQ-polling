#pragma once
//
// gnp/gpunetio.hpp - the GPU-direct receive path.
//
// The NIC writes whole Ethernet frames straight into GPU memory, and a
// persistent CUDA kernel reads them there. No CPU thread touches a packet:
//
//   * the bypass is done by the NIC's own hardware steering (DOCA Flow)
//     rather than by an eBPF program, so there is no XDP hook and no UMEM;
//   * the packet buffer is doca_gpu_mem_alloc'd VRAM, mapped for the NIC
//     through dmabuf (or nvidia-peermem on older stacks);
//   * there is no descriptor ring for us to publish into. DOCA owns the NIC
//     completion queue and the kernel calls into it, so the owner-bit protocol
//     in ring.hpp has no counterpart here.
//
// Needs a ConnectX-6 Dx / -7 or BlueField-2/3 NIC, and a GPU with enough BAR1
// space (pre-Hopper parts additionally need an MCST QP, which setup enables).
// Built only when the DOCA SDK was found; see scripts/doca-build.sh.
//

#include "gnp/common.hpp"
#include "gnp/metrics.hpp"

namespace gnp {

struct GpunetioConfig {
    const char* nic_pci = nullptr;  ///< NIC PCIe address, e.g. "0000:c1:00.0"
    const char* gpu_pci = nullptr;  ///< GPU PCIe address, e.g. "0000:01:00.0"
    uint16_t    port    = 0;        ///< destination UDP port to steer into VRAM
};

struct GpunetioSession;  ///< opaque; owns the DOCA device, queue and VRAM buffer

/// Open the NIC and GPU, allocate the receive buffer in VRAM, start the queue
/// with its data path on the GPU, install the flow rule steering `gcfg.port`
/// into it, and launch the persistent receive kernel.
///
/// Returns nullptr on failure, after printing why and undoing whatever was set
/// up. Needs the same privileges any NIC-programming tool does.
GpunetioSession* gpunetio_start(const RunConfig& cfg, const GpunetioConfig& gcfg);

/// Ask the kernel to retire, wait for it, collect its counters, then tear down
/// the flow, queue, buffer and devices. Frees the handle.
void gpunetio_stop(GpunetioSession* s, PollStats& out);

/// Backend name for the report.
const char* gpunetio_name();

}  // namespace gnp
