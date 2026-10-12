// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ckgit/ci_store.hpp"
#include "ckgit/ci_workflow.hpp"

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

  // The queued job's id, when this run came from the spool: the run reuses it
  // as its run_id so it becomes the same run the dashboard already published as
  // Pending (see enqueueCiJob), rather than a second, disconnected record.
  // Empty generates a fresh id instead (the one-shot `run` command, and tests).
  std::string run_id;

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

  // Renews the runner's lease on the job (see renewCiLease), called about every
  // keep_alive_seconds while the run is in progress -- from the step heartbeat
  // and between setup phases. Returning false means the lease was lost: the
  // job has been requeued for another runner, so the run stops at once, kills
  // its step, and writes nothing further. Empty when there is no lease to keep
  // (the one-shot `run` command, and tests).
  std::function<bool()> keep_alive;
  unsigned keep_alive_seconds = 15;
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
// is the host's). When the mount namespace is available, the runner also
// covers the service tree — the state root, the CI build root, the cache
// root, Pages, and the bare-repository root — with a private tmpfs at each
// real path, so a step can see and write only its own checkout and its own
// declared cache; filesystem_masked reports whether that mounting actually
// took effect (a kernel or LSM could silently refuse it), and like
// loopback_available is meaningless (left false) when no mount namespace was
// created at all.
struct CiSandboxReport {
  bool probed = false;  // false when the run ended before anything executed (skipped, cancelled)
  bool namespaces_available = false;
  bool network_isolated = false;
  bool loopback_available = false;
  bool filesystem_masked = false;
};

// --- preparing a job (always on the main host) ------------------------------

// A sister project as the main host resolved it for one run: the project name
// and the exact commit every attempt of the run builds against, wherever it
// executes.
struct CiResolvedSister {
  std::string name;
  std::string commit;
};

// A queued job as the main host has prepared it for whichever runner executes
// it: the workflow read from the commit, the trigger decision already made, and
// sister refs already resolved to commits. A runner needs nothing else from the
// repositories than the commits named here.
struct CiPreparedJob {
  CiJobRequest job;                       // job_id doubles as the run id
  std::string workflow_text;              // the commit's .ckgit/ci.yml
  CiWorkflow workflow;                    // parsed from workflow_text
  bool is_release = false;                // a tag build that publishes a release
  std::string release_tag;
  bool is_default_branch = false;         // only such a build may publish Pages
  std::vector<CiResolvedSister> sisters;  // in workflow order
};

// Either a prepared job, or the terminal outcome a job has without running
// anything: Skipped (no workflow, or the ref is not a trigger) or Error (an
// unparsable workflow, an unknown sister).
struct CiPreparation {
  std::optional<CiPreparedJob> prepared;
  CiRunStatus status = CiRunStatus::Pending;
  std::string detail;
};

// Prepares `job` from the bare repository `repository`; sisters are looked up
// as siblings of it (<repository's parent>/<name>.git).
CiPreparation prepareCiJob(const std::filesystem::path& repository, const CiJobRequest& job);

// --- executing a job (on any runner) ----------------------------------------

// Where a run's source comes from. The primary reads the server's own bare
// repositories; a remote runner fetches through the server.
class CiSourceProvider {
 public:
  virtual ~CiSourceProvider() = default;
  // Materialises `commit` of `project` into the empty directory `destination`.
  // Throws std::runtime_error with a short reason when it cannot.
  virtual void checkout(const std::string& project, const std::string& commit,
                        const std::filesystem::path& destination) = 0;
};

// Where a run's progress and results go. The primary writes the server's state
// root directly; a remote runner relays everything to the server. The executor
// calls these in the order the dashboard relies on: runStarted, then for each
// step stepStarted, appendLog..., stepFinished, and finally finish.
class CiResultSink {
 public:
  virtual ~CiResultSink() = default;
  // The run is about to set up: publish it as Running.
  virtual void runStarted(const CiRunRecord& record) = 0;
  // Step `index` (== record.steps.size()) starts; its log begins empty.
  virtual void stepStarted(const CiRunRecord& record, std::size_t index) = 0;
  // More output of step `index`, already capped by the executor.
  virtual void appendLog(std::size_t index, std::string_view bytes) = 0;
  // Step `index` ended; its result is record.steps.back().
  virtual void stepFinished(const CiRunRecord& record, std::size_t index) = 0;
  // Called about once a second while a step runs and between setup phases.
  virtual void heartbeat(const CiRunRecord& record) = 0;
  // Called every few hundred milliseconds while a step runs (log flushing).
  virtual void tick() {}
  // The run's detail line changed while it is still running (e.g. uploads).
  virtual void progress(const CiRunRecord& record) = 0;
  virtual bool cancelRequested() = 0;
  // True once the runner's lease on the job is gone: stop and write nothing.
  virtual bool leaseLost() = 0;
  // One job's declared artifact, packed at `bundle` (or not, with `note` set).
  // Returns the record as stored (sizes, checksum, expiry, or a note).
  virtual CiArtifactRecord putArtifact(const CiArtifact& artifact, const std::filesystem::path& bundle,
                                       const std::string& note, bool release) = 0;
  // The Pages site directory of a successful default-branch build.
  virtual void putPagesSite(const std::filesystem::path& site) = 0;
  // Commits everything staged according to `record.status` and writes the
  // terminal record; may turn a success into an error (a site that failed to
  // publish). Returns the record as finally stored.
  virtual CiRunRecord finish(CiRunRecord record) = 0;
};

