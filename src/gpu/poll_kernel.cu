//
// src/gpu/poll_kernel.cu - the persistent completion-queue pollers.
//
// This is the heart of the project: a long-running CUDA kernel that watches the
// completion rings from SMs and never asks the CPU for anything. The only
// host interaction is stop_flag + publish_limit, checked on the idle path.
//
// One launch covers every queue: <<<n_queues, 1>>>, block b owns queue b. The
// blocks share nothing, so each one runs exactly the single-queue loop.
//

#include <cuda_runtime.h>

#include "gnp/common.hpp"
#include "gnp/gpu_poll.hpp"
#include "gnp/metrics.hpp"
#include "gnp/packet_handler.hpp"
#include "gnp/ring.hpp"

namespace gnp {
namespace {

/// Publish the consumer watermark this often. Every entry would put a PCIe
/// write on the critical path; every 64 keeps the producer's free-slot estimate
/// fresh enough while staying off the fast path.
constexpr unsigned long long kPublishMask = 63ull;

cudaStream_t g_stream = nullptr;
PollQueue*   g_dev_queues = nullptr;  ///< device copy of the launch's PollQueue array

}  // namespace

/// Block b polls queues[b] until the host asks it to stop *and* it has reached
/// that queue's publish limit. One thread per block on purpose: a CQ is
/// consumed in order, so width comes from more queues, not more lanes.
__global__ void gnp_poll_kernel(const PollQueue* queues, unsigned int n_queues,
                                unsigned long long max_run_ns, unsigned int idle_backoff_ns) {
    if (threadIdx.x != 0 || blockIdx.x >= n_queues) return;

    // Read once at entry, never again: the hot loop below only touches this
    // block's own ring/ctrl/stats, exactly as the single-queue kernel did.
    const CompletionRing ring = queues[blockIdx.x].ring;
    RingControl* const ctrl = queues[blockIdx.x].ctrl;
    PollStats* const stats = queues[blockIdx.x].stats;

    // volatile => every load goes past the non-coherent L1. Producer stores
    // arrive from outside this SM (host mapped memory over PCIe).
    volatile CompletionDesc* descs = ring.descs;
    volatile unsigned int* stop = &ctrl->stop_flag;
    volatile unsigned long long* publish_limit = &ctrl->publish_limit;

    const unsigned long long t_start = device_now_ns();

    PollStats s = {};

    unsigned long long idx = 0;
    unsigned int next_id = 0;
    bool have_prev = false;
    bool saw_stop = false;

    for (;;) {
        const unsigned int slot = ring_slot(ring, idx);
        const unsigned int want = ring_expected_owner(ring, idx);

        // System-scoped acquire was needed when the CQ lived in host-mapped
        // memory. The CQ is now device-resident; a GPU-scope acquire is enough
        // and much cheaper on the critical path.
        unsigned int st;
        asm volatile("ld.acquire.gpu.u32 %0, [%1];"
                     : "=r"(st)
                     : "l"(&descs[slot].status)
                     : "memory");

        if (desc_ready(st, want)) {
            const unsigned int len = descs[slot].byte_len;
            const unsigned int pid = descs[slot].packet_id;

            // The arena is host memory, so the kernel has no payload pointer.
            const CompletionDesc observed{descs[slot].payload_offset, len, pid,
                                          descs[slot].post_ns, 0u, st};
            on_packet(observed, nullptr);

            ++s.packets;
            s.bytes += len;
            if (have_prev && pid != next_id) ++s.gaps;
            next_id = pid + 1;
            have_prev = true;

            ++idx;

            if ((idx & kPublishMask) == 0) {
                ctrl->consumed = idx;
                // Avoid copying the whole stats struct every time; host reads it
                // at teardown. A release-style system fence publishes `consumed`.
                __threadfence_system();
                if (device_now_ns() - t_start > max_run_ns) break;
            }
        } else {
            ++s.idle_spins;

            // Drain protocol: stop alone is not enough. Host publishes the final
            // produced count first; we only retire once idx has caught it.
            if (*stop) {
                if (!saw_stop) saw_stop = true;
                else ++s.drain_spins;
                __threadfence_system();
                if (idx >= *publish_limit) break;
            }

            if ((s.idle_spins & 1023ull) == 0 && device_now_ns() - t_start > max_run_ns) break;
#if __CUDA_ARCH__ >= 700
            if (idle_backoff_ns) __nanosleep(idle_backoff_ns);
#endif
        }
    }

    ctrl->consumed = idx;
    s.run_ns = device_now_ns() - t_start;
    *stats = s;
    __threadfence_system();
}

uint32_t backend_max_queues() {
    int device = 0;
    GNP_CUDA_CHECK(cudaGetDevice(&device));
    int sms = 0;
    GNP_CUDA_CHECK(cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, device));

