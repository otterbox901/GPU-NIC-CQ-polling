#pragma once
//
// gnp/packet_handler.hpp - where received packets get used.
//
// on_packet() is called once per descriptor, by both pollers, right after the
// descriptor is observed. It is intentionally empty: replace the body with the
// real consumer (parsing, filtering, forwarding, ...). It sits on the poll
// loop's critical path, so whatever goes here sets the per-packet budget.
//

#include "gnp/common.hpp"
#include "gnp/ring.hpp"

namespace gnp {

/// `desc` is the observed descriptor. `payload` points at desc.byte_len bytes of
/// UDP payload on the CPU poller; it is nullptr on the CUDA poller, because the
/// payload arena is host memory the kernel cannot read.
GNP_HD GNP_FORCEINLINE void on_packet(const CompletionDesc& desc, const uint8_t* payload) {
    (void)desc;
    (void)payload;
}

}  // namespace gnp
