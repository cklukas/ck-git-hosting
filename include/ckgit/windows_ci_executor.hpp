// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#ifdef _WIN32
#include "ckgit/ci_runner.hpp"
#include <map>
namespace ckgit {
// Operator-controlled toolchain, copied into each job's read-only tools tree.
// Environment values may contain ${TOOLS}, replaced with that staged path.
// No environment from the broker is implicitly inherited by a job.
struct WindowsCiToolchain {
  std::filesystem::path root;
  std::filesystem::path launcher;
  std::vector<std::filesystem::path> binary_directories{"bin"};
  std::map<std::string, std::string> environment;
};
CiRunRecord executeWindowsCiJob(const CiPreparedJob& prepared, const CiExecutionOptions& options,
    const WindowsCiToolchain& toolchain, CiSourceProvider& source, CiResultSink& sink,
    CiSandboxReport* report = nullptr);
}
#endif
