#pragma once
//
// gnp_testing/sim.hpp - a software stand-in for the NIC.
//
// A host thread per queue that fabricates packets and publishes them with
// gnp::ring_publish(), the same function the real AF_XDP ingest uses. It lets
// the ring and the pollers be exercised and benchmarked with no network at all.
//

#include <atomic>
#include <thread>

#include "gnp/common.hpp"
#include "gnp/ring.hpp"

namespace gnp_testing {

/// One queue's injection thread and the counters it fills in. Neither copyable
/// nor movable (it holds an atomic), so hold these by value in place - a
/// std::vector<Simulator> sized once at construction is fine.
struct Simulator {
    std::thread thread;
    std::atomic<bool> stop{false};
    gnp::IngestStats stats;
};

/// Start `sim`'s injection thread for queue `queue`. `host_ring.descs` is the
/// producer staging buffer; `device_descs` is what the poller reads (may alias
/// host on the CPU backend). Simulators share no state, so one per queue can
/// run concurrently; each numbers its packets from 0.
///
/// Returns false if the buffers are inconsistent, leaving `sim` unstarted: it
/// must then not be passed to sim_request_stop() or sim_stop().
bool sim_start(Simulator& sim, uint32_t queue, const gnp::CompletionRing& host_ring,
               gnp::CompletionDesc* device_descs, gnp::RingControl* ctrl, uint8_t* arena,
               size_t arena_bytes, const gnp::RunConfig& cfg);

/// Ask the injection thread to stop, without waiting. With several queues,
/// signal them all before joining any, or later queues keep producing while
/// earlier ones are being joined.
void sim_request_stop(Simulator& sim);

/// Stop (if not already asked), join the injection thread and collect its
/// counters.
void sim_stop(Simulator& sim, gnp::IngestStats& out);

}  // namespace gnp_testing
