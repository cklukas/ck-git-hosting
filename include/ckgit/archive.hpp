// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ckgit {
struct TarPackOptions {
  std::uintmax_t max_bytes = 0;
  std::size_t max_entries = 100000;
  std::chrono::seconds timeout{300};
  std::function<bool()> should_continue;
  // Pages publishes regular content only; omit links and special nodes.
  // The source root and every traversed ancestor must still be real directories.
  bool skip_non_regular = false;
};
// Uncompressed PAX TAR containing only directories and regular files.
// The caller owns a quiescent source tree and an output outside that tree.
// No links, reparse points, external filters or subprocesses are used.
// Files are streamed from validated handles; failure removes partial output.
void packRegularTreeTar(const std::filesystem::path& root,
                        const std::vector<std::string>& paths,
                        const std::filesystem::path& output, const TarPackOptions& options);

struct TarExtractOptions {
  std::uintmax_t max_archive_bytes = 0;
  std::uintmax_t max_bytes = 0;
  std::uintmax_t max_file_bytes = 0;
  std::size_t max_entries = 100000;
  std::chrono::seconds timeout{120};
  std::function<bool()> should_continue;
  bool skip_non_regular = false;
};
// Extract regular content from an uncompressed TAR into a new private directory.
// The caller owns the input and destination parent. Existing destinations are
// refused; failure removes the new tree. Never creates links or restores owners,
// device nodes, permissions, extended attributes or other archive metadata.
void extractRegularTreeTar(const std::filesystem::path& input,
                           const std::filesystem::path& destination, const TarExtractOptions& options);
}
