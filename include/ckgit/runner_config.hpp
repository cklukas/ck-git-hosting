// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <map>
#include <vector>
namespace ckgit {
// /etc/ck-git-hosting/runner.ini, strict key=value like server.ini:
//
//   schema_version=1
//   server=ckgit@rpi4                # SSH destination of the main host
//   name=uconsole                    # this runner's client id (its key's)
//   ci_build_root=/var/lib/ck-git-runner/ci-build
//   identity=/var/lib/ck-git-runner/.ssh/id_ed25519   # optional
//   known_hosts=/var/lib/ck-git-runner/.ssh/known_hosts  # optional
//   ssh_program=/usr/bin/ssh         # optional
//   claim_wait_seconds=60            # optional long-poll length (1..120)
//   ci_cache_root=...                # optional; unset keeps the runner clean
//   ci_allow_network=false           # optional; may only tighten the server
//   ci_timeout_seconds=...           # optional; may only tighten the server
//   ci_allow_degraded=false          # optional; run without a sandbox?
struct RunnerConfig {
  std::string server;
  std::string name;
  std::filesystem::path build_root;
  std::optional<std::filesystem::path> identity;
  std::optional<std::filesystem::path> known_hosts;
#ifdef _WIN32
  std::filesystem::path ssh_program{"ssh.exe"};
#else
  std::filesystem::path ssh_program{"/usr/bin/ssh"};
#endif
  unsigned claim_wait_seconds = 60;
  std::optional<std::filesystem::path> cache_root;
  bool allow_network = true;  // true = whatever the server allows
  std::optional<unsigned> timeout_seconds;
  bool allow_degraded = false;
  // Windows: operator-prepared tool tree, copied read-only into each LPAC job.
  std::optional<std::filesystem::path> toolchain_root;
  std::vector<std::filesystem::path> toolchain_paths{"bin"};
  std::map<std::string, std::string> toolchain_environment;
};

RunnerConfig loadRunnerConfig(const std::filesystem::path& path);
std::string renderRunnerConfig(const RunnerConfig& config);

}  // namespace ckgit
