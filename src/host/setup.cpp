//
// src/host/setup.cpp - allocate and tear down everything one run needs.
//

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "gnp/common.hpp"
#include "gnp/gpu_poll.hpp"
#include "gnp/metrics.hpp"
#include "gnp/ring.hpp"

namespace gnp {

uint64_t host_now_ns() {
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch())
            .count());
}

namespace {

uint32_t log2_exact(uint32_t v) {
    uint32_t n = 0;
    while ((1u << n) < v) ++n;
    return n;
}

/// Allocate one queue. Every buffer is a separate allocation so queues share
/// no memory - in particular, no cache line holding another queue's control
/// or stats words, which other SMs write concurrently.
bool queue_create(const RunConfig& cfg, QueueSession& q) {
    q.ring.capacity = cfg.ring_capacity;
    q.ring.mask = cfg.ring_capacity - 1;
    q.ring.shift = log2_exact(cfg.ring_capacity);

    const size_t desc_bytes = static_cast<size_t>(cfg.ring_capacity) * sizeof(CompletionDesc);
    CompletionDesc* device_descs = nullptr;
    if (!backend_alloc_ring(desc_bytes, &q.host_descs, &device_descs)) {
        std::fprintf(stderr, "[gnp] ring allocation failed\n");
        return false;
    }
    q.ring.descs = device_descs;

    q.ctrl = static_cast<RingControl*>(backend_alloc_shared(sizeof(RingControl)));
    q.stats = static_cast<PollStats*>(backend_alloc_shared(sizeof(PollStats)));

    q.arena_bytes = static_cast<size_t>(cfg.ring_capacity) * cfg.payload_bytes;
    q.arena = static_cast<uint8_t*>(backend_alloc_host(q.arena_bytes));

    if (!q.host_descs || !q.ring.descs || !q.ctrl || !q.stats || !q.arena) {
        std::fprintf(stderr, "[gnp] allocation failed\n");
        return false;
    }

    std::memset(q.ctrl, 0, sizeof(RingControl));
    q.ctrl->publish_limit = ~0ull;
    std::memset(q.arena, 0, q.arena_bytes);
    *q.stats = PollStats{};
    return true;
}

void queue_destroy(QueueSession& q) {
    backend_free_host(q.arena);
    backend_free_shared(q.stats);
    backend_free_shared(q.ctrl);
    backend_free_ring(q.host_descs, q.ring.descs);
    q = QueueSession{};
}

}  // namespace

bool session_create(const RunConfig& cfg, Session& out) {
    if (!is_power_of_two(cfg.ring_capacity)) {
        std::fprintf(stderr, "[gnp] ring capacity %u is not a power of two\n", cfg.ring_capacity);
        return false;
    }
    if (cfg.payload_bytes == 0) {
        std::fprintf(stderr, "[gnp] payload size must be non-zero\n");
        return false;
    }
    if (cfg.queues == 0) {
        std::fprintf(stderr, "[gnp] queue count must be at least 1\n");
        return false;
    }
    if (!backend_init(cfg.verbose)) return false;

    // Checked before anything is allocated, so a rejected count needs no
    // partial teardown.
    const uint32_t max_queues = backend_max_queues();
    if (cfg.verbose) {
        std::printf("[gnp] max concurrent queues on %s backend: %u\n", backend_name(),
                    max_queues);
    }
    if (cfg.queues > max_queues) {
        std::fprintf(stderr,
                     "[gnp] --queues %u exceeds this backend's limit of %u (on CUDA: pollers are "
                     "persistent, so all of them must be resident at once)\n",
                     cfg.queues, max_queues);
        backend_shutdown();
        return false;
    }

    out.queues.resize(cfg.queues);
    for (QueueSession& q : out.queues) {
        if (!queue_create(cfg, q)) {
            session_destroy(out);
            return false;
        }
    }
    if (!backend_setup_copy(cfg.queues, cfg.copy_timing)) {
        session_destroy(out);
        return false;
    }

    if (cfg.verbose) {
        const size_t desc_bytes = static_cast<size_t>(cfg.ring_capacity) * sizeof(CompletionDesc);
        std::printf("[gnp] memory: %s\n", backend_memory_strategy());
        std::printf("[gnp] %u queue(s) x ring: %u entries (%zu KiB), arena %zu KiB (host-only)\n",
                    cfg.queues, cfg.ring_capacity, desc_bytes / 1024,
                    out.queues[0].arena_bytes / 1024);
        for (size_t i = 0; i < out.queues.size(); ++i) {
            const QueueSession& q = out.queues[i];
            std::printf("[gnp] queue %zu ring pointers: host_staging=%p device_cq=%p%s\n", i,
                        static_cast<void*>(q.host_descs), static_cast<void*>(q.ring.descs),
                        q.host_descs == q.ring.descs ? " (alias)" : "");
        }
    }
    return true;
}

void session_destroy(Session& s) {
    for (QueueSession& q : s.queues) queue_destroy(q);
    s = Session{};
    backend_shutdown();
}

void session_retire_pollers(Session& s, const IngestStats* ingest) {
    for (size_t q = 0; q < s.queues.size(); ++q) {
        reinterpret_cast<std::atomic<unsigned long long>*>(&s.queues[q].ctrl->publish_limit)
            ->store(ingest[q].produced, std::memory_order_release);
    }
    // One fence for the whole session, between the two phases: every
    // publish_limit is visible before any stop_flag is.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (QueueSession& q : s.queues) {
        reinterpret_cast<std::atomic<uint32_t>*>(&q.ctrl->stop_flag)
            ->store(1u, std::memory_order_release);
    }
}

}  // namespace gnp
