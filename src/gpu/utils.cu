//
// src/gpu/utils.cu - CUDA backend plumbing: device probe, shared memory, clocks.
//

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "gnp/common.hpp"
#include "gnp/gpu_poll.hpp"
#include "gnp/ring.hpp"

namespace gnp {
namespace {

int  g_device = 0;
bool g_initialised = false;
const char* g_mem_strategy = "uninitialised";

/// H2D copy streams, one per queue up to this cap; past it, queues share
/// streams round-robin. See the NOTE below for why both halves matter.
constexpr uint32_t kMaxCopyStreams = 32;
cudaStream_t g_copy_streams[kMaxCopyStreams] = {};
uint32_t g_copy_stream_count = 0;

cudaStream_t copy_stream(uint32_t queue) {
    return g_copy_streams[queue % g_copy_stream_count];
}

/// Time one flush in this many. The sampled flush blocks its producer until
/// the DMA lands, so timing every flush would perturb the run being measured.
/// Even 1 in 32 does near saturation: at 2 x 200 kpps the stall throttles the
/// producer, the copy backlog shrinks, and mean latency drops from ~11 us to
/// ~7 us. At 50 kpps it makes no measurable difference. Hence opt-in
/// (--copy-timing), so ordinary runs report an unperturbed latency.
constexpr uint32_t kCopySampleEvery = 32;

/// Per-queue flush timing. Strictly one slot per queue (not per stream): each
/// slot is touched only by its queue's producer thread, so no locking.
///
/// Timed with a host clock around cudaStreamSynchronize, not a CUDA event pair.
/// On a GTX 1650, recording events around a 32-byte copy more than doubled the
/// time until the poller saw the data (3.5 -> 8 us) and the pair itself read
/// 5.7 us; a stream sync leaves the copy alone and only adds ~1.4 us of host
/// wake-up to the measured figure, so it reads high, never low.
struct CopyTiming {
    uint64_t calls   = 0;
    uint64_t ns_sum  = 0;  ///< producer submit -> sync returns
    uint64_t samples = 0;
};
std::vector<CopyTiming> g_copy_timing;

/// Spins until the host raises `flag`, then records the GPU clock.
__global__ void gnp_clock_probe(volatile unsigned int* flag, unsigned long long* out) {
    while (*flag == 0) {
    }
    *out = device_now_ns();
}

// NOTE: H2D flush uses the DMA copy engine (cudaMemcpyAsync), not a compute
// kernel. A persistent poller would starve a flush kernel on this GPU; the copy
// engine runs concurrently with the SM poll loop.
//
// Copy streams, measured on a GTX 1650 (16 SMs, 12 host threads):
//  * One stream shared by every queue serialises all producers' flushes behind
//    each other: 8 queues x 100 kpps delivered ~0.1 Mpps total.
//  * One stream per queue fixes that, and up to 32 queues it gives the best
//    latency (32 queues x 25 kpps: 16 us mean vs 27-178 us with 2-16 shared).
//  * Streams must exist before the persistent pollers launch. Per-thread
//    streams (cudaStreamPerThread), which the driver creates lazily on first
//    use - i.e. mid-run, from producer threads - were unreliable: at 32-128
//    queues most flushes never executed while the pollers ran (e.g. 187 of
//    4326 CQEs observed) and only drained once the pollers hit their watchdog.
//  * A capped pool created up front (backend_setup_copy) is correct from 1 to
//    256 queues; past the cap, queues share streams round-robin.

}  // namespace

const char* backend_name() { return "cuda"; }

const char* backend_memory_strategy() { return g_mem_strategy; }

bool backend_init(bool verbose) {
    if (g_initialised) return true;

    int count = 0;
    const cudaError_t e = cudaGetDeviceCount(&count);
    if (e != cudaSuccess || count == 0) {
        std::fprintf(stderr, "[gnp] no CUDA device available: %s\n",
                     e == cudaSuccess ? "count is 0" : cudaGetErrorString(e));
        return false;
    }

    GNP_CUDA_CHECK(cudaSetDevice(g_device));
    g_mem_strategy = "device CQ + pinned staging (DMA H2D flush, copy stream per queue)";

    if (verbose) {
        cudaDeviceProp p{};
        GNP_CUDA_CHECK(cudaGetDeviceProperties(&p, g_device));
        std::printf("[gnp] device %d: %s (sm_%d%d, %d SMs)\n", g_device, p.name, p.major, p.minor,
                    p.multiProcessorCount);
        std::printf("[gnp] shared-memory strategy: %s\n", g_mem_strategy);
        std::printf("[gnp] note: one 1-thread block per queue (each CQ is strictly ordered)\n");
    }

    g_initialised = true;
    return true;
}

void* backend_alloc_shared(size_t bytes, bool write_combined) {
    void* p = nullptr;
    unsigned flags = cudaHostAllocMapped;
    if (write_combined) flags |= cudaHostAllocWriteCombined;
    GNP_CUDA_CHECK(cudaHostAlloc(&p, bytes, flags));
    return p;
}

void backend_free_shared(void* p) {
    if (p) cudaFreeHost(p);
}

bool backend_alloc_ring(size_t bytes, CompletionDesc** host_out, CompletionDesc** device_out) {
    void* host = nullptr;
    void* device = nullptr;
    GNP_CUDA_CHECK(cudaHostAlloc(&host, bytes, cudaHostAllocMapped));
    GNP_CUDA_CHECK(cudaMalloc(&device, bytes));
    GNP_CUDA_CHECK(cudaMemset(device, 0, bytes));
    std::memset(host, 0, bytes);
    *host_out = static_cast<CompletionDesc*>(host);
    *device_out = static_cast<CompletionDesc*>(device);
    return true;
}

void backend_free_ring(CompletionDesc* host, CompletionDesc* device) {
    if (host) cudaFreeHost(host);
    if (device) cudaFree(device);
}

bool backend_setup_copy(uint32_t n_queues, bool timing) {
    backend_teardown_copy();
    const uint32_t n = n_queues < kMaxCopyStreams ? n_queues : kMaxCopyStreams;
    for (uint32_t i = 0; i < n; ++i) {
        GNP_CUDA_CHECK(cudaStreamCreateWithFlags(&g_copy_streams[i], cudaStreamNonBlocking));
    }
    g_copy_stream_count = n;

    if (timing) g_copy_timing.assign(n_queues, CopyTiming{});
    return true;
}

void backend_teardown_copy() {
    for (uint32_t i = 0; i < g_copy_stream_count; ++i) {
        cudaStreamSynchronize(g_copy_streams[i]);
        cudaStreamDestroy(g_copy_streams[i]);
        g_copy_streams[i] = nullptr;
    }
    g_copy_stream_count = 0;
    g_copy_timing.clear();
}

void backend_flush_descs(uint32_t queue, CompletionDesc* host, CompletionDesc* device,
                         uint32_t capacity, uint64_t start_idx, uint32_t count) {
    if (!host || !device || count == 0 || host == device) return;
    cudaStream_t stream = copy_stream(queue);

    // DMA copy engine runs concurrently with the persistent poller. Prefer one
    // contiguous memcpy per non-wrapping span. status is the last field of each
    // CQE, so address-ordered DMA publishes the owner bit after the payload.
    const uint32_t mask = capacity - 1;
    uint32_t slot = static_cast<uint32_t>(start_idx) & mask;
    uint32_t left = count;

    CopyTiming* timing = queue < g_copy_timing.size() ? &g_copy_timing[queue] : nullptr;
    const bool sample = timing && (++timing->calls % kCopySampleEvery) == 0;
    const uint64_t submit_ns = sample ? host_now_ns() : 0;

    while (left > 0) {
        const uint32_t span = (slot + left <= capacity) ? left : (capacity - slot);
        const size_t bytes = static_cast<size_t>(span) * sizeof(CompletionDesc);
        GNP_CUDA_CHECK(
            cudaMemcpyAsync(device + slot, host + slot, bytes, cudaMemcpyHostToDevice, stream));
        slot = (slot + span) & mask;
        left -= span;
    }

    if (sample) {
        // Includes queueing behind this stream's earlier flushes: that is part
        // of how long the producer's CQEs take to reach the poller.
        GNP_CUDA_CHECK(cudaStreamSynchronize(stream));
        timing->ns_sum += host_now_ns() - submit_ns;
        ++timing->samples;
    }
}

void backend_copy_stats(uint32_t queue, SimStats& out) {
    if (queue >= g_copy_timing.size()) return;
    const CopyTiming& t = g_copy_timing[queue];
    out.copy_ns_sum = t.ns_sum;
    out.copy_samples = t.samples;
}

void backend_flush_wait(uint32_t queue) {
    GNP_CUDA_CHECK(cudaStreamSynchronize(copy_stream(queue)));
}

void* backend_alloc_host(size_t bytes) {
    return ::operator new(bytes, std::align_val_t(64), std::nothrow);
}

void backend_free_host(void* p) {
    if (p) ::operator delete(p, std::align_val_t(64));
}

int64_t backend_clock_offset_ns() {
    constexpr int kSamples = 16;

    volatile unsigned int* flag = nullptr;
    unsigned long long* gpu_ns = nullptr;
    GNP_CUDA_CHECK(cudaHostAlloc((void**)&flag, sizeof(unsigned int), cudaHostAllocMapped));
    GNP_CUDA_CHECK(cudaHostAlloc((void**)&gpu_ns, sizeof(unsigned long long), cudaHostAllocMapped));

    int64_t best_offset = 0;
    uint64_t best_window = ~0ull;

    for (int i = 0; i < kSamples; ++i) {
        *flag = 0;
        *gpu_ns = 0;
        gnp_clock_probe<<<1, 1>>>(flag, gpu_ns);
        GNP_CUDA_CHECK(cudaGetLastError());

        for (volatile int spin = 0; spin < 200000; ++spin) {
        }

        const uint64_t t0 = host_now_ns();
        *flag = 1;
        GNP_CUDA_CHECK(cudaDeviceSynchronize());
        const uint64_t t1 = host_now_ns();

        const uint64_t window = t1 - t0;
        if (window < best_window) {
            best_window = window;
            best_offset = static_cast<int64_t>(t0 + window / 2) - static_cast<int64_t>(*gpu_ns);
        }
    }

    cudaFreeHost((void*)flag);
    cudaFreeHost(gpu_ns);
    return best_offset;
}

}  // namespace gnp
