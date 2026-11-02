// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "staged_source.hpp"
#include "ckgit/ci_vm_protocol.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ckgit {
namespace fs = std::filesystem;
namespace {
bool matches(const CiCheckoutRequest& a, const CiCheckoutRequest& b) {
  return a.project == b.project && a.commit == b.commit && a.ref == b.ref && a.spec.depth == b.spec.depth &&
         a.spec.since == b.spec.since && a.spec.tags == b.spec.tags && a.spec.archive == b.spec.archive;
}
void checkContinue(const std::function<bool()>& should_continue) {
  if (should_continue && !should_continue()) throw std::runtime_error("VM checkout cancelled");
}
struct Fd { int value; ~Fd() { if (value >= 0) close(value); } };
void copyFile(const fs::path& source, const fs::path& destination, const std::function<bool()>& should_continue) {
  Fd input{open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
  struct stat info {};
  if (input.value < 0 || fstat(input.value, &info) != 0 || !S_ISREG(info.st_mode))
    throw std::runtime_error("cannot open VM checkout file");
  Fd output{open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
  if (output.value < 0) throw std::runtime_error("cannot create VM checkout file");
  std::array<char, 256 * 1024> bytes{};
  for (;;) {
    checkContinue(should_continue);
    const auto count = read(input.value, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) throw std::runtime_error("cannot read VM checkout file");
    if (count == 0) break;
    ssize_t offset = 0;
    while (offset < count) {
      checkContinue(should_continue);
      const auto written = write(output.value, bytes.data() + offset, static_cast<std::size_t>(count - offset));
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) throw std::runtime_error("cannot write VM checkout file");
      offset += written;
    }
  }
  if (fchmod(output.value, info.st_mode & 0777) != 0) throw std::runtime_error("cannot set VM checkout permissions");
}
}
CiVmStagedSource::CiVmStagedSource(const CiPreparedJob& assignment, fs::path exchange,
                                   std::function<bool()> should_continue)
    : assignment_(assignment), exchange_(std::move(exchange)), should_continue_(std::move(should_continue)) {
  if (!exchange_.is_absolute()) throw std::invalid_argument("VM exchange must be absolute");
}
void CiVmStagedSource::checkout(const CiCheckoutRequest& request, const fs::path& destination) {
  checkout(request, destination, should_continue_);
}
void CiVmStagedSource::checkout(const CiCheckoutRequest& request, const fs::path& destination,
                                const std::function<bool()>& should_continue) {
  checkContinue(should_continue);
  std::size_t index = 0;
  for (; index <= assignment_.sisters.size(); ++index) {
    if (matches(request, parseCiVmCheckoutRequest(formatCiVmCheckoutRequest(index), assignment_))) break;
  }
  if (index > assignment_.sisters.size()) throw std::invalid_argument("VM checkout not in assignment");
  const fs::path source = exchange_ / "source" / std::to_string(index);
  if (!fs::is_directory(fs::symlink_status(exchange_)) || !fs::is_directory(fs::symlink_status(exchange_ / "source")) ||
      !fs::is_directory(fs::symlink_status(source)) || !fs::is_directory(fs::symlink_status(destination)) ||
      !fs::is_empty(destination)) throw std::runtime_error("VM checkout directories are invalid or not empty");
  std::vector<std::pair<fs::path, fs::perms>> directories;
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(source)) {
    checkContinue(should_continue);
    const fs::path relative = entry.path().lexically_relative(source);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
      throw std::runtime_error("invalid VM source entry");
    const fs::path target = destination / relative;
    const auto status = entry.symlink_status();
    if (fs::is_symlink(status)) fs::copy_symlink(entry.path(), target);
    else if (fs::is_directory(status)) {
      if (!fs::create_directory(target)) throw std::runtime_error("cannot create VM checkout directory");
      directories.emplace_back(target, status.permissions());
    } else if (fs::is_regular_file(status)) {
      copyFile(entry.path(), target, should_continue);
    } else throw std::runtime_error("unsupported VM source file type");
  }
  for (auto it = directories.rbegin(); it != directories.rend(); ++it) fs::permissions(it->first, it->second);
  checkContinue(should_continue);
}
}
