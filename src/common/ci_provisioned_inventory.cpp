// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_provisioned_inventory.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "ckgit/hash.hpp"
#include "ckgit/validation.hpp"

#ifdef __APPLE__
#include <cerrno>
#include <fcntl.h>
#include <sys/acl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace ckgit {
namespace {
[[noreturn]] void fail(const char* reason) { throw std::runtime_error(std::string("provisioned inventory: ") + reason); }
struct Entry { bool directory; std::string digest; unsigned mode; };
std::vector<std::string_view> lines(std::string_view input) {
  if (input.empty() || input.size() > 64 * 1024 * 1024 || input.back() != '\n') fail("invalid manifest size or terminator");
  std::vector<std::string_view> result;
  while (!input.empty()) {
    const auto end = input.find('\n');
    const auto line = input.substr(0, end);
    if (line.empty() || line.size() > 4096 || result.size() >= 1000006) fail("invalid manifest line or count");
    result.push_back(line); input.remove_prefix(end + 1);
  }
  return result;
}
#ifdef __APPLE__
struct Fd { int n; explicit Fd(int value) : n(value) {} ~Fd() { if (n >= 0) ::close(n); } };
struct stat protectedStat(int fd, bool directory, dev_t device = 0) {
  struct stat st{};
  if (fd < 0 || fstat(fd, &st) || st.st_uid != 0 || (st.st_mode & 06022) ||
      (directory ? !S_ISDIR(st.st_mode) : (!S_ISREG(st.st_mode) || st.st_nlink != 1)) ||
      (device && st.st_dev != device)) fail("unsafe owner, permissions, type, link count or volume");
  errno = 0;
  acl_t acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
  if (acl) { acl_free(acl); fail("extended ACLs are not supported"); }
  if (errno != ENOENT) fail("cannot verify extended ACL");
  const auto attrs = flistxattr(fd, nullptr, 0, 0);
  if (attrs != 0) fail("extended attributes are not supported or cannot be verified");
  return st;
}
std::string fileDigest(int fd, const struct stat& before) {
  Sha256 digest; unsigned char buffer[65536]; std::uint64_t total = 0;
  for (;;) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) fail("cannot read file");
    if (!n) break;
    total += static_cast<std::uint64_t>(n); digest.add(buffer, static_cast<std::size_t>(n));
  }
  const auto after = protectedStat(fd, false, before.st_dev);
  if (before.st_ino != after.st_ino || before.st_size != after.st_size ||
      before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec || before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
      before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec || before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec ||
      before.st_size < 0 || total != static_cast<std::uint64_t>(before.st_size)) fail("file changed while hashing");
  return digest.finish();
}
#endif
}  // namespace
CiProvisionedInventoryIdentity verifyCiProvisionedInventory(
    const std::filesystem::path& root, std::string_view manifest,
    std::string_view expected_manifest_sha256, std::string_view expected_lock_sha256,
    std::string_view platform, std::string_view architecture, std::string_view abi) {
  if (!isValidSha256Hex(expected_manifest_sha256) || !isValidSha256Hex(expected_lock_sha256) ||
      sha256Hex(manifest) != expected_manifest_sha256) fail("manifest identity mismatch");
  const auto data = lines(manifest);
  if (data.size() < 6 || data[0] != "ckgit-inventory-v1" || data[1] != std::string("lock=") + std::string(expected_lock_sha256) ||
      data[2] != std::string("platform=") + std::string(platform) || data[3] != std::string("architecture=") + std::string(architecture) ||
      data[4] != std::string("abi=") + std::string(abi) || !isValidCiLabel(abi) ||
      platform != "macos" || (architecture != "arm64" && architecture != "x64")) fail("manifest target mismatch");
  std::map<std::string, Entry> entries;
  std::string previous;
  for (std::size_t i = 5; i < data.size(); ++i) {
    const auto line = data[i];
    const auto a = line.find('\t'), b = a == std::string_view::npos ? a : line.find('\t', a + 1);
    const auto c = b == std::string_view::npos ? b : line.find('\t', b + 1);
    if (a != 1 || b == std::string_view::npos || c == std::string_view::npos ||
        line.find('\t', c + 1) != std::string_view::npos) fail("invalid entry");
    const auto kind = line.substr(0, a), digest = line.substr(a + 1, b - a - 1);
    const auto mode_text = line.substr(b + 1, c - b - 1), path = line.substr(c + 1);
    unsigned mode = 0;
    if (mode_text.size() != 4 || mode_text[0] != '0') fail("invalid mode");
    for (char digit : mode_text) {
      if (digit < '0' || digit > '7') fail("invalid mode");
      mode = mode * 8 + static_cast<unsigned>(digit - '0');
    }
    if (!isSafeRelativePath(path) || path.back() == '/' || (kind != "D" && kind != "F") ||
        (mode & 0022) || (kind == "D" ? digest != "-" : !isValidSha256Hex(digest)) ||
        (!previous.empty() && path <= previous))
      fail("invalid, duplicate or unsorted entry");
    previous = std::string(path); entries.emplace(previous, Entry{kind == "D", std::string(digest), mode});
  }
#ifndef __APPLE__
  (void)root; fail("protected filesystem verification is not implemented on this platform");
#else
  if (!root.is_absolute() || root == root.root_path() || root.lexically_normal() != root) fail("root must be absolute and canonical");
  Fd anchor(::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  protectedStat(anchor.n, true);
  int current = ::dup(anchor.n);
  if (current < 0) fail("cannot open root");
  for (const auto& part : root.relative_path()) {
    const int next = ::openat(current, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ::close(current); current = next; protectedStat(current, true);
  }
  Fd protected_root(current);
  const auto root_stat = protectedStat(protected_root.n, true);
  struct statfs filesystem {};
  if (fstatfs(protected_root.n, &filesystem) != 0 || (filesystem.f_flags & MNT_IGNORE_OWNERSHIP))
    fail("filesystem ownership enforcement is unavailable");
  std::size_t seen = 0;
  for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
    const auto relative = item.path().lexically_relative(root).generic_string();
    const auto found = entries.find(relative);
    if (found == entries.end()) fail("unlisted filesystem entry");
    struct stat named {};
    if (lstat(item.path().c_str(), &named) != 0 ||
        (found->second.directory ? !S_ISDIR(named.st_mode) : !S_ISREG(named.st_mode)))
      fail("unexpected filesystem type");
    const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (found->second.directory ? O_DIRECTORY : 0);
    Fd fd(::open(item.path().c_str(), flags));
    const auto info = protectedStat(fd.n, found->second.directory, root_stat.st_dev);
    if (named.st_dev != info.st_dev || named.st_ino != info.st_ino) fail("entry changed during verification");
    if ((info.st_mode & 0777) != found->second.mode) fail("file mode mismatch");
    if (!found->second.directory && fileDigest(fd.n, info) != found->second.digest) fail("file digest mismatch");
    ++seen;
  }
  if (seen != entries.size()) fail("missing filesystem entry");
  return {std::string(expected_manifest_sha256), std::string(expected_lock_sha256), std::string(platform),
          std::string(architecture), std::string(abi), seen};
#endif
}
}  // namespace ckgit