// The runner-side knobs of an execution.
struct CiExecutionOptions {
  std::filesystem::path build_root;
  unsigned timeout_seconds = 1800;
  std::size_t max_log_bytes = 1u << 20;
  bool allow_network = false;
  bool keep_scratch = false;
  std::size_t artifact_max_bytes = static_cast<std::size_t>(256) << 20;
  std::filesystem::path cache_root;
  std::vector<std::string> extra_env;
  // Absolute paths of the service tree a step must never see (masked with an
  // empty tmpfs inside its mount namespace); the build root's other runs and
  // the cache root's other projects are always added.
  std::vector<std::filesystem::path> hidden_paths;
};

// Executes a prepared job: checkout, sisters, caches, steps in the sandbox,
// artifacts, Pages, and the terminal record through `sink`. Returns the record
// as the sink stored it. Does not throw for a job outcome.
CiRunRecord executeCiJob(const CiPreparedJob& prepared, const CiExecutionOptions& options,
                         CiSourceProvider& source, CiResultSink& sink, CiSandboxReport* sandbox = nullptr);

// --- the primary's direct path ----------------------------------------------

// Reads source straight from the server's bare repositories: `repository` for
// the project itself, its siblings for sisters.
class LocalCiSource : public CiSourceProvider {
 public:
  LocalCiSource(std::filesystem::path repository, std::string project_name);
  void checkout(const std::string& project, const std::string& commit,
                const std::filesystem::path& destination) override;

 private:
  std::filesystem::path repository_;
  std::string project_name_;
};

// Writes progress and results straight into the server's state root, Pages
// root, and release store, using the options' policy (retention, Pages
// versions) and lease keeping.
class LocalCiSink : public CiResultSink {
 public:
  LocalCiSink(const CiRunnerOptions& options, const CiPreparedJob& prepared);
  ~LocalCiSink() override;
  void runStarted(const CiRunRecord& record) override;
  void stepStarted(const CiRunRecord& record, std::size_t index) override;
  void appendLog(std::size_t index, std::string_view bytes) override;
  void stepFinished(const CiRunRecord& record, std::size_t index) override;
  void heartbeat(const CiRunRecord& record) override;
  void progress(const CiRunRecord& record) override;
  bool cancelRequested() override;
  bool leaseLost() override;
  CiArtifactRecord putArtifact(const CiArtifact& artifact, const std::filesystem::path& bundle,
                               const std::string& note, bool release) override;
  void putPagesSite(const std::filesystem::path& site) override;
  CiRunRecord finish(CiRunRecord record) override;

 private:
  void write(const CiRunRecord& record, bool force);
  void keepAlive(bool force);

  const CiRunnerOptions& options_;
  const CiPreparedJob& prepared_;
  int log_fd_ = -1;
  std::uint64_t last_write_ = 0;
  std::uint64_t last_keep_alive_ = 0;
  bool lease_lost_ = false;
  std::optional<std::filesystem::path> site_;
};

// Runs the workflow found at .ckgit/ci.yml in `commit` and records the result
// under the state root (a run.ini plus steps/<n>.log): prepareCiJob, then
// executeCiJob with the local source and sink. A missing workflow or a branch
// the workflow does not list is recorded as Skipped; an unparsable workflow or
// a failed checkout is recorded as Error; a step exiting non-zero is Failure
// and a step exceeding its budget is Timeout (both stop the run). The returned
// record is the same one written to disk. This function does not throw for a
// job outcome — only for an options/state-root problem it cannot record.
CiRunRecord runCiWorkflow(const CiRunnerOptions& options, CiSandboxReport* sandbox = nullptr);

// Writes the terminal record of a job that ends without running (a prepared
// Skipped or Error outcome, or a cancel before it started), and clears any
// cancel marker. Returns the record written.
CiRunRecord recordCiOutcome(const std::filesystem::path& state_root, const CiJobRequest& job, CiRunStatus status,
                            const std::string& detail);

// Whether this build was compiled with the Linux namespace sandbox. False on
// macOS and other non-Linux hosts; a caller/test can use it to scope
// network-isolation expectations.
bool ciSandboxCompiledIn();

}  // namespace ckgit
