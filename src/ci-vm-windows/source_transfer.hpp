// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "ckgit/ci_vm_protocol.hpp"
namespace ckgit {
inline constexpr std::uint64_t kWindowsVmSourceBytes = 2ull << 30;
inline constexpr std::uint64_t kWindowsVmTotalSourceBytes = 4ull << 30;
// Fixed ordinal order derived from the host's assignment; no guest-selected
// host path or checkout request is used. Archives are byte streams, not shares.
void sendWindowsVmSources(CiVmChannel&,const CiPreparedJob&,const std::vector<std::filesystem::path>&,
                          const std::function<bool()>& keep = {});
std::vector<std::filesystem::path> receiveWindowsVmSources(CiVmChannel&,const CiPreparedJob&,
                          const std::filesystem::path& empty_root,const std::function<bool()>& keep = {});
}
