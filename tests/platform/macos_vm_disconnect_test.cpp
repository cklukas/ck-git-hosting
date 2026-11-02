// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Opt-in real VM test: no guest service is required.
#include "ckgit/macos_vm_workspace.hpp"
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc != 4) { std::cerr << "usage: test HELPER BASE WORK-ROOT\n"; return 2; }
  try {
    ckgit::MacosVmWorkspace workspace(argv[2], argv[3], 1);
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) throw std::runtime_error("socketpair failed");
    const auto bundle = workspace.bundle().string(), exchange = workspace.exchange().string();
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
      close(sockets[0]);
      if (dup2(sockets[1], STDIN_FILENO) < 0 || dup2(sockets[1], STDOUT_FILENO) < 0) _exit(126);
      close(sockets[1]);
      execl(argv[1], argv[1], "run", bundle.c_str(), exchange.c_str(), "--broker", static_cast<char*>(nullptr));
      _exit(127);
    }
    close(sockets[1]);
    // Leave the private channel open briefly, then simulate runner death before
    // any guest connects. Do not send a shutdown signal to the helper.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    close(sockets[0]);
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto result = waitpid(child, &status, WNOHANG);
      if (result == child) { exited = true; break; }
      if (result < 0 && errno != EINTR) throw std::runtime_error("cannot inspect VM helper");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!exited) {
      kill(child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      throw std::runtime_error("VM helper did not stop after channel loss; workspace retained");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      throw std::runtime_error("VM helper did not confirm a clean stop; workspace retained");
    workspace.removeAfterConfirmedStop();
    std::cout << "VM stopped after pre-connection channel loss; clone removed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << "VM disconnect test: " << e.what() << '\n'; return 1; }
}
