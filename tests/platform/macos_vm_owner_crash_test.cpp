// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Opt-in: kills only a task-owned runner surrogate, never the VM helper.
#include "ckgit/macos_vm_workspace.hpp"
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc != 4) { std::cerr << "usage: test HELPER BASE EMPTY-WORK-ROOT\n"; return 2; }
  int events[2];
  if (pipe(events) != 0) return 1;
  const pid_t owner = fork();
  if (owner < 0) return 1;
  if (owner == 0) {
    close(events[0]);
    try {
      ckgit::MacosVmWorkspace workspace(argv[2], argv[3], 1);
      const auto job = workspace.bundle().parent_path().string();
      const auto bundle = workspace.bundle().string(), exchange = workspace.exchange().string();
      const auto announcement = "workspace=" + job + "\n";
      if (write(events[1], announcement.data(), announcement.size()) != static_cast<ssize_t>(announcement.size())) _exit(1);
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) _exit(1);
      const pid_t helper = fork();
      if (helper < 0) _exit(1);
      if (helper == 0) {
        close(sockets[0]);
        if (dup2(sockets[1], STDIN_FILENO) < 0 || dup2(sockets[1], STDOUT_FILENO) < 0 ||
            dup2(events[1], STDERR_FILENO) < 0) _exit(126);
        close(sockets[1]); close(events[1]);
        execl(argv[1], argv[1], "run", bundle.c_str(), exchange.c_str(), "--broker", static_cast<char*>(nullptr));
        _exit(127);
      }
      close(sockets[1]); close(events[1]);
      for (;;) pause();  // Keep the broker socket open until the parent kills this process.
    } catch (...) { _exit(1); }
  }
  close(events[1]);
  bool owner_reaped = false;
  try {
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (output.find("VM guest channel connected\n") == std::string::npos && std::chrono::steady_clock::now() < deadline) {
      pollfd pfd{events[0], POLLIN | POLLHUP, 0};
      const int ready = poll(&pfd, 1, 250);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) throw std::runtime_error("cannot observe VM startup");
      if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
        char buffer[1024]; const auto n = read(events[0], buffer, sizeof(buffer));
        if (n <= 0) throw std::runtime_error("VM helper exited before startup: " + output);
        output.append(buffer, static_cast<std::size_t>(n));
      }
    }
    const auto start = output.find("workspace="), end = output.find('\n', start);
    if (output.find("VM guest channel connected\n") == std::string::npos || start == std::string::npos || end == std::string::npos)
      throw std::runtime_error("VM guest did not connect: " + output);
    const auto original = std::filesystem::path(output.substr(start + 10, end - start - 10));
    if (original.parent_path() != std::filesystem::path(argv[3])) throw std::runtime_error("unexpected test workspace");
    if (kill(owner, SIGKILL) != 0) throw std::runtime_error("cannot stop runner surrogate");
    int status = 0;
    if (waitpid(owner, &status, 0) != owner || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
      throw std::runtime_error("runner surrogate termination not confirmed");
    owner_reaped = true;
    const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    std::string last_error;
    while (std::chrono::steady_clock::now() < recovery_deadline) {
      try {
        ckgit::MacosVmWorkspace next(argv[2], argv[3], 1);
        if (std::filesystem::exists(original)) throw std::runtime_error("old workspace still exists");
        next.removeAfterConfirmedStop();
        close(events[0]);
        std::cout << "Real VM stopped after runner SIGKILL; same-boot workspace recovered\n";
        return 0;
      } catch (const std::exception& error) { last_error = error.what(); }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("same-boot recovery did not complete; workspace retained: " + last_error);
  } catch (const std::exception& error) {
    if (!owner_reaped) { kill(owner, SIGKILL); int status = 0; while (waitpid(owner, &status, 0) < 0 && errno == EINTR) {} }
    close(events[0]);
    std::cerr << "VM owner crash test: " << error.what() << '\n';
    return 1;
  }
}
