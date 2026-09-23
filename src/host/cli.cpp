//
// src/host/cli.cpp - command-line flags shared by the driver binaries.
//

#include "gnp/cli.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gnp {
namespace {

/// Every flag taking an unsigned value. `usage` is both the name we match on
/// and the help text's left column, so the two can never drift apart. Flags
/// line up at kUsageWidth; each binary's own usage lines use the same width.
constexpr int kUsageWidth = 18;

struct U32Flag {
    const char* usage;   ///< "--queues N": the flag, then its value placeholder
    const char* help;
    uint32_t RunConfig::* field;
    FlagScope scope;
};

constexpr U32Flag kU32Flags[] = {
    {"--queues N", "independent receive queues, one poller each (default 1)",
     &RunConfig::queues, FlagScope::kCore},
    {"--duration MS", "how long to run (default 2000)", &RunConfig::duration_ms,
     FlagScope::kCore},
    {"--ring N", "entries per completion ring, power of two (default 1024)",
     &RunConfig::ring_capacity, FlagScope::kRing},
    {"--size B", "arena bytes per ring slot = max payload size (default 1024)",
     &RunConfig::payload_bytes, FlagScope::kRing},
    {"--backoff NS", "relax the poll loop when idle, 0 = pure spin (default 0)",
     &RunConfig::idle_backoff_ns, FlagScope::kRing},
};

bool in_scope(FlagScope flag, FlagScope wanted) {
    return flag == FlagScope::kCore || wanted == FlagScope::kRing;
}

/// True when `a` is this flag, i.e. it equals `usage` up to the placeholder.
bool flag_matches(const char* a, const char* usage) {
    const size_t n = std::strcspn(usage, " ");
    return std::strncmp(a, usage, n) == 0 && a[n] == '\0';
}

}  // namespace

void print_common_usage(FlagScope scope) {
    for (const U32Flag& f : kU32Flags) {
        if (in_scope(f.scope, scope)) std::printf("  %-*s%s\n", kUsageWidth, f.usage, f.help);
    }
    if (scope == FlagScope::kRing) {
        std::printf("  %-*s%s\n", kUsageWidth, "--copy-timing",
                    "time 1 in 32 H2D flushes (CUDA); throttles near-saturated runs");
    }
    std::printf("  %-*s%s\n", kUsageWidth, "--verbose", "print device and allocation details");
    std::printf("  --help\n");
}

bool take_u64(int argc, char** argv, int& i, uint64_t& out) {
    if (i + 1 >= argc) return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(argv[++i], &end, 10);
    if (end == argv[i] || *end != '\0') return false;
    out = v;
    return true;
}

FlagResult parse_common_flag(int argc, char** argv, int& i, RunConfig& cfg, FlagScope scope) {
    const char* a = argv[i];
    uint64_t v = 0;
    bool ok = true;

    if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
        return FlagResult::kHelp;
    } else if (!std::strcmp(a, "--verbose")) {
        cfg.verbose = true;
    } else if (scope == FlagScope::kRing && !std::strcmp(a, "--copy-timing")) {
        cfg.copy_timing = true;
    } else {
        const U32Flag* flag = nullptr;
        for (const U32Flag& f : kU32Flags) {
            if (in_scope(f.scope, scope) && flag_matches(a, f.usage)) { flag = &f; break; }
        }
        if (!flag) return FlagResult::kNotMine;

        ok = take_u64(argc, argv, i, v) && v <= UINT32_MAX;
        cfg.*(flag->field) = static_cast<uint32_t>(v);
    }

    if (!ok) {
        std::fprintf(stderr, "[gnp] bad or missing value for %s\n", a);
        return FlagResult::kBad;
    }
    return FlagResult::kOk;
}

}  // namespace gnp
