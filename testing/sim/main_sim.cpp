//
// testing/sim/main_sim.cpp - gnp_sim: the pollers fed by the fake NIC.
//
// Same orchestration as src/host/main.cpp, but one simulated producer per
// queue instead of AF_XDP, so any number of queues and any arrival pattern can
// be exercised without a network. The correctness sweep and the benchmarks in
// testing/scripts/ drive this binary.
//

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

/// Signal the first `started` producers before joining any of them, so all
/// queues stop together. Queues that never started are left alone; their
/// IngestStats stay zeroed, which is what "published nothing" means.
void stop_simulators(std::vector<gnp_testing::Simulator>& sims, size_t started,
                     std::vector<gnp::IngestStats>& out) {
    for (size_t q = 0; q < started; ++q) gnp_testing::sim_request_stop(sims[q]);
    for (size_t q = 0; q < started; ++q) gnp_testing::sim_stop(sims[q], out[q]);
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
                                    cfg)) {
        gnp::session_destroy(session);
        return 1;
    }

    // 2. Then one producer per queue. Each writes its own host staging; flushes
    //    copy CQEs into the device ring its poller watches.
    std::vector<gnp_testing::Simulator> sims(n);
    std::vector<gnp::IngestStats> sim_stats(n);
    for (size_t q = 0; q < n; ++q) {
        gnp::QueueSession& qs = session.queues[q];
        gnp::CompletionRing host_ring = qs.ring;
        host_ring.descs = qs.host_descs;
        if (!gnp_testing::sim_start(sims[q], static_cast<uint32_t>(q), host_ring, qs.ring.descs,
                                    qs.ctrl, qs.arena, qs.arena_bytes, cfg)) {
            std::fprintf(stderr, "[gnp] failed to start the simulator for queue %zu\n", q);
            stop_simulators(sims, q, sim_stats);  // only the ones that started
            gnp::session_retire_pollers(session, sim_stats.data());
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
    stop_simulators(sims, n, sim_stats);

    // 4. Retire the pollers, then collect per-queue results.
    gnp::session_retire_pollers(session, sim_stats.data());
    const bool ok = gnp::backend_wait_poller();

    std::vector<gnp::PollStats> poll_stats(n);
    for (size_t q = 0; q < n; ++q) poll_stats[q] = *session.queues[q].stats;

    gnp::report(cfg, poll_stats.data(), sim_stats.data(), static_cast<uint32_t>(n),
                gnp::backend_name(), "simulated NIC");
    gnp::session_destroy(session);
    return ok ? 0 : 1;
}
