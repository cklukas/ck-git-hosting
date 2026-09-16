// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_runner.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#include <sys/mount.h>
#endif

#include "ckgit/ci_store.hpp"
#include "ckgit/ci_workflow.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/process.hpp"
#include "ckgit/validation.hpp"

extern char** environ;

namespace ckgit {
namespace {

std::uint64_t nowEpoch() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string branchOf(const std::string& ref) {
  const std::string prefix = "refs/heads/";
  return ref.rfind(prefix, 0) == 0 ? ref.substr(prefix.size()) : ref;
}

// The repository's default branch, taken from its HEAD symbolic ref (e.g.
// "main"). std::nullopt when HEAD does not name a branch (an unborn or detached
// bare HEAD), in which case the caller runs the workflow rather than guessing.
std::optional<std::string> defaultBranch(const std::filesystem::path& repository) {
  ProcessOptions options;
  options.timeout = std::chrono::seconds(20);
  options.output_limit = 4096;
  const ProcessResult result = runProcess(
      {"git", "--git-dir", repository.string(), "symbolic-ref", "--short", "HEAD"}, options);
  if (result.timed_out || result.exit_code != 0) return std::nullopt;
  std::string branch = result.output;
  while (!branch.empty() && (branch.back() == '\n' || branch.back() == '\r')) branch.pop_back();
  if (branch.empty()) return std::nullopt;
  return branch;
}

// Reads .ckgit/ci.yml from the commit object; std::nullopt means the commit has
// no workflow (a non-CI push), which the caller records as Skipped.
std::optional<std::string> readWorkflowBlob(const std::filesystem::path& repository,
                                            const std::string& commit) {
  ProcessOptions options;
  options.timeout = std::chrono::seconds(20);
  options.output_limit = kMaximumCiWorkflowBytes + 4096;
  const ProcessResult result = runProcess(
      {"git", "--git-dir", repository.string(), "cat-file", "-p", commit + ":.ckgit/ci.yml"}, options);
  if (result.timed_out) throw std::runtime_error("reading .ckgit/ci.yml timed out");
  if (result.exit_code != 0) return std::nullopt;  // path absent in this commit
  return result.output;
}

// Materialises the commit's tree into `scratch` via `git archive | tar -x`,
// which runs no checkout hooks and pulls in no submodules.
void checkoutCommit(const std::filesystem::path& repository, const std::string& commit,
                    const std::filesystem::path& scratch, const std::filesystem::path& tar_path) {
  ProcessOptions options;
  options.timeout = std::chrono::seconds(120);
  const ProcessResult archive = runProcess(
      {"git", "--git-dir", repository.string(), "archive", "--format=tar", "--output", tar_path.string(),
       commit},
      options);
  if (archive.exit_code != 0 || archive.timed_out) {
    throw std::runtime_error("git archive failed for commit " + commit);
  }
  const ProcessResult extract =
      runProcess({"tar", "-xf", tar_path.string(), "-C", scratch.string()}, options);
  if (extract.exit_code != 0 || extract.timed_out) throw std::runtime_error("unpacking the commit failed");
  std::error_code error;
  std::filesystem::remove(tar_path, error);
}

// Base + workflow + job + caller env, later definitions winning, as KEY=VALUE.
std::vector<std::string> buildEnv(const CiWorkflow& workflow, const CiJob& job,
                                  const CiRunnerOptions& options, const std::filesystem::path& home) {
  std::vector<std::pair<std::string, std::string>> ordered = {
      {"PATH", "/usr/local/bin:/usr/bin:/bin"},
      {"HOME", home.string()},
      {"LANG", "C"},
      {"LC_ALL", "C"},
      {"CKGIT_CI", "1"},
      {"CI", "true"},
  };
  const auto put = [&ordered](const std::string& key, const std::string& value) {
    for (auto& entry : ordered) {
      if (entry.first == key) { entry.second = value; return; }
    }
    ordered.emplace_back(key, value);
  };
  for (const auto& [key, value] : workflow.env) put(key, value);
  for (const auto& [key, value] : job.env) put(key, value);
  for (const std::string& assignment : options.extra_env) {
    const std::size_t equals = assignment.find('=');
    if (equals != std::string::npos) put(assignment.substr(0, equals), assignment.substr(equals + 1));
  }
  std::vector<std::string> env;
  env.reserve(ordered.size());
  for (const auto& [key, value] : ordered) env.push_back(key + "=" + value);
  return env;
}

#if defined(__linux__)
void writeProcFile(const char* path, const std::string& content) {
  const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return;
  const ssize_t written = ::write(fd, content.data(), content.size());
  ::close(fd);
  (void)written;
}

// In the freshly forked child: enter unprivileged user, mount, and (unless
// allowed) network namespaces, so the step has no network and cannot see the
// host mount table. Returns false if the kernel denies unprivileged namespaces.
bool enterSandbox(bool allow_network) {
  const uid_t uid = ::getuid();
  const gid_t gid = ::getgid();
  int flags = CLONE_NEWUSER | CLONE_NEWNS;
  if (!allow_network) flags |= CLONE_NEWNET;
  if (::unshare(flags) != 0) return false;
  writeProcFile("/proc/self/setgroups", "deny");
  writeProcFile("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1\n");
  writeProcFile("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1\n");
  // Keep our mount changes from propagating back to the host namespace.
  ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
  return true;
}

bool probeUserNamespaces(bool allow_network) {
  const pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    int flags = CLONE_NEWUSER | CLONE_NEWNS;
    if (!allow_network) flags |= CLONE_NEWNET;
    _exit(::unshare(flags) == 0 ? 0 : 1);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#else
bool enterSandbox(bool) { return false; }
bool probeUserNamespaces(bool) { return false; }
#endif

void applyRlimits() {
  // Rely on the wall-clock timeout and the sandbox for the important bounds;
  // keep rlimits to what cannot break a legitimate parallel build. No core
  // dumps, and a generous single-file size cap so a step cannot fill the disk
  // with one enormous file.
  const rlimit no_core{0, 0};
  ::setrlimit(RLIMIT_CORE, &no_core);
  const rlimit file_size{static_cast<rlim_t>(4) << 30, static_cast<rlim_t>(4) << 30};
  ::setrlimit(RLIMIT_FSIZE, &file_size);
}

struct StepOutcome {
  int exit_code = 0;
  bool timed_out = false;
  bool truncated = false;
  bool spawn_failed = false;
};

StepOutcome executeStep(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                        const std::vector<std::string>& env, const CiRunnerOptions& options,
                        const std::filesystem::path& log_path) {
  StepOutcome outcome;
  int pipe_fd[2];
  if (::pipe(pipe_fd) != 0) {
    outcome.spawn_failed = true;
    return outcome;
  }
  const int log = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (log < 0) {
    ::close(pipe_fd[0]);
    ::close(pipe_fd[1]);
    outcome.spawn_failed = true;
    return outcome;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipe_fd[0]);
    ::close(pipe_fd[1]);
    ::close(log);
    outcome.spawn_failed = true;
    return outcome;
  }
  if (pid == 0) {
    ::close(pipe_fd[0]);
    ::close(log);
    ::setpgid(0, 0);
    enterSandbox(options.allow_network);
    applyRlimits();
    const int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::close(devnull);
    }
    ::dup2(pipe_fd[1], STDOUT_FILENO);
    ::dup2(pipe_fd[1], STDERR_FILENO);
    ::close(pipe_fd[1]);
    if (::chdir(cwd.c_str()) != 0) _exit(126);
    std::vector<char*> raw_argv;
    for (const std::string& argument : argv) raw_argv.push_back(const_cast<char*>(argument.c_str()));
    raw_argv.push_back(nullptr);
    static std::vector<char*> raw_env;  // static: outlives the exec setup in this child
    raw_env.clear();
    for (const std::string& assignment : env) raw_env.push_back(const_cast<char*>(assignment.c_str()));
    raw_env.push_back(nullptr);
    environ = raw_env.data();
    ::execvp(raw_argv[0], raw_argv.data());
    _exit(127);
  }

