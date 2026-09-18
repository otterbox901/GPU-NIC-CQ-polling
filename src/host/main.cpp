//
// src/host/main.cpp - set up, launch the poller, then get out of the way.
//
// The whole point of the project is that this thread does nothing during
// steady-state receive. It allocates, launches, sleeps, and reports.
//

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gnp/common.hpp"
#include "gnp/gpu_poll.hpp"
#include "gnp/metrics.hpp"
#include "gnp/ring.hpp"

namespace {

void usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "  --queues N        independent completion rings, one poller each (default 1)\n"
        "  --ring N          entries per ring, power of two (default 1024)\n"
        "  --size B          simulated packet size in bytes (default 1024)\n"
        "  --pps R           injection rate PER QUEUE, 0 = unpaced (default 100000)\n"
        "  --packets N       stop after N packets per queue, 0 = until duration (default 0)\n"
        "  --duration MS     how long to run (default 2000)\n"
        "  --burst N         descriptors published back-to-back (default 1)\n"
        "  --backoff NS      relax the poll loop when idle, 0 = pure spin (default 0)\n"
        "  --copy-timing     time 1 in 32 H2D flushes (CUDA); throttles near-saturated runs\n"
        "  --verbose         print device and allocation details\n"
        "  --help\n",
        argv0);
}

/// Returns false if the value is missing or unparseable.
bool take_u64(int argc, char** argv, int& i, uint64_t& out) {
    if (i + 1 >= argc) return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(argv[++i], &end, 10);
    if (end == argv[i] || *end != '\0') return false;
    out = v;
    return true;
}

