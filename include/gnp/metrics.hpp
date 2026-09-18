#pragma once
//
// gnp/metrics.hpp - counters the poller fills in, and the end-of-run report.
//

#include "gnp/common.hpp"
#include "gnp/ring.hpp"

namespace gnp {

/// Written by the poller (device or CPU fallback), read by the host at teardown.
///
/// `unsigned long long` throughout so the fields stay atomicAdd-compatible if
/// the poll loop is ever widened to more than one thread.
struct PollStats {
    unsigned long long packets;        ///< descriptors observed
    unsigned long long bytes;          ///< sum of byte_len
    unsigned long long idle_spins;     ///< poll iterations that found nothing
    unsigned long long lat_sum_ns;     ///< sum of sampled publish -> detect latency
    unsigned long long lat_min_ns;
    unsigned long long lat_max_ns;
    unsigned long long lat_samples;    ///< how many packets contributed to lat_*
    unsigned long long gaps;           ///< packet_id discontinuities
    unsigned long long clamped;        ///< latencies clamped to 0 by clock skew
    unsigned long long drain_spins;    ///< idle spins after stop while catching publish_limit
    unsigned long long run_ns;         ///< device-side elapsed (%globaltimer) while poller ran
};

void stats_reset(PollStats& s);

/// Combine per-queue stats into one aggregate. Counters are summed; latency is
/// pooled (min of mins, max of maxes, total sum / total samples); run_ns is the
/// longest-running poller, since pollers run concurrently, not back to back.
PollStats stats_aggregate(const PollStats* poll, uint32_t n_queues);

/// Combine per-queue producer stats: counts summed, time window is the union.
SimStats sim_aggregate(const SimStats* sim, uint32_t n_queues);

/// Print the end-of-run summary to stdout. `poll` and `sim` hold one entry per
/// queue. The top sections always describe the aggregate with the same labels
/// as a single-queue run (scripts/run_sim.sh parses them); with more than one
/// queue a per-queue breakdown follows.
void report(const RunConfig& cfg, const PollStats* poll, const SimStats* sim, uint32_t n_queues,
            const char* backend, int64_t clock_offset_ns);

}  // namespace gnp
