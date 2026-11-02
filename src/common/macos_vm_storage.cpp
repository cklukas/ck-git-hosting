// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/macos_vm_storage.hpp"

#include <stdexcept>
#include <string>
#include <limits>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/mount.h>
#endif

namespace ckgit {
namespace {
namespace fs = std::filesystem;

fs::path checkedDirectory(const fs::path& path, const char* label) {
  if (!path.is_absolute() || path == path.root_path())
    throw std::runtime_error(std::string(label) + " must be an absolute non-root directory");
  std::error_code error;
  const auto resolved = fs::canonical(path, error);
  if (error || resolved != path.lexically_normal() || !fs::is_directory(resolved, error) || error)
    throw std::runtime_error(std::string(label) + " must exist and have no symlink components");
  return resolved;
}

bool contains(const fs::path& parent, const fs::path& child) {
  auto a = parent.begin();
  auto b = child.begin();
  for (; a != parent.end(); ++a, ++b)
    if (b == child.end() || *a != *b) return false;
  return true;
}
}

MacosVmStorageStatus inspectMacosVmStorage(const fs::path& base, const fs::path& work_root) {
  const auto source = checkedDirectory(base, "VM base");
  const auto work = checkedDirectory(work_root, "VM work root");
  if (contains(source, work) || contains(work, source))
    throw std::runtime_error("VM base and work root must be separate directories");
  struct stat info {}, base_info {};
  if (::lstat(work.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() ||
      (info.st_mode & 0777) != 0700)
    throw std::runtime_error("VM work root must be owned by the runner and private (mode 0700)");
  if (::lstat(source.c_str(), &base_info) != 0 || !S_ISDIR(base_info.st_mode) || base_info.st_uid != ::geteuid() ||
      (base_info.st_mode & 0777) != 0700)
    throw std::runtime_error("VM base must be owned by the runner and private (mode 0700)");
  if (base_info.st_dev != info.st_dev)
    throw std::runtime_error("VM base and work root must be on the same volume for copy-on-write cloning");
  const int base_fd = ::open(source.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (base_fd < 0) throw std::runtime_error("cannot open VM base directory");
  struct Close { int fd; ~Close() { ::close(fd); } } close_base{base_fd};
  struct stat opened_base {};
  if (::fstat(base_fd, &opened_base) != 0 || opened_base.st_dev != base_info.st_dev ||
      opened_base.st_ino != base_info.st_ino)
    throw std::runtime_error("VM base directory changed during validation");
  std::uintmax_t image_maximum = 0;
  std::uintmax_t base_allocated = 0;
  for (const char* name : {"Disk.img", "AuxiliaryStorage", "MachineIdentifier", "HardwareModel"}) {
    const int fd = ::openat(base_fd, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("VM base is missing an Apple VM image component");
    Close close_file{fd};
    struct stat component {};
    if (::fstat(fd, &component) != 0 || !S_ISREG(component.st_mode) || component.st_uid != ::geteuid() ||
        component.st_nlink != 1 || (component.st_mode & 0022) != 0 || component.st_size <= 0 ||
        component.st_dev != info.st_dev)
      throw std::runtime_error("VM base components must be owned, nonempty regular files on the VM volume");
    const auto size = static_cast<std::uintmax_t>(component.st_size);
    if (size > std::numeric_limits<std::uintmax_t>::max() - image_maximum)
      throw std::runtime_error("VM base image size overflow");
    image_maximum += size;
    if (component.st_blocks < 0 || static_cast<std::uintmax_t>(component.st_blocks) >
            (std::numeric_limits<std::uintmax_t>::max() - base_allocated) / 512)
      throw std::runtime_error("VM base allocated size overflow");
    base_allocated += static_cast<std::uintmax_t>(component.st_blocks) * 512;
  }
#ifdef __APPLE__
  // A stale directory below /Volumes must not silently become a system-disk
  // fallback if its external volume was unmounted.
  const fs::path volumes("/Volumes");
  if (contains(volumes, work)) {
    auto it = work.begin();
    ++it;  // root
    ++it;  // Volumes
    if (it == work.end()) throw std::runtime_error("VM work root must not be /Volumes");
    const fs::path expected = volumes / *it;
    struct statfs mount {};
    if (::statfs(work.c_str(), &mount) != 0 || fs::path(mount.f_mntonname) != expected)
      throw std::runtime_error("the configured VM volume is not mounted at its expected path");
  }
#endif
  std::error_code error;
  const auto space = fs::space(work, error);
  if (error) throw std::runtime_error("cannot inspect available VM volume space");
  return {space.available, image_maximum, base_allocated};
}

std::uintmax_t validateMacosVmStorage(const fs::path& base, const fs::path& work_root, unsigned min_free_gib) {
  if (min_free_gib == 0 || min_free_gib > 65536) throw std::runtime_error("invalid VM free-space reserve");
  const auto status = inspectMacosVmStorage(base, work_root);
  const std::uintmax_t minimum = static_cast<std::uintmax_t>(min_free_gib) * 1024 * 1024 * 1024;
  if (status.available < minimum || status.available - minimum < status.image_maximum)
    throw std::runtime_error("insufficient VM volume space for the reserve and a worst-case full VM disk");
  return status.available;
}
}
