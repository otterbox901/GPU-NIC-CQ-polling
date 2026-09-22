//
// testing/unit/test_ring.cpp - the owner-bit protocol, ring independence (the basis of
// multi-queue) and the report's cross-queue aggregation, on plain host memory.
//
// No GPU and no CUDA toolkit needed: the index math in ring.hpp is the same
// code the kernel runs, so testing it here catches real protocol bugs.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gnp/common.hpp"
#include "gnp/metrics.hpp"
#include "gnp/ring.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

gnp::CompletionRing make_ring(std::vector<gnp::CompletionDesc>& storage, uint32_t capacity,
                              uint32_t shift) {
    storage.assign(capacity, gnp::CompletionDesc{});
    std::memset(storage.data(), 0, capacity * sizeof(gnp::CompletionDesc));
    gnp::CompletionRing r;
    r.descs = storage.data();
    r.capacity = capacity;
    r.mask = capacity - 1;
    r.shift = shift;
    return r;
}

void test_layout() {
    CHECK(sizeof(gnp::CompletionDesc) == 32);
    CHECK(alignof(gnp::CompletionDesc) == 32);
    CHECK(gnp::is_power_of_two(1024));
    CHECK(!gnp::is_power_of_two(1000));
    CHECK(!gnp::is_power_of_two(0));
}

void test_index_math() {
    std::vector<gnp::CompletionDesc> storage;
    const gnp::CompletionRing r = make_ring(storage, 8, 3);

    CHECK(gnp::ring_slot(r, 0) == 0);
    CHECK(gnp::ring_slot(r, 7) == 7);
    CHECK(gnp::ring_slot(r, 8) == 0);
    CHECK(gnp::ring_slot(r, 19) == 3);

    // Pass 0 expects owner 1, pass 1 expects 0, pass 2 expects 1 again.
    CHECK(gnp::ring_expected_owner(r, 0) == 1);
    CHECK(gnp::ring_expected_owner(r, 7) == 1);
    CHECK(gnp::ring_expected_owner(r, 8) == 0);
    CHECK(gnp::ring_expected_owner(r, 15) == 0);
    CHECK(gnp::ring_expected_owner(r, 16) == 1);
}

/// A freshly zeroed ring must look empty for the whole first pass, otherwise the
/// poller would report phantom packets the moment it starts.
void test_zeroed_ring_is_empty() {
    std::vector<gnp::CompletionDesc> storage;
    const gnp::CompletionRing r = make_ring(storage, 8, 3);
    for (uint64_t i = 0; i < r.capacity; ++i) {
        CHECK(!gnp::desc_ready(r.descs[gnp::ring_slot(r, i)].status,
                               gnp::ring_expected_owner(r, i)));
    }
}

/// Publish then consume across several wraps, mirroring the kernel's loop.
void test_publish_consume_wraps() {
    constexpr uint32_t kCap = 8;
    constexpr uint64_t kTotal = kCap * 5 + 3;  // deliberately not a whole number of passes

    std::vector<gnp::CompletionDesc> storage;
    const gnp::CompletionRing r = make_ring(storage, kCap, 3);

    uint64_t consumed = 0;
    for (uint64_t produced = 0; produced < kTotal; ++produced) {
        // Stale entry from the previous pass must not read as ready yet.
        CHECK(!gnp::desc_ready(r.descs[gnp::ring_slot(r, produced)].status,
                               gnp::ring_expected_owner(r, produced)));

        gnp::ring_publish(r, produced, produced * 64, 512, static_cast<uint32_t>(produced),
                         gnp::host_now_ns());

        // Consumer catches up one entry at a time, exactly like the kernel.
        CHECK(gnp::desc_ready(r.descs[gnp::ring_slot(r, consumed)].status,
                              gnp::ring_expected_owner(r, consumed)));
        const gnp::CompletionDesc& d = r.descs[gnp::ring_slot(r, consumed)];
        CHECK(d.packet_id == static_cast<uint32_t>(consumed));
        CHECK(d.byte_len == 512);
        CHECK(d.payload_offset == consumed * 64);
        ++consumed;
    }
    CHECK(consumed == kTotal);
}

/// Fill the ring completely before draining it: catches an expected-owner bug
/// that a lock-step producer/consumer would hide.
void test_full_ring_then_drain() {
    constexpr uint32_t kCap = 16;
    std::vector<gnp::CompletionDesc> storage;
    const gnp::CompletionRing r = make_ring(storage, kCap, 4);

    for (uint64_t pass = 0; pass < 3; ++pass) {
        const uint64_t base = pass * kCap;
        for (uint32_t i = 0; i < kCap; ++i) {
            gnp::ring_publish(r, base + i, i * 128, 64, static_cast<uint32_t>(base + i),
                             gnp::host_now_ns());
        }
        for (uint32_t i = 0; i < kCap; ++i) {
            const uint64_t idx = base + i;
            CHECK(gnp::desc_ready(r.descs[gnp::ring_slot(r, idx)].status,
                                  gnp::ring_expected_owner(r, idx)));
            CHECK(r.descs[gnp::ring_slot(r, idx)].packet_id == static_cast<uint32_t>(idx));
        }
        // The next unpublished index must still read as empty.
        const uint64_t next = base + kCap;
        CHECK(!gnp::desc_ready(r.descs[gnp::ring_slot(r, next)].status,
                               gnp::ring_expected_owner(r, next)));
    }
}

