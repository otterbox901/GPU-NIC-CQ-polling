#pragma once
//
// gnp/gpu_poll.hpp - the polling backend, plus the session that owns its memory.
//
// Exactly one backend is compiled in:
//   * src/gpu/poll_kernel.cu + src/gpu/utils.cu  when a CUDA compiler was found
//   * src/gpu/poll_cpu.cpp                       otherwise
// Both implement the free functions below, so no host .cpp ever includes a
// CUDA header.
//
// Multi-queue: a run has RunConfig::queues independent completion rings. Each
// ring has its own control block, stats block, producer and poller, and shares
// nothing with the others, so no queue ever contends with another.
//

#include <vector>

#include "gnp/common.hpp"
#include "gnp/metrics.hpp"
#include "gnp/ring.hpp"

namespace gnp {

/// Everything one poller needs, and nothing else. POD: the CUDA backend copies
/// an array of these to device memory and block `b` polls element `b`.
struct PollQueue {
    CompletionRing ring;   ///< .descs = device-side pointer
    RingControl*   ctrl;
    PollStats*     stats;
};

/// "cuda" or "cpu-fallback". Printed in the report.
const char* backend_name();

/// Select/probe the device. Returns false if the backend is unusable.
bool backend_init(bool verbose);

/// Control/stats: memory both sides can access (pinned mapped on CUDA).
void* backend_alloc_shared(size_t bytes, bool write_combined = false);
void  backend_free_shared(void* p);

/// Completion ring: host staging (producer) + device-resident CQ (poller).
/// On the CPU backend both pointers are equal. On CUDA, `host_out` is pinned
/// staging and `device_out` is cudaMalloc'd GPU memory the SM polls locally.
bool backend_alloc_ring(size_t bytes, CompletionDesc** host_out, CompletionDesc** device_out);
void backend_free_ring(CompletionDesc* host, CompletionDesc* device);

/// Create the H2D copy channels for `n_queues` queues. Must run before the
/// pollers launch. `timing` enables backend_copy_stats() sampling. No-op on the
/// CPU backend.
bool backend_setup_copy(uint32_t n_queues, bool timing);
void backend_teardown_copy();

/// Copy `count` completed CQEs of queue `queue` from host staging into its
/// device ring, starting at monotonic index `start_idx`. Status is published
/// last per slot so the poller never sees a torn descriptor. No-op when
/// host == device. Each queue flushes on its own copy stream (shared
/// round-robin past 32 queues), so producers rarely wait on each other.
void backend_flush_descs(uint32_t queue, CompletionDesc* host, CompletionDesc* device,
                         uint32_t capacity, uint64_t start_idx, uint32_t count);

/// Wait for queue `queue`'s outstanding flushes. Each producer calls this
/// before it exits, so once sim_stop() returns that queue's CQEs are on device.
void backend_flush_wait(uint32_t queue);

/// Fill `out`'s copy_* fields with queue `queue`'s sampled flush timings. Call
/// from that queue's producer after its last flush. Leaves them zero when
/// timing is off, and on the CPU backend, which has no copy.
void backend_copy_stats(uint32_t queue, SimStats& out);

/// Host-only allocation (payload arena). The poller never touches packet bytes.
void* backend_alloc_host(size_t bytes);
void  backend_free_host(void* p);

/// Short string describing the active shared-memory strategy (for --verbose).
const char* backend_memory_strategy();

/// Nanoseconds to add to a device timestamp to express it on the host_now_ns()
/// epoch. Zero for the CPU fallback.
int64_t backend_clock_offset_ns();

/// Most queues that can be polled at once. Call after backend_init().
///
/// CUDA: the poller is persistent, so every block must be resident at the same
/// time or the late ones never start and the run deadlocks. The limit is
/// max-active-blocks-per-SM (from the occupancy API) x SM count.
/// CPU fallback: threads are preemptible, so this is only a sanity bound.
uint32_t backend_max_queues();

/// Launch one persistent poller per queue. CUDA: a single <<<n_queues, 1>>>
/// grid, block b polls queues[b]. CPU: one thread per queue.
bool backend_launch_poller(const PollQueue* queues, uint32_t n_queues, int64_t clock_offset_ns,
                           const RunConfig& cfg);

/// Wait for every poller to retire.
bool backend_wait_poller();

void backend_shutdown();

// --- session ----------------------------------------------------------------

/// One independent queue. Each pointer is its own allocation, so per-queue
/// control/stats words that different SMs write never share a cache line.
struct QueueSession {
    CompletionRing ring;                  ///< .descs = device pointer (poller)
    CompletionDesc* host_descs = nullptr; ///< producer staging (equals ring.descs on CPU)
    RingControl*   ctrl   = nullptr;
    PollStats*     stats  = nullptr;
    uint8_t*       arena  = nullptr;
    size_t         arena_bytes = 0;

    PollQueue poll_queue() const { return PollQueue{ring, ctrl, stats}; }
};

struct Session {
    std::vector<QueueSession> queues;     ///< RunConfig::queues entries
    int64_t clock_offset_ns = 0;          ///< one device clock, so one calibration
};

bool session_create(const RunConfig& cfg, Session& out);
void session_destroy(Session& s);

}  // namespace gnp
