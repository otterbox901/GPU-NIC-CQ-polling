#pragma once
//
// gnp/cli.hpp - command-line flags shared by every driver binary.
//

#include "gnp/common.hpp"

namespace gnp {

/// Print the help lines for the flags parse_common_flag() understands.
void print_common_usage();

/// Parse the value after argv[i] as an unsigned integer, advancing i.
/// Returns false if it is missing or unparseable.
bool take_u64(int argc, char** argv, int& i, uint64_t& out);

enum class FlagResult { kNotMine, kOk, kBad, kHelp };

/// Try argv[i] as one of --queues --ring --size --duration --backoff
/// --copy-timing --verbose --help. On kOk, i has been advanced past any value.
FlagResult parse_common_flag(int argc, char** argv, int& i, RunConfig& cfg);

}  // namespace gnp
