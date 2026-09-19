#pragma once
//
// gnp/xdp_ingest.hpp - the real NIC producer: AF_XDP kernel bypass.
//
// An eBPF/XDP program (src/bpf/udp_redirect.bpf.c) redirects one UDP port on
// one NIC RX queue into an AF_XDP socket before the kernel network stack sees
// it. A host thread drains that socket, copies each UDP payload into the
// payload arena and publishes a CompletionDesc with ring_publish().
//
// Needs root (or CAP_NET_ADMIN, CAP_NET_RAW, CAP_BPF and CAP_IPC_LOCK).
// Only built when libxdp/libbpf and clang were found (GNP_WITH_XDP).
//

#include "gnp/common.hpp"
#include "gnp/ring.hpp"

namespace gnp {

struct XdpConfig {
    const char* iface    = nullptr;  ///< interface to attach to, e.g. "enp1s0"
    uint16_t    port     = 0;        ///< destination UDP port to capture
    uint32_t    queue_id = 0;        ///< NIC RX queue the socket binds to
    const char* bpf_obj  = nullptr;  ///< udp_redirect.bpf.o; nullptr = next to the binary
    bool        skb_mode = false;    ///< force generic XDP (any interface, slower)
};

struct XdpIngest;  ///< opaque; owns the BPF program, the socket and the RX thread

/// Attach the XDP program, open the AF_XDP socket and start the RX thread,
/// which publishes into queue `queue` of the session. `host_ring`,
/// `device_descs`, `ctrl` and `arena` have the same meaning as for every other
/// producer (see gnp::backend_flush_descs). Returns nullptr on failure, after
/// printing why and undoing whatever was set up.
XdpIngest* xdp_ingest_start(uint32_t queue, const CompletionRing& host_ring,
                            CompletionDesc* device_descs, RingControl* ctrl, uint8_t* arena,
                            size_t arena_bytes, const RunConfig& cfg, const XdpConfig& xdp);

/// Stop the RX thread, detach the program from the interface, close the
/// socket and collect the counters. Frees the handle.
void xdp_ingest_stop(XdpIngest* ingest, IngestStats& out);

}  // namespace gnp
