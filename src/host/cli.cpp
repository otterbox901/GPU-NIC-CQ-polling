//
// src/host/cli.cpp - command-line flags shared by every driver binary.
//

#include "gnp/cli.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gnp {

void print_common_usage() {
    std::printf(
        "  --queues N        independent completion rings, one poller each (default 1)\n"
        "  --ring N          entries per ring, power of two (default 1024)\n"
        "  --size B          arena bytes per ring slot = max payload size (default 1024)\n"
        "  --duration MS     how long to run (default 2000)\n"
        "  --backoff NS      relax the poll loop when idle, 0 = pure spin (default 0)\n"
        "  --copy-timing     time 1 in 32 H2D flushes (CUDA); throttles near-saturated runs\n"
        "  --verbose         print device and allocation details\n"
        "  --help\n");
}

bool take_u64(int argc, char** argv, int& i, uint64_t& out) {
    if (i + 1 >= argc) return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(argv[++i], &end, 10);
    if (end == argv[i] || *end != '\0') return false;
    out = v;
    return true;
}

FlagResult parse_common_flag(int argc, char** argv, int& i, RunConfig& cfg) {
    const char* a = argv[i];
    uint64_t v = 0;
    bool ok = true;

    if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
        return FlagResult::kHelp;
    } else if (!std::strcmp(a, "--verbose")) {
        cfg.verbose = true;
    } else if (!std::strcmp(a, "--copy-timing")) {
        cfg.copy_timing = true;
    } else if (!std::strcmp(a, "--queues")) {
        ok = take_u64(argc, argv, i, v) && v <= UINT32_MAX;
        cfg.queues = static_cast<uint32_t>(v);
    } else if (!std::strcmp(a, "--ring")) {
        ok = take_u64(argc, argv, i, v) && v <= UINT32_MAX;
        cfg.ring_capacity = static_cast<uint32_t>(v);
    } else if (!std::strcmp(a, "--size")) {
        ok = take_u64(argc, argv, i, v) && v <= UINT32_MAX;
        cfg.payload_bytes = static_cast<uint32_t>(v);
    } else if (!std::strcmp(a, "--duration")) {
        ok = take_u64(argc, argv, i, v) && v <= UINT32_MAX;
        cfg.duration_ms = static_cast<uint32_t>(v);
    } else if (!std::strcmp(a, "--backoff")) {
        ok = take_u64(argc, argv, i, v) && v <= UINT32_MAX;
        cfg.idle_backoff_ns = static_cast<uint32_t>(v);
    } else {
        return FlagResult::kNotMine;
    }

    if (!ok) {
        std::fprintf(stderr, "[gnp] bad or missing value for %s\n", a);
        return FlagResult::kBad;
    }
    return FlagResult::kOk;
}

}  // namespace gnp
