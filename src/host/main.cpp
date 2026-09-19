//
// src/host/main.cpp - attach to the NIC, launch the poller, then get out of the way.
//
// The whole point of the project is that this thread does nothing during
// steady-state receive. It allocates, launches, sleeps, and reports; the
// AF_XDP RX thread feeds the ring and the poller drains it.
//

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "gnp/cli.hpp"
#include "gnp/common.hpp"
#include "gnp/gpu_poll.hpp"
#include "gnp/metrics.hpp"
#include "gnp/ring.hpp"
#include "gnp/xdp_ingest.hpp"

namespace {

void usage(const char* argv0) {
    std::printf("usage: %s --iface IFACE --port N [options]\n", argv0);
    std::printf(
        "  --iface NAME      interface to attach the XDP program to (required)\n"
        "  --port N          destination UDP port to capture (required)\n"
        "  --nic-queue N     NIC RX queue to bind to (default 0); steer the port there\n"
        "  --skb-mode        force generic XDP: any interface, slower\n"
        "  --bpf-obj PATH    udp_redirect.bpf.o (default: next to this binary)\n");
    gnp::print_common_usage();
}

bool parse_args(int argc, char** argv, gnp::RunConfig& cfg, gnp::XdpConfig& xdp,
                bool& want_help) {
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
        if (!std::strcmp(a, "--iface")) {
            ok = i + 1 < argc;
            if (ok) xdp.iface = argv[++i];
        } else if (!std::strcmp(a, "--port")) {
            ok = gnp::take_u64(argc, argv, i, v) && v >= 1 && v <= 65535;
            xdp.port = static_cast<uint16_t>(v);
        } else if (!std::strcmp(a, "--nic-queue")) {
            ok = gnp::take_u64(argc, argv, i, v) && v < 64;  // xsks_map size
            xdp.queue_id = static_cast<uint32_t>(v);
        } else if (!std::strcmp(a, "--bpf-obj")) {
            ok = i + 1 < argc;
            if (ok) xdp.bpf_obj = argv[++i];
        } else if (!std::strcmp(a, "--skb-mode")) {
            xdp.skb_mode = true;
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

/// publish_limit first, then stop_flag: the poller only retires once it has
/// consumed everything that was published, so the drain cannot be truncated.
void retire_poller(gnp::QueueSession& qs, uint64_t produced) {
    reinterpret_cast<std::atomic<unsigned long long>*>(&qs.ctrl->publish_limit)
        ->store(produced, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    reinterpret_cast<std::atomic<uint32_t>*>(&qs.ctrl->stop_flag)
        ->store(1u, std::memory_order_release);
}

}  // namespace

int main(int argc, char** argv) {
    gnp::RunConfig cfg;
    gnp::XdpConfig xdp;
    bool want_help = false;
    if (!parse_args(argc, argv, cfg, xdp, want_help)) return 2;
    if (want_help) {
        usage(argv[0]);
        return 0;
    }

#if !defined(GNP_WITH_XDP)
    std::fprintf(stderr,
                 "[gnp] built without AF_XDP support (libxdp, libbpf or clang missing at "
                 "configure time), so there is no packet source.\n"
                 "[gnp] install them and reconfigure, or use the simulator: "
                 "cmake --preset dev && ./build/dev/testing/gnp_sim\n");
    return 1;
#else
    if (!xdp.iface || xdp.port == 0) {
        std::fprintf(stderr, "[gnp] --iface and --port are required\n");
        usage(argv[0]);
        return 2;
    }
    if (cfg.queues != 1) {
        std::fprintf(stderr, "[gnp] AF_XDP ingest captures one port on one queue; "
                             "use --queues 1 (gnp_sim covers multi-queue)\n");
        return 2;
    }

    cfg.target_pps = 0;  // arrival rate is whatever the wire delivers

    gnp::Session session;
    if (!gnp::session_create(cfg, session)) return 1;
    gnp::QueueSession& qs = session.queues[0];

    // 1. The poller goes first, so it is already spinning when packets arrive.
    const gnp::PollQueue poll_queue = qs.poll_queue();
    const uint8_t* arena = qs.arena;
    if (!gnp::backend_launch_poller(&poll_queue, &arena, 1, session.clock_offset_ns, cfg)) {
        gnp::session_destroy(session);
        return 1;
    }

    // 2. Then the NIC: attach XDP, open the socket, start draining it.
    gnp::CompletionRing host_ring = qs.ring;
    host_ring.descs = qs.host_descs;
    gnp::XdpIngest* ingest = gnp::xdp_ingest_start(0, host_ring, qs.ring.descs, qs.ctrl, qs.arena,
                                                   qs.arena_bytes, cfg, xdp);
    if (!ingest) {
        retire_poller(qs, 0);
        gnp::backend_wait_poller();
        gnp::session_destroy(session);
        return 1;
    }

    std::printf("[gnp] capturing UDP port %u on %s queue %u, polling on %s backend for %u ms...\n",
                xdp.port, xdp.iface, xdp.queue_id, gnp::backend_name(), cfg.duration_ms);

    // 3. Steady state: this thread sleeps while the RX thread and poller work.
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.duration_ms));

    // The RX thread flushes and waits before exiting, so every published CQE is
    // visible to the poller once this returns.
    gnp::IngestStats ingest_stats;
    gnp::xdp_ingest_stop(ingest, ingest_stats);

    // 4. Retire the poller and report.
    retire_poller(qs, ingest_stats.produced);
    const bool ok = gnp::backend_wait_poller();

    const gnp::PollStats poll_stats = *qs.stats;
    gnp::report(cfg, &poll_stats, &ingest_stats, 1, gnp::backend_name(), "AF_XDP ingest",
                session.clock_offset_ns);
    gnp::session_destroy(session);
    return ok ? 0 : 1;
#endif
}
