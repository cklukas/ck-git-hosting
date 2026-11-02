// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
namespace ckgit {
struct MacNativeResult {
  int exit_code = -1;
  bool cancelled = false;
  bool timed_out = false;
  bool truncated = false;
};
// A successful health response also proves startup reconciliation completed.
void macNativeReady(const std::filesystem::path& socket, const std::filesystem::path& build_root);
MacNativeResult runMacNative(const std::filesystem::path& socket,
    const std::filesystem::path& workspace, const std::vector<std::string>& argv,
    const std::vector<std::string>& environment, unsigned timeout_seconds,
    std::size_t log_limit, const std::function<void(std::string_view)>& output,
    const std::function<bool()>& cancel, const std::function<void()>& tick);
} // namespace ckgit
