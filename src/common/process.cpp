// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <initializer_list>
#include <ctime>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ckgit {

namespace {

// Children currently owned by runProcess, visible to the async-signal-safe
// forwarding handler. Four HTTP workers, one index worker and the control
// thread can spawn concurrently; leave room for administrative child calls.
constexpr std::size_t kActiveChildSlots = 16;
std::atomic<pid_t> active_children[kActiveChildSlots]{};

void registerActiveChild(pid_t child) {
  for (auto& slot : active_children) {
    pid_t expected = 0;
    if (slot.compare_exchange_strong(expected, child)) {
      return;
    }
  }
}

void unregisterActiveChild(pid_t child) {
  for (auto& slot : active_children) {
    pid_t expected = child;
    if (slot.compare_exchange_strong(expected, 0)) {
      return;
    }
  }
}

void signalProcessTree(pid_t child, int signal_number, bool child_alive) {
  // The child is its own session and process-group leader, so the negative
  // ID reaches every descendant that has not changed its group.  A process
  // ID stays reserved while its process group has members, so the group
  // signal is safe even after the leader itself was reaped; the direct
  // signal is only sent while the leader is known to be alive.
  kill(-child, signal_number);
  if (child_alive) {
    kill(child, signal_number);
  }
}

void forwardTerminationSignal(int signal_number) {
  for (auto& slot : active_children) {
    const pid_t child = slot.load();
    if (child > 0) {
      kill(-child, SIGTERM);
      kill(child, SIGTERM);
    }
  }
  signal(signal_number, SIG_DFL);
  raise(signal_number);
}

void sleepBriefly() {
  timespec pause{};
  pause.tv_nsec = 20 * 1000 * 1000;
  nanosleep(&pause, nullptr);
}

}  // namespace

void installChildTerminationForwarding() {
  for (const int signal_number : {SIGINT, SIGTERM, SIGHUP, SIGPIPE}) {
    struct sigaction action {};
    action.sa_handler = forwardTerminationSignal;
    sigemptyset(&action.sa_mask);
    sigaction(signal_number, &action, nullptr);
  }
}

ProcessResult runProcess(const std::vector<std::string>& arguments,
                         std::chrono::milliseconds timeout,
                         std::size_t output_limit) {
  ProcessOptions options;
  options.timeout = timeout;
  options.output_limit = output_limit;
  return runProcess(arguments, options);
}

ProcessResult runProcess(const std::vector<std::string>& arguments, const ProcessOptions& options) {
  if (arguments.empty() || arguments.front().empty()) {
    throw std::invalid_argument("cannot run an empty command");
  }

  int pipe_fds[2] = {-1, -1};
#ifdef __linux__
  const int pipe_result = pipe2(pipe_fds, O_CLOEXEC);
#else
  const int pipe_result = pipe(pipe_fds);
#endif
  if (pipe_result != 0) {
    throw std::runtime_error("could not create process output pipe: " +
                             std::string(std::strerror(errno)));
  }

#ifndef __linux__
  fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
#endif
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
  if (!options.inherit_stderr) {
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
  } else {
    posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDERR_FILENO);
  }
  posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
  posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);

  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_SETSID
#ifdef __APPLE__
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
#else
  posix_spawnattr_setpgroup(&attributes, 0);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
#endif

  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  pid_t child = 0;
  const int spawn_result =
      posix_spawnp(&child, argv[0], &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  close(pipe_fds[1]);
  if (spawn_result != 0) {
    close(pipe_fds[0]);
    throw std::runtime_error("could not start " + arguments.front() + ": " +
                             std::string(std::strerror(spawn_result)));
  }
  registerActiveChild(child);

  ProcessResult result;
  const auto deadline = std::chrono::steady_clock::now() + options.timeout;
  bool pipe_open = true;
  bool child_reaped = false;
  int child_status = 0;
  std::array<char, 4096> buffer{};

  const auto abandon = [&](const std::string& message, int saved_errno) {
    signalProcessTree(child, SIGKILL, !child_reaped);
    if (pipe_open) {
      close(pipe_fds[0]);
    }
    if (!child_reaped) {
      waitpid(child, nullptr, 0);
    }
    unregisterActiveChild(child);
    throw std::runtime_error(message + ": " + std::string(std::strerror(saved_errno)));
  };

  while (pipe_open || !child_reaped) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline && !result.timed_out) {
      result.timed_out = true;
      signalProcessTree(child, SIGKILL, !child_reaped);
      // Whatever still holds the pipe after the kill has left the session; it
      // must not keep this caller waiting, so reading simply stops here.
      if (pipe_open) {
        close(pipe_fds[0]);
        pipe_open = false;
      }
      if (!child_reaped) {
        waitpid(child, &child_status, 0);
        child_reaped = true;
      }
      break;
    }

    if (pipe_open) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const int poll_timeout = static_cast<int>(std::min<std::int64_t>(std::max<std::int64_t>(remaining.count(), 0), 100));
      pollfd descriptor{pipe_fds[0], POLLIN | POLLHUP, 0};
      const int poll_result = poll(&descriptor, 1, poll_timeout);
      if (poll_result < 0 && errno != EINTR) {
        abandon("could not read process output", errno);
      }
      if (poll_result > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
        const ssize_t bytes_read = read(pipe_fds[0], buffer.data(), buffer.size());
        if (bytes_read > 0) {
          const std::size_t available = options.output_limit > result.output.size()
                                            ? options.output_limit - result.output.size()
                                            : 0;
          const std::size_t to_copy =
              std::min<std::size_t>(available, static_cast<std::size_t>(bytes_read));
          result.output.append(buffer.data(), to_copy);
          result.output_truncated = result.output_truncated ||
                                    to_copy != static_cast<std::size_t>(bytes_read);
        } else if (bytes_read == 0) {
          close(pipe_fds[0]);
          pipe_open = false;
        } else if (errno != EINTR) {
          abandon("could not read process output", errno);
        }
      }
    } else {
      sleepBriefly();  // Output is finished; only the exit status is pending.
    }
    if (!child_reaped) {
      const pid_t wait_result = waitpid(child, &child_status, WNOHANG);
      if (wait_result == child) {
        child_reaped = true;
      } else if (wait_result < 0 && errno != EINTR) {
        abandon("could not wait for process", errno);
      }
    }
  }
  unregisterActiveChild(child);

  if (WIFEXITED(child_status)) {
    result.exit_code = WEXITSTATUS(child_status);
  } else if (WIFSIGNALED(child_status)) {
    result.exit_code = 128 + WTERMSIG(child_status);
  }
  return result;
}

int spawnAttachedProcess(const std::vector<std::string>& arguments) {
  if (arguments.empty() || arguments.front().empty()) {
    throw std::invalid_argument("cannot run an empty command");
  }
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  pid_t child = 0;
  const int spawn_result = posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ);
  if (spawn_result != 0) {
    throw std::runtime_error("could not start " + arguments.front() + ": " +
                             std::string(std::strerror(spawn_result)));
  }
  return static_cast<int>(child);
}

}  // namespace ckgit