/// Multi-queue rests on rings sharing nothing. Drive two rings of different
/// sizes at different rates, interleaved, and check neither sees the other's
/// traffic: each keeps its own ids, owner phase and empty/ready state.
void test_independent_rings() {
    std::vector<gnp::CompletionDesc> storage_a;
    std::vector<gnp::CompletionDesc> storage_b;
    const gnp::CompletionRing a = make_ring(storage_a, 8, 3);
    const gnp::CompletionRing b = make_ring(storage_b, 16, 4);

    uint64_t produced_a = 0, consumed_a = 0;
    uint64_t produced_b = 0, consumed_b = 0;

    for (int step = 0; step < 100; ++step) {
        // Ring A gets one entry per step, ring B three: they wrap at different
        // times, so their owner phases drift apart.
        gnp::ring_publish(a, produced_a, produced_a * 64, 100, static_cast<uint32_t>(produced_a),
                         gnp::host_now_ns());
        ++produced_a;
        for (int k = 0; k < 3; ++k) {
            gnp::ring_publish(b, produced_b, produced_b * 64, 200,
                             static_cast<uint32_t>(1000000 + produced_b), gnp::host_now_ns());
            ++produced_b;
        }

        while (consumed_a < produced_a) {
            const gnp::CompletionDesc& d = a.descs[gnp::ring_slot(a, consumed_a)];
            CHECK(gnp::desc_ready(d.status, gnp::ring_expected_owner(a, consumed_a)));
            CHECK(d.packet_id == static_cast<uint32_t>(consumed_a));
            CHECK(d.byte_len == 100);
            ++consumed_a;
        }
        while (consumed_b < produced_b) {
            const gnp::CompletionDesc& d = b.descs[gnp::ring_slot(b, consumed_b)];
            CHECK(gnp::desc_ready(d.status, gnp::ring_expected_owner(b, consumed_b)));
            CHECK(d.packet_id == static_cast<uint32_t>(1000000 + consumed_b));
            CHECK(d.byte_len == 200);
            ++consumed_b;
        }

        // Both drained: the next slot of each must read empty, whatever the
        // other ring just did.
        CHECK(!gnp::desc_ready(a.descs[gnp::ring_slot(a, produced_a)].status,
                               gnp::ring_expected_owner(a, produced_a)));
        CHECK(!gnp::desc_ready(b.descs[gnp::ring_slot(b, produced_b)].status,
                               gnp::ring_expected_owner(b, produced_b)));
    }
    CHECK(consumed_a == 100);
    CHECK(consumed_b == 300);
}

/// The report's aggregate: counters sum and run time is the max, since pollers
/// run concurrently rather than back to back.
void test_stats_aggregate() {
    gnp::PollStats q[3];
    for (gnp::PollStats& s : q) gnp::stats_reset(s);

    q[0].packets = 10; q[0].bytes = 1000; q[0].idle_spins = 5; q[0].gaps = 0;
    q[0].run_ns = 1000;

    q[1].packets = 30; q[1].bytes = 3000; q[1].idle_spins = 7; q[1].gaps = 2;
    q[1].run_ns = 1500;

    // q[2]: saw nothing, and must not affect the aggregate.

    const gnp::PollStats a = gnp::stats_aggregate(q, 3);
    CHECK(a.packets == 40);
    CHECK(a.bytes == 4000);
    CHECK(a.idle_spins == 12);
    CHECK(a.gaps == 2);
    CHECK(a.run_ns == 1500);

    // A single queue must aggregate to itself, so N=1 reports are unchanged.
    const gnp::PollStats one = gnp::stats_aggregate(q, 1);
    CHECK(std::memcmp(&one, &q[0], sizeof(gnp::PollStats)) == 0);

    gnp::IngestStats s[3];
    s[0].produced = 10; s[0].overruns = 1; s[0].start_ns = 100; s[0].end_ns = 900;
    s[1].produced = 30; s[1].overruns = 0; s[1].start_ns = 50;  s[1].end_ns = 800;
    // s[2]: producer never ran (zero window) - must not widen the window.
    const gnp::IngestStats sa = gnp::ingest_aggregate(s, 3);
    CHECK(sa.produced == 40);
    CHECK(sa.overruns == 1);
    CHECK(sa.start_ns == 50);
    CHECK(sa.end_ns == 900);
}

}  // namespace

int main() {
    test_layout();
    test_index_math();
    test_zeroed_ring_is_empty();
    test_publish_consume_wraps();
    test_full_ring_then_drain();
    test_independent_rings();
    test_stats_aggregate();

    if (g_failures) {
        std::fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all ring-protocol checks passed\n");
    return 0;
}
