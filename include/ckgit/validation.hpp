// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ckgit {

// First-release repository names are deliberately a narrow, portable subset.
// The returned string is empty when the value is valid.
std::string projectNameError(std::string_view name);

bool isValidProjectName(std::string_view name);
bool isValidClientId(std::string_view client_id);
bool isValidBranchName(std::string_view branch);
// A full Git object name: 40 (SHA-1) or 64 (SHA-256) lowercase hex digits.
bool isValidObjectId(std::string_view object_id);
// A SHA-256 digest as 64 lowercase hex digits.
bool isValidSha256Hex(std::string_view digest);

// A path that is safe to resolve inside a checkout or a site: relative,
// within the tree, no control bytes or backslashes. A single trailing '/'
// (a directory) is accepted; empty, absolute, '.'/'..' or empty components,
// and more than kMaximumSafeRelativePathBytes are not. Shared by CI
// workflows (artifact and pages paths) and ckdocs configuration.
inline constexpr std::size_t kMaximumSafeRelativePathBytes = 1024;
bool isSafeRelativePath(std::string_view path);

}  // namespace ckgit