    // Must match the real launch: 1 thread per block, no dynamic shared memory.
    // The binding limit for this tiny kernel is the per-SM resident-block cap.
    int blocks_per_sm = 0;
    GNP_CUDA_CHECK(
        cudaOccupancyMaxActiveBlocksPerMultiprocessor(&blocks_per_sm, gnp_poll_kernel, 1, 0));

    return static_cast<uint32_t>(blocks_per_sm) * static_cast<uint32_t>(sms);
}

bool backend_launch_poller(const PollQueue* queues, const uint8_t* const* /*arenas*/,
                           uint32_t n_queues, const RunConfig& cfg) {
    // session_create already enforces this; re-check because a launch past the
    // residency limit does not fail - it hangs forever in backend_wait_poller.
    const uint32_t max_queues = backend_max_queues();
    if (n_queues == 0 || n_queues > max_queues) {
        std::fprintf(stderr,
                     "[gnp] refusing to launch %u persistent pollers: at most %u can be resident "
                     "at once on this device, and a non-resident poller would never start\n",
                     n_queues, max_queues);
        return false;
    }

    if (!g_stream) {
        GNP_CUDA_CHECK(cudaStreamCreateWithFlags(&g_stream, cudaStreamNonBlocking));
    }

    if (g_dev_queues) {
        GNP_CUDA_CHECK(cudaFree(g_dev_queues));
        g_dev_queues = nullptr;
    }
    const size_t bytes = static_cast<size_t>(n_queues) * sizeof(PollQueue);
    GNP_CUDA_CHECK(cudaMalloc(&g_dev_queues, bytes));
    GNP_CUDA_CHECK(cudaMemcpy(g_dev_queues, queues, bytes, cudaMemcpyHostToDevice));

    const unsigned long long max_run_ns =
        (static_cast<unsigned long long>(cfg.duration_ms) + 5000ull) * 1000000ull;

    if (cfg.verbose) {
        int device = 0;
        int sms = 0;
        GNP_CUDA_CHECK(cudaGetDevice(&device));
        GNP_CUDA_CHECK(cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, device));
        std::printf("[gnp] launching <<<%u, 1>>>: %u poller(s), max resident %u (%u SMs)%s\n",
                    n_queues, n_queues, max_queues, static_cast<unsigned>(sms),
                    n_queues > static_cast<uint32_t>(sms)
                        ? " - more pollers than SMs, some will share a warp scheduler"
                        : "");
    }

    gnp_poll_kernel<<<n_queues, 1, 0, g_stream>>>(g_dev_queues, n_queues, max_run_ns,
                                                  cfg.idle_backoff_ns);

    const cudaError_t e = cudaGetLastError();
    if (e != cudaSuccess) {
        std::fprintf(stderr, "[gnp] poller launch failed: %s\n", cudaGetErrorString(e));
        return false;
    }
    return true;
}

bool backend_wait_poller() {
    if (!g_stream) return true;
    const cudaError_t e = cudaStreamSynchronize(g_stream);
    if (e != cudaSuccess) {
        std::fprintf(stderr, "[gnp] poller failed: %s\n", cudaGetErrorString(e));
        return false;
    }
    return true;
}

void backend_shutdown() {
    backend_teardown_copy();
    if (g_dev_queues) {
        cudaFree(g_dev_queues);
        g_dev_queues = nullptr;
    }
    if (g_stream) {
        cudaStreamDestroy(g_stream);
        g_stream = nullptr;
    }
}

}  // namespace gnp
