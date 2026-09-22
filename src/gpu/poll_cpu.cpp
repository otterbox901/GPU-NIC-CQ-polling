//
// src/gpu/poll_cpu.cpp - CPU-thread stand-in for the persistent CUDA kernel.
//
// One thread per queue, mirroring one block per queue on the GPU.
//

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

#include "gnp/common.hpp"
#include "gnp/gpu_poll.hpp"
#include "gnp/packet_handler.hpp"

namespace gnp {
namespace {

constexpr unsigned long long kPublishMask = 63ull;

/// Threads are preemptible, so nothing deadlocks past this; it only stops a
/// typo like --queues 100000 from spawning 200k spinning threads.
constexpr uint32_t kMaxQueues = 256;

std::vector<std::thread> g_pollers;

void poll_loop(CompletionRing ring, RingControl* ctrl, PollStats* stats, const uint8_t* arena,
               unsigned long long max_run_ns, unsigned int idle_backoff_ns) {
    auto status_of = [](CompletionDesc* d) {
        return reinterpret_cast<std::atomic<uint32_t>*>(&d->status);
    };

    const uint64_t t_start = host_now_ns();

    PollStats s = {};

    unsigned long long idx = 0;
    uint32_t next_id = 0;
    bool have_prev = false;
    bool saw_stop = false;

    auto* stop = reinterpret_cast<std::atomic<uint32_t>*>(&ctrl->stop_flag);
    auto* publish_limit =
        reinterpret_cast<std::atomic<unsigned long long>*>(&ctrl->publish_limit);

    for (;;) {
        CompletionDesc* d = &ring.descs[ring_slot(ring, idx)];
        const uint32_t want = ring_expected_owner(ring, idx);

        if (desc_ready(status_of(d)->load(std::memory_order_acquire), want)) {
            const uint32_t len = d->byte_len;
            const uint32_t pid = d->packet_id;

            on_packet(*d, arena ? arena + d->payload_offset : nullptr);

            ++s.packets;
            s.bytes += len;
            if (have_prev && pid != next_id) ++s.gaps;
            next_id = pid + 1;
            have_prev = true;

            ++idx;

            if ((idx & kPublishMask) == 0) {
                reinterpret_cast<std::atomic<unsigned long long>*>(&ctrl->consumed)
                    ->store(idx, std::memory_order_release);
                *stats = s;
                if (host_now_ns() - t_start > max_run_ns) break;
            }
        } else {
            ++s.idle_spins;
            if (stop->load(std::memory_order_acquire)) {
                if (!saw_stop) saw_stop = true;
                else ++s.drain_spins;
                if (idx >= publish_limit->load(std::memory_order_acquire)) break;
            }
            if ((s.idle_spins & 1023ull) == 0 && host_now_ns() - t_start > max_run_ns) break;
            if (idle_backoff_ns) std::this_thread::yield();
        }
    }

    reinterpret_cast<std::atomic<unsigned long long>*>(&ctrl->consumed)
        ->store(idx, std::memory_order_release);
    s.run_ns = host_now_ns() - t_start;
    *stats = s;
}

}  // namespace

const char* backend_name() { return "cpu-fallback"; }

const char* backend_memory_strategy() { return "host heap (cpu-fallback)"; }

bool backend_init(bool verbose) {
    if (verbose) {
        std::printf("[gnp] no CUDA compiler at build time; polling on a host thread\n");
        std::printf("[gnp] shared-memory strategy: %s\n", backend_memory_strategy());
    }
    return true;
}

void* backend_alloc_shared(size_t bytes, bool /*write_combined*/) {
    return ::operator new(bytes, std::align_val_t(256), std::nothrow);
}

void backend_free_shared(void* p) {
    if (p) ::operator delete(p, std::align_val_t(256));
}

bool backend_alloc_ring(size_t bytes, CompletionDesc** host_out, CompletionDesc** device_out) {
    void* p = ::operator new(bytes, std::align_val_t(256), std::nothrow);
    if (!p) return false;
    std::memset(p, 0, bytes);
    *host_out = static_cast<CompletionDesc*>(p);
    *device_out = static_cast<CompletionDesc*>(p);
    return true;
}

void backend_free_ring(CompletionDesc* host, CompletionDesc* device) {
    (void)device;
    if (host) ::operator delete(host, std::align_val_t(256));
}

// Producer and poller share one host ring here, so there is nothing to copy.
bool backend_setup_copy(uint32_t, bool) { return true; }

void backend_teardown_copy() {}

void backend_flush_descs(uint32_t, CompletionDesc*, CompletionDesc*, uint32_t, uint64_t,
                         uint32_t) {}

void backend_flush_wait(uint32_t) {}

// No flush, so no copy cost: the IngestStats copy fields stay zero.
void backend_copy_stats(uint32_t, IngestStats&) {}

void* backend_alloc_host(size_t bytes) {
    return ::operator new(bytes, std::align_val_t(64), std::nothrow);
}

void backend_free_host(void* p) {
    if (p) ::operator delete(p, std::align_val_t(64));
}

uint32_t backend_max_queues() { return kMaxQueues; }

bool backend_launch_poller(const PollQueue* queues, const uint8_t* const* arenas,
                           uint32_t n_queues, const RunConfig& cfg) {
    if (n_queues == 0 || n_queues > kMaxQueues) {
        std::fprintf(stderr, "[gnp] cpu-fallback supports 1..%u queues, got %u\n", kMaxQueues,
                     n_queues);
        return false;
    }

    // Each queue costs two spinning threads: its poller and its producer.
    const unsigned hw = std::thread::hardware_concurrency();
    if (cfg.verbose && hw && 2u * n_queues > hw) {
        std::printf("[gnp] warning: %u queues need %u spinning threads but only %u hardware "
                    "threads exist; latency will reflect OS scheduling, not polling\n",
                    n_queues, 2u * n_queues, hw);
    }

    const unsigned long long max_run_ns =
        (static_cast<unsigned long long>(cfg.duration_ms) + 5000ull) * 1000000ull;
    g_pollers.reserve(n_queues);
    for (uint32_t q = 0; q < n_queues; ++q) {
        g_pollers.emplace_back(poll_loop, queues[q].ring, queues[q].ctrl, queues[q].stats,
                               arenas ? arenas[q] : nullptr, max_run_ns, cfg.idle_backoff_ns);
#if defined(__linux__)
        char name[16];
        std::snprintf(name, sizeof(name), "gnp-poll-%u", q);
        pthread_setname_np(g_pollers.back().native_handle(), name);
#endif
    }
    return true;
}

bool backend_wait_poller() {
    for (std::thread& t : g_pollers) {
        if (t.joinable()) t.join();
    }
    g_pollers.clear();
    return true;
}

void backend_shutdown() {}

}  // namespace gnp
