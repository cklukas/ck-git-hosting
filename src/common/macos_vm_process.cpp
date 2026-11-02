// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/macos_vm_process.hpp"

#ifdef __APPLE__
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ckgit {
MacosVmProcess::MacosVmProcess(const std::filesystem::path& helper, const std::filesystem::path& bundle,
                               const std::filesystem::path& exchange) {
  if (!helper.is_absolute() || !bundle.is_absolute() || !exchange.is_absolute())
    throw std::invalid_argument("VM helper and workspace paths must be absolute");
  int pair[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) throw std::runtime_error("cannot create VM broker socket");
  for (int& fd : pair) {
    if (fd < 3) {
      const int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
      if (moved < 0) { close(pair[0]); close(pair[1]); throw std::runtime_error("cannot protect VM broker socket"); }
      close(fd);
      fd = moved;
    } else if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
      close(pair[0]); close(pair[1]); throw std::runtime_error("cannot protect VM broker socket");
    }
  }
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  bool actions_ready = false, attr_ready = false;
  try {
    if (posix_spawn_file_actions_init(&actions) != 0) throw std::runtime_error("cannot prepare VM helper");
    actions_ready = true;
    if (posix_spawnattr_init(&attr) != 0) throw std::runtime_error("cannot prepare VM helper attributes");
    attr_ready = true;
    // The owner may cancel immediately after spawn, before the helper has
    // installed its dispatch signal sources. Defer termination until then.
    sigset_t child_mask;
    if (sigprocmask(SIG_SETMASK, nullptr, &child_mask) != 0 || sigaddset(&child_mask, SIGTERM) != 0 ||
        sigaddset(&child_mask, SIGINT) != 0 || posix_spawnattr_setsigmask(&attr, &child_mask) != 0 ||
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, pair[1], STDIN_FILENO) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, pair[1], STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDERR_FILENO) != 0 ||
        posix_spawn_file_actions_addclose(&actions, pair[0]) != 0 ||
        posix_spawn_file_actions_addclose(&actions, pair[1]) != 0)
      throw std::runtime_error("cannot configure VM helper descriptors");
    std::string executable = helper.string(), vm = bundle.string(), shared = exchange.string();
    char run[] = "run", broker[] = "--broker";
    char* args[] = {executable.data(), run, vm.data(), shared.data(), broker, nullptr};
    pid_t pid = -1;
    if (posix_spawn(&pid, executable.c_str(), &actions, &attr, args, environ) != 0)
      throw std::runtime_error("cannot start VM helper");
    child_ = static_cast<int>(pid);
    socket_ = pair[0];
    close(pair[1]);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
  } catch (...) {
    if (attr_ready) posix_spawnattr_destroy(&attr);
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    close(pair[0]); close(pair[1]);
    throw;
  }
}
bool MacosVmProcess::stop(std::chrono::milliseconds timeout, const std::function<void()>& on_wait) {
  if (reaped_) return confirmed_;
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::minutes(5))
    throw std::invalid_argument("invalid VM shutdown timeout");
  if (socket_ >= 0) { shutdown(socket_, SHUT_RDWR); close(socket_); socket_ = -1; }
  if (kill(child_, SIGTERM) != 0 && errno != ESRCH) return false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto next_callback = std::chrono::steady_clock::now();
  for (;;) {
    int status = 0;
    const auto result = waitpid(child_, &status, WNOHANG);
    if (result == child_) {
      reaped_ = true;
      confirmed_ = WIFEXITED(status) && WEXITSTATUS(status) == 0;
      return confirmed_;
    }
    if (result < 0 && errno != EINTR) { reaped_ = true; return false; }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    if (on_wait && now >= next_callback) {
      on_wait();
      next_callback = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}
MacosVmProcess::~MacosVmProcess() {
  if (!reaped_) {
    try { (void)stop(std::chrono::seconds(5)); } catch (...) {}
  }
  if (!reaped_ && child_ > 0) {
    kill(child_, SIGKILL);
    int status = 0;
    while (waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
  }
  if (socket_ >= 0) close(socket_);
}
}
#endif