  ::setpgid(pid, pid);
  ::close(pipe_fd[1]);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);
  std::size_t written = 0;
  bool killed = false;
  char buffer[8192];
  for (;;) {
    if (!killed) {
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) {
        ::kill(-pid, SIGKILL);
        killed = true;
        outcome.timed_out = true;
      }
    }
    pollfd descriptor{pipe_fd[0], POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, 200);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) continue;  // re-evaluate the deadline
    const ssize_t received = ::read(pipe_fd[0], buffer, sizeof(buffer));
    if (received < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (received == 0) break;  // child closed the pipe
    if (!killed) {
      if (written < options.max_log_bytes) {
        const std::size_t room = options.max_log_bytes - written;
        const std::size_t take = std::min(room, static_cast<std::size_t>(received));
        std::size_t offset = 0;
        while (offset < take) {
          const ssize_t w = ::write(log, buffer + offset, take - offset);
          if (w < 0 && errno == EINTR) continue;
          if (w <= 0) break;
          offset += static_cast<std::size_t>(w);
        }
        written += take;
        if (static_cast<std::size_t>(received) > take) outcome.truncated = true;
      } else {
        outcome.truncated = true;
      }
    }
  }
  ::close(pipe_fd[0]);
  ::close(log);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (outcome.timed_out) {
    outcome.exit_code = -1;
  } else if (WIFEXITED(status)) {
    outcome.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    outcome.exit_code = 128 + WTERMSIG(status);
  }
  return outcome;
}

