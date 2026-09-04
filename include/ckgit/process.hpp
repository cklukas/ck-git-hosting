// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace ckgit {

struct ProcessResult {
  int exit_code{-1};
  bool timed_out{false};
  bool output_truncated{false};
  std::string output;
};

// Executes an argument vector directly, without a shell.  Output is combined
// and bounded so a malformed repository cannot make an audit consume memory
// without limit.
ProcessResult runProcess(const std::vector<std::string>& arguments,
                         std::chrono::milliseconds timeout =
                             std::chrono::seconds(5),
                         std::size_t output_limit = 256 * 1024);

}  // namespace ckgit
