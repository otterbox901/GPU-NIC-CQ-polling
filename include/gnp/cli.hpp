#pragma once
//
// gnp/cli.hpp - command-line flags shared by the driver binaries.
//

#include "gnp/common.hpp"

namespace gnp {

/// Which flags a binary should accept.
///
/// The two receive paths do not share a data plane, so they do not share a
/// full flag set either. `kRing` adds the flags that only mean something where
/// a completion ring and a payload arena exist (--ring, --size, --backoff,
/// --copy-timing); on the GPU-direct path the NIC owns the queue and those
/// would be lies.
enum class FlagScope { kCore, kRing };

/// Print the help lines for the flags parse_common_flag() accepts at `scope`.
void print_common_usage(FlagScope scope);

/// Parse the value after argv[i] as an unsigned integer, advancing i.
/// Returns false if it is missing or unparseable.
bool take_u64(int argc, char** argv, int& i, uint64_t& out);

enum class FlagResult { kNotMine, kOk, kBad, kHelp };

/// Try argv[i] as one of the flags available at `scope`. On kOk, i has been
/// advanced past any value.
FlagResult parse_common_flag(int argc, char** argv, int& i, RunConfig& cfg, FlagScope scope);

}  // namespace gnp
