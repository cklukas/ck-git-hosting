// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "ckgit/process.hpp"

namespace ckgit {
inline constexpr std::size_t kMaximumPageBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaximumCommitParents = 128;
inline constexpr std::size_t kMaximumTreeDepth = 32;
inline constexpr std::size_t kMaximumTreeEntries = 5000;
inline constexpr std::size_t kMaximumPreviewBytes = 512 * 1024;
inline constexpr std::size_t kMaximumRawBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaximumInlineImageBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaximumDiffBytes = 512 * 1024;
inline constexpr std::size_t kCommitPageSize = 50;
inline constexpr std::size_t kMaximumDayCommits = 200;
class WebError : public std::runtime_error {
 public:
  WebError(int status, const std::string& message) : std::runtime_error(message), status(status) {}
  int status;
};
// A complete sidebar may be too large while individual directories remain
// browsable. Callers can fall back without hiding unrelated read failures.
class TreeLimitError : public WebError {
 public:
  explicit TreeLimitError(const std::string& message) : WebError(503, message) {}
};
struct WebCommit {
  std::string id, author, committer, subject, message;
  std::int64_t epoch{}, author_epoch{};
  std::vector<std::string> parents;
};
struct TreeEntry {
  std::string mode, type, id, name;
  std::uint64_t size{};
};
struct ResolvedRef { std::string id, name; bool ambiguous{false}; };
struct CommitPage { std::vector<WebCommit> commits; bool has_more{false}; };
struct ActivityData { std::map<std::string, std::size_t> counts; bool truncated{false}; };
// A request owns one reader and one absolute deadline for all its Git calls.
class WebRepository {
 public:
  explicit WebRepository(std::filesystem::path repository,
      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25));
  ResolvedRef resolve(const std::string& ref) const;
  std::vector<TreeEntry> tree(const std::string& id, const std::string& path) const;
  // One bounded recursive tree read; names are full repository paths. Sizes
  // are left at zero because sidebar rows do not need blob-size lookups.
  std::vector<TreeEntry> fileTree(const std::string& id) const;
  TreeEntry entry(const std::string& id, const std::string& path) const;
  std::string blob(const TreeEntry& entry, std::size_t limit) const;
  WebCommit commit(const std::string& id) const;
  CommitPage commits(const std::string& id, const std::string& cursor = {}) const;
  ActivityData activity(const std::string& id) const;
  CommitPage day(const std::string& id, const std::string& date) const;
  std::string asOf(const std::string& id, const std::string& date) const;
  ProcessResult diff(const std::string& id, bool stat) const;
 private:
  ProcessResult git(std::vector<std::string> arguments, std::size_t limit,
                    bool allow_failure = false, bool allow_truncation = false) const;
  std::filesystem::path repository_;
  std::chrono::steady_clock::time_point deadline_;
};
std::string utcDate(std::int64_t epoch);
std::string rawContentType(const std::string& path);
bool isImageType(const std::string& type);
bool isTextBlob(const std::string& content);
std::string chooseReadme(const std::vector<TreeEntry>& entries);
}  // namespace ckgit
