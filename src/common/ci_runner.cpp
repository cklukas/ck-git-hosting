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

#include "ckgit/ci_results.hpp"
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


// Base + workflow + job + caller env, later definitions winning, as KEY=VALUE.
std::vector<std::string> buildEnv(const CiWorkflow& workflow, const CiJob& ci_job, const CiJobRequest& job,
                                  const std::vector<std::string>& extra_env, const std::filesystem::path& home,
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
      // into the artifacts even from a depth-1 checkout or `mode: archive`.
      {"CKGIT_COMMIT", job.commit_id},
      {"CKGIT_REF", job.ref},
  };
  if (job.ref.rfind("refs/heads/", 0) == 0) ordered.emplace_back("CKGIT_BRANCH", job.ref.substr(11));
  if (job.ref.rfind("refs/tags/", 0) == 0) ordered.emplace_back("CKGIT_TAG", job.ref.substr(10));
  const auto put = [&ordered](const std::string& key, const std::string& value) {
    for (auto& entry : ordered) {
      if (entry.first == key) { entry.second = value; return; }
    }
    ordered.emplace_back(key, value);
  };
  for (const auto& [key, value] : workflow.env) put(key, value);
  for (const auto& [key, value] : ci_job.env) put(key, value);
  for (const std::string& assignment : extra_env) {
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

// A user/mount namespace alone is not enough to promise the canonical /mnt
// workspace: some hardened Linux hosts (including GitHub-hosted runners) let
// an unprivileged user create the namespace and mount tmpfs, but reject a bind
// of the runner-owned checkout.  Probe the exact operation after the scratch
// exists, so a step can fall back to that physical scratch rather than dying
// at chdir(/mnt/src) with exit 126.  The namespace and service-tree masks stay
// active in that fallback; only the cosmetic fixed workspace path is absent.
bool probeSandboxWorkspace(const std::filesystem::path& source) {
  const pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) _exit(1);
    writeProcFile("/proc/self/setgroups", "deny");
    writeProcFile("/proc/self/uid_map", "0 " + std::to_string(::getuid()) + " 1\n");
    writeProcFile("/proc/self/gid_map", "0 " + std::to_string(::getgid()) + " 1\n");
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) _exit(1);
    if (::mount(source.c_str(), kSandboxRoot.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) _exit(1);
    struct stat info {};
    _exit(::stat((kSandboxRoot / "src").c_str(), &info) == 0 && S_ISDIR(info.st_mode) ? 0 : 1);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#else
bool enterSandbox(bool, const std::filesystem::path&, const SandboxMounts&) { return false; }
bool probeUserNamespaces(bool) { return false; }
bool probeLoopback() { return false; }
bool probeFilesystemMask() { return false; }
bool probeSandboxWorkspace(const std::filesystem::path&) { return false; }
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
  std::function<void()> tick;  // every poll round (<= 200 ms): lets the sink flush output
};

// Runs one step in the sandbox, streaming its combined output to the sink (up
// to the log cap) and reporting liveness and cancellation through `hooks`.
StepOutcome executeStep(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                        const std::vector<std::string>& env, const CiExecutionOptions& options,
                        CiResultSink& sink, std::size_t step_index, const std::filesystem::path& bind_source,
                        const SandboxMounts& mounts, const StepHooks& hooks) {
  StepOutcome outcome;
  int pipe_fd[2];
  if (::pipe(pipe_fd) != 0) {
    outcome.spawn_failed = true;
    return outcome;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipe_fd[0]);
    ::close(pipe_fd[1]);
    outcome.spawn_failed = true;
    return outcome;
  }
  if (pid == 0) {
    ::close(pipe_fd[0]);
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
    if (hooks.tick) hooks.tick();
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
        sink.appendLog(step_index, std::string_view(buffer, take));
        written += take;
        if (static_cast<std::size_t>(received) > take) outcome.truncated = true;
      } else {
        outcome.truncated = true;
      }
    }
  }
  ::close(pipe_fd[0]);
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

