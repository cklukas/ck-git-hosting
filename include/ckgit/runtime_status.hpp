// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ckgit {

// The identity a running daemon records at startup, so the dashboard's About
// dialog and `ckgit version` can report the version of each *running* process
// (and notice one left on an older build by an install that did not restart
// it). Every daemon in the suite is built from one buildVersion(), so a healthy
// deployment reports the same version for all of them.
struct RuntimeComponent {
  std::string name;   // program name, e.g. "ck-git-hostingd"
  std::string version;  // buildVersion() captured at startup
  long pid = 0;         // recording process id (0 when unknown)
  std::uint64_t started_epoch_seconds = 0;
  bool running = false;  // the recorded pid is still alive (best-effort)
};

// Records this process under <state_root>/runtime/<name>. Best-effort: every
// failure (a missing or read-only state root) is swallowed so recording a
// version can never keep a daemon from serving. A non-empty state_root and a
// name without path separators are required; otherwise it does nothing.
void recordRuntimeComponent(const std::filesystem::path& state_root,
                            const std::string& name, const std::string& version);

// Reads every component recorded under <state_root>/runtime, in a stable suite
// order (hosting, runner, pages, then any others alphabetically). A missing
// directory yields an empty list. Never throws.
std::vector<RuntimeComponent> readRuntimeComponents(const std::filesystem::path& state_root);

}  // namespace ckgit
