#pragma once
//
// gnp/common.hpp - definitions shared by host (.cpp) and device (.cu) code.
//
// This header must stay compilable by a plain C++17 compiler: everything that
// needs nvcc is guarded by __CUDACC__.
//

#include <cstddef>
#include <cstdint>

// Mark functions that must exist on both sides. Expands to nothing when the
// translation unit is built by the host compiler (CPU-fallback backend).
#if defined(__CUDACC__)
#  define GNP_HD __host__ __device__
#  define GNP_FORCEINLINE __forceinline__
#else
#  define GNP_HD
#  define GNP_FORCEINLINE inline __attribute__((always_inline))
#endif

namespace gnp {

/// Monotonic clock in nanoseconds (std::chrono::steady_clock).
///
/// The host's time base: the producer stamps every descriptor's post_ns with it,
/// and the run's own timing uses it. It is not comparable with the GPU's
/// %globaltimer; see docs/design.md.
uint64_t host_now_ns();

/// Everything the user can tune from the command line.
struct RunConfig {
    uint32_t queues         = 1;       ///< independent completion rings, one poller each
    uint32_t ring_capacity  = 1024;    ///< entries per completion queue, power of two
    uint32_t payload_bytes  = 1024;    ///< arena bytes per ring slot; real ingest drops larger UDP payloads
    uint64_t target_pps     = 100000;  ///< simulator only: injection rate per queue (packets/sec)
    uint64_t max_packets    = 0;       ///< per queue; 0 = run until the duration expires
    uint32_t duration_ms    = 2000;    ///< how long to keep the poller alive
    uint32_t burst          = 1;       ///< descriptors published back-to-back
    uint32_t idle_backoff_ns = 0;      ///< 0 = pure spin; >0 relaxes the poll loop
    bool     copy_timing    = false;   ///< time sampled H2D flushes (CUDA; perturbs heavy load)
    bool     verbose        = false;
};

}  // namespace gnp

#if defined(__CUDACC__)
#include <cstdio>
#include <cstdlib>

/// Abort loudly on a failed CUDA runtime call. Only visible inside .cu files.
#define GNP_CUDA_CHECK(expr)                                                  \
    do {                                                                      \
        cudaError_t _e = (expr);                                              \
        if (_e != cudaSuccess) {                                              \
            std::fprintf(stderr, "[gnp] CUDA error %s:%d: %s -> %s\n",        \
                         __FILE__, __LINE__, #expr, cudaGetErrorString(_e));  \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

namespace gnp {

/// GPU-side nanosecond counter.
///
/// %globaltimer is a free-running ns counter shared by all SMs. It shares
/// neither an epoch nor a reliable rate with host_now_ns(), so it is used only
/// for device-side intervals (how long the poller ran, when to retire), never
/// compared against a host timestamp.
__device__ __forceinline__ uint64_t device_now_ns() {
    uint64_t t;
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
    return t;
}

}  // namespace gnp
#endif  // __CUDACC__
