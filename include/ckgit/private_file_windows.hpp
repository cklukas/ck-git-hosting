// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#ifdef _WIN32
#include <filesystem>
#include <string_view>
namespace ckgit {
// Same-directory staging, explicit owner/SYSTEM ACL, flush and atomic rename.
// Pins non-reparse parent directories through the rename. Existing targets
// must be regular files owned by the current user. No cross-volume copy.
void writePrivateWindowsFile(const std::filesystem::path& path, std::string_view content,
                             bool overwrite, bool create_parents);
}
#endif
