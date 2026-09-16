// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "ckgit/project_summary.hpp"

namespace ckgit {

inline constexpr std::size_t kProjectIndexCommitLimit = 200000;
inline constexpr std::size_t kProjectIndexRefLimit = 500;
inline constexpr std::size_t kProjectIndexReadmeLimit = 512 * 1024;
inline constexpr std::size_t kProjectIndexCiLogLimit = 1u << 20;

// In-memory snapshots. Construction only inventories the root and creates
// placeholders: start() performs the Git work on one background thread. All
// HTTP readers only copy already-published records, never run Git or write.
class ProjectIndex {
 public:
  explicit ProjectIndex(
      const std::filesystem::path& repo_root,
      const std::optional<std::filesystem::path>& state_root = std::nullopt);
  ~ProjectIndex();
  ProjectIndex(const ProjectIndex&) = delete;
  ProjectIndex& operator=(const ProjectIndex&) = delete;

  void start();
  void stop();
  // Deduplicated, asynchronous, and deliberately forced even when refs have
  // not changed: registration and event updates also use this operation.
  void refresh(std::string_view project_name);
  // Publishes checkout/event changes immediately, with no Git subprocesses.
  // Read failures preserve metadata and appear in index_error, so refreshing
  // a derived cache cannot fail an already-successful registration RPC.
  void refreshMetadata(std::string_view project_name);
  // Synchronous sweep for administration and tests. Background sweeps run
  // every 60 seconds; unchanged fingerprints avoid expensive rebuilding.
  void sweep();
  void erase(std::string_view project_name);
  std::vector<ProjectSummary> snapshot() const;
  // Table rows omit the potentially large README, activity, refs and event
  // collections. Full snapshots and find() retain every indexed field.
  std::vector<ProjectSummary> tableSnapshot() const;
  std::optional<ProjectSummary> find(std::string_view project_name) const;
  // Reads one CI step log on demand (bounded), for the read-only log view. It
  // touches only the private state root and never runs Git.
  std::optional<std::string> readCiLog(std::string_view project_name, std::string_view run_id,
                                       std::size_t step_index) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ckgit
