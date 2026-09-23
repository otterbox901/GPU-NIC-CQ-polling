#pragma once
//
// gnp/ring.hpp - the completion ring and its publish protocol.
//
// The ring is the single point where "how packets arrive" is decoupled from
// "who notices they arrived". The producer is the AF_XDP ingest thread
// (src/host/xdp_ingest.cpp), or the fake NIC in testing/sim/ during
// development. The consumer is a persistent CUDA kernel on an SM.
//

#include "gnp/common.hpp"

namespace gnp {

/// Owner ("phase") bit inside CompletionDesc::status.
///
/// This is how real NIC completion queues signal a new entry without any
/// doorbell or interrupt: the producer flips the bit each time it wraps the
/// ring, and the consumer compares it against the phase it expects at that
/// index. It is the reason a GPU can poll a CQ with nothing but loads.
constexpr uint32_t kOwnerMask = 0x1u;

/// One completion-queue entry.
///
/// 32 bytes: two entries per 64-byte cache line, the same order of magnitude
/// as a real CQE, so the poll loop has realistic memory behaviour.
struct alignas(32) CompletionDesc {
    uint64_t payload_offset;  ///< byte offset of the packet inside the data arena
    uint32_t byte_len;        ///< received length
    uint32_t packet_id;       ///< monotonic id, lets the poller detect gaps
    uint64_t post_ns;         ///< host_now_ns() when the producer published this
    uint32_t reserved;        ///< keeps the struct at 32 bytes
    uint32_t status;          ///< bit0 = owner/phase. MUST be published last.
};
static_assert(sizeof(CompletionDesc) == 32, "CQE layout must stay 32 bytes");

/// The ring descriptor. Deliberately POD: it is passed to the kernel by value.
struct CompletionRing {
    CompletionDesc* descs = nullptr;  ///< capacity entries, reachable by both sides
    uint32_t capacity = 0;            ///< power of two
    uint32_t mask     = 0;            ///< capacity - 1
    uint32_t shift    = 0;            ///< log2(capacity)
};

/// Out-of-band control block. Kept separate so the ring stays a pure data plane.
struct RingControl {
    uint32_t stop_flag;                  ///< host -> device: retire request (see publish_limit)
    uint32_t _pad;
    unsigned long long consumed;         ///< device -> host: consumer index, updated periodically
    /// Host sets this to the final produced count *before* stop_flag. ~0ull means
    /// "no limit yet". The poller only retires on the idle path once
    /// `idx >= publish_limit`, so a stop that races ahead of the last CQE cannot
    /// truncate the drain.
    unsigned long long publish_limit;
};

// --- index math, shared verbatim by the pollers and every producer -----------

GNP_HD inline bool is_power_of_two(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

GNP_HD inline uint32_t ring_slot(const CompletionRing& r, uint64_t idx) {
    return static_cast<uint32_t>(idx) & r.mask;
}

/// Owner bit the consumer expects at `idx`. Pass 0 expects 1, pass 1 expects 0, ...
GNP_HD inline uint32_t ring_expected_owner(const CompletionRing& r, uint64_t idx) {
    return static_cast<uint32_t>(((idx >> r.shift) & 1ull) ^ 1ull);
}

GNP_HD inline bool desc_ready(uint32_t status, uint32_t expected_owner) {
    return (status & kOwnerMask) == expected_owner;
}

// --- producer side -----------------------------------------------------------

struct IngestStats {
    uint64_t produced = 0;   ///< descriptors published
    uint64_t overruns = 0;   ///< times the producer had to wait for a free slot
    uint64_t dropped  = 0;   ///< frames received but not published (malformed / too big)
    uint64_t start_ns = 0;
    uint64_t end_ns   = 0;
    /// Sampled H2D flushes; zero unless --copy-timing is set.
    uint64_t copy_ns_sum  = 0;  ///< producer submit -> flush complete on device
    uint64_t copy_samples = 0;  ///< flushes that contributed to copy_ns_sum
};

/// Publish one descriptor at `idx` using the owner-bit protocol.
/// Payload fields are written first, then a store-store barrier, then status.
/// Every producer goes through this one function (src/host/ring.cpp).
void ring_publish(const CompletionRing& ring, uint64_t idx, uint64_t payload_offset,
                  uint32_t byte_len, uint32_t packet_id, uint64_t post_ns);

}  // namespace gnp
