// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ckgit {

// One advisory lock per client configuration.  Scheduled and manual sync or
// publish runs that share a configuration cannot overlap; a second run fails
// fast instead of queueing behind an unknown amount of Git transfer.
class SyncLock {
 public:
  // Returns an engaged lock, or nullopt when another process holds it.  The
  // lock file is never deleted, which keeps the lock identity stable.
  static std::optional<SyncLock> tryAcquire(const std::filesystem::path& lock_path);

  ~SyncLock();
  SyncLock(SyncLock&& other) noexcept;
  SyncLock& operator=(SyncLock&& other) noexcept;
  SyncLock(const SyncLock&) = delete;
  SyncLock& operator=(const SyncLock&) = delete;

 private:
  explicit SyncLock(int descriptor);
  int descriptor_{-1};
};

// The private managed inventory records one absolute main-checkout path per
// project, independent of the path information reported to the server. Publish,
// clone, register, sync, and explicit selection all share this inventory. The
// canonical.ini filename and API names remain compatible with earlier explicit
// selections. The file is a strict, bounded `project=/absolute/path` list beside
// client.ini, written with user-only permissions; status and previews only read it.
std::map<std::string, std::filesystem::path> loadCanonicalCheckouts(
    const std::filesystem::path& file);

// Replaces one project's selection through a same-directory staged, fsynced,
// atomic write.  Every other selection in the file is preserved.
void saveCanonicalCheckout(const std::filesystem::path& file,
                           std::string_view project,
                           const std::filesystem::path& checkout);

// Atomically forgets one local selection, preserving all other projects. Does
// not inspect or delete the checkout. Returns false when already absent.
bool forgetCanonicalCheckout(const std::filesystem::path& file, std::string_view project);

// Heuristic used only to propose a canonical checkout: a final path component
// carrying a mktemp-style random suffix such as `ckmux-v013-gate.Jthl4i` is
// treated as ephemeral.  Parent directories are deliberately not judged, so a
// stable checkout beneath a scratch volume still qualifies.
bool looksEphemeralCheckoutPath(const std::filesystem::path& path);

}  // namespace ckgit