struct PackOutcome {
  int exit_code = 0;
  bool truncated = false;
  bool spawn_failed = false;
  std::uint64_t bytes = 0;
};

// Runs `argv` (a tar creating to stdout), streaming its output into out_path up
// to `cap` bytes. Over the cap the child is killed and `truncated` is set. No
// sandbox: this packs the runner's own scratch tree, not untrusted execution.
PackOutcome packToFile(const std::vector<std::string>& argv, const std::filesystem::path& out_path,
                       std::size_t cap, unsigned timeout_seconds) {
  PackOutcome outcome;
  int pipe_fd[2];
  if (::pipe(pipe_fd) != 0) {
    outcome.spawn_failed = true;
    return outcome;
  }
  const int out = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (out < 0) {
    ::close(pipe_fd[0]);
    ::close(pipe_fd[1]);
    outcome.spawn_failed = true;
    return outcome;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipe_fd[0]);
    ::close(pipe_fd[1]);
    ::close(out);
    outcome.spawn_failed = true;
    return outcome;
  }
  if (pid == 0) {
    ::close(pipe_fd[0]);
    ::close(out);
    ::setpgid(0, 0);
    ::dup2(pipe_fd[1], STDOUT_FILENO);
    const int devnull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    ::close(pipe_fd[1]);
    std::vector<char*> raw;
    for (const std::string& argument : argv) raw.push_back(const_cast<char*>(argument.c_str()));
    raw.push_back(nullptr);
    ::execvp(raw[0], raw.data());
    _exit(127);
  }
  ::setpgid(pid, pid);
  ::close(pipe_fd[1]);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  bool killed = false;
  char buffer[65536];
  for (;;) {
    if (!killed && std::chrono::steady_clock::now() >= deadline) {
      ::kill(-pid, SIGKILL);
      killed = true;
      outcome.truncated = true;
    }
    pollfd descriptor{pipe_fd[0], POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, 200);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) continue;
    const ssize_t received = ::read(pipe_fd[0], buffer, sizeof(buffer));
    if (received < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (received == 0) break;
    if (killed) continue;
    if (outcome.bytes + static_cast<std::size_t>(received) > cap) {
      outcome.truncated = true;
      ::kill(-pid, SIGKILL);
      killed = true;
      continue;
    }
    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(received)) {
      const ssize_t written = ::write(out, buffer + offset, static_cast<std::size_t>(received) - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) {
        outcome.truncated = true;  // could not persist the bundle in full
        ::kill(-pid, SIGKILL);
        killed = true;
        break;
      }
      offset += static_cast<std::size_t>(written);
    }
    outcome.bytes += static_cast<std::size_t>(received);
  }
  ::close(pipe_fd[0]);
  ::close(out);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (WIFEXITED(status)) outcome.exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) outcome.exit_code = 128 + WTERMSIG(status);
  return outcome;
}

