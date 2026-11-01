// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/process.hpp"
#include "process_tools_internal.hpp"
#include "ckgit/path_utf8.hpp"

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
#include <sys/stat.h>
#include <sys/socket.h>
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

std::string resolveProcessExecutable(const std::string& requested) {
  const auto name = selectedProcessProgram(requested);
  if (name.empty() || name.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid executable name");
  const auto usable = [](const std::filesystem::path& candidate) {
    std::error_code error;
    return std::filesystem::is_regular_file(candidate, error) &&
           ::access(candidate.c_str(), X_OK) == 0;
  };
  const auto program = pathFromUtf8(name);
  if (name.find('/') != std::string::npos) {
    if (usable(program)) return pathToUtf8(std::filesystem::absolute(program));
  } else {
    const char* inherited = std::getenv("PATH");
    std::string search;
    if (inherited) search = inherited;
    else {
      const auto size = confstr(_CS_PATH, nullptr, 0);
      if (size) { search.resize(size); confstr(_CS_PATH, search.data(), size); search.pop_back(); }
    }
    for (std::size_t start = 0; start <= search.size();) {
      const auto end = search.find(':', start);
      const auto directory = search.substr(start, end == std::string::npos ? end : end - start);
      const auto candidate = pathFromUtf8(directory.empty() ? "." : directory) / program;
      if (usable(candidate)) return pathToUtf8(std::filesystem::absolute(candidate));
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  throw std::runtime_error("cannot resolve executable: " + name);
}

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

ProcessResult runProcess(const std::vector<std::string>& requested, const ProcessOptions& requested_options) {
  const auto arguments = selectedProcessArguments(requested);
  const auto selected_options = selectedProcessOptions(requested, requested_options);
  const auto& options = selected_options ? *selected_options : requested_options;
  if (arguments.empty() || arguments.front().empty()) {
    throw std::invalid_argument("cannot run an empty command");
  }

  std::vector<std::string> environment;
  std::vector<char*> environment_pointers;
  if (!options.environment.empty()) {
    for (char** item = environ; item && *item; ++item) environment.emplace_back(*item);
    for (const auto& [key, value] : options.environment) {
      if (key.empty() || key.find('=') != std::string::npos || key.find('\0') != std::string::npos ||
          (value && value->find('\0') != std::string::npos)) throw std::invalid_argument("invalid process environment");
      const auto prefix = key + "=";
      std::erase_if(environment, [&](const auto& entry) { return entry.rfind(prefix, 0) == 0; });
      if (value) environment.push_back(prefix + *value);
    }
    for (auto& entry : environment) environment_pointers.push_back(entry.data());
    environment_pointers.push_back(nullptr);
  }

  struct Input {
    int fd = -1;
    ~Input() { if (fd >= 0) close(fd); }
  } input;
  if (options.input_file) {
    if (!options.input_file->is_absolute()) throw std::invalid_argument("process input must be absolute");
    input.fd = open(options.input_file->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat status{};
    if (input.fd < 0 || fstat(input.fd, &status) != 0 || !S_ISREG(status.st_mode))
      throw std::runtime_error("process input must be a readable regular file");
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
  if (input.fd >= 0) {
    posix_spawn_file_actions_adddup2(&actions, input.fd, STDIN_FILENO);
    if (input.fd != STDIN_FILENO) posix_spawn_file_actions_addclose(&actions, input.fd);
  } else {
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  }
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
      posix_spawnp(&child, argv[0], &actions, &attributes, argv.data(),
                   options.environment.empty() ? environ : environment_pointers.data());
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
    throw std::runtime_error(message + ": " + std::string(std::strerror(saved_errno)));
  };

  try {
    while (pipe_open || !child_reaped) {
      const auto now = std::chrono::steady_clock::now();
      const bool cancelled = options.should_continue && !options.should_continue();
      if (cancelled || now >= deadline) {
        result.cancelled = cancelled;
        result.timed_out = !cancelled;
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
            if (options.on_output) options.on_output(std::string_view(buffer.data(), static_cast<std::size_t>(bytes_read)));
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
  } catch (...) {
    signalProcessTree(child, SIGKILL, !child_reaped);
    if (pipe_open) close(pipe_fds[0]);
    if (!child_reaped) {
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    }
    unregisterActiveChild(child);
    throw;
  }
  unregisterActiveChild(child);

  if (WIFEXITED(child_status)) {
    result.exit_code = WEXITSTATUS(child_status);
  } else if (WIFSIGNALED(child_status)) {
    result.exit_code = 128 + WTERMSIG(child_status);
  }
  return result;
}

struct ManagedProcess::State {
  pid_t child = -1;
  std::optional<int> exit;
};
ManagedProcess::ManagedProcess(const std::vector<std::string>& requested) : state_(std::make_unique<State>()) {
  const auto arguments = selectedProcessArguments(requested);
  if (arguments.empty() || arguments.front().empty()) throw std::invalid_argument("empty background command");
  std::vector<char*> argv;
  for (const auto& argument : arguments) {
    if (argument.find('\0') != std::string::npos) throw std::invalid_argument("NUL in background argument");
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  const auto child = fork();
  if (child < 0) throw std::runtime_error("could not start background process");
  if (child == 0) {
    if (setsid() < 0) _exit(127);
    const int input = open("/dev/null", O_RDONLY);
    if (input < 0 || dup2(input, STDIN_FILENO) < 0) _exit(127);
    if (input != STDIN_FILENO) close(input);
    execvp(argv.front(), argv.data());
    _exit(127);
  }
  state_->child = child;
  registerActiveChild(child);
}
ManagedProcess::~ManagedProcess() { stop(); }
unsigned long ManagedProcess::id() const { return static_cast<unsigned long>(state_->child); }
std::optional<int> ManagedProcess::poll() {
  if (state_->exit) return state_->exit;
  int status = 0;
  const auto result = waitpid(state_->child, &status, WNOHANG);
  if (result == 0 || (result < 0 && errno == EINTR)) return std::nullopt;
  if (result < 0) throw std::runtime_error("could not inspect background process");
  state_->exit = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
  signalProcessTree(state_->child, SIGKILL, false);
  unregisterActiveChild(state_->child);
  return state_->exit;
}
void ManagedProcess::stop() noexcept {
  if (!state_ || state_->exit || state_->child < 0) return;
  signalProcessTree(state_->child, SIGTERM, true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  try {
    while (std::chrono::steady_clock::now() < deadline) {
      if (poll()) return;
      sleepBriefly();
    }
  } catch (...) {}
  signalProcessTree(state_->child, SIGKILL, true);
  int status = 0;
  while (waitpid(state_->child, &status, 0) < 0 && errno == EINTR) {}
  state_->exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + SIGKILL;
  unregisterActiveChild(state_->child);
}

int spawnAttachedProcess(const std::vector<std::string>& requested) {
  const auto arguments = selectedProcessArguments(requested);
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


int execProcess(const std::vector<std::string>& requested) {
  const auto arguments = selectedProcessArguments(requested);
  if (arguments.empty() || arguments.front().empty()) throw std::invalid_argument("empty exec command");
  std::vector<char*> argv;
  for (const auto& argument : arguments) {
    if (argument.find('\0') != std::string::npos) throw std::invalid_argument("NUL in exec argument");
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  execvp(argv.front(), argv.data());
  return 127;
}

struct ProcessStream::State {
  int input = -1, output = -1;
  pid_t child = -1;
  std::optional<int> exit;
  ~State() { if (input >= 0) close(input); if (output >= 0) close(output); }
};

ProcessStream::ProcessStream(const std::vector<std::string>& requested) : state_(std::make_unique<State>()) {
  const auto arguments = selectedProcessArguments(requested);
  if (arguments.empty() || arguments.front().empty()) throw std::invalid_argument("empty stream command");
  std::vector<char*> argv;
  for (const auto& argument : arguments) {
    if (argument.find('\0') != std::string::npos) throw std::invalid_argument("NUL in stream argument");
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  struct Pipes {
    int in[2]{-1, -1}, out[2]{-1, -1};
    ~Pipes() { for (const auto fd : {in[0], in[1], out[0], out[1]}) if (fd >= 0) close(fd); }
  } pipes;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipes.in) || pipe(pipes.out))
    throw std::runtime_error("cannot create stream pipes");
  for (const auto fd : {pipes.in[0], pipes.in[1], pipes.out[0], pipes.out[1]})
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) throw std::runtime_error("cannot protect stream pipes");
  for (const auto fd : {pipes.in[0], pipes.out[0]})
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) throw std::runtime_error("cannot configure stream pipes");
#ifdef __APPLE__
  int enabled = 1;
  if (setsockopt(pipes.in[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)))
    throw std::runtime_error("cannot suppress stream SIGPIPE");
#endif
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipes.in[1], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipes.out[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDERR_FILENO);
  for (const auto fd : {pipes.in[0], pipes.in[1], pipes.out[0], pipes.out[1]})
    if (fd > STDERR_FILENO) posix_spawn_file_actions_addclose(&actions, fd);
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
  pid_t child = -1;
  const int result = posix_spawnp(&child, argv[0], &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (result) throw std::runtime_error("cannot start stream process: " + std::string(std::strerror(result)));
  state_->child = child;
  state_->input = pipes.in[0]; pipes.in[0] = -1;
  state_->output = pipes.out[0]; pipes.out[0] = -1;
  registerActiveChild(child);
}
ProcessStream::~ProcessStream() { stop(); }
std::ptrdiff_t ProcessStream::read(char* buffer, std::size_t capacity) {
  if (!capacity) return 0;
  if (state_->output < 0) return -1;
  const auto received = ::read(state_->output, buffer, capacity);
  if (received > 0) return received;
  if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
  close(state_->output); state_->output = -1;
  return -1;
}
std::ptrdiff_t ProcessStream::write(std::string_view bytes) {
  if (state_->input < 0) return -1;
  if (bytes.empty()) return 0;
#ifdef MSG_NOSIGNAL
  const auto sent = send(state_->input, bytes.data(), bytes.size(), MSG_NOSIGNAL);
#else
  const auto sent = send(state_->input, bytes.data(), bytes.size(), 0);
#endif
  if (sent >= 0) return sent;
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
  closeInput(); return -1;
}
bool ProcessStream::closeInput() {
  if (state_->input >= 0) { close(state_->input); state_->input = -1; }
  return true;
}
std::optional<int> ProcessStream::poll() {
  if (state_->exit || state_->child <= 0) return state_->exit;
  // Keep the exited leader waitable until stop(), reserving its PID while
  // the stream still owns (and may need to terminate) the process group.
  siginfo_t info{};
  const auto result = waitid(P_PID, static_cast<id_t>(state_->child), &info, WEXITED | WNOHANG | WNOWAIT);
  if (result == 0 && info.si_pid == state_->child) {
    state_->exit = info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status;
  } else if (result < 0 && errno != EINTR) {
    state_->exit = -1;
    unregisterActiveChild(state_->child);
    state_->child = -1;
  }
  return state_->exit;
}
void ProcessStream::stop() noexcept {
  closeInput();
  if (state_->child <= 0) return;
  signalProcessTree(state_->child, SIGKILL, !state_->exit);
  int status = 0;
  pid_t done;
  do { done = waitpid(state_->child, &status, 0); } while (done < 0 && errno == EINTR);
  if (!state_->exit) state_->exit = done < 0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  unregisterActiveChild(state_->child);
  state_->child = -1;
  if (state_->output >= 0) { close(state_->output); state_->output = -1; }
}

}  // namespace ckgit
