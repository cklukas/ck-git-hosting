// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ckgit {

ProcessResult runProcess(const std::vector<std::string>& arguments,
                         std::chrono::milliseconds timeout,
                         std::size_t output_limit) {
  if (arguments.empty() || arguments.front().empty()) {
    throw std::invalid_argument("cannot run an empty command");
  }

  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    throw std::runtime_error("could not create process output pipe: " +
                             std::string(std::strerror(errno)));
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
  posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);

  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  pid_t child = 0;
  const int spawn_result =
      posix_spawnp(&child, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipe_fds[1]);
  if (spawn_result != 0) {
    close(pipe_fds[0]);
    throw std::runtime_error("could not start " + arguments.front() + ": " +
                             std::string(std::strerror(spawn_result)));
  }

  ProcessResult result;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool pipe_open = true;
  bool child_reaped = false;
  int child_status = 0;
  std::array<char, 4096> buffer{};

  while (pipe_open || !child_reaped) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline && !result.timed_out) {
      result.timed_out = true;
      kill(child, SIGKILL);
    }

    if (pipe_open) {
      int poll_timeout = 0;
      if (!result.timed_out) {
        const auto remaining = deadline > now
                                   ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                                   : std::chrono::milliseconds::zero();
        poll_timeout = static_cast<int>(std::min<std::int64_t>(remaining.count(), 100));
      }
      pollfd descriptor{pipe_fds[0], POLLIN | POLLHUP, 0};
      const int poll_result = poll(&descriptor, 1, poll_timeout);
      if (poll_result < 0 && errno != EINTR) {
        kill(child, SIGKILL);
        close(pipe_fds[0]);
        waitpid(child, nullptr, 0);
        throw std::runtime_error("could not read process output: " +
                                 std::string(std::strerror(errno)));
      }
      if (poll_result > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
        const ssize_t bytes_read = read(pipe_fds[0], buffer.data(), buffer.size());
        if (bytes_read > 0) {
          const std::size_t available = output_limit > result.output.size()
                                            ? output_limit - result.output.size()
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
          kill(child, SIGKILL);
          close(pipe_fds[0]);
          waitpid(child, nullptr, 0);
          throw std::runtime_error("could not read process output: " +
                                   std::string(std::strerror(errno)));
        }
      }
    }
    if (!child_reaped) {
      const pid_t wait_result = waitpid(child, &child_status, WNOHANG);
      if (wait_result == child) {
        child_reaped = true;
      } else if (wait_result < 0 && errno != EINTR) {
        if (pipe_open) {
          close(pipe_fds[0]);
        }
        throw std::runtime_error("could not wait for process: " +
                                 std::string(std::strerror(errno)));
      }
    }
  }

  if (WIFEXITED(child_status)) {
    result.exit_code = WEXITSTATUS(child_status);
  } else if (WIFSIGNALED(child_status)) {
    result.exit_code = 128 + WTERMSIG(child_status);
  }
  return result;
}

}  // namespace ckgit
