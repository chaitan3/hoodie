#pragma once

#include <string>
#include <vector>

namespace hoodie::util {

struct ProcessResult {
  int exit_code = -1;
  std::string output;  // combined stdout + stderr
};

// Run an executable directly (no shell) with the given argv, capturing combined
// stdout/stderr. Never invokes /bin/sh, so arguments are not subject to shell
// interpretation.
ProcessResult run_process(const std::vector<std::string>& argv);

}  // namespace hoodie::util
