// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#ifdef _WIN32
#include "ckgit/process.hpp"
#include <filesystem>
#include <map>
#include <memory>

namespace ckgit {
// Host-side LPAC qualification backend. It is deliberately not advertised as
// a remote CI runner until scheduler, lease and artifact integration is ready.
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
  void sealTools();
  void* sid() const;
  std::vector<void*> capabilitySids() const;
  std::wstring desktopName() const;
 private:
  struct State;
  std::unique_ptr<State> state_;
};

// The caller supplies a host-approved, explicit environment and absolute exe.
// TEMP, TMP, TMPDIR and USERPROFILE are always replaced with per-job paths.
ProcessResult runWindowsSandboxProcess(const WindowsSandbox& sandbox,
    const std::vector<std::string>& arguments, const ProcessOptions& options,
    const std::map<std::wstring, std::wstring>& environment);
}
#endif
