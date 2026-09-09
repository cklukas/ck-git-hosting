// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ckgit {

inline constexpr std::size_t kMaximumMarkdownInputBytes = 512 * 1024;
inline constexpr std::size_t kMaximumMarkdownOutputBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaximumMarkdownDepth = 8;

struct LinkContext {
  std::string project;
  std::string commit_id;
  std::string directory;
  // Ordinary document links retain the selected branch/tag; image URLs use
  // commit_id so all images belong to the snapshot read for this page.
  std::string ref{};
};

// A bounded CommonMark subset. Raw HTML is always escaped; relative links
// resolve inside the repository and images use the immutable raw route.
// Exceeding a size or work bound throws std::length_error; no partial page is
// returned. Deeper nesting becomes escaped text rather than further recursion.
std::string renderMarkdown(std::string_view source, const LinkContext& context);

}  // namespace ckgit