bool parse_args(int argc, char** argv, gnp::RunConfig& cfg, bool& want_help) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        uint64_t v = 0;
        bool ok = true;

        if (a == "--help" || a == "-h") {
            want_help = true;
            return true;
        } else if (a == "--verbose") {
            cfg.verbose = true;
        } else if (a == "--copy-timing") {
            cfg.copy_timing = true;
        } else if (a == "--queues") {
            ok = take_u64(argc, argv, i, v) && v <= UINT32_MAX;
            cfg.queues = static_cast<uint32_t>(v);
        } else if (a == "--ring") {
            ok = take_u64(argc, argv, i, v);
            cfg.ring_capacity = static_cast<uint32_t>(v);
        } else if (a == "--size") {
            ok = take_u64(argc, argv, i, v);
            cfg.payload_bytes = static_cast<uint32_t>(v);
        } else if (a == "--pps") {
            ok = take_u64(argc, argv, i, v);
            cfg.target_pps = v;
        } else if (a == "--packets") {
            ok = take_u64(argc, argv, i, v);
            cfg.max_packets = v;
        } else if (a == "--duration") {
            ok = take_u64(argc, argv, i, v);
            cfg.duration_ms = static_cast<uint32_t>(v);
        } else if (a == "--burst") {
            ok = take_u64(argc, argv, i, v);
            cfg.burst = static_cast<uint32_t>(v);
        } else if (a == "--backoff") {
            ok = take_u64(argc, argv, i, v);
            cfg.idle_backoff_ns = static_cast<uint32_t>(v);
        } else {
            std::fprintf(stderr, "[gnp] unknown option: %s\n", a.c_str());
            return false;
        }

        if (!ok) {
            std::fprintf(stderr, "[gnp] bad or missing value for %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

/// Retire every poller. Per queue: publish_limit first, then stop_flag. The
/// kernel only leaves the idle path once idx has caught publish_limit, so a
/// stop that becomes visible before the last CQE cannot truncate the drain.
/// Queues are independent, so there is no ordering requirement across them.
void retire_pollers(gnp::Session& session, const std::vector<gnp::SimStats>& sim_stats) {
    for (size_t q = 0; q < session.queues.size(); ++q) {
        reinterpret_cast<std::atomic<unsigned long long>*>(&session.queues[q].ctrl->publish_limit)
            ->store(sim_stats[q].produced, std::memory_order_release);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (gnp::QueueSession& q : session.queues) {
        reinterpret_cast<std::atomic<uint32_t>*>(&q.ctrl->stop_flag)
            ->store(1u, std::memory_order_release);
    }
}

/// Signal every producer before joining any, so all queues stop together.
void stop_simulators(std::vector<gnp::Simulator*>& sims, std::vector<gnp::SimStats>& out) {
    for (gnp::Simulator* sim : sims) gnp::sim_request_stop(sim);
    for (size_t q = 0; q < sims.size(); ++q) {
        gnp::sim_stop(sims[q], out[q]);
        sims[q] = nullptr;
    }
}

}  // namespace

int main(int argc, char** argv) {
    gnp::RunConfig cfg;
    bool want_help = false;
    if (!parse_args(argc, argv, cfg, want_help)) return 2;
    if (want_help) {
        usage(argv[0]);
        return 0;
    }

#if !defined(GNP_SIMULATION)
    std::fprintf(stderr,
                 "[gnp] built with GNP_SIMULATION=OFF and no hardware producer exists yet\n");
    return 1;
#else
    gnp::Session session;
    if (!gnp::session_create(cfg, session)) return 1;

    const size_t n = session.queues.size();

    // Every simulated producer busy-spins on the host. Past one per hardware
    // thread they preempt each other (and CUDA's internal locks), flushes land
    // late, and pollers can hit their watchdog before draining. That is a limit
    // of the software NIC, not of the pollers - real NIC queues are hardware.
    const unsigned hw_threads = std::thread::hardware_concurrency();
    if (hw_threads && n > hw_threads) {
        std::fprintf(stderr,
                     "[gnp] warning: %zu simulated producers exceed %u hardware threads; "
                     "results will reflect host scheduling, not polling\n",
                     n, hw_threads);
    }

    // 1. The pollers go first, so they are already spinning when packets appear.
    std::vector<gnp::PollQueue> poll_queues;
    poll_queues.reserve(n);
    for (const gnp::QueueSession& q : session.queues) poll_queues.push_back(q.poll_queue());

    if (!gnp::backend_launch_poller(poll_queues.data(), static_cast<uint32_t>(n),
                                    session.clock_offset_ns, cfg)) {
        gnp::session_destroy(session);
        return 1;
    }

    // 2. Then one producer per queue. In the hardware build these are the NIC's
    //    Rx queues. Each writes its own host staging; flushes copy CQEs into the
    //    device ring its poller watches.
    std::vector<gnp::Simulator*> sims(n, nullptr);
    std::vector<gnp::SimStats> sim_stats(n);
    for (size_t q = 0; q < n; ++q) {
        gnp::QueueSession& qs = session.queues[q];
        gnp::CompletionRing host_ring = qs.ring;
        host_ring.descs = qs.host_descs;
        sims[q] = gnp::sim_start(static_cast<uint32_t>(q), host_ring, qs.ring.descs, qs.ctrl,
                                 qs.arena, qs.arena_bytes, cfg);
        if (!sims[q]) {
            std::fprintf(stderr, "[gnp] failed to start the simulator for queue %zu\n", q);
            sims.resize(q);  // only stop the ones that started; the rest publish 0
            stop_simulators(sims, sim_stats);
            retire_pollers(session, sim_stats);
            gnp::backend_wait_poller();
            gnp::session_destroy(session);
            return 1;
        }
    }

    std::printf("[gnp] polling %zu queue(s) on %s backend for %u ms...\n", n, gnp::backend_name(),
                cfg.duration_ms);

    // 3. Steady state. This is the interesting part: the host is asleep while
    //    the SMs do all the receive-side work.
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.duration_ms));

    // Each producer flushes and waits on its queue's copy stream before its
    // thread exits, so after this every published CQE is visible to its poller.
    stop_simulators(sims, sim_stats);

    // 4. Retire the pollers, then collect per-queue results.
    retire_pollers(session, sim_stats);
    const bool ok = gnp::backend_wait_poller();

    std::vector<gnp::PollStats> poll_stats(n);
    for (size_t q = 0; q < n; ++q) poll_stats[q] = *session.queues[q].stats;

    gnp::report(cfg, poll_stats.data(), sim_stats.data(), static_cast<uint32_t>(n),
                gnp::backend_name(), session.clock_offset_ns);
    gnp::session_destroy(session);
    return ok ? 0 : 1;
#endif
}
