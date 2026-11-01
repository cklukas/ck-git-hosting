// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <string_view>
namespace ckgit {
// Atomically create a new directory; the caller owns cleanup. Parent is always
// explicit so build tools can honour the selected development volume.
std::filesystem::path createTempDirectory(const std::filesystem::path& parent, std::string_view prefix);
}
