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
///
/// There is deliberately no publish -> observe latency here. Measuring it means
/// comparing the GPU's %globaltimer with the host clock, and on this hardware
/// that comparison is not good enough to be worth reporting; see
/// docs/design.md. Every field below is a counter or a device-side interval, so
/// none of them needs a second clock.
struct PollStats {
    unsigned long long packets;        ///< descriptors observed
    unsigned long long bytes;          ///< sum of byte_len
    unsigned long long idle_spins;     ///< poll iterations that found nothing
    unsigned long long gaps;           ///< packet_id discontinuities
    unsigned long long drain_spins;    ///< idle spins after stop while catching publish_limit
    unsigned long long run_ns;         ///< device-side elapsed (%globaltimer) while poller ran
};

void stats_reset(PollStats& s);

/// Combine per-queue stats into one aggregate. Counters are summed; run_ns is
/// the longest-running poller, since pollers run concurrently, not back to back.
PollStats stats_aggregate(const PollStats* poll, uint32_t n_queues);

/// Combine per-queue producer stats: counts summed, time window is the union.
IngestStats ingest_aggregate(const IngestStats* sim, uint32_t n_queues);

/// Print the end-of-run summary to stdout. `poll` and `ingest` hold one entry
/// per queue; `ingest_label` names the producer ("AF_XDP ingest", ...). The top
/// sections always describe the aggregate with the same labels as a
/// single-queue run (testing/scripts/run_sim.sh parses them); with more than
/// one queue a per-queue breakdown follows.
void report(const RunConfig& cfg, const PollStats* poll, const IngestStats* ingest,
            uint32_t n_queues, const char* backend, const char* ingest_label);

}  // namespace gnp
