#pragma once
//
// gnp/packet_handler.hpp - where received packets get used.
//
// on_packet() is called once per packet by whichever receive path is compiled
// in, right after the packet is observed. It is intentionally empty: replace
// the body with the real consumer (parsing, filtering, forwarding, ...). It
// sits on the receive loop's critical path, so whatever goes here sets the
// per-packet budget.
//

#include "gnp/common.hpp"

namespace gnp {

/// One received packet, as the poller observed it.
///
/// GPUNetIO path: `data` points at the full Ethernet frame in GPU memory, put
/// there by the NIC with no CPU involvement. This is the path where the hook
/// can do real work.
///
/// AF_XDP path: `data` is always nullptr. That path stages UDP payloads in a
/// host arena and DMAs only 32-byte descriptors to the device, so the kernel
/// has no packet bytes to hand over - it can report that a packet arrived and
/// how long it was, nothing more.
struct PacketView {
    const uint8_t* data;  ///< nullptr when the poller cannot reach the bytes
    uint32_t       len;   ///< received length in bytes
};

GNP_HD GNP_FORCEINLINE void on_packet(const PacketView& pkt) {
    (void)pkt;
}

}  // namespace gnp
