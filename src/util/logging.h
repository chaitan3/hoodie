#pragma once

#include <string>

namespace hoodie::util {

// Initialise the process-wide logger. `level` is one of: trace, debug, info,
// warn, error, critical, off (case-insensitive). Unknown values fall back to
// info. Safe to call once at startup.
void init_logging(const std::string& level = "info");

}  // namespace hoodie::util
