// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "ckgit/ci_vm_protocol.hpp"
#include <memory>
namespace ckgit {
// Connect only to the runtime identity returned for this broker-owned VM.
// Owns Winsock and one non-inheritable, nonblocking Hyper-V socket.
class WindowsVmConnection {
 public:
 explicit WindowsVmConnection(const std::string& runtime_id,
  std::chrono::milliseconds timeout,const std::function<bool()>& keep={});
 ~WindowsVmConnection();
 WindowsVmConnection(const WindowsVmConnection&)=delete;
 WindowsVmConnection& operator=(const WindowsVmConnection&)=delete;
 CiVmSocket socket() const;
 private:
 struct State;std::unique_ptr<State> state_;
};
}
