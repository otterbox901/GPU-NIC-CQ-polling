//
// src/host/ring.cpp - the producer half of the owner-bit protocol.
//

#include <atomic>

#include "gnp/ring.hpp"

namespace gnp {

void ring_publish(const CompletionRing& ring, uint64_t idx, uint64_t payload_offset,
                  uint32_t byte_len, uint32_t packet_id, uint64_t post_ns) {
    CompletionDesc* d = &ring.descs[ring_slot(ring, idx)];

    d->payload_offset = payload_offset;
    d->byte_len = byte_len;
    d->packet_id = packet_id;
    d->post_ns = post_ns;
    d->reserved = 0;

    // Full barrier, not just a release fence. On x86 a release fence is only a
    // compiler barrier, which is not enough here: these stores may land in
    // write-combining PCIe-mapped memory, where the CPU is free to reorder them.
    // A seq_cst fence emits a real mfence and drains the WC buffers.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    reinterpret_cast<std::atomic<uint32_t>*>(&d->status)
        ->store(ring_expected_owner(ring, idx), std::memory_order_relaxed);
}

}  // namespace gnp
