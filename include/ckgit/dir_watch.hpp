// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <filesystem>
#include <vector>

namespace ckgit {

// Waits for changes in a small set of directories without polling them: inotify
// on Linux, kqueue on macOS. It answers only "something changed in one of the
// watched directories" — the caller rescans what it cares about — which is all
// the CI spool needs: the store commits every job with a same-directory rename,
// so a new, claimed, or released job always shows up as a directory change.
//
// Notifications are an optimisation, never the source of truth. When the
// kernel facility is unavailable or a watch cannot be added, active() is false
// and wait() simply sleeps for its timeout, so a caller that also rescans on
// its own schedule stays correct either way. A queue overflow counts as a
// change.
class DirectoryWatch {
 public:
  DirectoryWatch();
  ~DirectoryWatch();
  DirectoryWatch(const DirectoryWatch&) = delete;
  DirectoryWatch& operator=(const DirectoryWatch&) = delete;

  // Watches `directory` for entries created, renamed in or out, written, or
  // deleted. Returns false (and leaves the watch inactive for that directory)
  // when it cannot be watched.
  bool add(const std::filesystem::path& directory);

  // True when at least one directory is watched by the kernel facility.
  bool active() const;

  // Blocks until a change is seen or `timeout` passes. Returns true for a
  // change; pending events are consumed, so several changes in a burst wake the
  // caller once. Returns false on timeout (and always sleeps the full timeout
  // when inactive).
  bool wait(std::chrono::milliseconds timeout);

 private:
  int descriptor_ = -1;
#if defined(__APPLE__)
  std::vector<int> watched_;
#endif
  bool active_ = false;
};

}  // namespace ckgit
