//
// testing/sim/main_sim.cpp - gnp_sim: the pollers fed by the fake NIC.
//
// Same orchestration as src/host/main.cpp, but one simulated producer per
// queue instead of AF_XDP, so any number of queues and any arrival pattern can
// be exercised without a network. The correctness sweep and the benchmarks in
// testing/scripts/ drive this binary.
//

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "gnp/cli.hpp"
#include "gnp/common.hpp"
#include "gnp/gpu_poll.hpp"
#include "gnp/metrics.hpp"
#include "gnp/ring.hpp"
#include "gnp_testing/sim.hpp"

namespace {

void usage(const char* argv0) {
    std::printf("usage: %s [options]\n", argv0);
    gnp::print_common_usage();
    std::printf(
        "  --pps R           injection rate PER QUEUE, 0 = unpaced (default 100000)\n"
        "  --packets N       stop after N packets per queue, 0 = until duration (default 0)\n"
        "  --burst N         descriptors published back-to-back (default 1)\n");
}

bool parse_args(int argc, char** argv, gnp::RunConfig& cfg, bool& want_help) {
    for (int i = 1; i < argc; ++i) {
        switch (gnp::parse_common_flag(argc, argv, i, cfg)) {
            case gnp::FlagResult::kOk: continue;
            case gnp::FlagResult::kBad: return false;
            case gnp::FlagResult::kHelp: want_help = true; return true;
            case gnp::FlagResult::kNotMine: break;
        }

        const char* a = argv[i];
        uint64_t v = 0;
        bool ok = true;
        if (!std::strcmp(a, "--pps")) {
            ok = gnp::take_u64(argc, argv, i, v);
            cfg.target_pps = v;
        } else if (!std::strcmp(a, "--packets")) {
            ok = gnp::take_u64(argc, argv, i, v);
            cfg.max_packets = v;
        } else if (!std::strcmp(a, "--burst")) {
            ok = gnp::take_u64(argc, argv, i, v) && v <= UINT32_MAX;
            cfg.burst = static_cast<uint32_t>(v);
        } else {
            std::fprintf(stderr, "[gnp] unknown option: %s\n", a);
            return false;
        }
        if (!ok) {
            std::fprintf(stderr, "[gnp] bad or missing value for %s\n", a);
            return false;
        }
    }
    return true;
}

/// Retire every poller. Per queue: publish_limit first, then stop_flag. The
/// kernel only leaves the idle path once idx has caught publish_limit, so a
/// stop that becomes visible before the last CQE cannot truncate the drain.
void retire_pollers(gnp::Session& session, const std::vector<gnp::IngestStats>& sim_stats) {
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
void stop_simulators(std::vector<gnp_testing::Simulator*>& sims,
                     std::vector<gnp::IngestStats>& out) {
    for (gnp_testing::Simulator* sim : sims) gnp_testing::sim_request_stop(sim);
    for (size_t q = 0; q < sims.size(); ++q) {
        gnp_testing::sim_stop(sims[q], out[q]);
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
    std::vector<const uint8_t*> arenas;
    for (const gnp::QueueSession& q : session.queues) {
        poll_queues.push_back(q.poll_queue());
        arenas.push_back(q.arena);
    }

    if (!gnp::backend_launch_poller(poll_queues.data(), arenas.data(), static_cast<uint32_t>(n),
                                    session.clock_offset_ns, cfg)) {
        gnp::session_destroy(session);
        return 1;
    }

    // 2. Then one producer per queue. Each writes its own host staging; flushes
    //    copy CQEs into the device ring its poller watches.
    std::vector<gnp_testing::Simulator*> sims(n, nullptr);
    std::vector<gnp::IngestStats> sim_stats(n);
    for (size_t q = 0; q < n; ++q) {
        gnp::QueueSession& qs = session.queues[q];
        gnp::CompletionRing host_ring = qs.ring;
        host_ring.descs = qs.host_descs;
        sims[q] = gnp_testing::sim_start(static_cast<uint32_t>(q), host_ring, qs.ring.descs,
                                         qs.ctrl, qs.arena, qs.arena_bytes, cfg);
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

    // 3. Steady state: the host sleeps while the pollers do the receive work.
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
                gnp::backend_name(), "simulated NIC", session.clock_offset_ns);
    gnp::session_destroy(session);
    return ok ? 0 : 1;
}
