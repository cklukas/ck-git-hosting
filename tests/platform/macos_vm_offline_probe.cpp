// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Opt-in only. Uses an operator-prepared base containing the qualification
// guest agent. Runs one disposable VM with no virtual network device.
#include "ckgit/macos_vm_workspace.hpp"

#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
struct ChildGuard {
  pid_t pid;
  bool reaped = false;
  ~ChildGuard() {
    if (reaped) return;
    kill(pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid || (waited < 0 && errno == ECHILD)) return;
      if (waited < 0 && errno != EINTR) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  }
};
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: ckgit-macos-vm-offline-probe BASE WORK-ROOT VM-HELPER\n";
    return 2;
  }
  try {
    ckgit::MacosVmWorkspace workspace(argv[1], argv[2], 1);
    const auto bundle = workspace.bundle().string();
    const auto exchange = workspace.exchange().string();
    const auto result = (workspace.bundle().parent_path() / "vsock-result").string();
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("cannot start VM helper");
    if (child == 0) {
      execl(argv[3], argv[3], "run", bundle.c_str(), exchange.c_str(), "--vsock-probe", "--probe-result",
            result.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    ChildGuard guard{child};
    bool got_result = false;
    bool interface_error = false;
    bool exited = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(4);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto waited = waitpid(child, &status, WNOHANG);
      if (waited == child) { exited = true; guard.reaped = true; break; }
      if (waited < 0) throw std::runtime_error("cannot inspect VM helper");
      struct stat marker {};
      if (lstat(result.c_str(), &marker) == 0) {
        if (!S_ISREG(marker.st_mode) || marker.st_uid != geteuid() || marker.st_nlink != 1 ||
            (marker.st_mode & 0777) != 0600) throw std::runtime_error("invalid host probe result");
        std::ifstream file(result);
        std::string value;
        std::getline(file, value);
        got_result = value == "ckgit-vsock-offline-v1";
        interface_error = value == "ckgit-vsock-network-v1";
        if (!got_result && !interface_error) throw std::runtime_error("invalid host probe result contents");
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!exited && kill(child, SIGTERM) != 0) throw std::runtime_error("cannot request VM shutdown");
    const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!exited && std::chrono::steady_clock::now() < stop_deadline) {
      const auto waited = waitpid(child, &status, WNOHANG);
      if (waited == child) { exited = true; guard.reaped = true; }
      else if (waited < 0) throw std::runtime_error("cannot wait for VM shutdown");
      else std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
      throw std::runtime_error("VM stop was not confirmed; workspace retained and new jobs blocked");
    workspace.removeAfterConfirmedStop();
    if (interface_error) throw std::runtime_error("guest reported an active interface or could not inspect interfaces");
    if (!got_result) throw std::runtime_error("guest did not complete the offline Virtio probe");
    std::cout << "Offline Apple VM Virtio probe passed; disposable workspace removed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ckgit-macos-vm-offline-probe: " << error.what() << '\n';
    return 1;
  }
}
