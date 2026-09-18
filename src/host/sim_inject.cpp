//
// src/host/sim_inject.cpp - stand-in for the NIC.
//
// A host thread that publishes completion descriptors at a paced rate. When
// real hardware arrives this file is what gets replaced: the ring, the owner-bit
// protocol and the poller all stay exactly as they are.
//

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#endif

#include "gnp/common.hpp"
#include "gnp/gpu_poll.hpp"
#include "gnp/ring.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace gnp {
namespace {

inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
}

/// Flush host staging into the device CQ the SM polls. Batches of at least
/// kFlushBatch cut CUDA launch overhead on the unpaced path; paced runs flush
/// every burst so detection latency stays honest.
constexpr uint32_t kFlushBatch = 32;

}  // namespace

void sim_publish(const CompletionRing& ring, uint64_t idx, uint64_t payload_offset,
                 uint32_t byte_len, uint32_t packet_id, uint64_t post_ns) {
    CompletionDesc* d = &ring.descs[ring_slot(ring, idx)];

    d->payload_offset = payload_offset;
    d->byte_len = byte_len;
    d->packet_id = packet_id;
    d->post_ns = post_ns;
    d->reserved = 0;

    // Full barrier, not just a release fence. On x86 a release fence is only a
    // compiler barrier, which is not enough here: these stores may land in
    // write-combining PCIe-mapped memory, where the CPU is free to reorder them.
    // A seq_cst fence emits a real mfence and drains the WC buffers.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    reinterpret_cast<std::atomic<uint32_t>*>(&d->status)
        ->store(ring_expected_owner(ring, idx), std::memory_order_relaxed);
}

struct Simulator {
    std::thread thread;
    std::atomic<bool> stop{false};
    SimStats stats;
};

namespace {

void inject_loop(Simulator* sim, uint32_t queue, CompletionRing host_ring,
                 CompletionDesc* device_descs, RingControl* ctrl, uint8_t* arena, RunConfig cfg) {
    const uint32_t burst = cfg.burst ? cfg.burst : 1u;
    const uint64_t interval_ns =
        cfg.target_pps ? (1000000000ull * burst) / cfg.target_pps : 0ull;

    const auto* consumed =
        reinterpret_cast<const std::atomic<unsigned long long>*>(&ctrl->consumed);

    uint64_t produced = 0;
    uint64_t flushed = 0;
    uint64_t overruns = 0;
    sim->stats.start_ns = host_now_ns();
    uint64_t next_ns = sim->stats.start_ns;

    auto flush_available = [&](bool force) {
        const uint64_t n = produced - flushed;
        if (n == 0) return;
        if (!force && n < kFlushBatch) return;
        backend_flush_descs(queue, host_ring.descs, device_descs, host_ring.capacity, flushed,
                            static_cast<uint32_t>(n));
        flushed = produced;
    };

    while (!sim->stop.load(std::memory_order_relaxed)) {
        if (cfg.max_packets && produced >= cfg.max_packets) break;

        if (interval_ns) {
            while (host_now_ns() < next_ns) {
                if (sim->stop.load(std::memory_order_relaxed)) goto done;
                cpu_relax();
            }
            next_ns += interval_ns;
        }

        const uint64_t burst_start = produced;
        for (uint32_t b = 0; b < burst; ++b) {
            if (cfg.max_packets && produced >= cfg.max_packets) break;

            while (produced - consumed->load(std::memory_order_acquire) >= host_ring.capacity) {
                flush_available(true);  // free slots only appear after H2D + poll
                ++overruns;
                if (sim->stop.load(std::memory_order_relaxed)) goto done;
                cpu_relax();
            }

            const uint64_t offset =
                static_cast<uint64_t>(ring_slot(host_ring, produced)) * cfg.payload_bytes;

            const uint32_t id = static_cast<uint32_t>(produced);
            std::memcpy(arena + offset, &id, sizeof(id));

            sim_publish(host_ring, produced, offset, cfg.payload_bytes, id, host_now_ns());
            ++produced;
        }

        // Paced: flush each burst for low detection latency.
        // Unpaced: coalesce into kFlushBatch to amortise launch cost.
        flush_available(interval_ns != 0 || (produced - burst_start) >= kFlushBatch);
    }

done:
    flush_available(true);
    backend_flush_wait(queue);
    backend_copy_stats(queue, sim->stats);
    sim->stats.produced = produced;
    sim->stats.overruns = overruns;
    sim->stats.end_ns = host_now_ns();
}

}  // namespace

Simulator* sim_start(uint32_t queue, const CompletionRing& host_ring, CompletionDesc* device_descs,
                     RingControl* ctrl, uint8_t* arena, size_t arena_bytes, const RunConfig& cfg) {
    if (!host_ring.descs || !device_descs || !ctrl || !arena) return nullptr;
    if (arena_bytes < static_cast<size_t>(host_ring.capacity) * cfg.payload_bytes) return nullptr;

    auto* sim = new Simulator();
    sim->thread =
        std::thread(inject_loop, sim, queue, host_ring, device_descs, ctrl, arena, cfg);
#if defined(__linux__)
    // Visible in top -H and /proc/<pid>/task/*/comm; scripts/bench.py uses it
    // to attribute CPU time to producers vs pollers.
    char name[16];
    std::snprintf(name, sizeof(name), "gnp-prod-%u", queue);
    pthread_setname_np(sim->thread.native_handle(), name);
#endif
    return sim;
}

void sim_request_stop(Simulator* sim) {
    if (sim) sim->stop.store(true, std::memory_order_relaxed);
}

void sim_stop(Simulator* sim, SimStats& out) {
    if (!sim) return;
    sim_request_stop(sim);
    if (sim->thread.joinable()) sim->thread.join();
    out = sim->stats;
    delete sim;
}

}  // namespace gnp
