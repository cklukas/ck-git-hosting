// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_runner.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
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
#include <net/if.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#endif

#include "ckgit/ci_store.hpp"
#include "ckgit/ci_workflow.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/pages_store.hpp"
#include "ckgit/process.hpp"
#include "ckgit/validation.hpp"

extern char** environ;

namespace ckgit {
namespace {

// When the Linux sandbox is active, the per-run scratch tree is bind-mounted
// here, so every step runs under one fixed path no matter where the scratch
// physically lives (a workstation temp dir, a Pi's build root, a CI prefix).
// A build and everything it renders — a checkout at /mnt/src, a tool's TMPDIR
// at /mnt/tmp, a sister at /mnt/<name> — then look identical on every run,
// which keeps generated output such as documentation screenshots reproducible
// without pinning the physical scratch location or its length. /mnt is the
// FHS temporary-mount directory and always exists on the target.
const std::filesystem::path kSandboxRoot = "/mnt";

// D6/WP3: what enterSandbox exposes and hides inside a step's mount
// namespace, beyond the scratch-at-kSandboxRoot bind it always does. Computed
// once per run (not per step, though it is applied fresh in every step's own
// namespace), so a step cannot see or write the CI spool, other projects'
// releases, Pages, other projects' caches, or the bare repositories directly
// — the point of D6: with only the scratch bind, none of that was actually
// true, since a step could still reach every one of those by absolute path.
struct SandboxMounts {
  // Real host path -> path relative to the scratch root where enterSandbox
  // must bind it before the scratch itself is bound to kSandboxRoot, so a
  // persistent cache still appears at the same fixed path
  // (kSandboxRoot/.cache/<name>) on every run regardless of where it
  // physically lives. Only persistent (cache_root-backed) caches need an
  // entry here: an ephemeral cache already lives inside the scratch tree and
  // is carried across by the scratch bind alone.
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> cache_binds;
  // Absolute host paths to cover with an empty, private tmpfs after the
  // scratch (and cache) binds are established. Order does not matter to the
  // caller; enterSandbox covers the longest paths first so a root nested
  // beneath another configured root is masked before its parent, and a path
  // that does not exist is silently skipped.
  std::vector<std::filesystem::path> hide;
};

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

// A tag trigger pattern matches by exact name or a single trailing-'*' prefix.
bool tagMatches(const std::string& tag, const std::string& pattern) {
  if (!pattern.empty() && pattern.back() == '*') {
    const std::string prefix = pattern.substr(0, pattern.size() - 1);
    return tag.size() >= prefix.size() && tag.compare(0, prefix.size(), prefix) == 0;
  }
  return tag == pattern;
}

bool anyTagMatches(const std::string& tag, const std::vector<std::string>& patterns) {
  for (const std::string& pattern : patterns) {
    if (tagMatches(tag, pattern)) return true;
  }
  return false;
}

// The message of an annotated tag object, for release notes. Empty for a
// lightweight tag (which is a commit, carrying no annotation) or on any error.
std::string readTagNotes(const std::filesystem::path& repository, const std::string& id) {
  ProcessOptions options;
  options.timeout = std::chrono::seconds(10);
  options.output_limit = 64 * 1024;
  const ProcessResult type =
      runProcess({"git", "--git-dir", repository.string(), "cat-file", "-t", id}, options);
  if (type.exit_code != 0 || type.timed_out) return {};
  std::string kind = type.output;
  while (!kind.empty() && (kind.back() == '\n' || kind.back() == '\r')) kind.pop_back();
  if (kind != "tag") return {};
  const ProcessResult object =
      runProcess({"git", "--git-dir", repository.string(), "cat-file", "-p", id}, options);
  if (object.exit_code != 0 || object.timed_out) return {};
  const std::size_t blank = object.output.find("\n\n");
  if (blank == std::string::npos) return {};
  std::string notes = object.output.substr(blank + 2);
  while (!notes.empty() && notes.back() == '\n') notes.pop_back();
  if (notes.size() > kMaximumReleaseNotesBytes) notes.resize(kMaximumReleaseNotesBytes);
  return notes;
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
                                  const CiRunnerOptions& options, const std::filesystem::path& home,
                                  const std::filesystem::path& tmp,
                                  const std::vector<std::string>& extra_run_env) {
  std::vector<std::pair<std::string, std::string>> ordered = {
      {"PATH", "/usr/local/bin:/usr/bin:/bin"},
      {"HOME", home.string()},
      {"TMPDIR", tmp.string()},
      {"LANG", "C"},
      {"LC_ALL", "C"},
      {"CKGIT_CI", "1"},
      {"CI", "true"},
      // The commit and ref under build, so a step can stamp a reliable version
      // into the artifacts (the sandbox has no .git for `git describe`).
      {"CKGIT_COMMIT", options.commit_id},
      {"CKGIT_REF", options.ref},
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
  // Runner-provided paths (sisters, caches) last: a workflow cannot shadow the
  // CKGIT_SISTER_<NAME> / CKGIT_CACHE_<NAME> exports or a cache's bound vars.
  for (const std::string& assignment : extra_run_env) {
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

// Brings the "lo" interface up in the calling process's current network
// namespace. A freshly unshared (CLONE_NEWNET) namespace starts with loopback
// down, which otherwise makes 127.0.0.1/::1 unreachable even though the LAN is
// correctly denied — this is what let a step reach the network at all. Needs no
// host privilege: CLONE_NEWUSER made this process uid 0 inside the user
// namespace that owns the new network namespace, and that is what
// CAP_NET_ADMIN over "lo" is evaluated against.
bool bringUpLoopback() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  ifreq request{};
  std::strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);
  if (::ioctl(fd, SIOCGIFFLAGS, &request) != 0) {
    ::close(fd);
    return false;
  }
  request.ifr_flags |= IFF_UP | IFF_RUNNING;
  const bool up = ::ioctl(fd, SIOCSIFFLAGS, &request) == 0;
  ::close(fd);
  return up;
}

// In the freshly forked child: enter unprivileged user, mount, and (unless
// allowed) network namespaces, so the step has no network and cannot see the
// host mount table. Returns false if the kernel denies unprivileged namespaces.
bool enterSandbox(bool allow_network, const std::filesystem::path& bind_source,
                  const SandboxMounts& mounts) {
  const uid_t uid = ::getuid();
  const gid_t gid = ::getgid();
  int flags = CLONE_NEWUSER | CLONE_NEWNS;
  if (!allow_network) flags |= CLONE_NEWNET;
  if (::unshare(flags) != 0) return false;
  writeProcFile("/proc/self/setgroups", "deny");
  writeProcFile("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1\n");
  writeProcFile("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1\n");
  // A fresh network namespace has no interfaces up at all; bring loopback up so
  // a step can still reach its own local server, database, or test fixture on
  // 127.0.0.1/::1. Best-effort like the mounts below: a systemic failure here
  // is caught early by probeLoopback() into CiSandboxReport, not discovered
  // silently the first time a step's own loopback traffic fails.
  if (!allow_network) bringUpLoopback();
  // Keep our mount changes from propagating back to the host namespace.
  ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
  if (!bind_source.empty()) {
    // Bind each persistent cache into the scratch tree at its fixed relative
    // location first, so it is carried into kSandboxRoot by the whole-scratch
    // bind right below — the same fixed path (kSandboxRoot/.cache/<name>) on
    // every run, whether or not the cache is actually persistent.
    for (const auto& [real, relative] : mounts.cache_binds) {
      const std::filesystem::path target = bind_source / relative;
      // Already created by provisionCaches in the parent; defensive here.
      std::error_code error;
      std::filesystem::create_directories(target, error);
      ::mount(real.c_str(), target.c_str(), nullptr, MS_BIND | MS_REC, nullptr);
    }
    // Present the scratch tree at one fixed path (kSandboxRoot). The bind
    // lives only in this namespace: it never touches the host and is gone
    // when the step exits. With the same privileges that carried the unshare
    // it does not fail in practice; the caller runs the step with
    // kSandboxRoot as its cwd.
    ::mount(bind_source.c_str(), kSandboxRoot.c_str(), nullptr, MS_BIND | MS_REC, nullptr);
  }
  // Cover the service tree — the state root, the CI scratch parent, the
  // cache root, Pages, and the bare-repository root — with an empty, private
  // tmpfs at each real absolute path, so a step cannot read or write any of
  // it directly, only through the sanctioned channels above (its own
  // checkout, sisters, its own cache). Longest path first, so a root nested
  // under another configured root is covered before its parent; a bind
  // established above (the scratch itself living under the CI build root, or
  // a persistent cache living under the cache root) keeps working after its
  // source is covered, because a bind mount is resolved once, at the point it
  // is established, not by the path string afterward.
  std::vector<std::filesystem::path> hide = mounts.hide;
  std::sort(hide.begin(), hide.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
    return a.native().size() > b.native().size();
  });
  for (const std::filesystem::path& path : hide) {
    if (path.empty()) continue;
    std::error_code error;
    if (!std::filesystem::exists(path, error)) continue;
    ::mount("tmpfs", path.c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700,size=65536");
  }
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

// Whether the isolated per-step network namespace's loopback interface can be
// brought up on this host, probed directly (its own unshare + bring-up, not a
// whole run) so a systemic failure shows up once in CiSandboxReport rather
// than being discovered only when a step's own loopback traffic silently
// fails. Only meaningful when that namespace is actually created; the caller
// short-circuits this when it is not (allow_network, or namespaces_available
// is already false).
bool probeLoopback() {
  const pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    if (::unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) _exit(1);
    writeProcFile("/proc/self/setgroups", "deny");
    writeProcFile("/proc/self/uid_map", "0 " + std::to_string(::getuid()) + " 1\n");
    writeProcFile("/proc/self/gid_map", "0 " + std::to_string(::getgid()) + " 1\n");
    _exit(bringUpLoopback() ? 0 : 1);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// D6/WP3: whether an unprivileged mount namespace here can actually create a
// tmpfs mount and have it take effect — the mechanism enterSandbox uses to
// cover the service tree. Probed generically (its own throwaway directory,
// not any real run's paths) once per run, the same way probeUserNamespaces
// checks namespace creation and probeLoopback checks loopback: a kernel or
// LSM that silently refuses this would otherwise leave every hide/cache_binds
// mount in enterSandbox a harmless no-op, and a step would see the real,
// unmasked service tree with nothing to say so.
bool probeFilesystemMask() {
  char pattern[] = "/tmp/ckgit-mask-probe-XXXXXX";
  if (::mkdtemp(pattern) == nullptr) return false;
  const std::string probe_dir = pattern;
  const std::string sentinel = probe_dir + "/sentinel";
  const int sentinel_fd = ::open(sentinel.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (sentinel_fd >= 0) {
    const ssize_t written = ::write(sentinel_fd, "x", 1);
    ::close(sentinel_fd);
    (void)written;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    std::error_code error;
    std::filesystem::remove_all(probe_dir, error);
    return false;
  }
  if (pid == 0) {
    if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) _exit(1);
    writeProcFile("/proc/self/setgroups", "deny");
    writeProcFile("/proc/self/uid_map", "0 " + std::to_string(::getuid()) + " 1\n");
    writeProcFile("/proc/self/gid_map", "0 " + std::to_string(::getgid()) + " 1\n");
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) _exit(1);
    if (::mount("tmpfs", probe_dir.c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700,size=65536") != 0) {
      _exit(1);
    }
    // The mount must actually take effect in this namespace: the sentinel
    // written into probe_dir before the fork must now be hidden.
    struct stat info {};
    _exit(::stat(sentinel.c_str(), &info) == 0 ? 1 : 0);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  // The child's mount lived only in its own (now-exited) mount namespace; the
  // parent's view of probe_dir, including the sentinel, was never touched.
  std::error_code error;
  std::filesystem::remove_all(probe_dir, error);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#else
bool enterSandbox(bool, const std::filesystem::path&, const SandboxMounts&) { return false; }
bool probeUserNamespaces(bool) { return false; }
bool probeLoopback() { return false; }
bool probeFilesystemMask() { return false; }
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
  bool cancelled = false;
};

// Progress callbacks a running step reports through, polled roughly once a
// second between output reads. `heartbeat` advances the run's liveness stamp;
// `cancel_requested` returns true when an operator asked to stop the run, and
// the runner then kills the step's process group. Either may be empty.
struct StepHooks {
  std::function<void()> heartbeat;
  std::function<bool()> cancel_requested;
};

StepOutcome executeStep(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                        const std::vector<std::string>& env, const CiRunnerOptions& options,
                        const std::filesystem::path& log_path, const std::filesystem::path& bind_source,
                        const SandboxMounts& mounts, const StepHooks& hooks) {
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
    enterSandbox(options.allow_network, bind_source, mounts);
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
  auto last_hook = std::chrono::steady_clock::now();
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
    // Roughly once a second, report liveness and honour a cancel request. A
    // cancel kills the whole step process group, exactly as a timeout does.
    if (!killed && std::chrono::steady_clock::now() - last_hook >= std::chrono::seconds(1)) {
      last_hook = std::chrono::steady_clock::now();
      if (hooks.heartbeat) hooks.heartbeat();
      if (hooks.cancel_requested && hooks.cancel_requested()) {
        ::kill(-pid, SIGKILL);
        killed = true;
        outcome.cancelled = true;
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
  if (outcome.timed_out || outcome.cancelled) {
    outcome.exit_code = -1;  // the step was killed, not exited: its code is meaningless
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

// Packs one job's declared artifact from the checkout into `dir` as <name>.tar
// and returns its record. `expires` of 0 marks a durable release asset;
// otherwise it is an ephemeral CI artifact. A bundle that cannot be packed or is
// over the cap is returned with a note and no stored bytes. The caller writes
// the sidecar (which commits the artifact) and never lets this fail the build.
CiArtifactRecord packArtifactInto(const CiArtifact& artifact, const std::filesystem::path& work,
                                  const std::filesystem::path& dir, std::size_t max_bytes,
                                  std::uint64_t created, std::uint64_t expires) {
  CiArtifactRecord stored;
  stored.name = artifact.name;
  stored.created_epoch_seconds = created;
  stored.expires_epoch_seconds = expires;
  const std::filesystem::path blob = dir / (artifact.name + ".tar");
  std::vector<std::string> argv = {"tar", "-cf", "-", "-C", work.string(), "--"};
  for (const std::string& path : artifact.paths) argv.push_back(path);
  const PackOutcome outcome = packToFile(argv, blob, max_bytes, 300);
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
  return stored;
}

std::string firstLine(const std::string& text) {
  const std::size_t newline = text.find('\n');
  std::string line = newline == std::string::npos ? text : text.substr(0, newline);
  if (line.size() > kMaximumCiDetailBytes) line.resize(kMaximumCiDetailBytes);
  return line;
}

// The suffix of a sister's CKGIT_SISTER_<NAME> variable: the project name
// uppercased with every non-alphanumeric byte mapped to '_', so it is a valid
// shell variable name.
std::string sisterEnvName(const std::string& name) {
  std::string out = "CKGIT_SISTER_";
  for (const unsigned char c : name) {
    out.push_back(std::isalnum(c) ? static_cast<char>(std::toupper(c)) : '_');
  }
  return out;
}

// Materialises each declared sister project's snapshot into the scratch tree at
// scratch/<name> (reached from the checkout as ../<name>), read-only via `git
// archive` — no hooks, no submodules, no network, same-server projects only.
// `ref` pins a sister to a branch, tag, or commit; empty takes the project's
// current default branch (its bare HEAD). Returns the CKGIT_SISTER_<NAME>=<path>
// assignments to export, where <path> is where the step will see the sister:
// under `visible_root` (the canonical kSandboxRoot when sandboxed, else the
// physical scratch). Throws with a clear message when a named sister is not a
// hosted repository or a pinned ref does not resolve.
std::vector<std::string> materializeSisters(const CiWorkflow& workflow,
                                            const std::filesystem::path& repository,
                                            const std::string& self,
                                            const std::filesystem::path& scratch,
                                            const std::filesystem::path& visible_root) {
  std::vector<std::string> env;
  const std::filesystem::path repo_root = repository.parent_path();
  for (const CiSister& sister : workflow.sisters) {
    if (sister.name == self) throw std::runtime_error("a project cannot list itself as a sister: " + sister.name);
    if (sister.name == "src" || sister.name == "home" || sister.name == "tmp") {
      throw std::runtime_error("reserved sister name: " + sister.name);
    }
    const std::filesystem::path sister_repo = repo_root / (sister.name + ".git");
    if (!std::filesystem::is_directory(sister_repo)) {
      throw std::runtime_error("sister project '" + sister.name + "' is not hosted here");
    }
    // Resolve the requested ref (or the default branch) to a single commit.
    const std::string spec = sister.ref.empty() ? "HEAD" : sister.ref;
    ProcessOptions resolve;
    resolve.timeout = std::chrono::seconds(20);
    resolve.output_limit = 256;
    const ProcessResult resolved = runProcess(
        {"git", "--git-dir", sister_repo.string(), "rev-parse", "--verify", "--end-of-options",
         spec + "^{commit}"},
        resolve);
    if (resolved.timed_out || resolved.exit_code != 0) {
      throw std::runtime_error("sister project '" + sister.name + "' has no " +
                               (sister.ref.empty() ? "commits" : "ref '" + sister.ref + "'"));
    }
    std::string commit = resolved.output;
    while (!commit.empty() && (commit.back() == '\n' || commit.back() == '\r')) commit.pop_back();
    const std::filesystem::path dest = scratch / sister.name;
    std::error_code error;
    std::filesystem::create_directories(dest, error);
    if (error) throw std::runtime_error("could not create the sister workspace for '" + sister.name + "'");
    checkoutCommit(sister_repo, commit, dest, scratch / (sister.name + ".sister.tar"));
    env.push_back(sisterEnvName(sister.name) + "=" + (visible_root / sister.name).string());
  }
  return env;
}

// The default variable a cache is exported as: CKGIT_CACHE_<NAME>, the name
// uppercased with non-alphanumeric bytes mapped to '_'.
std::string cacheEnvName(const std::string& name) {
  std::string out = "CKGIT_CACHE_";
  for (const unsigned char c : name) {
    out.push_back(std::isalnum(c) ? static_cast<char>(std::toupper(c)) : '_');
  }
  return out;
}

// The result of provisioning a workflow's declared caches: the environment
// pointing steps at each one, plus (D6/WP3) the real-path -> scratch-relative
// bind that enterSandbox must establish, each step, for a *persistent*
// (cache_root-backed) cache under the Linux sandbox, so it still appears at
// the same fixed path (visible_root/.cache/<name>) as an ephemeral one would.
// Empty in degraded mode (no Linux sandbox at all) and for an ephemeral
// cache, which is already inside the scratch tree the whole-scratch bind
// carries across — see provisionCaches for exactly when an entry is added.
struct CacheProvision {
  std::vector<std::string> env;
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> mounts;
};

// Provisions each declared build cache. With a configured cache_root a cache
// persists across runs at cache_root/<project>/<name>; without one it falls
// back to a directory inside the scratch (so the workflow still runs, only
// without persistence). Under the Linux sandbox a step always sees it at the
// same fixed path — visible_root/.cache/<name> — so a workflow does not need
// to know whether this server configured persistence; the directory is then,
// together with the scratch checkout itself, the only place a step can write
// (see the hide list built in runCiWorkflow). In degraded mode (no Linux
// sandbox: no bind is available to redirect a fixed path to) a persistent
// cache is exported at its real, persistent path directly instead.
CacheProvision provisionCaches(const CiWorkflow& workflow, const std::string& project,
                               const std::filesystem::path& cache_root,
                               const std::filesystem::path& scratch,
                               const std::filesystem::path& visible_root) {
  // The scratch-at-kSandboxRoot bind (established in enterSandbox, per step)
  // is what makes a fixed visible_root/.cache/<name> path meaningful. Without
  // it -- degraded mode: no Linux sandbox at all, so visible_root is scratch
  // itself -- a step runs directly in the real filesystem and there is no
  // bind to redirect a persistent cache's fixed path to; it must be exported
  // at its real, persistent path instead, exactly as before this cache also
  // supported the fixed-path form.
  const bool sandboxed = visible_root != scratch;
  CacheProvision result;
  for (const CiCache& cache : workflow.caches) {
    const std::filesystem::path relative = std::filesystem::path(".cache") / cache.name;
    std::filesystem::path visible;
    std::error_code error;
    if (!cache_root.empty()) {
      const std::filesystem::path real = cache_root / project / cache.name;
      std::filesystem::create_directories(real, error);
      if (error) throw std::runtime_error("could not create the cache '" + cache.name + "'");
      if (sandboxed) {
        // A mount point for enterSandbox's bind, created empty here; its
        // content comes from `real` only inside each step's own namespace.
        // This is what makes a persistent cache appear at the same fixed
        // path every run, exactly like an ephemeral one below.
        std::filesystem::create_directories(scratch / relative, error);
        if (error) throw std::runtime_error("could not create a mount point for cache '" + cache.name + "'");
        result.mounts.emplace_back(real, relative);
        visible = visible_root / relative;
      } else {
        visible = real;  // no bind is coming; the real path is the only path
      }
    } else {
      std::filesystem::create_directories(scratch / relative, error);  // ephemeral, wiped with the run
      if (error) throw std::runtime_error("could not create the cache '" + cache.name + "'");
      visible = visible_root / relative;  // == scratch/relative in degraded mode: already correct
    }
    const std::string path = visible.string();
    result.env.push_back(cacheEnvName(cache.name) + "=" + path);
    for (const std::string& var : cache.env) result.env.push_back(var + "=" + path);
  }
  return result;
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
  record.run_id = options.run_id.empty() ? generateCiId() : options.run_id;
  record.project_name = options.project_name;
  record.ref = options.ref;
  record.commit_id = options.commit_id;
  record.status = CiRunStatus::Running;
  record.started_epoch_seconds = nowEpoch();

  const auto finish = [&](CiRunStatus status, const std::string& detail) -> CiRunRecord {
    record.status = status;
    record.detail = firstLine(detail);
    record.finished_epoch_seconds = nowEpoch();
    record.heartbeat_epoch_seconds = record.finished_epoch_seconds;
    writeCiRunRecord(options.state_root, record);
    // A cancel marker never outlives the run it targeted.
    try {
      clearCiCancel(options.state_root, options.project_name, record.run_id);
    } catch (const std::exception&) {
    }
    return record;
  };

  // A queued job can be cancelled while it still sits in the spool, before any
  // runner claims it: the dashboard publishes it as Pending (same run_id) as
  // soon as it is queued, and its Cancel button writes the ordinary marker.
  // Honor that before doing anything else — no sandbox probe, no checkout — so
  // the record goes straight from Pending to Cancelled and never shows Running.
  try {
    if (isCiCancelRequested(options.state_root, options.project_name, record.run_id)) {
      return finish(CiRunStatus::Cancelled, "cancelled before it started");
    }
  } catch (const std::exception&) {
  }

  CiSandboxReport report;
  report.namespaces_available = probeUserNamespaces(options.allow_network);
  report.network_isolated = report.namespaces_available && !options.allow_network;
  // Loopback only needs its own probe when the isolated network namespace is
  // actually created (network_isolated): otherwise a step shares the host's
  // namespace (allow_network, or namespaces unavailable at all in degraded
  // mode) and its loopback trivially works, same as the host's.
  report.loopback_available = !report.network_isolated || probeLoopback();
  // D6/WP3: the tmpfs/bind masking below is Linux-only, exactly like the
  // namespaces it runs inside; there is nothing to probe when they are
  // unavailable, since no masking mount is attempted at all in that case.
  report.filesystem_masked = report.namespaces_available && probeFilesystemMask();
  if (sandbox != nullptr) *sandbox = report;

  CiWorkflow workflow;
  try {
    const std::optional<std::string> text = readWorkflowBlob(options.repository, options.commit_id);
    if (!text.has_value()) return finish(CiRunStatus::Skipped, "no .ckgit/ci.yml in this commit");
    workflow = parseCiWorkflow(*text);
  } catch (const std::exception& error) {
    return finish(CiRunStatus::Error, std::string("workflow: ") + error.what());
  }

  // Decide whether this ref triggers the workflow, and whether it is a release.
  // Tag pushes match `on.tags`; branch pushes match `on.branches`, or the
  // repository's default branch when the workflow omits `on:` entirely.
  const std::string kTagPrefix = "refs/tags/";
  const bool is_tag = options.ref.rfind(kTagPrefix, 0) == 0;
  bool is_release = false;
  std::string release_tag;
  if (is_tag) {
    const std::string tag = options.ref.substr(kTagPrefix.size());
    if (!anyTagMatches(tag, workflow.tags)) {
      return finish(CiRunStatus::Skipped, "tag " + tag + " is not a trigger");
    }
    if (!isValidReleaseTag(tag)) {
      return finish(CiRunStatus::Skipped, "tag " + tag + " is not a supported release tag");
    }
    is_release = true;
    release_tag = tag;
  } else {
    const std::string branch = branchOf(options.ref);
    if (workflow.triggers_default_branch) {
      const std::optional<std::string> def = defaultBranch(options.repository);
      if (def.has_value() && branch != *def) {
        return finish(CiRunStatus::Skipped, "branch " + branch + " is not the default branch " + *def);
      }
    } else if (std::find(workflow.branches.begin(), workflow.branches.end(), branch) ==
               workflow.branches.end()) {
      return finish(CiRunStatus::Skipped, "branch " + branch + " is not a trigger");
    }
  }

  std::filesystem::path run_dir;
  std::filesystem::path scratch;
  try {
    run_dir = prepareCiRunDirectory(options.state_root, options.project_name, record.run_id);
    // Publish the run as Running before any slow setup (checkout, sisters,
    // caches) so the dashboard shows it immediately, then let the daemon refresh
    // its cache rather than wait for the next periodic sweep.
    record.heartbeat_epoch_seconds = record.started_epoch_seconds;
    writeCiRunRecord(options.state_root, record);
    if (options.on_run_started) options.on_run_started();
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

  const std::filesystem::path work = scratch / "src";  // physical: host-side packing/publishing

  // Where a running step sees the tree. When the Linux sandbox is active its
  // bind mount presents the scratch at one fixed canonical path (kSandboxRoot),
  // so cwd, HOME, TMPDIR and every sister look identical on every run; without
  // the sandbox the step uses the physical scratch directly. Holding this
  // constant is what keeps a build's rendered paths reproducible.
  const bool sandbox_active = report.namespaces_available;
  const std::filesystem::path visible_root = sandbox_active ? kSandboxRoot : scratch;
  const std::filesystem::path bind_source = sandbox_active ? scratch : std::filesystem::path{};
  const std::filesystem::path work_visible = visible_root / "src";
  const std::filesystem::path home_visible = visible_root / "home";
  const std::filesystem::path tmp_visible = visible_root / "tmp";
  std::error_code scratch_error;
  std::filesystem::create_directories(scratch / "home", scratch_error);
  std::filesystem::create_directories(scratch / "tmp", scratch_error);

  // Materialise the sister projects this workflow declared before any step runs.
  std::vector<std::string> sister_env;
  try {
    sister_env = materializeSisters(workflow, options.repository, options.project_name, scratch, visible_root);
  } catch (const std::exception& error) {
    return finish(CiRunStatus::Error, std::string("sisters: ") + error.what());
  }
  // Provision persistent build caches (a warm ccache store, say), which survive
  // across runs when the server configures a cache root.
  CacheProvision cache_provision;
  try {
    cache_provision = provisionCaches(workflow, options.project_name, options.cache_root, scratch, visible_root);
    sister_env.insert(sister_env.end(), cache_provision.env.begin(), cache_provision.env.end());
  } catch (const std::exception& error) {
    return finish(CiRunStatus::Error, std::string("cache: ") + error.what());
  }

  // D6/WP3: the service tree a step must never see or write directly,
  // regardless of where the scratch physically lives — this is what makes the
  // checkout and a declared cache "the only places a step can write" actually
  // true, not just documented. Built once per run (not per step, though
  // enterSandbox applies it fresh in every step's own namespace): the private
  // state root (spool, run records, other projects' releases), the CI build
  // root (sibling runs' scratch), the cache root (other projects' caches —
  // this project's own is exempted via the bind above), Pages, and the
  // bare-repository root (every hosted project's real Git data; sisters are
  // the sanctioned, read-only, git-archived way to reach another one). Only
  // non-empty, existing paths are included; enterSandbox silently skips the
  // rest.
  SandboxMounts sandbox_mounts;
  sandbox_mounts.cache_binds = cache_provision.mounts;
  for (const std::filesystem::path* candidate : {&options.state_root, &options.pages_root}) {
    if (!candidate->empty()) sandbox_mounts.hide.push_back(*candidate);
  }
  if (!options.repository.empty()) sandbox_mounts.hide.push_back(options.repository.parent_path());

  // Do not mount over the whole build root: the active scratch lives beneath
  // it and is already bound at /mnt for the step.  On GitHub-hosted Linux
  // runners, covering that parent invalidates the descendant bind, leaving
  // /mnt/src unreachable (and every otherwise-valid step exits 126).  Cover
  // each sibling run instead.  They remain inaccessible while the current
  // run's source, HOME, and TMPDIR stay available through the sanctioned bind.
  const auto hideSiblings = [&](const std::filesystem::path& root,
                                const std::filesystem::path& keep) {
    if (root.empty()) return;
    std::error_code error;
    std::filesystem::directory_iterator iterator(root, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
      const std::filesystem::path child = iterator->path();
      if (child != keep) sandbox_mounts.hide.push_back(child);
      iterator.increment(error);
    }
  };
  hideSiblings(options.build_root, scratch);
  // A project can use only the caches it declares, but it must never access
  // another project's cache tree.  Leave its own parent reachable so a
  // declared cache bind is not obscured by the sibling masking.
  hideSiblings(options.cache_root, options.cache_root / options.project_name);

  // Liveness and cancellation share one small surface. writeProgress republishes
  // the run.ini (Running plus the steps finished so far) with a fresh heartbeat,
  // throttled unless forced; cancelRequested polls the cancel marker.
  std::uint64_t last_progress = record.started_epoch_seconds;
  const auto writeProgress = [&](bool force) {
    const std::uint64_t now = nowEpoch();
    if (!force && now - last_progress < 3) return;  // bound fsync churn on chatty steps
    last_progress = now;
    record.heartbeat_epoch_seconds = now;
    try {
      writeCiRunRecord(options.state_root, record);
    } catch (const std::exception&) {
      // A dropped heartbeat only delays liveness; the terminal write is authoritative.
    }
  };
  const auto cancelRequested = [&]() -> bool {
    try {
      return isCiCancelRequested(options.state_root, options.project_name, record.run_id);
    } catch (const std::exception&) {
      return false;
    }
  };
  const StepHooks hooks{[&]() { writeProgress(false); }, cancelRequested};

  CiRunStatus status = CiRunStatus::Success;
  std::string detail;
  std::size_t step_index = 0;
  bool stop = false;
  for (const CiJob& job : workflow.jobs) {
    if (stop) break;
    if (cancelRequested()) {
      status = CiRunStatus::Cancelled;
      detail = "run cancelled before job '" + job.name + "'";
      break;
    }
    const std::vector<std::string> env =
        buildEnv(workflow, job, options, home_visible, tmp_visible, sister_env);
    for (const CiStep& step : job.steps) {
      if (cancelRequested()) {
        status = CiRunStatus::Cancelled;
        detail = "run cancelled before step '" + (step.name.empty() ? job.name : step.name) + "'";
        stop = true;
        break;
      }
      const std::filesystem::path log_path = run_dir / "steps" / (std::to_string(step_index) + ".log");
      std::vector<std::string> argv;
      if (step.usesShell()) {
        argv = {"/bin/sh", "-ec", step.script};
      } else {
        argv = step.argv;
      }
      const StepOutcome outcome =
          executeStep(argv, work_visible, env, options, log_path, bind_source, sandbox_mounts, hooks);
      CiStepResult result;
      result.name = step.name.empty() ? job.name : step.name;
      result.exit_code = outcome.exit_code;
      result.timed_out = outcome.timed_out;
      result.output_truncated = outcome.truncated;
      record.steps.push_back(result);
      ++step_index;
      writeProgress(true);  // publish the just-finished step promptly
      if (outcome.cancelled) {
        status = CiRunStatus::Cancelled;
        detail = "run cancelled during step '" + result.name + "'";
        stop = true;
        break;
      }
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
      const std::uint64_t created = nowEpoch();
      try {
        if (is_release) {
          const std::filesystem::path dir =
              prepareCiReleaseDirectory(options.state_root, options.project_name, release_tag);
          const CiArtifactRecord stored =
              packArtifactInto(*job.artifact, work, dir, options.artifact_max_bytes, created, /*expires=*/0);
          writeCiReleaseArtifactRecord(options.state_root, options.project_name, release_tag, stored);
          record.artifacts.push_back(stored);
        } else {
          unsigned days = job.artifact->retention_days == 0 ? options.artifact_retention_days
                                                            : job.artifact->retention_days;
          if (options.artifact_max_retention_days != 0 && days > options.artifact_max_retention_days) {
            days = options.artifact_max_retention_days;
          }
          const std::uint64_t expires = created + static_cast<std::uint64_t>(days) * 86400ull;
          const std::filesystem::path dir =
              prepareCiArtifactDirectory(options.state_root, options.project_name, record.run_id);
          const CiArtifactRecord stored =
              packArtifactInto(*job.artifact, work, dir, options.artifact_max_bytes, created, expires);
          writeCiArtifactRecord(options.state_root, options.project_name, record.run_id, stored);
          record.artifacts.push_back(stored);
        }
      } catch (const std::exception&) {
        // Artifact collection never fails the build; the run already succeeded.
      }
    }
  }

  // A tag build publishes a release on success, and publishes nothing (cleaning
  // up any partial assets) when it fails, so a release is always complete.
  if (is_release) {
    if (status == CiRunStatus::Success) {
      try {
        CiReleaseRecord release;
        release.tag = release_tag;
        release.commit_id = options.commit_id;
        release.created_epoch_seconds = nowEpoch();
        release.notes = readTagNotes(options.repository, options.commit_id);
        writeCiReleaseRecord(options.state_root, options.project_name, release);
      } catch (const std::exception&) {
      }
    } else {
      try {
        removeCiRelease(options.state_root, options.project_name, release_tag);
      } catch (const std::exception&) {
      }
    }
  }

  // A successful default-branch build publishes its declared Pages directory as
  // the project's site. Only the default branch may replace the live site.
  if (status == CiRunStatus::Success && !is_release && workflow.pages_path.has_value() &&
      !options.pages_root.empty()) {
    const std::optional<std::string> def = defaultBranch(options.repository);
    if (def.has_value() && branchOf(options.ref) == *def) {
      try {
        const std::filesystem::path site = work / *workflow.pages_path;
        std::error_code error;
        if (std::filesystem::is_directory(site, error)) {
          publishPagesSite(options.pages_root, options.project_name, record.run_id, site,
                           options.pages_keep_versions);
        }
      } catch (const std::exception& publish_error) {
        // A build whose steps all passed but whose site failed to publish would
        // otherwise report success while the live site silently stayed on its
        // previous version. Fail the run so the broken publish is never hidden.
        status = CiRunStatus::Error;
        detail = std::string("steps passed but the Pages site failed to publish: ") + publish_error.what();
      }
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
