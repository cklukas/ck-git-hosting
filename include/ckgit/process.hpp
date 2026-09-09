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

struct ProcessOptions {
  std::chrono::milliseconds timeout{std::chrono::seconds(5)};
  std::size_t output_limit{256 * 1024};
  // When set, the child's standard error stays on this process's own standard
  // error (for Git's live transfer progress); only standard output is captured.
  bool inherit_stderr{false};
};

// Executes an argument vector directly, without a shell.  Output is combined
// and bounded so a malformed repository cannot make an audit consume memory
// without limit.  The child runs in its own session with standard input from
// /dev/null: it can never wait on a terminal prompt, and a timeout kills the
// whole process tree (for example git together with its ssh transport) so
// nothing keeps running or keeps the caller waiting afterwards.
ProcessResult runProcess(const std::vector<std::string>& arguments, const ProcessOptions& options);

// Because captured children live in their own session, a signal that ends
// this process would leave them running.  A command-line client calls this
// once at startup: SIGINT, SIGTERM, SIGHUP, and SIGPIPE then terminate every
// active child tree before the signal's default action ends the client.
void installChildTerminationForwarding();
ProcessResult runProcess(const std::vector<std::string>& arguments,
                         std::chrono::milliseconds timeout =
                             std::chrono::seconds(5),
                         std::size_t output_limit = 256 * 1024);

// Starts a long-running child that shares this process's terminal, again
// without a shell.  The caller owns the returned process ID and must reap it.
int spawnAttachedProcess(const std::vector<std::string>& arguments);

}  // namespace ckgit