// Packs one job's declared artifact from the checkout into `bundle` (a tar).
// Returns an empty string on success, or the short note recorded instead of
// the bundle when it cannot be packed or is over the cap (and then removes any
// partial bundle). Never fails the build.
std::string packArtifact(const CiArtifact& artifact, const std::filesystem::path& work,
                         const std::filesystem::path& bundle, std::size_t max_bytes) {
  std::vector<std::string> argv = {"tar", "-cf", "-", "-C", work.string(), "--"};
  for (const std::string& path : artifact.paths) argv.push_back(path);
  const PackOutcome outcome = packToFile(argv, bundle, max_bytes, 300);
  std::error_code error;
  if (outcome.spawn_failed) return "could not start packing the artifact";
  if (outcome.truncated) {
    std::filesystem::remove(bundle, error);
    return "artifact exceeds the size cap";
  }
  if (outcome.exit_code != 0) {
    std::filesystem::remove(bundle, error);
    return "packing failed (a path may be missing)";
  }
  return {};
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

void cloneCiCheckout(const std::string& url, const CiCheckoutRequest& request, const std::filesystem::path& destination,
                     const std::vector<std::string>& environment, const std::string& upload_pack) {
  if (!isValidObjectId(request.commit)) throw std::runtime_error("invalid commit for checkout");
  // Every git process runs with a scrubbed configuration: no system or user
  // config, no prompts, and no hooks (fetch and checkout run none here anyway).
  std::vector<std::string> base = {"env", "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null",
                                   "GIT_TERMINAL_PROMPT=0", "LANG=C"};
  base.insert(base.end(), environment.begin(), environment.end());
  base.insert(base.end(), {"git", "-c", "core.hooksPath=/dev/null", "-c", "advice.detachedHead=false"});
  const auto git = [&](std::vector<std::string> arguments, std::chrono::seconds timeout, const std::string& what) {
    std::vector<std::string> command = base;
    command.insert(command.end(), arguments.begin(), arguments.end());
    ProcessOptions options;
    options.timeout = timeout;
    options.output_limit = 64 * 1024;
    const ProcessResult result = runProcess(command, options);
    if (result.timed_out || result.exit_code != 0) {
      std::string reason = result.output;
      while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r')) reason.pop_back();
      const std::size_t last = reason.rfind('\n');
      if (last != std::string::npos) reason = reason.substr(last + 1);
      throw std::runtime_error(what + (result.timed_out ? " timed out" : " failed") +
                               (reason.empty() ? "" : ": " + reason.substr(0, 200)));
    }
  };
  const std::string dir = destination.string();
  git({"init", "--quiet", "--initial-branch=ckgit-ci", dir}, std::chrono::seconds(30), "git init");
  std::vector<std::string> fetch = {"-C", dir, "fetch", "--quiet", "--no-tags", "--no-recurse-submodules"};
  if (!request.spec.since.empty()) {
    fetch.push_back("--shallow-since=" + request.spec.since);
  } else if (request.spec.depth != 0) {
    fetch.push_back("--depth=" + std::to_string(request.spec.depth));
  }
  if (!upload_pack.empty()) fetch.push_back("--upload-pack=" + upload_pack);
  fetch.push_back("--end-of-options");
  fetch.push_back(url);
  fetch.push_back(request.commit);
  if (request.spec.tags) fetch.push_back("+refs/tags/*:refs/tags/*");
  git(fetch, std::chrono::seconds(1800), "git fetch of " + request.project);
  if (!request.spec.tags && request.ref.rfind("refs/tags/", 0) == 0) {
    // The build's own tag too, so `git describe` names it even at depth 1.
    // Best-effort: the commit is what the run needs, and the tag may already
    // have been deleted or moved since the push.
    fetch.pop_back();
    fetch.push_back("+" + request.ref + ":" + request.ref);
    try {
      git(fetch, std::chrono::seconds(300), "git fetch of " + request.ref);
    } catch (const std::exception&) {
    }
  }
  git({"-C", dir, "checkout", "--quiet", "--detach", request.commit}, std::chrono::seconds(600), "git checkout");
  if (request.ref.rfind("refs/heads/", 0) == 0) {
    const std::string branch = request.ref.substr(11);
    if (isValidBranchName(branch)) {
      git({"-C", dir, "update-ref", "refs/remotes/origin/" + branch, request.commit}, std::chrono::seconds(30),
          "git update-ref");
    }
  }
  if (request.spec.archive) {
    std::error_code error;
    std::filesystem::remove_all(destination / ".git", error);
    if (error) throw std::runtime_error("could not drop .git for an archive checkout");
  }
}

bool ciSandboxAvailable() { return probeUserNamespaces(false); }

bool ciSandboxCompiledIn() {
#if defined(__linux__)
  return true;
#else
  return false;
#endif
}

CiPreparation prepareCiJob(const std::filesystem::path& repository, const CiJobRequest& job) {
  CiPreparation result;
  CiPreparedJob prepared;
  prepared.job = job;
  const auto outcome = [&result](CiRunStatus status, std::string detail) {
    result.status = status;
    result.detail = std::move(detail);
    return result;
  };
  try {
    const std::optional<std::string> text = readWorkflowBlob(repository, job.commit_id);
    if (!text.has_value()) return outcome(CiRunStatus::Skipped, "no .ckgit/ci.yml in this commit");
    prepared.workflow_text = *text;
    prepared.workflow = parseCiWorkflow(*text);
  } catch (const std::exception& error) {
    return outcome(CiRunStatus::Error, std::string("workflow: ") + error.what());
  }
  const CiWorkflow& workflow = prepared.workflow;

  // Decide whether this ref triggers the workflow, and whether it is a release.
  // Tag pushes match `on.tags`; branch pushes match `on.branches`, or the
  // repository's default branch when the workflow omits `on:` entirely.
  const std::string kTagPrefix = "refs/tags/";
  if (job.ref.rfind(kTagPrefix, 0) == 0) {
    const std::string tag = job.ref.substr(kTagPrefix.size());
    if (!anyTagMatches(tag, workflow.tags)) return outcome(CiRunStatus::Skipped, "tag " + tag + " is not a trigger");
    if (!isValidReleaseTag(tag)) {
      return outcome(CiRunStatus::Skipped, "tag " + tag + " is not a supported release tag");
    }
    prepared.is_release = true;
    prepared.release_tag = tag;
  } else {
    const std::string branch = branchOf(job.ref);
    const std::optional<std::string> def = defaultBranch(repository);
    if (workflow.triggers_default_branch) {
      if (def.has_value() && branch != *def) {
        return outcome(CiRunStatus::Skipped, "branch " + branch + " is not the default branch " + *def);
      }
    } else if (std::find(workflow.branches.begin(), workflow.branches.end(), branch) == workflow.branches.end()) {
      return outcome(CiRunStatus::Skipped, "branch " + branch + " is not a trigger");
    }
    // Only the default branch may replace the live Pages site.
    prepared.is_default_branch = def.has_value() && branch == *def;
  }

  // Resolve every sister to one commit now, on the main host, so each attempt
  // of this run builds against the same sister source wherever it executes.
  try {
    const std::filesystem::path repo_root = repository.parent_path();
    for (const CiSister& sister : workflow.sisters) {
      // The project itself is allowed with an explicit ref: another branch's
      // tree beside the checkout (a docs portal rendering two branches, say).
      // Without one it would only repeat the checkout.
      if (sister.name == job.project_name && sister.ref.empty()) {
        throw std::runtime_error("a project can list itself as a sister only with an explicit ref: " + sister.name);
      }
      if (sister.name == "src" || sister.name == "home" || sister.name == "tmp") {
        throw std::runtime_error("reserved sister name: " + sister.name);
      }
      const std::filesystem::path sister_repo =
          sister.name == job.project_name ? repository : repo_root / (sister.name + ".git");
      if (!std::filesystem::is_directory(sister_repo)) {
        throw std::runtime_error("sister project '" + sister.name + "' is not hosted here");
      }
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
      if (!isValidObjectId(commit)) throw std::runtime_error("sister project '" + sister.name + "' did not resolve");
      prepared.sisters.push_back(CiResolvedSister{sister.name, commit, sister.checkout});
    }
  } catch (const std::exception& error) {
    return outcome(CiRunStatus::Error, std::string("sisters: ") + error.what());
  }
  result.prepared = std::move(prepared);
  return result;
}

CiRunRecord recordCiOutcome(const std::filesystem::path& state_root, const CiJobRequest& job, CiRunStatus status,
                            const std::string& detail) {
  CiRunRecord record;
  record.run_id = job.job_id;
  record.project_name = job.project_name;
  record.ref = job.ref;
  record.commit_id = job.commit_id;
  record.attempt = job.attempt;
  record.status = status;
  record.started_epoch_seconds = nowEpoch();
  record.finished_epoch_seconds = record.started_epoch_seconds;
  record.heartbeat_epoch_seconds = record.started_epoch_seconds;
  record.detail = firstLine(detail);
  writeCiRunRecord(state_root, record);
  try {
    clearCiCancel(state_root, job.project_name, job.job_id);
  } catch (const std::exception&) {
  }
  return record;
}

CiRunRecord executeCiJob(const CiPreparedJob& prepared, const CiExecutionOptions& options,
                         CiSourceProvider& source, CiResultSink& sink, CiSandboxReport* sandbox) {
  const CiJobRequest& job = prepared.job;
  const CiWorkflow& workflow = prepared.workflow;
  CiRunRecord record;
  record.run_id = job.job_id;
  record.project_name = job.project_name;
  record.ref = job.ref;
  record.commit_id = job.commit_id;
  record.runner = options.runner;
  record.attempt = job.attempt;
  record.status = CiRunStatus::Running;
  record.started_epoch_seconds = nowEpoch();

  std::filesystem::path scratch;
  const auto removeScratch = [&]() {
    if (scratch.empty() || options.keep_scratch) return;
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
  };
  const auto finish = [&](CiRunStatus status, const std::string& detail) -> CiRunRecord {
    record.status = status;
    record.detail = firstLine(detail);
    record.finished_epoch_seconds = nowEpoch();
    record.heartbeat_epoch_seconds = record.finished_epoch_seconds;
    return sink.finish(record);
  };

  // A queued job can be cancelled while it still sits in the spool, before any
  // runner claims it: the dashboard publishes it as Pending (same run_id) as
  // soon as it is queued, and its Cancel button writes the ordinary marker.
  // Honor that before doing anything else — no sandbox probe, no checkout — so
  // the record goes straight from Pending to Cancelled and never shows Running.
  if (sink.cancelRequested()) return finish(CiRunStatus::Cancelled, "cancelled before it started");

  CiSandboxReport report;
  report.probed = true;
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

  try {
    // Publish the run as Running before any slow setup (checkout, sisters,
    // caches) so the dashboard shows it immediately.
    record.heartbeat_epoch_seconds = record.started_epoch_seconds;
    sink.runStarted(record);
    std::error_code error;
    std::filesystem::create_directories(options.build_root, error);
    scratch = options.build_root / record.run_id;
    std::filesystem::remove_all(scratch, error);
    std::filesystem::create_directories(scratch / "src", error);
    if (error) throw std::runtime_error("could not create the scratch workspace");
    source.checkout(CiCheckoutRequest{job.project_name, job.commit_id, workflow.checkout, job.ref}, scratch / "src");
  } catch (const std::exception& error) {
    removeScratch();
    return finish(CiRunStatus::Error, std::string("setup: ") + error.what());
  }
  sink.heartbeat(record);
  if (sink.leaseLost()) {
    removeScratch();
    return finish(CiRunStatus::Error, "lease lost");
  }

  const std::filesystem::path work = scratch / "src";  // physical: host-side packing/publishing

  // Where a running step sees the tree. When the Linux sandbox is active its
  // bind mount presents the scratch at one fixed canonical path (kSandboxRoot),
  // so cwd, HOME, TMPDIR and every sister look identical on every run; without
  // the sandbox the step uses the physical scratch directly. Holding this
  // constant is what keeps a build's rendered paths reproducible.
  const bool sandbox_active = report.namespaces_available && probeSandboxWorkspace(scratch);
  const std::filesystem::path visible_root = sandbox_active ? kSandboxRoot : scratch;
  const std::filesystem::path bind_source = sandbox_active ? scratch : std::filesystem::path{};
  const std::filesystem::path work_visible = visible_root / "src";
  const std::filesystem::path home_visible = visible_root / "home";
  const std::filesystem::path tmp_visible = visible_root / "tmp";
  std::error_code scratch_error;
  std::filesystem::create_directories(scratch / "home", scratch_error);
  std::filesystem::create_directories(scratch / "tmp", scratch_error);

  // Materialise the sister projects this workflow declared before any step
  // runs, at scratch/<name> (reached from the checkout as ../<name>) and
  // exported as CKGIT_SISTER_<NAME> where the step will see it.
  std::vector<std::string> run_env;
  try {
    for (const CiResolvedSister& sister : prepared.sisters) {
      const std::filesystem::path destination = scratch / sister.name;
      std::error_code error;
      std::filesystem::create_directories(destination, error);
      if (error) throw std::runtime_error("could not create the sister workspace for '" + sister.name + "'");
      source.checkout(CiCheckoutRequest{sister.name, sister.commit, sister.checkout, {}}, destination);
      run_env.push_back(sisterEnvName(sister.name) + "=" + (visible_root / sister.name).string());
    }
  } catch (const std::exception& error) {
    removeScratch();
    return finish(CiRunStatus::Error, std::string("sisters: ") + error.what());
  }
  sink.heartbeat(record);
  // Provision build caches (a warm ccache store, say), which survive across
  // runs when this runner configures a cache root.
  CacheProvision cache_provision;
  try {
    cache_provision = provisionCaches(workflow, job.project_name, options.cache_root, scratch, visible_root);
    run_env.insert(run_env.end(), cache_provision.env.begin(), cache_provision.env.end());
  } catch (const std::exception& error) {
    removeScratch();
    return finish(CiRunStatus::Error, std::string("cache: ") + error.what());
  }

  // D6/WP3: the service tree a step must never see or write directly,
  // regardless of where the scratch physically lives — this is what makes the
  // checkout and a declared cache "the only places a step can write" actually
  // true, not just documented. Built once per run (not per step, though
  // enterSandbox applies it fresh in every step's own namespace): the caller's
  // service paths (on the primary: the private state root, Pages, and the
  // bare-repository root), the CI build root (sibling runs' scratch), and the
  // cache root (other projects' caches — this project's own is exempted via
  // the bind above). Only non-empty, existing paths matter; enterSandbox
  // silently skips the rest.
  SandboxMounts sandbox_mounts;
  sandbox_mounts.cache_binds = cache_provision.mounts;
  for (const std::filesystem::path& hidden : options.hidden_paths) {
    if (!hidden.empty()) sandbox_mounts.hide.push_back(hidden);
  }

  // Do not mount over the whole build root: the active scratch lives beneath
  // it and is already bound at /mnt for the step.  On GitHub-hosted Linux
  // runners, covering that parent invalidates the descendant bind, leaving
  // /mnt/src unreachable (and every otherwise-valid step exits 126).  Cover
  // each sibling run instead.  They remain inaccessible while the current
  // run's source, HOME, and TMPDIR stay available through the sanctioned bind.
  const auto hideSiblings = [&](const std::filesystem::path& root, const std::filesystem::path& keep) {
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
  hideSiblings(options.cache_root, options.cache_root / job.project_name);

  const StepHooks hooks{[&]() { sink.heartbeat(record); },
                        [&]() { return sink.leaseLost() || sink.cancelRequested(); },
                        [&]() { sink.tick(); }};

  // Packed artifacts wait here, outside the checkout, until the sink takes them.
  const std::filesystem::path outputs = scratch / ".outputs";

  CiRunStatus status = CiRunStatus::Success;
  std::string detail;
  std::size_t step_index = 0;
  bool stop = false;
  for (const CiJob& ci_job : workflow.jobs) {
    if (stop) break;
    if (sink.cancelRequested()) {
      status = CiRunStatus::Cancelled;
      detail = "run cancelled before job '" + ci_job.name + "'";
      break;
    }
    const std::vector<std::string> env =
        buildEnv(workflow, ci_job, job, options.extra_env, home_visible, tmp_visible, run_env);
    for (const CiStep& step : ci_job.steps) {
      if (sink.cancelRequested()) {
        status = CiRunStatus::Cancelled;
        detail = "run cancelled before step '" + (step.name.empty() ? ci_job.name : step.name) + "'";
        stop = true;
        break;
      }
      std::vector<std::string> argv;
      if (step.usesShell()) {
        argv = {"/bin/sh", "-ec", step.script};
      } else {
        argv = step.argv;
      }
      sink.stepStarted(record, step_index);
      const StepOutcome outcome =
          executeStep(argv, work_visible, env, options, sink, step_index, bind_source, sandbox_mounts, hooks);
      CiStepResult result;
      result.name = step.name.empty() ? ci_job.name : step.name;
      result.exit_code = outcome.exit_code;
      result.timed_out = outcome.timed_out;
      result.output_truncated = outcome.truncated;
      record.steps.push_back(result);
      sink.stepFinished(record, step_index);  // publish the just-finished step promptly
      ++step_index;
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
        detail = "step '" + result.name + "' exceeded its " + std::to_string(options.timeout_seconds) + "s budget";
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
    if (!stop && ci_job.artifact.has_value()) {
      try {
        std::error_code error;
        std::filesystem::create_directories(outputs, error);
        const std::filesystem::path bundle = outputs / (ci_job.artifact->name + ".tar");
        const std::string note = packArtifact(*ci_job.artifact, work, bundle, options.artifact_max_bytes);
        record.artifacts.push_back(sink.putArtifact(*ci_job.artifact, bundle, note, prepared.is_release));
        std::filesystem::remove(bundle, error);
      } catch (const std::exception&) {
        // Artifact collection never fails the build; the run already succeeded.
      }
    }
  }

  if (sink.leaseLost()) {
    // The job was requeued under this run: leave every result to the attempt
    // that claims it next, and only clean up this runner's own scratch.
    removeScratch();
    return finish(CiRunStatus::Error, "lease lost");
  }

  // A successful default-branch build hands its declared Pages directory to
  // the sink, which publishes it as the project's site when the run commits.
  if (status == CiRunStatus::Success && !prepared.is_release && workflow.pages_path.has_value() &&
      prepared.is_default_branch) {
    const std::filesystem::path site = work / *workflow.pages_path;
    std::error_code error;
    if (std::filesystem::is_directory(site, error)) {
      try {
        sink.putPagesSite(site);
      } catch (const std::exception& publish_error) {
        status = CiRunStatus::Error;
        detail = std::string("steps passed but the Pages site failed to publish: ") + publish_error.what();
      }
    }
  }

  if (status == CiRunStatus::Success) detail = "all steps passed";
  // Commit before the scratch goes: the sink may still read the Pages site.
  const CiRunRecord stored = finish(status, detail);
  removeScratch();
  return stored;
}

// --- the primary's direct path ----------------------------------------------

LocalCiSource::LocalCiSource(std::filesystem::path repository, std::string project_name)
    : repository_(std::move(repository)), project_name_(std::move(project_name)) {}

void LocalCiSource::checkout(const CiCheckoutRequest& request, const std::filesystem::path& destination) {
  if (!isValidProjectName(request.project)) throw std::runtime_error("invalid checkout request");
  const std::filesystem::path repository =
      request.project == project_name_ ? repository_ : repository_.parent_path() / (request.project + ".git");
  // file:// rather than a plain path: only a transport honours --depth. The
  // repository's upload-pack is allowed to serve any reachable commit by hash,
  // so the run gets exactly its commit even after the branch moved on.
  cloneCiCheckout("file://" + std::filesystem::absolute(repository).string(), request, destination, {},
                  "git -c uploadpack.allowAnySHA1InWant=true upload-pack");
}

LocalCiSink::LocalCiSink(const CiRunnerOptions& options, const CiPreparedJob& prepared)
    : options_(options), prepared_(prepared) {}

LocalCiSink::~LocalCiSink() {
  if (log_fd_ >= 0) ::close(log_fd_);
}

void LocalCiSink::write(const CiRunRecord& record, bool force) {
  if (lease_lost_) return;
  const std::uint64_t now = nowEpoch();
  if (!force && now - last_write_ < 3) return;  // bound fsync churn on chatty steps
  last_write_ = now;
  CiRunRecord copy = record;
  copy.heartbeat_epoch_seconds = now;
  try {
    writeCiRunRecord(options_.state_root, copy);
  } catch (const std::exception&) {
    // A dropped heartbeat only delays liveness; the terminal write is authoritative.
  }
}

void LocalCiSink::keepAlive(bool force) {
  if (lease_lost_ || !options_.keep_alive) return;
  const std::uint64_t now = nowEpoch();
  if (!force && now - last_keep_alive_ < std::max(1u, options_.keep_alive_seconds)) return;
  last_keep_alive_ = now;
  try {
    if (!options_.keep_alive()) lease_lost_ = true;
  } catch (const std::exception&) {
    // A failed renewal is retried at the next heartbeat; only an explicit
    // "lost" answer gives the job up.
  }
}

void LocalCiSink::runStarted(const CiRunRecord& record) {
  prepareCiRunDirectory(options_.state_root, record.project_name, record.run_id);
  last_keep_alive_ = nowEpoch();
  write(record, true);
  if (options_.on_run_started) options_.on_run_started();
}

void LocalCiSink::stepStarted(const CiRunRecord& record, std::size_t index) {
  if (log_fd_ >= 0) ::close(log_fd_);
  log_fd_ = -1;
  if (lease_lost_) return;
  const std::filesystem::path run_dir =
      prepareCiRunDirectory(options_.state_root, record.project_name, record.run_id);
  const std::filesystem::path log_path = run_dir / "steps" / (std::to_string(index) + ".log");
  log_fd_ = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
}

void LocalCiSink::appendLog(std::size_t, std::string_view bytes) {
  if (log_fd_ < 0 || lease_lost_) return;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(log_fd_, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return;
    offset += static_cast<std::size_t>(written);
  }
}

void LocalCiSink::stepFinished(const CiRunRecord& record, std::size_t) {
  if (log_fd_ >= 0) ::close(log_fd_);
  log_fd_ = -1;
  write(record, true);
}

void LocalCiSink::heartbeat(const CiRunRecord& record) {
  keepAlive(false);
  write(record, false);
}

void LocalCiSink::progress(const CiRunRecord& record) { write(record, true); }

bool LocalCiSink::stopping() const {
  try {
    return options_.stop_requested && options_.stop_requested();
  } catch (const std::exception&) {
    return false;
  }
}

bool LocalCiSink::cancelRequested() {
  if (stopping()) return true;
  try {
    return isCiCancelRequested(options_.state_root, prepared_.job.project_name, prepared_.job.job_id);
  } catch (const std::exception&) {
    return false;
  }
}

bool LocalCiSink::leaseLost() { return lease_lost_; }

CiArtifactRecord LocalCiSink::putArtifact(const CiArtifact& artifact, const std::filesystem::path& bundle,
                                          const std::string& note, bool release) {
  // Staged now, committed at finish: a release asset only if the whole run
  // succeeds, a CI artifact in any case.
  CiDeclaredOutput output{artifact.name, release, artifact.retention_days, note};
  CiArtifactRecord provisional;
  provisional.name = artifact.name;
  provisional.note = note;
  if (note.empty() && !lease_lost_) {
    stageCiFile(options_.state_root, prepared_.job.project_name, prepared_.job.job_id,
                release ? CiUploadKind::kAsset : CiUploadKind::kArtifact, artifact.name, bundle);
    provisional.bytes = std::filesystem::file_size(bundle);
    provisional.sha256 = sha256HexOfFile(bundle);
  }
  outputs_.push_back(std::move(output));
  return provisional;
}

void LocalCiSink::putPagesSite(const std::filesystem::path& site) { site_ = site; }

CiRunRecord LocalCiSink::finish(CiRunRecord record) {
  if (log_fd_ >= 0) ::close(log_fd_);
  log_fd_ = -1;
  if (lease_lost_) {
    record.status = CiRunStatus::Error;
    record.detail = "this runner lost its lease on the job; it was requeued";
    return record;
  }
  bool operator_cancel = false;
  try {
    operator_cancel = isCiCancelRequested(options_.state_root, prepared_.job.project_name, prepared_.job.job_id);
  } catch (const std::exception&) {
  }
  if (stopping() && !operator_cancel && record.status != CiRunStatus::Success && options_.give_back) {
    // Shutting down mid-run: the stop (or the service manager's signal to the
    // step) ended the run, not the build itself. Hand the job back for the
    // next runner instead of committing a cancel or a failure nobody caused;
    // the requeue rewrites the run as Pending, keeps this attempt's step logs
    // aside, and drops anything it staged.
    bool given_back = false;
    try {
      given_back = options_.give_back();
    } catch (const std::exception&) {
    }
    lease_lost_ = true;  // either way this runner no longer owns the job
    record.status = CiRunStatus::Pending;
    record.detail = given_back ? "handed back: the runner is shutting down"
                               : "this runner lost its lease on the job; it was requeued";
    return record;
  }
  CiCommitPolicy policy;
  policy.state_root = options_.state_root;
  policy.repository = options_.repository;
  policy.pages_root = options_.pages_root;
  policy.pages_keep_versions = options_.pages_keep_versions;
  policy.artifact_retention_days = options_.artifact_retention_days;
  policy.artifact_max_retention_days = options_.artifact_max_retention_days;
  policy.artifact_max_bytes = options_.artifact_max_bytes;
  return commitCiRun(policy, prepared_, std::move(record), outputs_, site_);
}

std::vector<std::string> ciWorkflowRunsOn(const std::filesystem::path& repository, const std::string& commit) {
  try {
    if (const std::optional<std::string> text = readWorkflowBlob(repository, commit)) {
      return parseCiWorkflow(*text).runs_on;
    }
  } catch (const std::exception&) {
  }
  return {};
}

CiRunRecord runCiWorkflow(const CiRunnerOptions& options, CiSandboxReport* sandbox) {
  if (!isValidProjectName(options.project_name)) throw std::runtime_error("ci runner: invalid project name");
  CiJobRequest job;
  job.job_id = options.run_id.empty() ? generateCiId() : options.run_id;
  job.project_name = options.project_name;
  job.ref = options.ref;
  job.commit_id = options.commit_id;
  job.client_id = kCiPrimaryRunnerName;
  job.queued_epoch_seconds = nowEpoch();
  job.attempt = options.attempt;

  // A cancel requested while the job was still queued ends it before anything
  // is read: no workflow, no sandbox probe, no checkout.
  try {
    if (isCiCancelRequested(options.state_root, options.project_name, job.job_id)) {
      return recordCiOutcome(options.state_root, job, CiRunStatus::Cancelled, "cancelled before it started");
    }
  } catch (const std::exception&) {
  }
  const CiPreparation preparation = prepareCiJob(options.repository, job);
  if (!preparation.prepared.has_value()) {
    return recordCiOutcome(options.state_root, job, preparation.status, preparation.detail);
  }

  CiExecutionOptions execution;
  execution.runner = kCiPrimaryRunnerName;
  execution.build_root = options.build_root;
  execution.timeout_seconds = options.timeout_seconds;
  execution.max_log_bytes = options.max_log_bytes;
  execution.allow_network = options.allow_network;
  execution.keep_scratch = options.keep_scratch;
  execution.artifact_max_bytes = options.artifact_max_bytes;
  execution.cache_root = options.cache_root;
  execution.extra_env = options.extra_env;
  // The primary's service tree: the private state root (spool, run records,
  // other projects' releases), Pages, and the bare-repository root (every
  // hosted project's real Git data; sisters are the sanctioned, read-only way
  // to reach another one).
  execution.hidden_paths = {options.state_root, options.pages_root};
  if (!options.repository.empty()) execution.hidden_paths.push_back(options.repository.parent_path());

  LocalCiSource source(options.repository, options.project_name);
  LocalCiSink sink(options, *preparation.prepared);
  return executeCiJob(*preparation.prepared, execution, source, sink, sandbox);
}

}  // namespace ckgit
