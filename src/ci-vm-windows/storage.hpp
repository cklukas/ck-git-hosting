// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "hcs_vm.hpp"
#include <memory>
namespace ckgit {
// Operator profile only: immutable OS/work templates and read-only SDK image,
// each identified by SHA-256. Holds deny-write/delete handles until destruction.
class WindowsVmImages {
 public:
 explicit WindowsVmImages(const std::filesystem::path& profile,const std::function<bool()>& keep={});
 ~WindowsVmImages();
 WindowsVmImages(const WindowsVmImages&)=delete;
 WindowsVmImages& operator=(const WindowsVmImages&)=delete;
 HcsCiVmOptions createWorkspace(const std::filesystem::path& parent) const;
 // Recovery never mounts a disk or reads the guest filesystem. Unknown,
 // redirected or still-running workspaces cause a fail-closed refusal.
 static void createDifferencingDisk(const std::filesystem::path& parent,const std::filesystem::path& child);
 static void recover(const std::filesystem::path& parent);
 static void cleanup(const std::filesystem::path& workspace,const std::string& id);
 const std::string& fingerprint() const;
 private:
 struct State;std::unique_ptr<State> state_;
};
}
