// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ckgit {

struct ServerConfig {
  std::filesystem::path repo_root;
  std::filesystem::path control_socket;
  std::optional<std::filesystem::path> state_root;
  std::optional<std::filesystem::path> hook_directory;
  std::optional<unsigned short> http_port;
  // Public SSH destination used in copyable dashboard clone commands. It is
  // deliberately independent of the loopback HTTP listener or request Host.
  std::optional<std::string> ssh_clone_target{};

  // CI runner settings, read by ck-ci-runnerd (the daemon ignores them). The
  // build root is where each run's throwaway checkout is created; the rest tune
  // the per-step budgets and the spool poll cadence. Network is denied to steps
  // unless ci_allow_network is set.
  std::optional<std::filesystem::path> ci_build_root;
  std::optional<unsigned> ci_timeout_seconds;
  std::optional<unsigned long long> ci_max_log_bytes;
  std::optional<unsigned> ci_poll_seconds;
  bool ci_allow_network = false;

  // Artifact retention, enforced by the runner's periodic sweep. Days default
  // and cap bound how long an ephemeral CI artifact is kept; the byte budgets
  // trigger oldest-first eviction; runs_keep bounds how many run directories a
  // project retains; keep_latest protects each project's newest run's artifacts.
  std::optional<unsigned> ci_artifact_retention_days;
  std::optional<unsigned> ci_artifact_max_retention_days;
  std::optional<unsigned long long> ci_artifact_max_bytes;
  std::optional<unsigned long long> ci_artifact_max_project_bytes;
  std::optional<unsigned long long> ci_artifact_max_total_bytes;
  std::optional<unsigned> ci_runs_keep;
  std::optional<unsigned> ci_cleanup_interval_seconds;
  std::optional<bool> ci_artifact_keep_latest;

  // Pages hosting. The runner publishes sites under pages_root; the separate
  // ck-pagesd serves them on pages_http_port (LAN-exposable, a distinct origin
  // from the dashboard). pages_keep_versions bounds the rollback history.
  std::optional<std::filesystem::path> pages_root;
  std::optional<unsigned short> pages_http_port;
  std::optional<unsigned> pages_keep_versions;
};

// Loads the strict, bounded version-1 daemon configuration.  Every path must
// be absolute; unknown fields, duplicate fields, invalid UTF-8, and malformed
// values fail closed.  No path is opened here: the daemon validates each one
// when it starts serving, and `--check` reports the parsed values only.
ServerConfig loadServerConfig(const std::filesystem::path& path);

// Accepts a shell-safe user@host destination. SSH aliases can supply ports,
// IPv6 addresses, keys, and other transport settings in the user's SSH config.
bool isValidSshCloneTarget(std::string_view value);

// Renders the effective configuration in the same key=value form.
std::string renderServerConfig(const ServerConfig& config);

}  // namespace ckgit
