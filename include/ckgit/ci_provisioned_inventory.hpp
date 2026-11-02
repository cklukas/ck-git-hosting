// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace ckgit {
struct CiProvisionedInventoryIdentity {
  std::string manifest_sha256;
  std::string lock_sha256;
  std::string platform;
  std::string architecture;
  std::string abi;
  std::size_t entries = 0;
};
// Verify a complete version-1 manifest and protected on-disk tree. Currently
// implemented only on macOS; other platforms fail closed. This standalone
// verifier is not wired to CI admission and does not establish approval.
CiProvisionedInventoryIdentity verifyCiProvisionedInventory(
    const std::filesystem::path& root, std::string_view manifest,
    std::string_view expected_manifest_sha256, std::string_view expected_lock_sha256,
    std::string_view platform, std::string_view architecture, std::string_view abi);
}  // namespace ckgit
