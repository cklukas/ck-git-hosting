// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#ifdef __APPLE__
#include <chrono>
#include <filesystem>
#include <functional>

namespace ckgit {
// Owns the helper and its private duplex socket. A true stop result means the
// helper exited successfully after requesting a framework VM stop; only then
// may the caller consider removing its disposable workspace. Failure or an
// interrupted owner leaves the workspace intact for conservative recovery.
class MacosVmProcess {
 public:
  MacosVmProcess(const std::filesystem::path& helper, const std::filesystem::path& bundle,
                 const std::filesystem::path& exchange);
  ~MacosVmProcess();
  MacosVmProcess(const MacosVmProcess&) = delete;
  MacosVmProcess& operator=(const MacosVmProcess&) = delete;
  int socket() const { return socket_; }
  // The optional callback runs while waiting; it must not throw or block past
  // the caller's shutdown deadline.
  bool stop(std::chrono::milliseconds timeout, const std::function<void()>& on_wait = {});

 private:
  int socket_ = -1;
  int child_ = -1;
  bool confirmed_ = false;
  bool reaped_ = false;
};
}
#endif
