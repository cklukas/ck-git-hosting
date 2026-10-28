// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#ifdef _WIN32
#include "ckgit/process.hpp"
#include <filesystem>
#include <map>
#include <memory>

namespace ckgit {
// Owns one fresh profile and directory tree. Never edits ACLs on installed
// tools: the broker stages tools under tools() and then seals that copy.
class WindowsSandbox {
 public:
  explicit WindowsSandbox(const std::filesystem::path& parent);
  ~WindowsSandbox();
  WindowsSandbox(const WindowsSandbox&) = delete;
  WindowsSandbox& operator=(const WindowsSandbox&) = delete;
  std::filesystem::path work() const;
  std::filesystem::path tools() const;
  void sealTools(const std::function<bool()>& should_continue = {});
  void retainWorkspace();
  void* sid() const;
  std::vector<void*> capabilitySids() const;
  std::wstring desktopName() const;
 private:
  struct State;
  std::unique_ptr<State> state_;
};

// Call while holding the runner's build-root lock. Recovery considers only
// recorded directories of this user; live records remain exclusively locked.
class WindowsRunnerSession {
 public:
  explicit WindowsRunnerSession(const std::filesystem::path& build_root);
  ~WindowsRunnerSession();
  WindowsRunnerSession(const WindowsRunnerSession&) = delete;
  WindowsRunnerSession& operator=(const WindowsRunnerSession&) = delete;
  const std::filesystem::path& root() const;
 private:
  struct State;
  std::unique_ptr<State> state_;
};

// The caller supplies a host-approved, explicit environment and absolute exe.
// TEMP, TMP, TMPDIR and USERPROFILE are always replaced with per-job paths.
ProcessResult runWindowsSandboxProcess(const WindowsSandbox& sandbox,
    const std::vector<std::string>& arguments, const ProcessOptions& options,
    const std::map<std::wstring, std::wstring>& environment,
    const std::filesystem::path& working_subdirectory = {});
}
#endif
