//
// src/host/metrics.cpp - end-of-run reporting.
//

#include <cinttypes>
#include <cstdio>

#include "gnp/metrics.hpp"

namespace gnp {
namespace {

void rule() { std::printf("  --------------------------------------------------\n"); }

void row_u64(const char* label, unsigned long long v, const char* unit = "") {
    std::printf("  %-28s %14llu %s\n", label, v, unit);
}

void row_f64(const char* label, double v, const char* unit) {
    std::printf("  %-28s %14.3f %s\n", label, v, unit);
}

/// Per-queue table. Column names deliberately avoid the aggregate row labels
/// ("descriptors published", "packets observed", "packet-id gaps", "mean") so
/// scripts that grep the summary only ever match the aggregate.
void report_per_queue(const PollStats* poll, const IngestStats* sim, uint32_t n_queues) {
    std::printf("\n  per-queue breakdown\n");
    std::printf("  %-5s %11s %11s %6s %8s %10s %9s %10s %10s\n", "queue", "published",
                "observed", "gaps", "stalls", "idle/pkt", "us/pkt", "lat avg us", "lat max us");
    rule();
    for (uint32_t q = 0; q < n_queues; ++q) {
        const PollStats& p = poll[q];
        const double idle_per_pkt =
            p.packets ? static_cast<double>(p.idle_spins) / p.packets : 0.0;
        const double svc_us = p.packets ? (p.run_ns / static_cast<double>(p.packets)) / 1e3 : 0.0;
        std::printf("  %-5u %11llu %11llu %6llu %8llu %10.3f %9.3f", q,
                    static_cast<unsigned long long>(sim[q].produced), p.packets, p.gaps,
                    static_cast<unsigned long long>(sim[q].overruns), idle_per_pkt, svc_us);
        if (p.lat_samples) {
            std::printf(" %10.3f %10.3f\n",
                        (p.lat_sum_ns / static_cast<double>(p.lat_samples)) / 1000.0,
                        p.lat_max_ns / 1000.0);
        } else {
            std::printf(" %10s %10s\n", "-", "-");
        }
    }
}

}  // namespace

PollStats stats_aggregate(const PollStats* poll, uint32_t n_queues) {
    PollStats a;
    stats_reset(a);
    for (uint32_t q = 0; q < n_queues; ++q) {
        const PollStats& p = poll[q];
        a.packets += p.packets;
        a.bytes += p.bytes;
        a.idle_spins += p.idle_spins;
        a.lat_sum_ns += p.lat_sum_ns;
        a.lat_samples += p.lat_samples;
        a.gaps += p.gaps;
        a.clamped += p.clamped;
        a.drain_spins += p.drain_spins;
        if (p.lat_samples && p.lat_min_ns < a.lat_min_ns) a.lat_min_ns = p.lat_min_ns;
        if (p.lat_max_ns > a.lat_max_ns) a.lat_max_ns = p.lat_max_ns;
        if (p.run_ns > a.run_ns) a.run_ns = p.run_ns;
    }
    return a;
}

IngestStats ingest_aggregate(const IngestStats* sim, uint32_t n_queues) {
    IngestStats a;
    for (uint32_t q = 0; q < n_queues; ++q) {
        const IngestStats& s = sim[q];
        a.produced += s.produced;
        a.overruns += s.overruns;
        a.dropped += s.dropped;
        a.copy_ns_sum += s.copy_ns_sum;
        a.copy_samples += s.copy_samples;
        if (s.end_ns <= s.start_ns) continue;  // producer never ran
        if (a.start_ns == 0 || s.start_ns < a.start_ns) a.start_ns = s.start_ns;
        if (s.end_ns > a.end_ns) a.end_ns = s.end_ns;
    }
    return a;
}

void report(const RunConfig& cfg, const PollStats* per_queue_poll, const IngestStats* per_queue_sim,
            uint32_t n_queues, const char* backend, const char* ingest_label,
            int64_t clock_offset_ns) {
    const PollStats poll = stats_aggregate(per_queue_poll, n_queues);
    const IngestStats sim = ingest_aggregate(per_queue_sim, n_queues);
    const bool multi = n_queues > 1;

    const double elapsed_s =
        sim.end_ns > sim.start_ns ? (sim.end_ns - sim.start_ns) / 1e9 : 0.0;

    std::printf("\n=== gpu-nic-poll run summary ===\n");
    std::printf("  backend: %s   ", backend);
    if (multi) std::printf("queues: %u   ", n_queues);
    std::printf("ring: %u   payload slot: %u B", cfg.ring_capacity, cfg.payload_bytes);
    if (cfg.target_pps) {
        std::printf("   target: %llu pps%s", static_cast<unsigned long long>(cfg.target_pps),
                    multi ? "/queue" : "");
    }
    std::printf("\n\n");

    if (multi) {
        std::printf("  producers (%s, %u queues combined)\n", ingest_label, n_queues);
    } else {
        std::printf("  producer (%s)\n", ingest_label);
    }
    rule();
    row_u64("descriptors published", sim.produced);
    row_u64("ring-full stalls", sim.overruns);
    if (sim.dropped) row_u64("frames dropped", sim.dropped, "(malformed, or payload > --size)");
    row_f64("elapsed", elapsed_s, "s");
    if (elapsed_s > 0.0) {
        row_f64("achieved rate", sim.produced / elapsed_s / 1e6, "Mpps");
    }
    if (sim.copy_samples) {
        std::printf("  %-28s %14.3f us   (%llu flushes sampled)\n", "H2D flush, submit->done",
                    sim.copy_ns_sum / static_cast<double>(sim.copy_samples) / 1000.0,
                    static_cast<unsigned long long>(sim.copy_samples));
    }
    if (multi) {
        std::printf("\n  consumers (%u SM pollers combined)\n", n_queues);
    } else {
        std::printf("\n  consumer (SM poller)\n");
    }
    rule();
    row_u64("packets observed", poll.packets);
    row_u64("bytes observed", poll.bytes);
    row_u64("packet-id gaps", poll.gaps);
    row_u64("idle poll iterations", poll.idle_spins);
    if (poll.drain_spins) {
        row_u64("post-stop drain spins", poll.drain_spins);
    }
    if (poll.run_ns && poll.idle_spins && !multi) {
        // Busy iterations are counted with idle ones, so this reads slightly
        // low while traffic is heavy. A CQE that lands mid-iteration waits for
        // the next status load: on average half a period, at most one.
        row_f64("poll loop period",
                poll.run_ns / static_cast<double>(poll.idle_spins + poll.packets) / 1e3, "us");
    }
    if (poll.packets) {
        row_f64("idle spins per packet", static_cast<double>(poll.idle_spins) / poll.packets, "");
    }
    if (poll.run_ns) {
        row_f64(multi ? "longest poller active" : "poller active", poll.run_ns / 1e6, "ms");
        // Pollers run concurrently, so wall time / total packets is not any one
        // poller's service time. The per-queue table reports the real figure.
        if (poll.packets && !multi) {
            row_f64("service time", (poll.run_ns / static_cast<double>(poll.packets)) / 1e3, "us/pkt");
        }
    }
    if (elapsed_s > 0.0) {
        row_f64("goodput", poll.bytes * 8.0 / elapsed_s / 1e9, "Gb/s");
    }

    const long long missed =
        static_cast<long long>(sim.produced) - static_cast<long long>(poll.packets);
    if (missed != 0) {
        std::printf("  %-28s %14lld %s\n", "NOT observed", missed,
                    "(poller stopped before drain?)");
    }

    if (multi) {
        std::printf("\n  detection latency (publish -> SM observes, pooled over all queues)\n");
    } else {
        std::printf("\n  detection latency (publish -> SM observes)\n");
    }
    rule();
    if (poll.lat_samples) {
        row_f64("min", poll.lat_min_ns / 1000.0, "us");
        row_f64("mean", (poll.lat_sum_ns / static_cast<double>(poll.lat_samples)) / 1000.0, "us");
        row_f64("max", poll.lat_max_ns / 1000.0, "us");
        if (sim.copy_samples) {
            // Everything in the mean that is not the flush: batch-fill wait
            // before it (unpaced runs) plus the poller's reaction after it.
            // Signed on purpose: the flush figure reads ~1.4 us high (host
            // wake-up) and the mean carries the ~2 us clock-offset error, so
            // a negative value means "below what this subtraction resolves".
            // "poll loop period" above is the direct reaction estimate.
            const double rest_ns =
                poll.lat_sum_ns / static_cast<double>(poll.lat_samples) -
                sim.copy_ns_sum / static_cast<double>(sim.copy_samples);
            std::printf("  %-28s %14.3f us   (mean - H2D flush, +/- ~2 us)\n",
                        "outside H2D flush", rest_ns / 1000.0);
        }
        if (poll.clamped) {
            row_u64("clamped to zero", poll.clamped, "(clock-offset error)");
        }
    } else if (poll.packets) {
        std::printf("  %-28s %14s\n", "latency", "(not sampled)");
    } else {
        std::printf("  %-28s %14s\n", "no packets observed", "-");
    }
    std::printf("  cross-clock offset applied: %+lld ns (calibrated, ~2 us accurate)\n",
                static_cast<long long>(clock_offset_ns));

    if (multi) report_per_queue(per_queue_poll, per_queue_sim, n_queues);
    std::printf("\n");
}

}  // namespace gnp
