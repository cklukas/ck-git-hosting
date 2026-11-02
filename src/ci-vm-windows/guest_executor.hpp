// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "ckgit/ci_vm_protocol.hpp"
#include "ckgit/runner_config.hpp"
namespace ckgit {
// Linked only into the guest service, never the host runner. Called after an
// authenticated-to-this-VM Hyper-V channel delivers the bounded assignment.
CiRunRecord executeWindowsVmGuestJob(const CiVmAssignment&,const RunnerConfig& tool_profile,
 const std::vector<std::filesystem::path>& sources,const std::filesystem::path& scratch,CiResultSink& sink);
}
