// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace ckgit {
bool isValidSshCloneTarget(std::string_view value);


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

// A CI runner label (`arm64`, `macos`, `big-disk`): 1 to 32 bytes of
// lowercase letters, digits, `.`, `_` and `-`, starting with a letter or
// digit. A runner's own name always counts as one of its labels.
inline constexpr std::size_t kMaximumCiLabelBytes = 32;
inline constexpr std::size_t kMaximumCiLabels = 16;
bool isValidCiLabel(std::string_view label);
// A comma-separated label list ("a,b", or empty for none): at most
// kMaximumCiLabels valid labels, duplicates removed, sorted. std::nullopt
// when any part is invalid.
std::optional<std::vector<std::string>> parseCiLabelList(std::string_view text);
std::string formatCiLabelList(const std::vector<std::string>& labels);
// Whether a runner with `labels` (plus its `name`) may run a job that asks
// for `runs_on`: every requested label must be among them. Runners registered
// with the windows label require an explicitly labelled job.
bool ciRunnerMatches(std::string_view name, const std::vector<std::string>& labels,
                     const std::vector<std::string>& runs_on);

}  // namespace ckgit
