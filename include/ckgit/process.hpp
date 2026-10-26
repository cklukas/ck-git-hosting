// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>
#include <filesystem>
#include <map>
#include <functional>

namespace ckgit {

// Resolve a program using the platform's process PATH rules, without running
// it. Returns an absolute UTF-8 path or throws when no executable is found.
// Pass the result to runProcess to test the exact executable being reported.
std::string resolveProcessExecutable(const std::string& name);

struct ProcessResult {
  int exit_code{-1};
  bool timed_out{false};
  bool cancelled{false};
  bool output_truncated{false};
  std::string output;
};

struct ProcessOptions {
  std::chrono::milliseconds timeout{std::chrono::seconds(5)};
  std::size_t output_limit{256 * 1024};
  // When set, the child's standard error stays on this process's own standard
  // error (for Git's live transfer progress); only standard output is captured.
  bool inherit_stderr{false};
  // Optional regular file supplied as stdin (e.g. an SSH upload payload).
  // Must be absolute and is opened before spawning; no shell or input pipe
  // can block the caller while it drains the child's output.
  std::optional<std::filesystem::path> input_file;
  // Per-child overrides, without changing this process's environment. nullopt
  // removes an inherited variable. Sandboxed jobs use their explicit allowlist.
  std::map<std::string, std::optional<std::string>> environment;
  // Synchronous callbacks on the caller's thread; keep them bounded. Output is
  // delivered in full even after the capture limit, and the view lasts only
  // for the call. should_continue also runs while the child produces no output.
  // Returning false kills the tree and sets cancelled. Callback exceptions
  // terminate the tree before propagating to the caller.
  std::function<void(std::string_view)> on_output;
  std::function<bool()> should_continue;
};

// Executes an argument vector directly, without a shell.  Output is combined
// and bounded so a malformed repository cannot make an audit consume memory
// without limit.  The child runs in its own session with standard input from
// /dev/null (NUL on Windows), or the explicitly supplied input file: it cannot
// wait on a terminal prompt. A timeout kills the
// whole process tree (a Windows Job Object, or a POSIX process group) so
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

// For a protocol shim: preserve stdin/stdout/stderr without copying bytes or
// invoking a shell. POSIX replaces the caller (returns 127 on exec failure).
// Windows runs a child with only those handles inherited and returns its exit
// status; a kill-on-close job prevents orphan descendants if the shim dies.
int execProcess(const std::vector<std::string>& arguments);

// Owns a long-running background child (e.g. an SSH tunnel), including its
// descendants. stdin is closed to interaction; stdout/stderr stay attached.
// The destructor stops the tree. Exit status is normalized, not a waitpid word.
class ManagedProcess {
 public:
  explicit ManagedProcess(const std::vector<std::string>& arguments);
  ~ManagedProcess();
  ManagedProcess(const ManagedProcess&) = delete;
  ManagedProcess& operator=(const ManagedProcess&) = delete;
  std::optional<int> poll();
  void stop() noexcept;
  unsigned long id() const;
 private:
  struct State;
  std::unique_ptr<State> state_;
};

// Nonblocking, single-caller stdin/stdout transport for a long-lived child.
// stderr remains attached. read/write return -1 on closure, 0 when no progress
// is possible, otherwise the number of bytes received/accepted. At most 64 KiB
// is queued internally on Windows. Acceptance is not a peer acknowledgement.
// Destroying the stream terminates its entire child tree.
class ProcessStream {
 public:
  explicit ProcessStream(const std::vector<std::string>& arguments);
  ~ProcessStream();
  ProcessStream(const ProcessStream&) = delete;
  ProcessStream& operator=(const ProcessStream&) = delete;
  std::ptrdiff_t read(char* buffer, std::size_t capacity);
  std::ptrdiff_t write(std::string_view bytes);
  bool closeInput();  // false while an accepted asynchronous write is pending
  std::optional<int> poll();
  void stop() noexcept;
 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace ckgit
