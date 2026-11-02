// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
namespace ckgit {
inline constexpr char kWindowsVmServiceGuid[]="8f22bd55-d776-43ea-bd82-93b2ee39f6da";
struct HcsCiVmOptions {
 // The broker must journal this fresh identity before creation.
 std::string id;
 std::filesystem::path os_disk,work_disk,tools_disk;
 unsigned memory_mb=8192,processors=4;
};
// Only a typed offline CI topology is admitted: three disks and one private
// Hyper-V socket service. No caller JSON, NIC, share or redirected devices.
// The unique VM's HCS handle is never inherited by another process.
class HcsCiVm {
 public:
 explicit HcsCiVm(const HcsCiVmOptions&,const std::function<bool()>& keep={});
 ~HcsCiVm();
 HcsCiVm(const HcsCiVm&)=delete;
 HcsCiVm& operator=(const HcsCiVm&)=delete;
 const std::string& id() const;
 const std::string& runtimeId() const;
 bool running() const;
 // Operator base initialization only: no forced termination on failure.
 bool shutdownClean();
 // False means stop is unconfirmed: retain disks and refuse more jobs.
 bool stop(const std::function<bool()>& keep={});
 static std::string freshId();
 static bool confirmedAbsent(const std::string& id);
 static void preflight(const std::function<bool()>& keep={});
 private:
 struct State;std::unique_ptr<State> state_;
};
}
