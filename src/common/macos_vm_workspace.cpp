// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/macos_vm_workspace.hpp"
#include "ckgit/macos_vm_storage.hpp"
#include "ckgit/ci_store.hpp"

#include <stdexcept>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#ifdef __APPLE__
#include <dirent.h>
#include <fcntl.h>
#include <sys/clonefile.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

namespace ckgit {
#ifdef __APPLE__
namespace {
struct Fd {
  int value = -1;
  explicit Fd(int v = -1) : value(v) {}
  ~Fd() { if (value >= 0) ::close(value); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};
[[noreturn]] void fail(const char* text) { throw std::runtime_error(text); }
void requireOwnedDir(int fd, dev_t device, bool private_directory) {
  struct stat st {};
  if (fd < 0 || fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (private_directory && (st.st_mode & 0777) != 0700) || st.st_dev != device)
    fail("VM workspace directory changed, is not private, or crosses a mount");
}
void clearDirectory(int fd, dev_t device, unsigned depth, std::size_t& entries, std::string_view preserve = {},
                    const std::function<void()>& on_progress = {}, std::string_view preserve_second = {}) {
  if (depth > 64) fail("VM workspace nesting limit exceeded");
  Fd copied(dup(fd));
  if (copied.value < 0) fail("cannot inspect VM workspace");
  DIR* dir = fdopendir(copied.value);
  if (!dir) fail("cannot inspect VM workspace");
  copied.value = -1;
  try {
    errno = 0;
    while (auto* entry = readdir(dir)) {
      const std::string name(entry->d_name);
      if (name == "." || name == "..") continue;
      if (!preserve.empty() && name == preserve) continue;
      if (!preserve_second.empty() && name == preserve_second) continue;
      if (++entries > 1000000) fail("VM workspace entry limit exceeded");
      if (on_progress && entries % 128 == 0) on_progress();
      struct stat st {};
      if (fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || st.st_dev != device)
        fail("cannot safely inspect VM workspace entry");
      if (S_ISDIR(st.st_mode)) {
        Fd child(openat(fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        requireOwnedDir(child.value, device, false);
        struct stat opened {};
        if (fstat(child.value, &opened) != 0 || opened.st_ino != st.st_ino)
          fail("VM workspace entry changed while inspecting it");
        clearDirectory(child.value, device, depth + 1, entries, {}, on_progress);
        if (unlinkat(fd, name.c_str(), AT_REMOVEDIR) != 0) fail("cannot remove VM workspace directory");
      } else if (unlinkat(fd, name.c_str(), 0) != 0) {
        fail("cannot remove VM workspace entry");
      }
      errno = 0;
    }
    if (errno != 0) fail("cannot enumerate VM workspace");
    closedir(dir);
  } catch (...) {
    closedir(dir);
    throw;
  }
}
std::string uniqueName() {
  unsigned char bytes[16];
  arc4random_buf(bytes, sizeof(bytes));
  static const char digits[] = "0123456789abcdef";
  std::string name = "job-";
  for (unsigned char b : bytes) { name += digits[b >> 4]; name += digits[b & 15]; }
  return name;
}
std::string bootId() {
  char value[64]{};
  std::size_t size = sizeof(value);
  if (sysctlbyname("kern.bootsessionuuid", value, &size, nullptr, 0) != 0 || size != 37 || value[36] != '\0')
    fail("cannot identify the current macOS boot session");
  for (std::size_t i = 0; i < 36; ++i) {
    const char c = value[i];
    if ((i == 8 || i == 13 || i == 18 || i == 23) ? c != '-' :
        !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
      fail("invalid macOS boot session identifier");
  }
  std::string result(value, 36);
  for (char& c : result) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
  return result;
}
std::string workspaceMarker(const std::string& boot) { return "ckgit-macos-vm-v1\nboot=" + boot + "\n"; }
constexpr std::string_view stoppedMarker = "ckgit-vm-stopped-v1\n";
bool validStopMarker(int vm) {
  Fd proof(openat(vm, ".ckgit-vm-stopped", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  struct stat info {};
  char bytes[64]{};
  return proof.value >= 0 && fstat(proof.value, &info) == 0 && S_ISREG(info.st_mode) &&
         info.st_uid == geteuid() && info.st_nlink == 1 && (info.st_mode & 0777) == 0600 &&
         info.st_size == static_cast<off_t>(stoppedMarker.size()) &&
         read(proof.value, bytes, sizeof(bytes)) == static_cast<ssize_t>(stoppedMarker.size()) &&
         std::string_view(bytes, stoppedMarker.size()) == stoppedMarker;
}
// The caller holds the VM use lock and has established that the guest stopped.
// Keep both the lock and durable stop proof until all other data is removed.
void clearStoppedWorkspace(int job, int vm, dev_t device, const std::function<void()>& progress = {}) {
  std::size_t entries = 0;
  clearDirectory(job, device, 0, entries, ".ckgit-workspace", progress, "vm");
  if (fsync(job) != 0) fail("cannot sync VM workspace cleanup");
  clearDirectory(vm, device, 0, entries, ".ckgit-vm-stopped", progress, ".ckgit-vm-use.lock");
  if (fsync(vm) != 0 || unlinkat(vm, ".ckgit-vm-stopped", 0) != 0 ||
      unlinkat(vm, ".ckgit-vm-use.lock", 0) != 0 || fsync(vm) != 0 ||
      unlinkat(job, "vm", AT_REMOVEDIR) != 0 || fsync(job) != 0)
    fail("cannot finalize stopped VM cleanup");
}
void validateRunnerLock(int fd) {
  Fd lock(openat(fd, ".ckgit-runner.lock", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  struct stat st {};
  if (lock.value < 0 || fstat(lock.value, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      st.st_nlink != 1 || (st.st_mode & 0777) != 0600)
    fail("invalid VM runner lock file");
}
std::string readMarker(int fd) {
  Fd file(openat(fd, ".ckgit-workspace", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  struct stat st {};
  char bytes[128]{};
  if (file.value < 0 || fstat(file.value, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      st.st_nlink != 1 || (st.st_mode & 0777) != 0600)
    fail("refusing an unrecognized VM workspace marker");
  const auto count = read(file.value, bytes, sizeof(bytes));
  if (count != 60) fail("refusing an unrecognized VM workspace marker");
  const std::string value(bytes, static_cast<std::size_t>(count));
  if (!value.starts_with("ckgit-macos-vm-v1\nboot=") || value.back() != '\n')
    fail("refusing an unrecognized VM workspace marker");
  auto boot = value.substr(23, 36);
  for (std::size_t i = 0; i < boot.size(); ++i) {
    const char c = boot[i];
    if ((i == 8 || i == 13 || i == 18 || i == 23) ? c != '-' :
        !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
      fail("refusing an unrecognized VM workspace boot session");
  }
  for (char& c : boot) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
  return boot;
}
void reconcilePreviousBoot(int fd, dev_t device, const std::string& current_boot) {
  Fd copied(dup(fd));
  if (copied.value < 0) fail("cannot inspect VM work root");
  DIR* dir = fdopendir(copied.value);
  if (!dir) fail("cannot inspect VM work root");
  copied.value = -1;
  errno = 0;
  while (auto* entry = readdir(dir)) {
    const std::string name(entry->d_name);
    if (name == ".ckgit-runner.lock") {
      try { validateRunnerLock(fd); } catch (...) { closedir(dir); throw; }
      errno = 0; continue;
    }
    if (name != "." && name != "..") {
      try {
        if (name.size() != 36 || !name.starts_with("job-") ||
            name.find_first_not_of("0123456789abcdef", 4) != std::string::npos)
          fail("VM work root contains unknown data");
        Fd job(openat(fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        requireOwnedDir(job.value, device, true);
        Fd use_lock;
        Fd stopped_vm;
        if (readMarker(job.value) == current_boot) {
          // A surviving helper may have positively recorded a clean stop after
          // its runner died. Hold its exclusive lock throughout recovery; an
          // absent, malformed or still-locked proof never authorizes cleanup.
          stopped_vm.value = openat(job.value, "vm", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
          requireOwnedDir(stopped_vm.value, device, true);
          use_lock.value = openat(stopped_vm.value, ".ckgit-vm-use.lock", O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
          struct stat lock_info {}, named_lock {};
          if (use_lock.value < 0 || fstat(use_lock.value, &lock_info) != 0 || !S_ISREG(lock_info.st_mode) ||
              lock_info.st_uid != geteuid() || lock_info.st_nlink != 1 || (lock_info.st_mode & 0777) != 0600 ||
              flock(use_lock.value, LOCK_EX | LOCK_NB) != 0 || fstat(use_lock.value, &lock_info) != 0 || lock_info.st_nlink != 1 ||
              fstatat(stopped_vm.value, ".ckgit-vm-use.lock", &named_lock, AT_SYMLINK_NOFOLLOW) != 0 ||
              !S_ISREG(named_lock.st_mode) || named_lock.st_dev != lock_info.st_dev || named_lock.st_ino != lock_info.st_ino)
            fail("VM work root contains a workspace from this boot whose VM state is unknown");
          if (!validStopMarker(stopped_vm.value))
            fail("VM work root contains a workspace from this boot whose VM state is unknown");
        }
        struct stat opened {}, named {};
        if (fstat(job.value, &opened) != 0 || fstatat(fd, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
          fail("VM workspace changed during recovery");
        // A previous boot cannot retain a live guest. In this boot, a clean
        // helper stop and exclusive use lock are both required.
        if (stopped_vm.value >= 0) clearStoppedWorkspace(job.value, stopped_vm.value, device);
        else {
          std::size_t entries = 0;
          clearDirectory(job.value, device, 0, entries, ".ckgit-workspace");
        }
        if (fsync(job.value) != 0 || unlinkat(job.value, ".ckgit-workspace", 0) != 0 || fsync(job.value) != 0)
          fail("cannot finalize previous-boot VM workspace cleanup");
        if (fstatat(fd, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
          fail("VM workspace changed during recovery");
        if (unlinkat(fd, name.c_str(), AT_REMOVEDIR) != 0 || fsync(fd) != 0)
          fail("cannot remove previous-boot VM workspace");
      } catch (...) {
        closedir(dir);
        throw;
      }
    }
    errno = 0;
  }
  const int error = errno;
  if (!error) {
    rewinddir(dir);
    errno = 0;
    while (auto* entry = readdir(dir)) {
      const std::string name(entry->d_name);
      if (name == ".ckgit-runner.lock") {
        try { validateRunnerLock(fd); } catch (...) { closedir(dir); throw; }
        errno = 0; continue;
      }
      if (name != "." && name != "..") {
        closedir(dir);
        fail("VM work root still contains unreconciled data");
      }
    }
    if (errno != 0) { closedir(dir); fail("cannot recheck VM work root"); }
  }
  closedir(dir);
  if (error) fail("cannot enumerate VM work root");
}
void marker(int fd, const char* name, std::string_view value) {
  Fd file(openat(fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (file.value < 0 || write(file.value, value.data(), value.size()) != static_cast<ssize_t>(value.size()) || fsync(file.value) != 0)
    fail("cannot write VM workspace ownership marker");
}
}

struct MacosVmWorkspace::Impl {
  Fd root;
  Fd job;
  std::filesystem::path root_path;
  std::string name;
  std::string boot;
  dev_t device = 0;
  bool removed = false;
};
struct MacosVmRunnerLock::Impl { Fd root; Fd lock; };

namespace {
void requireSameEntry(int parent, const char* name, int opened) {
  struct stat expected {}, actual {};
  if (fstat(opened, &expected) != 0 || fstatat(parent, name, &actual, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(actual.st_mode) || expected.st_dev != actual.st_dev || expected.st_ino != actual.st_ino)
    fail("VM workspace directory was replaced");
}
void requireVisibleRoot(int opened, const std::filesystem::path& path) {
  struct stat expected {}, actual {};
  if (fstat(opened, &expected) != 0 || lstat(path.c_str(), &actual) != 0 || !S_ISDIR(actual.st_mode) ||
      expected.st_dev != actual.st_dev || expected.st_ino != actual.st_ino)
    fail("VM work volume or root directory changed");
}
}

void reconcileMacosVmRelays(const std::filesystem::path& root) {
  std::error_code error;
  const auto resolved = std::filesystem::canonical(root, error);
  if (error || !root.is_absolute() || resolved != root.lexically_normal()) fail("invalid VM relay root");
  Fd directory(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat root_info {};
  if (directory.value < 0 || fstat(directory.value, &root_info) != 0) fail("cannot open VM relay root");
  requireOwnedDir(directory.value, root_info.st_dev, true);
  requireVisibleRoot(directory.value, root);
  Fd copied(dup(directory.value));
  if (copied.value < 0) fail("cannot inspect VM relay root");
  DIR* entries = fdopendir(copied.value);
  if (!entries) fail("cannot inspect VM relay root");
  copied.value = -1;
  try {
    errno = 0;
    while (auto* entry = readdir(entries)) {
      const std::string name(entry->d_name);
      if (name == "." || name == "..") continue;
      if (!name.ends_with(".relay") || !isValidCiId(name.substr(0, name.size() - 6)))
        fail("VM relay root contains unknown data");
      Fd relay(openat(directory.value, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      requireOwnedDir(relay.value, root_info.st_dev, true);
      requireSameEntry(directory.value, name.c_str(), relay.value);
      Fd marker_file(openat(relay.value, ".ckgit-vm-relay", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
      struct stat marker_info {};
      char marker_bytes[32]{};
      constexpr std::string_view expected = "ckgit-vm-relay-v1\n";
      if (marker_file.value < 0 || fstat(marker_file.value, &marker_info) != 0 || !S_ISREG(marker_info.st_mode) ||
          marker_info.st_uid != geteuid() || marker_info.st_nlink != 1 || (marker_info.st_mode & 0777) != 0600)
        fail("VM relay directory has an invalid ownership marker");
      const auto count = read(marker_file.value, marker_bytes, sizeof(marker_bytes));
      if (count != static_cast<ssize_t>(expected.size()) || std::string_view(marker_bytes, expected.size()) != expected)
        fail("VM relay directory has an invalid ownership marker");
      std::size_t count_entries = 0;
      clearDirectory(relay.value, root_info.st_dev, 0, count_entries, ".ckgit-vm-relay");
      if (fsync(relay.value) != 0 || unlinkat(relay.value, ".ckgit-vm-relay", 0) != 0 || fsync(relay.value) != 0)
        fail("cannot finalize VM relay cleanup");
      requireSameEntry(directory.value, name.c_str(), relay.value);
      if (unlinkat(directory.value, name.c_str(), AT_REMOVEDIR) != 0 || fsync(directory.value) != 0)
        fail("cannot remove VM relay directory");
      errno = 0;
    }
    if (errno != 0) fail("cannot enumerate VM relay root");
    closedir(entries);
  } catch (...) {
    closedir(entries);
    throw;
  }
  requireVisibleRoot(directory.value, root);
}

MacosVmRunnerLock::MacosVmRunnerLock(const std::filesystem::path& root) : impl_(std::make_unique<Impl>()) {
  std::error_code error;
  const auto resolved = std::filesystem::canonical(root, error);
  if (error || !root.is_absolute() || resolved != root.lexically_normal()) fail("invalid VM runner lock root");
  impl_->root.value = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat info {};
  if (impl_->root.value < 0 || fstat(impl_->root.value, &info) != 0) fail("cannot open VM runner lock root");
  requireOwnedDir(impl_->root.value, info.st_dev, true);
  requireVisibleRoot(impl_->root.value, root);
  impl_->lock.value = openat(impl_->root.value, ".ckgit-runner.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  validateRunnerLock(impl_->root.value);
  struct stat opened {}, named {};
  if (impl_->lock.value < 0 || fstat(impl_->lock.value, &opened) != 0 ||
      fstatat(impl_->root.value, ".ckgit-runner.lock", &named, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(opened.st_mode) || opened.st_uid != geteuid() || opened.st_nlink != 1 ||
      (opened.st_mode & 0777) != 0600 || !S_ISREG(named.st_mode) ||
      opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
      flock(impl_->lock.value, LOCK_EX | LOCK_NB) != 0)
    fail("another VM runner holds this work root or its lock changed");
}
MacosVmRunnerLock::~MacosVmRunnerLock() = default;

MacosVmWorkspace::MacosVmWorkspace(const std::filesystem::path& base, const std::filesystem::path& root,
                                   unsigned min_free_gib) : impl_(std::make_unique<Impl>()) {
  validateMacosVmStorage(base, root, min_free_gib);
  impl_->root.value = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat root_stat {};
  if (impl_->root.value < 0 || fstat(impl_->root.value, &root_stat) != 0 ||
      flock(impl_->root.value, LOCK_EX | LOCK_NB) != 0)
    fail("cannot exclusively lock VM work root");
  impl_->device = root_stat.st_dev;
  requireOwnedDir(impl_->root.value, impl_->device, true);
  requireVisibleRoot(impl_->root.value, root);
  impl_->boot = bootId();
  reconcilePreviousBoot(impl_->root.value, impl_->device, impl_->boot);
  Fd source(open(base.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat base_stat {};
  if (source.value < 0 || fstat(source.value, &base_stat) != 0 || base_stat.st_dev != impl_->device ||
      base_stat.st_uid != geteuid() || (base_stat.st_mode & 0777) != 0700)
    fail("VM base must be private and on the same APFS volume as the work root");
  // Setup modifies the base disk and auxiliary storage. Hold the same lock
  // across every clone operation so no job can snapshot a running setup VM.
  if (flock(source.value, LOCK_SH | LOCK_NB) != 0)
    fail("VM base is in use by setup; stop setup before starting jobs");
  impl_->root_path = root;
  impl_->name = uniqueName();
  if (mkdirat(impl_->root.value, impl_->name.c_str(), 0700) != 0) fail("cannot create VM workspace");
  try {
    impl_->job.value = openat(impl_->root.value, impl_->name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    requireOwnedDir(impl_->job.value, impl_->device, true);
    marker(impl_->job.value, ".ckgit-workspace", workspaceMarker(impl_->boot));
    if (mkdirat(impl_->job.value, "vm", 0700) != 0 || mkdirat(impl_->job.value, "exchange", 0700) != 0)
      fail("cannot create VM workspace directories");
    Fd vm(openat(impl_->job.value, "vm", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    requireOwnedDir(vm.value, impl_->device, true);
    for (const char* name : {"Disk.img", "AuxiliaryStorage", "MachineIdentifier", "HardwareModel"}) {
      Fd original(openat(source.value, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
      struct stat st {};
      if (original.value < 0 || fstat(original.value, &st) != 0 || !S_ISREG(st.st_mode) || st.st_dev != impl_->device ||
          st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & 0022) != 0)
        fail("VM base is missing a regular Apple VM image component");
      if (fclonefileat(original.value, vm.value, name, 0) != 0)
        fail("APFS could not clone the VM base; no full-copy fallback is permitted");
      Fd clone(openat(vm.value, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
      if (clone.value < 0 || fchmod(clone.value, 0600) != 0) fail("cannot protect cloned VM image");
    }
    marker(vm.value, ".ckgit-disposable-vm", "ckgit-macos-vm-v1\n");
    marker(vm.value, ".ckgit-vm-use.lock", "ckgit-vm-use-v1\n");
    if (fsync(vm.value) != 0 || fsync(impl_->job.value) != 0 || fsync(impl_->root.value) != 0)
      fail("cannot sync VM workspace");
  } catch (...) {
    // No VM has started yet, so this task-owned, just-created workspace is safe to remove.
    const auto failure = std::current_exception();
    bool clean = true;
    if (impl_->job.value >= 0) {
      try {
        std::size_t entries = 0;
        clearDirectory(impl_->job.value, impl_->device, 0, entries, ".ckgit-workspace");
        if (fsync(impl_->job.value) != 0) fail("cannot sync partial VM workspace cleanup");
        struct stat marker_info {};
        if (fstatat(impl_->job.value, ".ckgit-workspace", &marker_info, AT_SYMLINK_NOFOLLOW) == 0) {
          if (!S_ISREG(marker_info.st_mode) || marker_info.st_uid != geteuid() || marker_info.st_nlink != 1 ||
              (marker_info.st_mode & 0777) != 0600 ||
              unlinkat(impl_->job.value, ".ckgit-workspace", 0) != 0 || fsync(impl_->job.value) != 0)
            fail("cannot finalize partial VM workspace cleanup");
        } else if (errno != ENOENT) fail("cannot inspect partial VM workspace marker");
      }
      catch (...) { clean = false; }
    }
    if (unlinkat(impl_->root.value, impl_->name.c_str(), AT_REMOVEDIR) != 0) clean = false;
    if (!clean) fail("VM workspace creation failed and its partial files could not be removed; refusing new jobs");
    std::rethrow_exception(failure);
  }
}
MacosVmWorkspace::~MacosVmWorkspace() = default;
std::filesystem::path MacosVmWorkspace::bundle() const {
  requireVisibleRoot(impl_->root.value, impl_->root_path);
  requireSameEntry(impl_->root.value, impl_->name.c_str(), impl_->job.value);
  return impl_->root_path / impl_->name / "vm";
}
std::filesystem::path MacosVmWorkspace::exchange() const {
  requireVisibleRoot(impl_->root.value, impl_->root_path);
  requireSameEntry(impl_->root.value, impl_->name.c_str(), impl_->job.value);
  return impl_->root_path / impl_->name / "exchange";
}
void MacosVmWorkspace::removeAfterConfirmedStop(const std::function<void()>& on_progress) {
  if (impl_->removed) fail("VM workspace already removed");
  requireVisibleRoot(impl_->root.value, impl_->root_path);
  requireSameEntry(impl_->root.value, impl_->name.c_str(), impl_->job.value);
  if (readMarker(impl_->job.value) != impl_->boot) fail("VM workspace boot session changed");
  requireOwnedDir(impl_->job.value, impl_->device, true);
  Fd vm(openat(impl_->job.value, "vm", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  requireOwnedDir(vm.value, impl_->device, true);
  Fd use_lock(openat(vm.value, ".ckgit-vm-use.lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC));
  struct stat lock_info {}, named_lock {};
  if (use_lock.value < 0 || fstat(use_lock.value, &lock_info) != 0 || !S_ISREG(lock_info.st_mode) ||
      lock_info.st_uid != geteuid() || lock_info.st_nlink != 1 || (lock_info.st_mode & 0777) != 0600 ||
      flock(use_lock.value, LOCK_EX | LOCK_NB) != 0 || fstat(use_lock.value, &lock_info) != 0 ||
      lock_info.st_nlink != 1 || fstatat(vm.value, ".ckgit-vm-use.lock", &named_lock, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(named_lock.st_mode) || named_lock.st_dev != lock_info.st_dev || named_lock.st_ino != lock_info.st_ino)
    fail("VM helper still holds the workspace or its lock is invalid");
  struct stat proof_info {};
  if (fstatat(vm.value, ".ckgit-vm-stopped", &proof_info, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno != ENOENT) fail("cannot inspect VM stop proof");
    Fd proof(openat(vm.value, ".ckgit-vm-stopped", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (proof.value < 0 || write(proof.value, stoppedMarker.data(), stoppedMarker.size()) != static_cast<ssize_t>(stoppedMarker.size()) ||
        fsync(proof.value) != 0 || fsync(vm.value) != 0) fail("cannot persist VM stop proof before cleanup");
  }
  if (!validStopMarker(vm.value)) fail("invalid VM stop proof");
  if (on_progress) on_progress();
  clearStoppedWorkspace(impl_->job.value, vm.value, impl_->device, on_progress);
  if (fsync(impl_->job.value) != 0 || unlinkat(impl_->job.value, ".ckgit-workspace", 0) != 0 ||
      fsync(impl_->job.value) != 0)
    fail("cannot finalize VM workspace cleanup");
  requireSameEntry(impl_->root.value, impl_->name.c_str(), impl_->job.value);
  if (unlinkat(impl_->root.value, impl_->name.c_str(), AT_REMOVEDIR) != 0 || fsync(impl_->root.value) != 0)
    fail("could not remove or sync VM workspace");
  impl_->removed = true;
}
#else
void reconcileMacosVmRelays(const std::filesystem::path&) { throw std::runtime_error("macOS VM relay recovery requires macOS"); }
struct MacosVmRunnerLock::Impl {};
MacosVmRunnerLock::MacosVmRunnerLock(const std::filesystem::path&) { throw std::runtime_error("macOS VM runner locks require macOS"); }
MacosVmRunnerLock::~MacosVmRunnerLock() = default;
struct MacosVmWorkspace::Impl {};
MacosVmWorkspace::MacosVmWorkspace(const std::filesystem::path&, const std::filesystem::path&, unsigned) {
  throw std::runtime_error("macOS VM workspaces require macOS");
}
MacosVmWorkspace::~MacosVmWorkspace() = default;
std::filesystem::path MacosVmWorkspace::bundle() const { return {}; }
std::filesystem::path MacosVmWorkspace::exchange() const { return {}; }
void MacosVmWorkspace::removeAfterConfirmedStop(const std::function<void()>&) { throw std::runtime_error("macOS VM workspaces require macOS"); }
#endif
}
