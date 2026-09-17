// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ckgit/ci_store.hpp"

namespace ckgit {

// What a single run needs to know. The runner never trusts the working tree of
// any repository: it reads the workflow and the file set from the pushed commit
// object, so a push cannot influence a run through anything but its own content.
struct CiRunnerOptions {
  std::filesystem::path repository;      // the bare git directory to read the commit from
  std::string project_name;              // validated project id (record + run directory)
  std::string ref;                       // e.g. refs/heads/main (branch-trigger + record)
  std::string commit_id;                 // the commit to check out and run (40/64 hex)
  std::filesystem::path state_root;      // where ci/runs/... records and logs are written
  std::filesystem::path build_root;      // scratch parent; a per-run subtree is created here

  unsigned timeout_seconds = 1800;       // per-step wall-clock budget
  std::size_t max_log_bytes = 1u << 20;  // per-step captured-output cap
  bool allow_network = false;            // Linux: keep a network namespace? default: deny
  bool keep_scratch = false;             // leave the checkout in place for debugging

  // A job's declared artifacts are packed after its steps succeed. Retention
  // defaults to artifact_retention_days when the workflow does not set one, and
  // any value is clamped to artifact_max_retention_days. A bundle over
  // artifact_max_bytes is dropped and recorded with a note.
  unsigned artifact_retention_days = 7;
  unsigned artifact_max_retention_days = 90;
  std::size_t artifact_max_bytes = static_cast<std::size_t>(256) << 20;

  // Pages hosting. When pages_root is set and a workflow declares `pages:`, a
  // successful default-branch build publishes that directory as the project's
  // site, keeping pages_keep_versions versions for rollback. Empty disables it.
  std::filesystem::path pages_root;
  unsigned pages_keep_versions = 3;

  // Persistent build caches. When cache_root is set, each cache a workflow
  // declares gets a directory under cache_root/<project>/ that survives across
  // runs (a warm ccache store, say); empty makes declared caches ephemeral
  // (created under the scratch), so a workflow runs either way. The directory
  // is owned by the runner account, the one place besides the scratch a step
  // can write, and is exported as CKGIT_CACHE_<NAME> plus any bound variables.
  std::filesystem::path cache_root;

  // Extra environment exported to every step, as KEY=VALUE. Used to expose the
  // sibling checkouts (e.g. CWORKS_CKVISION_DIR) a suite build needs. Values
  // are literal; the runner performs no substitution.
  std::vector<std::string> extra_env;

  // Called once, after the initial Running record is written and before the
  // first step. The serving daemon uses it to refresh its dashboard cache so a
  // new run appears promptly rather than at the next periodic sweep. Empty by
  // default (the one-shot `run` command needs no notification).
  std::function<void()> on_run_started;
};

// Isolation the runner applied to a run. On Linux the runner confines each step
// in unprivileged user, network and mount namespaces (so a step has no network
// unless allow_network is set, and cannot see the host mount table); rlimits and
// the wall-clock timeout apply on every platform. Where namespaces are
// unavailable (macOS development hosts, or a kernel without unprivileged user
// namespaces) the runner still applies rlimits and the timeout, and reports that
// the network was not isolated. When an isolated network namespace is created,
// the runner also brings its loopback interface up so 127.0.0.1/::1 stays
// reachable (a step's own local server or test fixture) while the LAN remains
// denied; loopback_available reports whether that succeeded, and is true
// unconditionally when no isolated namespace was created (loopback then simply
// is the host's).
struct CiSandboxReport {
  bool namespaces_available = false;
  bool network_isolated = false;
  bool loopback_available = false;
};

// Runs the workflow found at .ckgit/ci.yml in `commit` and records the result
// under the state root (a run.ini plus steps/<n>.log). A missing workflow or a
// branch the workflow does not list is recorded as Skipped; an unparsable
// workflow or a failed checkout is recorded as Error; a step exiting non-zero is
// Failure and a step exceeding its budget is Timeout (both stop the run). The
// returned record is the same one written to disk. This function does not throw
// for a job outcome — only for an options/state-root problem it cannot record.
CiRunRecord runCiWorkflow(const CiRunnerOptions& options, CiSandboxReport* sandbox = nullptr);

// Whether this build was compiled with the Linux namespace sandbox. False on
// macOS and other non-Linux hosts; a caller/test can use it to scope
// network-isolation expectations.
bool ciSandboxCompiledIn();

}  // namespace ckgit
