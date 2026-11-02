// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <functional>
#include <memory>

namespace ckgit {
// Call only after VM workspace preflight and while holding the build-root lock.
// Removes only recognized private runner-owned relay directories; unknown data
// or a changed mount is preserved and causes failure.
void reconcileMacosVmRelays(const std::filesystem::path& root);
// Held for the entire runner lifetime, including queue polling and relay
// cleanup. The persistent lock file is not a VM image or job workspace.
class MacosVmRunnerLock {
 public:
  explicit MacosVmRunnerLock(const std::filesystem::path& root);
  ~MacosVmRunnerLock();
  MacosVmRunnerLock(const MacosVmRunnerLock&) = delete;
  MacosVmRunnerLock& operator=(const MacosVmRunnerLock&) = delete;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// A private, single-runner workspace containing APFS clones of a prepared VM.
// The owner must keep this object alive while its VM can run. Destruction does
// not remove it: an interrupted process is not evidence that the guest stopped.
class MacosVmWorkspace {
 public:
  MacosVmWorkspace(const std::filesystem::path& base, const std::filesystem::path& root, unsigned min_free_gib);
  ~MacosVmWorkspace();
  MacosVmWorkspace(const MacosVmWorkspace&) = delete;
  MacosVmWorkspace& operator=(const MacosVmWorkspace&) = delete;
  std::filesystem::path bundle() const;
  std::filesystem::path exchange() const;
  // The caller must first prove that the VM and its host process have stopped.
  // Refuses links/mounts/unknown markers and throws on any incomplete cleanup.
  // The optional progress callback must not throw; cleanup must finish even
  // if lease renewal fails.
  void removeAfterConfirmedStop(const std::function<void()>& on_progress = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
