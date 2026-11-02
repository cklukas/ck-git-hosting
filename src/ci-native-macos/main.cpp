// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "wire.hpp"
#include "ckgit/runner_config.hpp"
#include "ckgit/process.hpp"
#include "ckgit/macos_native.hpp"
#include "ckgit/hash.hpp"
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <algorithm>
#include <charconv>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <iostream>
#include <libproc.h>
#include <pwd.h>
#include <sandbox.h>
#include <sstream>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <thread>

extern char** environ;
namespace fs = std::filesystem;
using namespace ckgit::native;
namespace {
volatile sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
unsigned number(const std::string& s, unsigned low, unsigned high) {
  unsigned n = 0;
  const auto [end, error] = std::from_chars(s.data(), s.data()+s.size(), n);
  check(error == std::errc{} && end == s.data()+s.size() && n >= low && n <= high, "invalid native numeric field");
  return n;
}
std::string readFile(const fs::path& p, std::size_t maximum = 1024*1024) {
  Fd fd(::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  check(fd.value >= 0, "cannot open native configuration");
  struct stat st{};
  check(::fstat(fd.value, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
        static_cast<std::size_t>(st.st_size) <= maximum, "invalid native configuration file");
  std::string text(static_cast<std::size_t>(st.st_size), '\0');
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto n = ::read(fd.value, text.data()+offset, text.size()-offset);
    if (n < 0 && errno == EINTR) continue;
    check(n > 0, "cannot read native configuration"); offset += n;
  }
  return text;
}
void writeNew(const fs::path& path, const std::string& text, mode_t mode = 0600) {
  Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
  check(fd.value >= 0, "refusing to overwrite existing native setup file");
  check(::fchmod(fd.value, mode) == 0, "cannot set native setup file permissions");
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto n = ::write(fd.value, text.data()+offset, text.size()-offset);
    if (n < 0 && errno == EINTR) continue;
    check(n > 0, "cannot write native setup file"); offset += n;
  }
  check(::fsync(fd.value) == 0, "cannot sync native setup file");
  Fd parent(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  check(parent.value >= 0 && ::fsync(parent.value) == 0, "cannot sync native state directory");
}
bool identifier(const std::string& s) {
  return !s.empty() && s.size() <= 128 && s != "." && s != ".." &&
         s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.") == std::string::npos;
}
void rootFile(const fs::path& p, bool directory) {
  struct stat st{};
  check(::lstat(p.c_str(), &st) == 0 && st.st_uid == 0 && !(st.st_mode & 0022) &&
        (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode)) && (directory || st.st_nlink == 1),
        "native service state must be root-owned and not writable by other users");
}
struct Config {
  uid_t broker = 0, job = 0;
  gid_t broker_group = 0, job_group = 0;
  std::string account;
  fs::path work, state, socket;
  std::vector<fs::path> tools;
};
void assertQualified(const Config& c);
Config configuration(const fs::path& state) {
  check(::geteuid() == 0, "native service requires root");
  rootFile(state, true); rootFile(state / "config", false);
  const auto fields = decode(readFile(state / "config", 65536));
  check(fields.size() >= 9 && fields[0] == "ckgit-native-v1", "unknown native configuration");
  Config c;
  c.state = state; c.broker = number(fields[1], 1, 0x7fffffff); c.broker_group = number(fields[2], 1, 0x7fffffff);
  c.job = number(fields[3], 1, 0x7fffffff); c.job_group = number(fields[4], 1, 0x7fffffff);
  c.account = fields[5]; c.work = fields[6]; c.socket = fields[7];
  check(fields[8] == "offline" && c.job != c.broker && c.job_group != c.broker_group,
        "native identities must be exclusive and network policy offline");
  const auto* user = ::getpwnam(c.account.c_str());
  check(user && user->pw_uid == c.job && user->pw_gid == c.job_group &&
        std::string(user->pw_shell) == "/usr/bin/false" && std::string(user->pw_dir) == "/var/empty",
        "native build account changed; refusing UID reuse");
  for (std::size_t i=9; i<fields.size(); ++i) c.tools.emplace_back(fields[i]);
  return c;
}
void validateWork(const Config& c) {
  rootFile(c.work.string()+".ckgit-owner",false);
  check(readFile(c.work.string()+".ckgit-owner",4096)==c.state.string(),"native work root belongs to another installation");
  check(c.work.is_absolute() && fs::canonical(c.work) == c.work, "native work root changed");
  struct stat st{};
  check(::lstat(c.work.c_str(), &st) == 0 && st.st_uid == c.broker && S_ISDIR(st.st_mode) &&
        !(st.st_mode & 0022) && (st.st_mode & 0001), "native work root permissions changed");
}
std::vector<pid_t> members(uid_t uid) {
  // Real and effective identity are both checked, even though policy forbids transitions.
  std::vector<pid_t> result;
  for (uint32_t type : {PROC_UID_ONLY, PROC_RUID_ONLY}) {
    std::vector<pid_t> list(1024);
    for (;;) {
      errno = 0;
      const int n = ::proc_listpids(type, uid, list.data(), static_cast<int>(list.size()*sizeof(pid_t)));
      check(n >= 0 && !(n == 0 && errno), "cannot enumerate native job processes");
      if (static_cast<std::size_t>(n) < list.size()*sizeof(pid_t)) {
        list.resize(static_cast<std::size_t>(n)/sizeof(pid_t)); break;
      }
      check(list.size() < 131072, "native process enumeration exceeded bound"); list.resize(list.size()*2);
    }
    // Keep every PID from the kernel's UID snapshot, including zombies and
    // PIDs that exit before userspace can inspect them. Filtering those away
    // could manufacture an empty list while newly forked descendants survive.
    for (const pid_t pid : list) if (pid > 1) result.push_back(pid);
  }
  std::sort(result.begin(), result.end()); result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}
void drop(uid_t uid, gid_t gid) {
  check(::setgroups(0, nullptr) == 0 && ::setgid(gid) == 0 && ::setuid(uid) == 0 &&
        ::getuid() == uid && ::geteuid() == uid && ::getgid() == gid && ::getegid() == gid,
        "cannot drop native job credentials");
}
void signalMembers(const Config& c, const std::vector<pid_t>& list, int sig) {
  if (list.empty()) return;
  const pid_t child = ::fork(); check(child >= 0, "cannot create restricted native cleanup helper");
  if (!child) {
    try {
      // Kernel credentials, not a racy root PID lookup, constrain signal targets.
      drop(c.job, c.job_group);
      for (const pid_t pid : list) if (pid != ::getpid()) ::kill(pid, sig);
      _exit(0);
    } catch (...) { _exit(126); }
  }
  int status = 0;
  while (::waitpid(child, &status, 0) < 0) { if (errno != EINTR) throw std::runtime_error("cleanup helper wait failed"); }
  // A hostile same-UID process can kill this expendable helper. The root
  // watchdog survives and subsequent bounded sweeps retry or quarantine.
}
void drain(const Config& c, pid_t child = -1, int* child_status = nullptr) {
  const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
  bool direct_done = child <= 0;
  do {
    if (!direct_done) {
      const auto reaped = ::waitpid(child,child_status,WNOHANG);
      check(reaped >= 0 || errno == EINTR || errno == ECHILD, "cannot reap native root child during cleanup");
      direct_done = reaped == child || (reaped < 0 && errno == ECHILD);
      // Also catch the trusted fork stub before it has dropped to the job UID.
      // This PID cannot be reused while our direct child remains unreaped.
      if (reaped == 0) check(::kill(child,SIGKILL)==0 || errno==ESRCH,"cannot stop native launch stub");
    }
    auto list = members(c.job);
    if (list.empty() && direct_done) return;
    signalMembers(c, list, SIGSTOP);
    signalMembers(c, members(c.job), SIGKILL);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } while (std::chrono::steady_clock::now() < deadline);
  check(direct_done && members(c.job).empty(), "native UID cleanup failed; slot quarantined");
}
void ownTree(int parent, const std::string& name, uid_t uid, gid_t gid, std::size_t& count, uid_t other_uid) {
  check(++count <= 500000, "native workspace entry limit exceeded");
  struct stat st{};
  check(::fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0, "cannot inspect native workspace");
  check(st.st_uid == uid || st.st_uid == other_uid, "foreign-owned entry in native workspace");
  check(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode) || S_ISLNK(st.st_mode), "special file in native workspace");
  check(!S_ISREG(st.st_mode) || st.st_nlink == 1, "hard-linked file in native workspace");
  // Symlink ownership is unnecessary for access or unlinking under the owned
  // directory. Avoid a path-based privileged chown that could race replacement.
  if (S_ISLNK(st.st_mode)) return;
  Fd fd(::openat(parent, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (S_ISDIR(st.st_mode) ? O_DIRECTORY : 0)));
  check(fd.value >= 0, "cannot open native workspace entry");
  struct stat opened{};
  check(::fstat(fd.value,&opened)==0 && opened.st_dev==st.st_dev && opened.st_ino==st.st_ino &&
        (opened.st_uid==uid || opened.st_uid==other_uid) &&
        (S_ISDIR(opened.st_mode) || (S_ISREG(opened.st_mode) && opened.st_nlink==1)),
        "native workspace changed during ownership validation");
  check(::fchown(fd.value, uid, gid) == 0 && ::fchmod(fd.value, S_ISDIR(st.st_mode) ? 0700 : (0600 | (st.st_mode & S_IXUSR))) == 0,
        "cannot set native workspace ownership");
  if (S_ISDIR(st.st_mode)) {
    DIR* directory = ::fdopendir(::dup(fd.value)); check(directory != nullptr, "cannot enumerate native workspace");
    try {
      errno = 0;
      while (auto* entry = ::readdir(directory)) {
        const std::string child = entry->d_name;
        if (child != "." && child != "..") ownTree(fd.value, child, uid, gid, count, other_uid);
        errno = 0;
      }
      check(errno == 0, "cannot read native workspace directory");
    } catch (...) { ::closedir(directory); throw; }
    ::closedir(directory);
  }
}
void ownWorkspace(const Config& c, const std::string& name, bool job) {
  Fd parent(::open(c.work.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  check(parent.value >= 0 && identifier(name), "invalid native workspace");
  struct stat st{};
  check(::fstatat(parent.value, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode), "workspace must be a directory");
  std::size_t count = 0;
  ownTree(parent.value, name, job ? c.job : c.broker, job ? c.job_group : c.broker_group, count, job ? c.broker : c.job);
}
std::string quoted(const fs::path& path) {
  std::string out = "\"";
  for (const unsigned char ch : path.string()) {
    check(ch >= 32 && ch != 127, "control character in sandbox path");
    if (ch == '\\' || ch == '"') out += '\\'; out += static_cast<char>(ch);
  }
  return out + "\"";
}
std::string policy(const fs::path& workspace, const std::vector<fs::path>& tools) {
  std::string p = "(version 1)\n(deny default)\n(allow process-fork)\n(allow sysctl-read)\n(allow signal (target children))\n";
  const std::vector<fs::path> reads{"/System", "/usr/bin", "/usr/lib", "/usr/libexec", "/usr/share", "/bin", "/sbin", "/private/var/db/dyld"};
  for (const auto& path : reads) p += "(allow file-read* (subpath " + quoted(path) + "))\n";
  p += "(allow file-read* file-write* (subpath " + quoted(workspace) + "))\n";
  p += "(allow file-read* (literal \"/\") (literal \"/usr\"))\n";
  p += "(allow file-read* (literal \"/Library/Preferences/com.apple.dt.Xcode.plist\") (literal \"/private/var/select/sh\"))\n";
  p += "(deny file-write-setugid)\n(allow file-read* file-write* (literal \"/dev/null\") (literal \"/dev/random\") (literal \"/dev/urandom\"))\n";
  std::vector<fs::path> executable{"/bin", "/usr/bin"};
  executable.insert(executable.end(), tools.begin(), tools.end());
  for (const auto& path : executable) {
    check(fs::canonical(path) == path, "toolchain path must be canonical");
    p += "(allow file-read* process-exec (subpath " + quoted(path) + "))\n";
    // Never let a set-ID executable change membership. Toolchains are trusted,
    // operator-managed inputs; scan at each step so updates are revalidated.
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
      struct stat st{};
      check(::lstat(entry.path().c_str(), &st) == 0, "cannot audit native executable tree");
      if (S_ISREG(st.st_mode) && (st.st_mode & (S_ISUID | S_ISGID)))
        p += "(deny process-exec (literal " + quoted(entry.path()) + "))\n";
    }
  }
  p += "(allow process-exec (subpath " + quoted(workspace) + "))\n";
  for (auto path : tools) {
    while (!path.empty()) { p += "(allow file-read-metadata (literal " + quoted(path) + "))\n"; if (path == path.parent_path()) break; path = path.parent_path(); }
  }
  for (auto path = workspace.parent_path(); !path.empty();) {
    p += "(allow file-read-metadata (literal " + quoted(path) + "))\n";
    if (path == path.parent_path()) break; path = path.parent_path();
  }
  return p;
}
void applyPolicy(const std::string& text) {
  char* error = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  const int status = ::sandbox_init(text.c_str(), 0, &error);
  std::string message = error ? error : "sandbox initialization failed";
  if (error) ::sandbox_free_error(error);
#pragma clang diagnostic pop
  if (status != 0) throw std::runtime_error(message);
}
void limits() {
  for (const auto& [resource, value] : std::vector<std::pair<int, rlim_t>>{{RLIMIT_CORE,0},{RLIMIT_NOFILE,512},
           {RLIMIT_NPROC,256},{RLIMIT_FSIZE,16ull*1024*1024*1024},{RLIMIT_CPU,86400}}) {
    rlimit limit{value,value}; check(::setrlimit(resource, &limit) == 0, "cannot apply native resource limit");
  }
}
void recover(const Config& c) {
  drain(c);
  validateWork(c);
  if (fs::exists(c.state / "active")) {
    const auto name = readFile(c.state / "active", 128);
    check(identifier(name), "invalid native recovery journal");
    ownWorkspace(c, name, false);
    check(::unlink((c.state / "active").c_str()) == 0, "cannot clear native recovery journal");
  }
}
void execute(const Config& c, int client, const std::vector<std::string>& fields) {
  check(fields.size() >= 6 && identifier(fields[1]), "invalid native run request");
  const unsigned seconds = number(fields[2], 1, 86400), cap = number(fields[3], 0, 64*1024*1024);
  const unsigned argc = number(fields[4], 1, 1024);
  check(fields.size() >= 5+argc, "missing native arguments");
  std::vector<std::string> args(fields.begin()+5, fields.begin()+5+argc), env(fields.begin()+5+argc, fields.end());
  const auto workspace = c.work / fields[1];
  for (const auto& assignment : env) {
    check(assignment.find('=') != std::string::npos, "invalid native environment");
  }
  std::string path;
  for (const auto& tool : c.tools) {
    for (const auto& relative : {"bin", "usr/bin", "Developer/usr/bin", "Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"}) {
      if (fs::is_directory(tool/relative)) path += (tool/relative).string()+":";
    }
  }
  for (const auto& tool : c.tools) {
    const auto developer = fs::is_directory(tool/"Developer/Toolchains") ? tool/"Developer" : tool;
    if (fs::is_directory(developer/"Toolchains") && fs::is_directory(developer/"Platforms")) {
      std::erase_if(env, [](const auto& s) { return s.starts_with("DEVELOPER_DIR="); });
      env.push_back("DEVELOPER_DIR="+developer.string());
      break; // Pin the selected Xcode; do not follow the host's global selector.
    }
  }
  std::string requested_path = "/usr/bin:/bin";
  for (const auto& assignment : env) if (assignment.starts_with("PATH=")) requested_path = assignment.substr(5);
  std::erase_if(env, [](const auto& assignment) { return assignment.starts_with("PATH="); });
  env.push_back("PATH="+path+requested_path);
  // Force all implicit caches and temporaries into this job, overriding workflow values.
  for (const auto& name : {"TMPDIR", "TMP", "TEMP", "HOME", "XDG_CACHE_HOME", "CLANG_MODULE_CACHE_PATH", "SWIFT_MODULECACHE_PATH"}) {
    const std::string prefix = std::string(name)+"=";
    std::erase_if(env, [&](const auto& s) { return s.starts_with(prefix); });
    env.push_back(prefix+(workspace / (std::string(name)=="HOME" ? "home" : "tmp")).string());
  }
  // Workflow-provided values never attest to this backend. Only the trusted
  // stub injects this marker after successful sandbox installation, and only
  // when the root-owned qualification matches this binary/config/OS build.
  std::erase_if(env, [](const auto& s) { return s.starts_with("CKGIT_MACOS_SANDBOX="); });
  bool qualified = false;
  try { assertQualified(c); qualified = true; } catch (...) {}
  const auto profile = policy(workspace, c.tools);
  recover(c);
  writeNew(c.state / "active", fields[1]); // fsynced before ownership or launch
  ownWorkspace(c, fields[1], true);
  int pipes[2]; check(::pipe(pipes) == 0, "native output pipe failed");
  Fd output(pipes[0]), writer(pipes[1]);
  const pid_t supervisor = ::getpid();
  const pid_t child = ::fork(); check(child >= 0, "native job fork failed");
  if (!child) {
    try {
      // If launchd is already restarting the supervisor, this pre-admission
      // stub must never enter untrusted code after its UID was reconciled.
      if (::getppid() != supervisor) _exit(126);
      drop(c.job, c.job_group);
      if (::getppid() != supervisor) _exit(126);
      ::dup2(writer.value, STDOUT_FILENO); ::dup2(writer.value, STDERR_FILENO);
      const int null = ::open("/dev/null", O_RDONLY); check(null >= 0, "cannot open native stdin"); ::dup2(null, STDIN_FILENO);
      const int maxfd = ::getdtablesize();
      check(maxfd > 0 && maxfd <= 1048576, "invalid native descriptor limit");
      for (int fd = 3; fd < maxfd; ++fd) ::close(fd); check(::setsid() >= 0, "native setsid failed");
      limits(); ::umask(0077);
      applyPolicy(profile);
      if (qualified) env.push_back("CKGIT_MACOS_SANDBOX=seatbelt-v1");
      check(::chdir((workspace / "src").c_str()) == 0, "native workspace inaccessible");
      std::vector<char*> raw_args, raw_env;
      for (auto& s : args) raw_args.push_back(s.data()); raw_args.push_back(nullptr);
      for (auto& s : env) raw_env.push_back(s.data()); raw_env.push_back(nullptr);
      environ = raw_env.data();
      ::execvp(raw_args.front(), raw_args.data()); _exit(127);
    } catch (const std::exception& error) { ::dprintf(2,"native setup: %s\n", error.what()); _exit(126); }
  }
  ::close(writer.value); writer.value = -1;
  ::fcntl(output.value, F_SETFL, O_NONBLOCK);
  bool timed_out = false, cancelled = false, truncated = false, connected = true;
  std::size_t bytes = 0;
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
  auto resource_check = std::chrono::steady_clock::now();
  try {
    for (;;) {
      const pid_t ended = ::waitpid(child, &status, WNOHANG);
      check(ended >= 0 || errno == EINTR, "native wait failed");
      if (ended == child) break;
      timed_out = std::chrono::steady_clock::now() >= deadline;
      if (timed_out || stopping) { cancelled = !timed_out; break; }
      if (std::chrono::steady_clock::now() >= resource_check) {
        resource_check = std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
        const auto processes = members(c.job);
        check(processes.size() <= 256, "native job process limit exceeded");
        uint64_t resident = 0;
        for (const auto pid : processes) {
          proc_taskinfo info{};
          errno = 0;
          const int n = ::proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof(info));
          if (!n && (errno == ESRCH || errno == ENOENT)) continue;
          check(n == sizeof(info), "cannot account native job memory");
          resident += info.pti_resident_size;
          check(resident <= 16ull*1024*1024*1024, "native job aggregate resident memory limit exceeded");
        }
      }
      pollfd pollers[2]{{output.value,POLLIN,0},{client,POLLIN,0}};
      const int ready = ::poll(pollers, 2, 100);
      if (ready < 0 && errno == EINTR) continue;
      check(ready >= 0, "native execution poll failed");
      if (pollers[1].revents) {
        char control; const auto n = ::recv(client, &control, 1, MSG_DONTWAIT);
        if (n >= 0 || (errno != EAGAIN && errno != EINTR)) { cancelled = true; connected = n > 0; break; }
      }
      if (pollers[0].revents) {
        char buffer[8192]; const auto n = ::read(output.value, buffer, sizeof(buffer));
        if (n > 0) {
          const auto take = std::min(static_cast<std::size_t>(n), static_cast<std::size_t>(cap)-bytes);
          if (take) send(client, "O"+std::string(buffer,take));
          bytes += take; truncated |= take < static_cast<std::size_t>(n);
        }
      }
    }
    drain(c,child,&status); // Includes descendants after a successful root exit, even with closed stdout.
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    // Drain buffered output without waiting for any untrusted writer.
    for (;;) {
      char buffer[8192]; const auto n = ::read(output.value, buffer, sizeof(buffer));
      if (n <= 0) break;
      const auto take = std::min(static_cast<std::size_t>(n), static_cast<std::size_t>(cap)-bytes);
      if (take && connected) send(client,"O"+std::string(buffer,take));
      bytes += take; truncated |= take < static_cast<std::size_t>(n);
    }
    recover(c);
    const int code = (timed_out || cancelled) ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status);
    if (connected) send(client,"R"+std::to_string(code)+" "+std::to_string(timed_out)+" "+std::to_string(cancelled)+" "+std::to_string(truncated));
  } catch (...) {
    // The durable active journal remains if any recovery operation fails.
    drain(c,child,&status); while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {} recover(c); throw;
  }
}
void qualify(const Config& c);
void assertQualified(const Config& c);
int serve(const fs::path& state) {
  const auto c = configuration(state);
  Fd lock(::open((state / "lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
  check(lock.value >= 0 && ::flock(lock.value, LOCK_EX | LOCK_NB) == 0, "native supervisor already running");
  recover(c); // Never expose a ready socket until previous UID membership is gone.
  // Keep broker ownership of this persistent inode; never replace it while a
  // runner may hold the flock across checkout, execution and artifact packing.
  Fd broker_lock(::open((c.socket.string()+".broker-lock").c_str(), O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600));
  struct stat broker_lock_info{};
  check(broker_lock.value >= 0 && ::fstat(broker_lock.value,&broker_lock_info)==0 && S_ISREG(broker_lock_info.st_mode) &&
        broker_lock_info.st_nlink==1 && (broker_lock_info.st_uid==0 || broker_lock_info.st_uid==c.broker) &&
        ::fchown(broker_lock.value,c.broker,c.broker_group)==0 && ::fchmod(broker_lock.value,0600)==0,
        "cannot secure native broker lock");
  Fd listener(::socket(AF_UNIX, SOCK_STREAM, 0)); check(listener.value >= 0, "native listener failed"); noSigpipe(listener.value);
  struct stat st{};
  if (::lstat(c.socket.c_str(), &st) == 0) {
    check(S_ISSOCK(st.st_mode) && st.st_uid == c.broker, "refusing to replace unexpected native socket");
    check(::unlink(c.socket.c_str()) == 0, "cannot remove stale native socket");
  }
  const auto addr = address(c.socket.string());
  check(::bind(listener.value, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0 &&
        ::chown(c.socket.c_str(), c.broker, c.broker_group) == 0 && ::chmod(c.socket.c_str(),0600) == 0 &&
        ::listen(listener.value,4) == 0, "cannot bind native supervisor socket");
  ::signal(SIGTERM, stop); ::signal(SIGINT, stop); ::signal(SIGPIPE, SIG_IGN);
  while (!stopping) {
    pollfd p{listener.value,POLLIN,0}; if (::poll(&p,1,200) <= 0) continue;
    Fd client(::accept(listener.value,nullptr,nullptr)); if (client.value < 0) continue; noSigpipe(client.value);
    uid_t uid; gid_t gid;
    if (::getpeereid(client.value,&uid,&gid) != 0 || uid != c.broker) continue;
    try {
      const auto fields = decode(receive(client.value));
      if (fields.size() == 2 && (fields[0] == "health" || fields[0] == "probe") && fields[1] == c.work.string()) {
        recover(c); if (fields[0] == "health") assertQualified(c); send(client.value,"ready-v1");
      }
      else if (fields[0] == "run") execute(c,client.value,fields);
      else throw std::runtime_error("invalid native operation");
    } catch (const std::exception& error) {
      std::cerr << "native request: " << error.what() << '\n';
      recover(c); // A failed recovery escapes the loop: launchd restarts, still fail-closed.
      try { send(client.value,"E"+std::string(error.what())); } catch (...) {}
    }
  }
  recover(c); ::unlink(c.socket.c_str()); return 0;
}
#include "setup.inc"
#include "qualify.inc"
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc >= 3 && std::string(argv[1]) == "job-probe") return jobProbe(argc,argv);
    if (argc == 3 && std::string(argv[1]) == "reconcile") {
      const auto c = configuration(argv[2]);
      Fd lock(::open((c.state/"lock").c_str(),O_RDWR|O_NOFOLLOW|O_CLOEXEC));
      check(lock.value >= 0 && ::flock(lock.value,LOCK_EX|LOCK_NB)==0,"stop the supervisor before manual reconciliation");
      recover(c); std::cout << "Native UID is empty and workspace ownership recovered.\n"; return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "qualify") { qualify(configuration(argv[2])); return 0; }
    if (argc == 3 && std::string(argv[1]) == "serve") return serve(argv[2]);
    if (argc >= 2 && std::string(argv[1]) == "setup") return setup(argc,argv);
    if (argc >= 3 && std::string(argv[1]) == "policy") {
      std::vector<fs::path> tools;
      for (int i=3; i<argc; ++i) tools.emplace_back(argv[i]);
      std::cout << policy(fs::canonical(argv[2]),tools); return 0;
    }
    std::cerr << "Usage: ckgit-macos-native setup --config FILE --broker-user USER --work-root DIR [--toolchain DIR] [--dry-run]\n";
    return 2;
  } catch (const std::exception& error) { std::cerr << "ckgit-macos-native: " << error.what() << '\n'; return 1; }
}
