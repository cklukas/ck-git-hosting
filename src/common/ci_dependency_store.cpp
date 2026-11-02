// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_dependency_store.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <map>
#include <random>
#include <set>
#include <stdexcept>

#include "ckgit/metadata_store.hpp"
#include "ckgit/validation.hpp"
namespace ckgit {
namespace {
[[noreturn]] void fail(const char* message) {
  throw std::runtime_error(std::string("dependency store: ") + message);
}
struct Fd {
  int n;
  explicit Fd(int v) : n(v) {}
  ~Fd() {
    if (n >= 0) ::close(n);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  operator int() const { return n; }
};
void check(int fd, bool directory) {
  struct stat st{};
  if (fd < 0 || fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0077) ||
      (directory ? !S_ISDIR(st.st_mode) : (!S_ISREG(st.st_mode) || st.st_nlink != 1)))
    fail("unsafe ownership, mode or type");
}
int dir(int parent, const std::string& name, bool create) {
  if (create && mkdirat(parent, name.c_str(), 0700) && errno != EEXIST) fail("cannot create directory");
  int fd = openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 && errno == ENOENT && !create) return -1;
  check(fd, true);
  return fd;
}
void lock(int fd, int op) {
  while (flock(fd, op))
    if (errno != EINTR) fail("cannot lock records");
}
std::string read(int parent, const char* name, std::size_t limit, bool optional = false) {
  int raw = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (raw < 0 && errno == ENOENT && optional) return {};
  Fd fd(raw);
  check(fd, false);
  struct stat st{};
  if (fstat(fd, &st) || st.st_size < 0 || static_cast<std::size_t>(st.st_size) > limit)
    fail("record exceeds limit");
  std::string bytes(static_cast<std::size_t>(st.st_size), '\0');
  std::size_t off = 0;
  while (off < bytes.size()) {
    auto n = ::read(fd, bytes.data() + off, bytes.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) fail("cannot read record");
    off += static_cast<std::size_t>(n);
  }
  return bytes;
}
void write(int parent, const char* name, std::string_view bytes) {
  std::random_device rd;
  auto tmp = std::string(".stage-") + std::to_string(getpid()) + "-" + std::to_string(rd());
  int raw = openat(parent, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (raw < 0) fail("cannot stage record");
  try {
    Fd fd(raw);
    std::size_t off = 0;
    while (off < bytes.size()) {
      auto n = ::write(fd, bytes.data() + off, bytes.size() - off);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) fail("cannot write record");
      off += static_cast<std::size_t>(n);
    }
    if (fsync(fd)) fail("cannot flush record");
  } catch (...) {
    unlinkat(parent, tmp.c_str(), 0);
    throw;
  }
  if (renameat(parent, tmp.c_str(), parent, name) || fsync(parent)) {
    unlinkat(parent, tmp.c_str(), 0);
    fail("cannot commit record");
  }
}
std::uint64_t number(std::string_view value) {
  std::uint64_t result = 0;
  auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (ec != std::errc{} || end != value.data() + value.size() || value.empty())
    fail("invalid journal number");
  return result;
}
std::vector<CiDependencyDecision> history(std::string_view bytes, const CiDependencyLock& parsed) {
  std::set<std::string> names;
  for (const auto& p : parsed.packages) names.insert(p.name);
  std::vector<CiDependencyDecision> result;
  while (!bytes.empty()) {
    auto end = bytes.find('\n');
    if (end == std::string_view::npos) fail("truncated journal");
    auto row = bytes.substr(0, end);
    bytes.remove_prefix(end + 1);
    std::vector<std::string_view> fields;
    while (true) {
      auto p = row.find('\t');
      fields.push_back(row.substr(0, p));
      if (p == std::string_view::npos) break;
      row.remove_prefix(p + 1);
    }
    if (fields.size() != 5 || result.size() >= 4096) fail("invalid journal");
    CiDependencyDecision d{number(fields[0]), number(fields[1]), number(fields[2]), std::string(fields[3]),
                           std::string(fields[4])};
    if (d.sequence != result.size() + 1 || !names.count(d.package) ||
        (d.action != "approve" && d.action != "reject" && d.action != "revoke"))
      fail("invalid decision");
    result.push_back(std::move(d));
  }
  return result;
}
std::vector<std::string> names(int fd) {
  int copied = dup(fd);
  if (copied < 0) fail("cannot list requests");
  DIR* d = fdopendir(copied);
  if (!d) {
    close(copied);
    fail("cannot list requests");
  }
  std::vector<std::string> out;
  errno = 0;
  while (auto* e = readdir(d)) {
    std::string name = e->d_name;
    if (isValidSha256Hex(name)) out.push_back(name);
    if (out.size() > 1024) {
      closedir(d);
      fail("too many requests");
    }
  }
  int error = errno;
  closedir(d);
  if (error) fail("cannot enumerate requests");
  std::sort(out.begin(), out.end());
  return out;
}
std::string projectName(std::string_view name) {
  if (!isValidProjectName(name)) fail("invalid project");
  return std::string(name);
}
struct Path {
  Fd root, ci, all, project;
  Path(const std::filesystem::path& state, std::string_view project_name, bool create)
      : root(open(validatedMetadataRoot(state).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)),
        ci(root.n < 0 ? -1 : dir(root, "ci", create)),
        all(ci.n < 0 ? -1 : dir(ci, "dependencies", create)),
        project(all.n < 0 ? -1 : dir(all, projectName(project_name), create)) {
    check(root, true);
  }
};
CiDependencyRequest load(int fd, std::string_view expected) {
  auto text = read(fd, "lock.yml", 256 * 1024);
  auto parsed = parseCiDependencyLock(text);
  if (parsed.sha256 != expected) fail("lock digest mismatch");
  auto events = history(read(fd, "journal", 1024 * 1024, true), parsed);
  std::vector<std::string> commits;
  std::set<std::string> unique;
  auto bytes = read(fd, "commits", 16 * 1024, true);
  std::string_view remaining(bytes);
  while (!remaining.empty()) {
    const auto end = remaining.find('\n');
    if (end == std::string_view::npos || commits.size() >= 128) fail("invalid request commits");
    std::string commit(remaining.substr(0, end));
    if (!isValidObjectId(commit) || !unique.insert(commit).second) fail("invalid request commit");
    commits.push_back(std::move(commit));
    remaining.remove_prefix(end + 1);
  }
  return {std::move(parsed), std::move(events), std::move(commits)};
}
}  // namespace
std::string_view ciDependencyApprovalStatus(const CiDependencyRequest& request) {
  if (request.lock.packages.empty() || !isValidSha256Hex(request.lock.sha256)) return "pending";
  std::map<std::string, std::string> latest;
  for (const auto& event : request.history) latest[event.package] = event.action;
  bool pending = false;
  for (const auto& package : request.lock.packages) {
    const auto it = latest.find(package.name);
    if (it != latest.end() && it->second == "reject") return "rejected";
    if (it == latest.end() || it->second != "approve") pending = true;
  }
  return pending ? "pending" : "approved";
}
std::string registerCiDependencyRequest(const std::filesystem::path& root, std::string_view project,
                                        std::string_view bytes, std::string_view commit) {
  (void)projectName(project);
  if (!commit.empty() && !isValidObjectId(commit)) fail("invalid request commit");
  auto parsed = parseCiDependencyLock(bytes);
  Path p(root, project, true);
  lock(p.project, LOCK_EX);
  Fd request(dir(p.project, parsed.sha256, true));
  lock(request, LOCK_EX);
  auto old = read(request, "lock.yml", 256 * 1024, true);
  if (!old.empty() && old != bytes) fail("digest collision");
  if (old.empty()) write(request, "lock.yml", bytes);
  const auto record = load(request, parsed.sha256);
  if (!commit.empty() && std::find(record.commits.begin(), record.commits.end(), commit) == record.commits.end()) {
    if (record.commits.size() >= 128) fail("too many request commits");
    auto commits = read(request, "commits", 16 * 1024, true);
    commits += std::string(commit) + "\n";
    write(request, "commits", commits);
  }
  return parsed.sha256;
}
void decideCiDependency(const std::filesystem::path& root, std::string_view project,
                        std::string_view digest_value, std::string_view package, std::string_view action) {
  (void)projectName(project);
  if (!isValidSha256Hex(digest_value) || (action != "approve" && action != "reject" && action != "revoke"))
    fail("invalid decision");
  Path p(root, project, false);
  if (p.project.n < 0) fail("request not found");
  lock(p.project, LOCK_SH);
  Fd request(dir(p.project, std::string(digest_value), false));
  if (request.n < 0) fail("request not found");
  lock(request, LOCK_EX);
  auto record = load(request, digest_value);
  if (std::none_of(record.lock.packages.begin(), record.lock.packages.end(),
                   [&](const auto& item) { return item.name == package; }))
    fail("package not found");
  if (record.history.size() >= 4096) fail("decision journal full");
  auto bytes = read(request, "journal", 1024 * 1024, true);
  auto now =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  bytes += std::to_string(record.history.size() + 1) + "\t" + std::to_string(now) + "\t" +
           std::to_string(geteuid()) + "\t" + std::string(package) + "\t" + std::string(action) + "\n";
  if (bytes.size() > 1024 * 1024) fail("decision journal full");
  write(request, "journal", bytes);
}
std::vector<CiDependencyRequest> listCiDependencyRequests(const std::filesystem::path& root,
                                                          std::string_view project, std::size_t limit,
                                                          bool* truncated) {
  (void)projectName(project);
  if (truncated) *truncated = false;
  Path p(root, project, false);
  if (p.project.n < 0) return {};
  lock(p.project, LOCK_SH);
  std::vector<CiDependencyRequest> result;
  const auto entries = names(p.project);
  if (truncated) *truncated = entries.size() > limit;
  for (std::size_t i = 0; i < entries.size() && i < limit; ++i) {
    const auto& name = entries[i];
    Fd request(dir(p.project, name, false));
    if (request.n < 0) continue;
    lock(request, LOCK_SH);
    result.push_back(load(request, name));
  }
  return result;
}
std::optional<CiDependencyRequest> getCiDependencyRequest(const std::filesystem::path& root,
                                                          std::string_view project, std::string_view digest_value) {
  (void)projectName(project);
  if (!isValidSha256Hex(digest_value)) fail("invalid request digest");
  Path p(root, project, false);
  if (p.project.n < 0) return std::nullopt;
  lock(p.project, LOCK_SH);
  Fd request(dir(p.project, std::string(digest_value), false));
  if (request.n < 0) return std::nullopt;
  lock(request, LOCK_SH);
  return load(request, digest_value);
}
void withApprovedCiDependencyReview(const std::filesystem::path& root, std::string_view project,
                                    std::string_view digest_value, std::string_view commit,
                                    std::string_view platform, std::string_view architecture,
                                    const std::function<void(const CiDependencyRequest&)>& transaction) {
  (void)projectName(project);
  if (!isValidSha256Hex(digest_value) || !isValidObjectId(commit) || !transaction)
    fail("invalid review admission request");
  Path p(root, project, false);
  if (p.project.n < 0) fail("request not found");
  lock(p.project, LOCK_SH);
  Fd request(dir(p.project, std::string(digest_value), false));
  if (request.n < 0) fail("request not found");
  lock(request, LOCK_SH);
  const auto record = load(request, digest_value);
  if (ciDependencyApprovalStatus(record) != "approved" ||
      std::find(record.commits.begin(), record.commits.end(), commit) == record.commits.end() ||
      record.lock.platform != platform || record.lock.architecture != architecture)
    fail("review is incomplete or does not match the requested commit and target");
  transaction(record);
}
}  // namespace ckgit
