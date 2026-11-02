// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "ckgit/ci_vm_protocol.hpp"

namespace ckgit {
struct CiMacosVmOptions {
  std::filesystem::path helper;
  std::filesystem::path base;
  std::filesystem::path work_root;
  std::filesystem::path relay_root;
  unsigned min_free_gib = 20;
};
// The caller must stop taking new work on any exception until VM workspace
// state has been checked. A lost lease must never be committed as a result.
CiRunRecord runCiMacosVmJob(const CiPreparedJob& assignment, const CiRemotePolicy& policy,
                            CiSourceProvider& source, CiResultSink& sink, const CiMacosVmOptions& options);
}
