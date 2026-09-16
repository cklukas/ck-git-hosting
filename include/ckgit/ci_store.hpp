// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ckgit {

// On-disk state for on-server CI, kept beside the checkout metadata under the
// same private state root and with the same discipline: strict, fixed-schema
// records written through a same-directory staged, fsynced, atomic rename, in
// directories that must be private (0700) and owned by the daemon user.
//
//   <state_root>/ci/spool/<job_id>.ini      queued by the post-receive hook
//   <state_root>/ci/working/<job_id>.ini    claimed by the runner
//   <state_root>/ci/runs/<project>/<run_id>/run.ini
//   <state_root>/ci/runs/<project>/<run_id>/steps/<n>.log
//
// The hook writes to the spool; the runner claims from it and writes runs; the
// daemon dashboard only ever reads runs. No component in this file executes
// anything — it is storage only.

enum class CiRunStatus {
  Pending,
  Running,
  Success,
  Failure,   // a step exited non-zero
  Timeout,   // a step exceeded its wall-clock budget
  Error,     // the runner could not set the job up (bad workflow, checkout failed, …)
  Skipped,   // the pushed commit declared no workflow for this branch
};

std::string_view ciRunStatusName(CiRunStatus status);
std::optional<CiRunStatus> ciRunStatusFromName(std::string_view name);

// A job the hook queued for the runner: enough to check out the exact commit
// and attribute the run, and nothing the runner should instead read from the
// commit itself.
struct CiJobRequest {
  std::string job_id;
  std::string project_name;
  std::string ref;         // e.g. refs/heads/main
  std::string commit_id;   // the pushed new object id (40 or 64 hex)
  std::string client_id;
  std::uint64_t queued_epoch_seconds = 0;
};

struct CiStepResult {
  std::string name;              // the step's label, or empty
  int exit_code = 0;
  bool timed_out = false;
  bool output_truncated = false;
};

struct CiRunRecord {
  std::string run_id;
  std::string project_name;
  std::string ref;
  std::string commit_id;
  CiRunStatus status = CiRunStatus::Pending;
  std::uint64_t started_epoch_seconds = 0;
  std::uint64_t finished_epoch_seconds = 0;
  std::string detail;  // one short human-readable line (e.g. the failing step)
  std::vector<CiStepResult> steps;
};

// Bounds, public for tests and callers.
inline constexpr std::size_t kMaximumCiIdBytes = 64;
inline constexpr std::size_t kMaximumCiDetailBytes = 512;
inline constexpr std::size_t kMaximumCiRunRecordBytes = 64 * 1024;
inline constexpr std::size_t kMaximumCiSpoolJobs = 4096;

// True for a syntactically valid job/run id: a sortable, filesystem-safe token
// this module generates. Ids are never taken from client input.
bool isValidCiId(std::string_view id);

// Generates a fresh, chronologically sortable id (zero-padded microseconds plus
// a random suffix) suitable for a job or a run.
std::string generateCiId();

// --- hook side --------------------------------------------------------------

// Atomically queues a job under <state_root>/ci/spool. Fails closed if the
// state root is not private and daemon-owned.
void enqueueCiJob(const std::filesystem::path& state_root, const CiJobRequest& request);

// --- runner side ------------------------------------------------------------

// Claims the oldest queued job by atomically renaming it into ci/working, then
// returns it. Returns std::nullopt when the spool is empty. A malformed spool
// entry is moved aside (never executed) and skipped.
std::optional<CiJobRequest> claimNextCiJob(const std::filesystem::path& state_root);

// Removes a claimed job from ci/working once its run has been recorded.
void releaseCiJob(const std::filesystem::path& state_root, std::string_view job_id);

// Ensures <state_root>/ci/runs/<project>/<run_id>/steps exists (private) and
// returns the run directory, so the runner can stream step logs into it.
std::filesystem::path prepareCiRunDirectory(const std::filesystem::path& state_root,
                                            std::string_view project_name, std::string_view run_id);

// Atomically writes the run record (run.ini) into its run directory.
void writeCiRunRecord(const std::filesystem::path& state_root, const CiRunRecord& record);

// --- dashboard side ---------------------------------------------------------

// Loads up to `maximum` newest run records for a project, newest first. A
// missing runs directory means no runs; a malformed record is skipped.
std::vector<CiRunRecord> loadCiRuns(const std::filesystem::path& state_root,
                                    std::string_view project_name, std::size_t maximum = 16);

}  // namespace ckgit