// Packs one job's declared artifact from the checkout into the run's artifact
// store and records it. `expires` of 0 stores it durably (a release asset);
// otherwise it is an ephemeral CI artifact the retention sweep may remove. A
// bundle that cannot be packed or is over the cap is recorded with a note.
void collectArtifact(const CiArtifact& artifact, const std::filesystem::path& work,
                     const CiRunnerOptions& options, const std::string& run_id, std::uint64_t created,
                     std::uint64_t expires, CiRunRecord& record) {
  CiArtifactRecord stored;
  stored.name = artifact.name;
  stored.created_epoch_seconds = created;
  stored.expires_epoch_seconds = expires;
  try {
    const std::filesystem::path dir =
        prepareCiArtifactDirectory(options.state_root, options.project_name, run_id);
    const std::filesystem::path blob = dir / (artifact.name + ".tar");
    std::vector<std::string> argv = {"tar", "-cf", "-", "-C", work.string(), "--"};
    for (const std::string& path : artifact.paths) argv.push_back(path);
    const PackOutcome outcome = packToFile(argv, blob, options.artifact_max_bytes, 300);
    std::error_code error;
    if (outcome.spawn_failed) {
      stored.note = "could not start packing the artifact";
    } else if (outcome.truncated) {
      std::filesystem::remove(blob, error);
      stored.note = "artifact exceeds the size cap";
    } else if (outcome.exit_code != 0) {
      std::filesystem::remove(blob, error);
      stored.note = "packing failed (a path may be missing)";
    } else {
      stored.bytes = outcome.bytes;
      stored.sha256 = sha256HexOfFile(blob);
    }
    writeCiArtifactRecord(options.state_root, options.project_name, run_id, stored);
    record.artifacts.push_back(stored);
  } catch (const std::exception&) {
    // Artifact collection never fails the build; the run already succeeded.
  }
}

std::string firstLine(const std::string& text) {
  const std::size_t newline = text.find('\n');
  std::string line = newline == std::string::npos ? text : text.substr(0, newline);
  if (line.size() > kMaximumCiDetailBytes) line.resize(kMaximumCiDetailBytes);
  return line;
}

}  // namespace

bool ciSandboxCompiledIn() {
#if defined(__linux__)
  return true;
#else
  return false;
#endif
}

