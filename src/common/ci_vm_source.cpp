// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_vm_source.hpp"
#include "ckgit/ci_vm_protocol.hpp"

#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace ckgit {
namespace {
struct Fd { int value; ~Fd() { if (value >= 0) close(value); } };
void requirePrivateDir(int fd, dev_t device) {
  struct stat info {};
  if (fd < 0 || fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
      (info.st_mode & 0777) != 0700 || info.st_dev != device)
    throw std::runtime_error("invalid VM source staging directory");
}
}
void stageCiVmSources(const CiPreparedJob& assignment, CiSourceProvider& source,
                      const std::filesystem::path& exchange, const std::function<bool()>& should_continue) {
  if (!exchange.is_absolute()) throw std::invalid_argument("VM exchange path must be absolute");
  Fd parent{open(exchange.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
  struct stat parent_stat {};
  if (parent.value < 0 || fstat(parent.value, &parent_stat) != 0) throw std::runtime_error("cannot open VM exchange");
  requirePrivateDir(parent.value, parent_stat.st_dev);
  if (should_continue && !should_continue()) throw std::runtime_error("VM source staging cancelled");
  if (mkdirat(parent.value, "source", 0700) != 0) throw std::runtime_error("VM source staging already exists");
  Fd root{openat(parent.value, "source", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
  requirePrivateDir(root.value, parent_stat.st_dev);
  struct stat root_stat {};
  if (fstat(root.value, &root_stat) != 0) throw std::runtime_error("cannot inspect VM source root");
  const auto verifyRoots = [&] {
    struct stat visible {}, named {};
    if (lstat(exchange.c_str(), &visible) != 0 || !S_ISDIR(visible.st_mode) ||
        visible.st_dev != parent_stat.st_dev || visible.st_ino != parent_stat.st_ino ||
        fstatat(parent.value, "source", &named, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(named.st_mode) ||
        named.st_dev != root_stat.st_dev || named.st_ino != root_stat.st_ino)
      throw std::runtime_error("VM source root changed during staging");
  };
  for (std::size_t index = 0; index <= assignment.sisters.size(); ++index) {
    if (should_continue && !should_continue()) throw std::runtime_error("VM source staging cancelled");
    verifyRoots();
    const auto name = std::to_string(index);
    if (mkdirat(root.value, name.c_str(), 0700) != 0) throw std::runtime_error("cannot create VM source checkout");
    Fd checkout{openat(root.value, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    requirePrivateDir(checkout.value, parent_stat.st_dev);
    struct stat before {}, after {};
    if (fstat(checkout.value, &before) != 0) throw std::runtime_error("cannot inspect VM source checkout");
    const auto request = parseCiVmCheckoutRequest(formatCiVmCheckoutRequest(index), assignment);
    source.checkout(request, exchange / "source" / name, should_continue);
    verifyRoots();
    if (should_continue && !should_continue()) throw std::runtime_error("VM source staging cancelled");
    if (fstatat(root.value, name.c_str(), &after, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(after.st_mode) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino)
      throw std::runtime_error("VM source checkout was replaced during staging");
  }
}
}
