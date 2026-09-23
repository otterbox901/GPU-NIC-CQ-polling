//
// src/gpunetio/receive_kernel.cu - the persistent GPU-direct receive loop.
//
// This is the heart of the GPU-direct path. The NIC has already written whole
// Ethernet frames into GPU memory, so the kernel reads packet bytes locally and
// never asks the CPU for anything: the only host interaction is the exit flag.
//
// Shape differs from the AF_XDP path's poller on purpose. That one runs a
// single thread per block because a completion ring is consumed strictly in
// order. Here DOCA hands back a *batch* of packets that landed in parallel, so
// width comes from threads inside the block, and delivery order across the
// batch is not guaranteed.
//

#include <doca_gpunetio_dev_eth_rxq.cuh>

#include "gnp/common.hpp"
#include "gnp/metrics.hpp"
#include "gnp/packet_handler.hpp"

#include "kernel.h"

namespace {

/// Threads per block, i.e. per receive queue.
constexpr uint32_t kBlockThreads = 512;

/// Packets pulled per receive call. DOCA wants a multiple of the block's
/// thread count, since every thread tries to receive at least once. Each entry
/// costs 16 bytes of shared memory for its attributes, so this trades shared
/// memory (and therefore occupancy) against syscall-free batching.
constexpr uint32_t kMaxRxPkts = 2 * kBlockThreads;

/// How long one receive call waits before returning empty-handed. This is the
/// granularity at which the exit flag is noticed, so it also bounds how long
/// teardown takes.
constexpr uint64_t kRxTimeoutNs = 500000;  // 500 us

}  // namespace

/// Block b drains receive queue b until the host sets the exit flag.
__global__ void gnp_receive_kernel(struct doca_gpu_eth_rxq** rxqs, uint32_t* exit_flag,
                                   gnp::PollStats* stats) {
    struct doca_gpu_eth_rxq* rxq = rxqs[blockIdx.x];
    gnp::PollStats* out = &stats[blockIdx.x];

    __shared__ uint64_t first_pkt_idx;
    __shared__ uint32_t pkt_num;
    __shared__ unsigned long long batch_bytes;
    __shared__ struct doca_gpu_dev_eth_rxq_attr attr[kMaxRxPkts];

    unsigned long long packets = 0;
    unsigned long long bytes = 0;
    unsigned long long idle = 0;

    while (DOCA_GPUNETIO_VOLATILE(*exit_flag) == 0) {
        const doca_error_t ret =
            doca_gpu_dev_eth_rxq_recv<DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK,
                                      DOCA_GPUNETIO_ETH_MCST_AUTO,
                                      DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO,
                                      DOCA_GPUNETIO_ETH_RX_ATTR_ALL>(
                rxq, kMaxRxPkts, kRxTimeoutNs, &first_pkt_idx, &pkt_num, attr);

        if (ret != DOCA_SUCCESS) {
            // A receive error is not recoverable for this queue, and leaving it
            // spinning would wedge the run. Stop everything and let the host
            // report the short count.
            if (threadIdx.x == 0) DOCA_GPUNETIO_VOLATILE(*exit_flag) = 1;
            break;
        }

        if (pkt_num == 0) {
            ++idle;
            continue;
        }

        if (threadIdx.x == 0) batch_bytes = 0;
        __syncthreads();

        unsigned long long mine = 0;
        for (uint32_t i = threadIdx.x; i < pkt_num; i += blockDim.x) {
            const uint64_t addr = doca_gpu_dev_eth_rxq_get_pkt_addr(rxq, first_pkt_idx + i);
            // The frame is in GPU memory, put there by the NIC. This is the
            // only path where on_packet() receives real bytes.
            gnp::on_packet(
                gnp::PacketView{reinterpret_cast<const uint8_t*>(addr), attr[i].bytes});
            mine += attr[i].bytes;
        }
        atomicAdd(&batch_bytes, mine);
        __syncthreads();

        packets += pkt_num;
        bytes += batch_bytes;
        __syncthreads();  // attr[] is reused by the next receive
    }

    if (threadIdx.x == 0) {
        out->packets = packets;
        out->bytes = bytes;
        out->idle_spins = idle;
        // No gap detection: a batch is delivered in parallel, so there is no
        // strictly ordered sequence to check for holes the way the ring has.
        out->gaps = 0;
        out->drain_spins = 0;
        out->run_ns = 0;
        __threadfence_system();
    }
}

extern "C" int gnp_launch_receive_kernel(cudaStream_t stream, struct doca_gpu_eth_rxq** rxqs,
                                         uint32_t n_queues, uint32_t* exit_flag,
                                         gnp::PollStats* stats) {
    if (cudaGetLastError() != cudaSuccess) return -1;

    gnp_receive_kernel<<<n_queues, kBlockThreads, 0, stream>>>(rxqs, exit_flag, stats);

    return cudaGetLastError() == cudaSuccess ? 0 : -1;
}