CiRunRecord runCiWorkflow(const CiRunnerOptions& options, CiSandboxReport* sandbox) {
  if (!isValidProjectName(options.project_name)) throw std::runtime_error("ci runner: invalid project name");
  CiRunRecord record;
  record.run_id = generateCiId();
  record.project_name = options.project_name;
  record.ref = options.ref;
  record.commit_id = options.commit_id;
  record.status = CiRunStatus::Running;
  record.started_epoch_seconds = nowEpoch();

  CiSandboxReport report;
  report.namespaces_available = probeUserNamespaces(options.allow_network);
  report.network_isolated = report.namespaces_available && !options.allow_network;
  if (sandbox != nullptr) *sandbox = report;

  const auto finish = [&](CiRunStatus status, const std::string& detail) -> CiRunRecord {
    record.status = status;
    record.detail = firstLine(detail);
    record.finished_epoch_seconds = nowEpoch();
    writeCiRunRecord(options.state_root, record);
    return record;
  };

  CiWorkflow workflow;
  try {
    const std::optional<std::string> text = readWorkflowBlob(options.repository, options.commit_id);
    if (!text.has_value()) return finish(CiRunStatus::Skipped, "no .ckgit/ci.yml in this commit");
    workflow = parseCiWorkflow(*text);
  } catch (const std::exception& error) {
    return finish(CiRunStatus::Error, std::string("workflow: ") + error.what());
  }

  {
    // A workflow with an `on: { branches: [...] }` list triggers only on those
    // branches; a workflow that omits `on:` triggers only on the repository's
    // default branch (its HEAD). Any other pushed branch is recorded Skipped.
    const std::string branch = branchOf(options.ref);
    if (!workflow.branches.empty()) {
      if (std::find(workflow.branches.begin(), workflow.branches.end(), branch) == workflow.branches.end()) {
        return finish(CiRunStatus::Skipped, "branch " + branch + " is not a trigger");
      }
    } else if (const std::optional<std::string> def = defaultBranch(options.repository);
               def.has_value() && branch != *def) {
      return finish(CiRunStatus::Skipped, "branch " + branch + " is not the default branch " + *def);
    }
  }

  std::filesystem::path run_dir;
  std::filesystem::path scratch;
  try {
    run_dir = prepareCiRunDirectory(options.state_root, options.project_name, record.run_id);
    std::error_code error;
    std::filesystem::create_directories(options.build_root, error);
    scratch = options.build_root / record.run_id;
    std::filesystem::remove_all(scratch, error);
    std::filesystem::create_directories(scratch / "src", error);
    if (error) throw std::runtime_error("could not create the scratch workspace");
    checkoutCommit(options.repository, options.commit_id, scratch / "src", scratch / "commit.tar");
  } catch (const std::exception& error) {
    return finish(CiRunStatus::Error, std::string("setup: ") + error.what());
  }

  const std::filesystem::path work = scratch / "src";
  const std::filesystem::path home = scratch / "home";
  std::error_code home_error;
  std::filesystem::create_directories(home, home_error);

  CiRunStatus status = CiRunStatus::Success;
  std::string detail;
  std::size_t step_index = 0;
  bool stop = false;
  for (const CiJob& job : workflow.jobs) {
    if (stop) break;
    const std::vector<std::string> env = buildEnv(workflow, job, options, home);
    for (const CiStep& step : job.steps) {
      const std::filesystem::path log_path = run_dir / "steps" / (std::to_string(step_index) + ".log");
      std::vector<std::string> argv;
      if (step.usesShell()) {
        argv = {"/bin/sh", "-ec", step.script};
      } else {
        argv = step.argv;
      }
      const StepOutcome outcome = executeStep(argv, work, env, options, log_path);
      CiStepResult result;
      result.name = step.name.empty() ? job.name : step.name;
      result.exit_code = outcome.exit_code;
      result.timed_out = outcome.timed_out;
      result.output_truncated = outcome.truncated;
      record.steps.push_back(result);
      ++step_index;
      if (outcome.spawn_failed) {
        status = CiRunStatus::Error;
        detail = "could not start step '" + result.name + "'";
        stop = true;
        break;
      }
      if (outcome.timed_out) {
        status = CiRunStatus::Timeout;
        detail = "step '" + result.name + "' exceeded its " +
                 std::to_string(options.timeout_seconds) + "s budget";
        stop = true;
        break;
      }
      if (outcome.exit_code != 0) {
        status = CiRunStatus::Failure;
        detail = "step '" + result.name + "' exited " + std::to_string(outcome.exit_code);
        stop = true;
        break;
      }
    }
    if (!stop && job.artifact.has_value()) {
      unsigned days = job.artifact->retention_days == 0 ? options.artifact_retention_days
                                                        : job.artifact->retention_days;
      if (options.artifact_max_retention_days != 0 && days > options.artifact_max_retention_days) {
        days = options.artifact_max_retention_days;
      }
      const std::uint64_t created = nowEpoch();
      const std::uint64_t expires = created + static_cast<std::uint64_t>(days) * 86400ull;
      collectArtifact(*job.artifact, work, options, record.run_id, created, expires, record);
    }
  }

  if (!options.keep_scratch) {
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
  }
  if (status == CiRunStatus::Success) detail = "all steps passed";
  return finish(status, detail);
}

}  // namespace ckgit
