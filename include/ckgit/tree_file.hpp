// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ckgit::platform {
enum class TreeFileStatus { Ok, Missing, NotRegular, TooLarge, Unreadable };
struct TreeFile {
  TreeFileStatus status = TreeFileStatus::Missing;
  std::uintmax_t size = 0;
  std::string content;
};
// Resolve each child relative to an opened directory handle. Reject links,
// junctions, special files and Windows alternate data streams. The caller may
// explicitly allow a link for the root they selected, never for its children.
TreeFile readTreeFile(const std::filesystem::path& root,
                     const std::vector<std::string>& components,
                     std::uintmax_t limit, bool read, bool follow_root);

struct TreeFileSink {
  // Called after opening and validating the file, before the first chunk.
  // Both callbacks refer to the same open handle, even if the path changes.
  std::function<void(std::uintmax_t)> begin;
  std::function<void(std::string_view)> chunk;
};
// Like readTreeFile, but bounded-memory streaming for artifact archives.
// content is empty; callbacks may throw to cancel, and handles are released.
// A read failure may follow delivered chunks: callers must discard that output.
TreeFile streamTreeFile(const std::filesystem::path& root,
                        const std::vector<std::string>& components,
                        std::uintmax_t limit, bool follow_root, const TreeFileSink& sink);
}
