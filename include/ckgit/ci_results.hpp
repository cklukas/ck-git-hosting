// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ckgit/ci_runner.hpp"
#include "ckgit/ci_store.hpp"

namespace ckgit {

// Staged results: everything a run produces as output -- CI artifacts, release
// assets, a Pages site -- is first staged under the run's own directory on the
// main host,
//
//   <state_root>/ci/runs/<project>/<run_id>/incoming/<kind>-<name>.part
//   <state_root>/ci/runs/<project>/<run_id>/incoming/<kind>-<name>.ini
//
// and only committed into its real place when the run finishes
// (commitCiRun). A run that fails, is cancelled, or loses its runner leaves
// nothing half-published: its staging is discarded. The primary stages by
// copying a finished bundle; a remote runner uploads it in resumable chunks.

enum class CiUploadKind { kArtifact, kAsset, kPages };

std::string_view ciUploadKindName(CiUploadKind kind);
std::optional<CiUploadKind> ciUploadKindFromName(std::string_view name);

// The Pages site travels as one tar under this fixed upload name.
inline constexpr char kCiPagesUploadName[] = "site";
// A chunk of a resumable upload is at most this large.
inline constexpr std::size_t kMaximumCiUploadChunkBytes = 4u << 20;

struct CiStagedUpload {
  CiUploadKind kind = CiUploadKind::kArtifact;
  std::string name;
  std::uint64_t total = 0;  // announced size
  std::string sha256;       // announced checksum, 64 lowercase hex
  std::uint64_t held = 0;   // bytes received so far
};

// Appends `bytes` to a staged upload at `offset`. The first chunk (offset 0)
// creates it with its announced `total` and `sha256`; every later chunk must
// name the same total and checksum and start exactly where the held bytes end,
// so a retried chunk is never stored twice. `max_total` caps the announced
// size before any byte is stored. Returns the bytes now held; throws
// std::runtime_error for an invalid or conflicting request.
std::uint64_t appendCiStagedUpload(const std::filesystem::path& state_root, std::string_view project,
                                   std::string_view run_id, CiUploadKind kind, std::string_view name,
                                   std::uint64_t total, std::string_view sha256, std::uint64_t offset,
                                   std::string_view bytes, std::uint64_t max_total);

// Stages a whole local file at once (the primary's path).
void stageCiFile(const std::filesystem::path& state_root, std::string_view project, std::string_view run_id,
                 CiUploadKind kind, std::string_view name, const std::filesystem::path& file);

std::vector<CiStagedUpload> listCiStagedUploads(const std::filesystem::path& state_root, std::string_view project,
                                                std::string_view run_id);

// Removes a run's whole staging directory.
void discardCiStagedUploads(const std::filesystem::path& state_root, std::string_view project,
                            std::string_view run_id);

// The main host's policy for committing results.
struct CiCommitPolicy {
  std::filesystem::path state_root;
  std::filesystem::path repository;  // the project's bare repository (release notes)
  std::filesystem::path pages_root;  // empty: Pages hosting is off
  unsigned pages_keep_versions = 3;
  unsigned artifact_retention_days = 7;
  unsigned artifact_max_retention_days = 90;
  std::size_t artifact_max_bytes = static_cast<std::size_t>(256) << 20;
};

// Commits a finished run on the main host and writes its terminal record:
//   - each declared CI artifact (stored even when a later job failed, as
//     before: it was the output of a job whose steps all passed);
//   - the release assets and release record, only when the run succeeded --
//     otherwise any earlier release under the tag is removed, so a release is
//     always complete;
//   - the Pages site, only for a successful default-branch run, and only when
//     no newer run's site is already live (a slow or requeued older run never
//     rolls a site back). `site_dir` is the primary's site directory; without
//     it a staged site upload is unpacked and published instead.
// A staged upload that is incomplete or fails its checksum is recorded with a
// note instead of bytes. A site that fails to publish turns a success into an
// error. The staging is discarded afterwards and any cancel marker cleared.
// Returns the record as stored.
CiRunRecord commitCiRun(const CiCommitPolicy& policy, const CiPreparedJob& prepared, CiRunRecord record,
                        const std::vector<CiDeclaredOutput>& outputs,
                        const std::optional<std::filesystem::path>& site_dir = std::nullopt);

// Collect verified outputs from a completed distributed graph and publish them
// through the parent. Returns nullopt while any child is active. Idempotent
// after its durable completion marker; child leases never publish directly.
std::optional<CiRunRecord> finalizeCiWorkflowResults(const CiCommitPolicy& policy,
                                                    const CiPreparedJob& parent);

// Reconstruct the immutable parent definition for a completed child and attempt
// group publication. Legacy jobs are a no-op.
std::optional<CiRunRecord> finalizeCiParentForChild(const CiCommitPolicy& policy, const CiJobRequest& child);

// Retry interrupted parent publication before claiming more work. Each parent
// is independent; failures are returned for logging without blocking other refs.
std::vector<std::string> recoverCiWorkflowResults(const CiCommitPolicy& defaults,
                                                const std::filesystem::path& repository_root);

}  // namespace ckgit
