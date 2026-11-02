// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <filesystem>

namespace ckgit {
struct MacosVmStorageStatus {
  std::uintmax_t available = 0;
  std::uintmax_t image_maximum = 0;
  std::uintmax_t base_allocated = 0;
};
// Read-only inspection verifies storage identity and ownership, but does not
// authorize a job or guarantee that free space will remain available.
MacosVmStorageStatus inspectMacosVmStorage(const std::filesystem::path& base,
                                          const std::filesystem::path& work_root);
// Preflight only: an executor must revalidate and hold directory handles when
// actually creating or removing VM files. This function does not start a VM.
std::uintmax_t validateMacosVmStorage(const std::filesystem::path& base,
                                      const std::filesystem::path& work_root,
                                      unsigned min_free_gib);
}
