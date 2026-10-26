// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <string>
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
}
