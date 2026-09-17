// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_store.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "ckgit/metadata_store.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

// The CI store keeps the same private-directory invariant the checkout metadata
// store enforces: every directory is 0700, every file 0600, both owned by the
// running user, and nothing is opened through a symlink. These helpers are a
// small, self-contained mirror of that discipline so the two stores stay
// independent modules.
constexpr mode_t kPrivateDirectoryMode = 0700;
constexpr mode_t kPrivateFileMode = 0600;

class Descriptor {
 public:
  explicit Descriptor(int value) : value_(value) {}
  ~Descriptor() { if (value_ >= 0) ::close(value_); }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  operator int() const { return value_; }
  int release() { const int held = value_; value_ = -1; return held; }
 private:
  int value_;
};

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("ci store: " + message); }

void verifyPrivate(int descriptor, bool directory) {
  struct stat status {};
  if (fstat(descriptor, &status) != 0 || status.st_uid != geteuid() || (status.st_mode & 0077) != 0 ||
      (directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode))) {
    fail("a CI state path is not private and owned by this user");
  }
}

int openDir(const std::filesystem::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) fail("could not open the CI state directory");
  verifyPrivate(descriptor, true);
  return descriptor;
}

int ensureDirAt(int parent, const std::string& name) {
  if (::mkdirat(parent, name.c_str(), kPrivateDirectoryMode) != 0 && errno != EEXIST) {
    fail("could not create a CI state directory");
  }
  const int descriptor = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) fail("could not open a CI state directory");
  verifyPrivate(descriptor, true);
  return descriptor;
}

// Opens an existing directory, reporting absence rather than throwing.
int openDirAt(int parent, const std::string& name, bool* missing) {
  *missing = false;
  const int descriptor = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == ENOENT) { *missing = true; return -1; }
    fail("could not open a CI state directory");
  }
  verifyPrivate(descriptor, true);
  return descriptor;
}

void lockExclusive(int descriptor) {
  while (::flock(descriptor, LOCK_EX) != 0) {
    if (errno != EINTR) fail("could not lock the CI spool");
  }
}

void writeAll(int descriptor, std::string_view content) {
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t written = ::write(descriptor, content.data() + offset, content.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) fail("could not write a CI record");
    offset += static_cast<std::size_t>(written);
  }
}

// Same-directory staged, fsynced, atomically renamed write.
void atomicWriteAt(int directory, const std::string& final_name, std::string_view content) {
  std::random_device device;
  const std::string staging = ".staging-" + std::to_string(::getpid()) + "-" + std::to_string(device());
  const int raw = ::openat(directory, staging.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, kPrivateFileMode);
  if (raw < 0) fail("could not stage a CI record");
  try {
    Descriptor file(raw);
    writeAll(file, content);
    if (::fsync(file) != 0) fail("could not flush a CI record");
  } catch (...) {
    ::unlinkat(directory, staging.c_str(), 0);
    throw;
  }
  if (::renameat(directory, staging.c_str(), directory, final_name.c_str()) != 0 ||
      ::fsync(directory) != 0) {
    ::unlinkat(directory, staging.c_str(), 0);
    fail("could not commit a CI record");
  }
}

std::string readCappedAt(int directory, const std::string& name, std::size_t cap, bool* missing) {
  *missing = false;
  const int raw = ::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (raw < 0) {
    if (errno == ENOENT) { *missing = true; return {}; }
    fail("could not open a CI record");
  }
  Descriptor file(raw);
  struct stat status {};
  if (::fstat(file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & 0077) != 0 || status.st_size < 0 ||
      static_cast<std::size_t>(status.st_size) > cap) {
    fail("a CI record is unsafe or exceeds its size limit");
  }
  std::string content;
  content.resize(static_cast<std::size_t>(status.st_size));
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t received = ::read(file, content.data() + offset, content.size() - offset);
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) fail("could not read a CI record");
    offset += static_cast<std::size_t>(received);
  }
  return content;
}

std::vector<std::string> listNames(int descriptor) {
  const int copy = ::dup(descriptor);
  DIR* directory = copy < 0 ? nullptr : ::fdopendir(copy);
  if (directory == nullptr) {
    if (copy >= 0) ::close(copy);
    fail("could not list a CI directory");
  }
  std::vector<std::string> names;
  errno = 0;
  while (dirent* entry = ::readdir(directory)) {
    const std::string_view name(entry->d_name);
    if (name != "." && name != "..") names.emplace_back(name);
    errno = 0;
  }
  const int scan_error = errno;
  ::closedir(directory);
  if (scan_error != 0) fail("could not list a CI directory");
  return names;
}

std::string toHex(std::string_view value) {
  static const char* const digits = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 0x0f]);
  }
  return out;
}

int hexNibble(unsigned char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  return -1;
}

std::string fromHex(std::string_view token, std::size_t byte_cap) {
  if (token.size() % 2 != 0 || token.size() / 2 > byte_cap) fail("a CI field is not valid hex");
  std::string out;
  out.reserve(token.size() / 2);
  for (std::size_t index = 0; index < token.size(); index += 2) {
    const int high = hexNibble(static_cast<unsigned char>(token[index]));
    const int low = hexNibble(static_cast<unsigned char>(token[index + 1]));
    if (high < 0 || low < 0) fail("a CI field is not valid hex");
    out.push_back(static_cast<char>((high << 4) | low));
  }
  return out;
}

bool isHexObjectId(std::string_view value) {
  return (value.size() == 40 || value.size() == 64) &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return hexNibble(c) >= 0; });
}

std::uint64_t parseEpoch(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) fail("a CI timestamp is malformed");
  return value;
}

std::vector<std::string_view> frame(std::string_view content) {
  if (content.empty() || content.back() != '\n') fail("a CI record has invalid framing");
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start < content.size()) {
    const std::size_t newline = content.find('\n', start);
    lines.push_back(content.substr(start, newline - start));
    start = newline + 1;
  }
  return lines;
}

std::string_view expectField(std::string_view line, std::string_view key) {
  if (line.rfind(key, 0) != 0) fail("a CI record field is out of order or missing");
  return line.substr(key.size());
}

std::string serializeJob(const CiJobRequest& job) {
  return "schema_version=1\n"
         "job_id=" + job.job_id + "\n"
         "project=" + job.project_name + "\n"
         "ref_hex=" + toHex(job.ref) + "\n"
         "commit=" + job.commit_id + "\n"
         "client_id=" + job.client_id + "\n"
         "queued_epoch=" + std::to_string(job.queued_epoch_seconds) + "\n";
}

CiJobRequest parseJob(std::string_view content) {
  const std::vector<std::string_view> lines = frame(content);
  if (lines.size() != 7 || lines[0] != "schema_version=1") fail("a CI job record has the wrong shape");
  CiJobRequest job;
  job.job_id = std::string(expectField(lines[1], "job_id="));
  job.project_name = std::string(expectField(lines[2], "project="));
  job.ref = fromHex(expectField(lines[3], "ref_hex="), 512);
  job.commit_id = std::string(expectField(lines[4], "commit="));
  job.client_id = std::string(expectField(lines[5], "client_id="));
  job.queued_epoch_seconds = parseEpoch(expectField(lines[6], "queued_epoch="));
  if (!isValidCiId(job.job_id) || !isValidProjectName(job.project_name) ||
      !isValidClientId(job.client_id) || !isHexObjectId(job.commit_id)) {
    fail("a CI job record contains invalid data");
  }
  return job;
}

std::string serializeRun(const CiRunRecord& run) {
  // schema_version 2 adds heartbeat_epoch after finished_epoch; parseRun still
  // reads the version-1 layout (which has no heartbeat line) written by earlier
  // builds so an upgrade never discards a project's run history.
  std::string out =
      "schema_version=2\n"
      "run_id=" + run.run_id + "\n"
      "project=" + run.project_name + "\n"
      "ref_hex=" + toHex(run.ref) + "\n"
      "commit=" + run.commit_id + "\n"
      "status=" + std::string(ciRunStatusName(run.status)) + "\n"
      "started_epoch=" + std::to_string(run.started_epoch_seconds) + "\n"
      "finished_epoch=" + std::to_string(run.finished_epoch_seconds) + "\n"
      "heartbeat_epoch=" + std::to_string(run.heartbeat_epoch_seconds) + "\n"
      "detail_hex=" + toHex(run.detail) + "\n"
      "step_count=" + std::to_string(run.steps.size()) + "\n";
  for (const CiStepResult& step : run.steps) {
    out += "step=" + std::to_string(step.exit_code) + "," + (step.timed_out ? "1" : "0") + "," +
           (step.output_truncated ? "1" : "0") + "," + toHex(step.name) + "\n";
  }
  return out;
}

CiRunRecord parseRun(std::string_view content) {
  const std::vector<std::string_view> lines = frame(content);
  if (lines.empty()) fail("a CI run record has the wrong shape");
  // Version 1 has no heartbeat line, so its header is one line shorter and the
  // fields after finished_epoch sit one position earlier.
  const bool v2 = lines[0] == "schema_version=2";
  if (!v2 && lines[0] != "schema_version=1") fail("a CI run record has the wrong shape");
  const std::size_t header = v2 ? 11 : 10;
  if (lines.size() < header) fail("a CI run record has the wrong shape");
  CiRunRecord run;
  run.run_id = std::string(expectField(lines[1], "run_id="));
  run.project_name = std::string(expectField(lines[2], "project="));
  run.ref = fromHex(expectField(lines[3], "ref_hex="), 512);
  run.commit_id = std::string(expectField(lines[4], "commit="));
  const std::optional<CiRunStatus> status = ciRunStatusFromName(expectField(lines[5], "status="));
  if (!status.has_value()) fail("a CI run record has an unknown status");
  run.status = *status;
  run.started_epoch_seconds = parseEpoch(expectField(lines[6], "started_epoch="));
  run.finished_epoch_seconds = parseEpoch(expectField(lines[7], "finished_epoch="));
  std::size_t next = 8;
  if (v2) run.heartbeat_epoch_seconds = parseEpoch(expectField(lines[next++], "heartbeat_epoch="));
  run.detail = fromHex(expectField(lines[next++], "detail_hex="), kMaximumCiDetailBytes);
  const std::string_view count_text = expectField(lines[next], "step_count=");
  std::size_t count = 0;
  const auto [end, error] = std::from_chars(count_text.data(), count_text.data() + count_text.size(), count);
  if (error != std::errc{} || end != count_text.data() + count_text.size()) fail("a CI step count is malformed");
  if (count > 4096 || lines.size() != header + count) fail("a CI run record has a mismatched step count");
  for (std::size_t index = 0; index < count; ++index) {
    const std::string_view step_line = expectField(lines[header + index], "step=");
    CiStepResult step;
    const std::size_t c1 = step_line.find(',');
    const std::size_t c2 = c1 == std::string_view::npos ? c1 : step_line.find(',', c1 + 1);
    const std::size_t c3 = c2 == std::string_view::npos ? c2 : step_line.find(',', c2 + 1);
    if (c1 == std::string_view::npos || c2 == std::string_view::npos || c3 == std::string_view::npos) {
      fail("a CI step line is malformed");
    }
    const std::string_view exit_text = step_line.substr(0, c1);
    const auto [exit_end, exit_error] =
        std::from_chars(exit_text.data(), exit_text.data() + exit_text.size(), step.exit_code);
    if (exit_error != std::errc{} || exit_end != exit_text.data() + exit_text.size()) fail("a CI step exit code is malformed");
    step.timed_out = step_line.substr(c1 + 1, c2 - c1 - 1) == "1";
    step.output_truncated = step_line.substr(c2 + 1, c3 - c2 - 1) == "1";
    step.name = fromHex(step_line.substr(c3 + 1), 256);
    run.steps.push_back(std::move(step));
  }
  if (!isValidCiId(run.run_id) || !isValidProjectName(run.project_name) || !isHexObjectId(run.commit_id)) {
    fail("a CI run record contains invalid data");
  }
  return run;
}

constexpr std::size_t kArtifactNameBytes = 64;

bool isValidArtifactName(std::string_view name) {
  return !name.empty() && name.size() <= kArtifactNameBytes &&
         std::all_of(name.begin(), name.end(), [](unsigned char character) {
           return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                  character == '-';
         });
}

bool endsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string serializeArtifact(const CiArtifactRecord& artifact) {
  return "schema_version=1\n"
         "name=" + artifact.name + "\n"
         "bytes=" + std::to_string(artifact.bytes) + "\n"
         "sha256=" + artifact.sha256 + "\n"
         "created_epoch=" + std::to_string(artifact.created_epoch_seconds) + "\n"
         "expires_epoch=" + std::to_string(artifact.expires_epoch_seconds) + "\n"
         "note_hex=" + toHex(artifact.note) + "\n";
}

CiArtifactRecord parseArtifact(std::string_view content) {
  const std::vector<std::string_view> lines = frame(content);
  if (lines.size() != 7 || lines[0] != "schema_version=1") fail("a CI artifact record has the wrong shape");
  CiArtifactRecord artifact;
  artifact.name = std::string(expectField(lines[1], "name="));
  artifact.bytes = parseEpoch(expectField(lines[2], "bytes="));
  const std::string_view sha = expectField(lines[3], "sha256=");
  if (!sha.empty() && (sha.size() != 64 || !std::all_of(sha.begin(), sha.end(), [](unsigned char c) {
        return hexNibble(c) >= 0;
      }))) {
    fail("a CI artifact sha256 is malformed");
  }
  artifact.sha256 = std::string(sha);
  artifact.created_epoch_seconds = parseEpoch(expectField(lines[4], "created_epoch="));
  artifact.expires_epoch_seconds = parseEpoch(expectField(lines[5], "expires_epoch="));
  artifact.note = fromHex(expectField(lines[6], "note_hex="), 256);
  if (!isValidArtifactName(artifact.name)) fail("a CI artifact record has an invalid name");
  return artifact;
}

std::string serializeRelease(const CiReleaseRecord& release) {
  return "schema_version=1\n"
         "tag=" + release.tag + "\n"
         "commit=" + release.commit_id + "\n"
         "created_epoch=" + std::to_string(release.created_epoch_seconds) + "\n"
         "notes_hex=" + toHex(release.notes) + "\n";
}

CiReleaseRecord parseRelease(std::string_view content) {
  const std::vector<std::string_view> lines = frame(content);
  if (lines.size() != 5 || lines[0] != "schema_version=1") fail("a release record has the wrong shape");
  CiReleaseRecord release;
  release.tag = std::string(expectField(lines[1], "tag="));
  release.commit_id = std::string(expectField(lines[2], "commit="));
  release.created_epoch_seconds = parseEpoch(expectField(lines[3], "created_epoch="));
  release.notes = fromHex(expectField(lines[4], "notes_hex="), kMaximumReleaseNotesBytes);
  if (!isValidReleaseTag(release.tag) || !isHexObjectId(release.commit_id)) {
    fail("a release record contains invalid data");
  }
  return release;
}

}  // namespace

std::string_view ciRunStatusName(CiRunStatus status) {
  switch (status) {
    case CiRunStatus::Pending: return "pending";
    case CiRunStatus::Running: return "running";
    case CiRunStatus::Success: return "success";
    case CiRunStatus::Failure: return "failure";
    case CiRunStatus::Timeout: return "timeout";
    case CiRunStatus::Error: return "error";
    case CiRunStatus::Skipped: return "skipped";
    case CiRunStatus::Cancelled: return "cancelled";
  }
  return "error";
}

std::optional<CiRunStatus> ciRunStatusFromName(std::string_view name) {
  if (name == "pending") return CiRunStatus::Pending;
  if (name == "running") return CiRunStatus::Running;
  if (name == "success") return CiRunStatus::Success;
  if (name == "failure") return CiRunStatus::Failure;
  if (name == "timeout") return CiRunStatus::Timeout;
  if (name == "error") return CiRunStatus::Error;
  if (name == "skipped") return CiRunStatus::Skipped;
  if (name == "cancelled") return CiRunStatus::Cancelled;
  return std::nullopt;
}

bool ciRunStatusIsActive(CiRunStatus status) {
  return status == CiRunStatus::Pending || status == CiRunStatus::Running;
}

std::string_view ciRunStatusIcon(CiRunStatus status) {
  switch (status) {
    case CiRunStatus::Pending: return "\xe2\x8f\xb3";              // ⏳ hourglass
    case CiRunStatus::Running: return "\xf0\x9f\x94\x84";          // 🔄 arrows
    case CiRunStatus::Success: return "\xe2\x9c\x85";              // ✅ check
    case CiRunStatus::Failure: return "\xe2\x9d\x8c";              // ❌ cross
    case CiRunStatus::Timeout: return "\xe2\x8f\xb1\xef\xb8\x8f";  // ⏱️ stopwatch
    case CiRunStatus::Error: return "\xe2\x9a\xa0\xef\xb8\x8f";    // ⚠️ warning
    case CiRunStatus::Skipped: return "\xe2\x8f\xad\xef\xb8\x8f";  // ⏭️ skip
    case CiRunStatus::Cancelled: return "\xe2\x9b\x94";            // ⛔ no entry
  }
  return "\xe2\x9a\xa0\xef\xb8\x8f";
}

bool isValidCiId(std::string_view id) {
  return !id.empty() && id.size() <= kMaximumCiIdBytes &&
         std::all_of(id.begin(), id.end(), [](unsigned char character) {
           return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '-';
         });
}

std::string generateCiId() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
  std::random_device device;
  const std::uint32_t noise = device();
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%020lld-%08x", static_cast<long long>(micros),
                static_cast<unsigned>(noise));
  return std::string(buffer);
}

void enqueueCiJob(const std::filesystem::path& state_root, const CiJobRequest& request) {
  if (!isValidCiId(request.job_id) || !isValidProjectName(request.project_name) ||
      !isValidClientId(request.client_id) || !isHexObjectId(request.commit_id)) {
    fail("refusing to queue an invalid CI job");
  }
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor ci_fd(ensureDirAt(root_fd, "ci"));
  Descriptor spool_fd(ensureDirAt(ci_fd, "spool"));
  lockExclusive(spool_fd);
  atomicWriteAt(spool_fd, request.job_id + ".ini", serializeJob(request));
}

std::optional<CiJobRequest> claimNextCiJob(const std::filesystem::path& state_root) {
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  bool missing = false;
  Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
  if (missing) return std::nullopt;
  Descriptor spool_fd(openDirAt(ci_fd, "spool", &missing));
  if (missing) return std::nullopt;
  Descriptor working_fd(ensureDirAt(ci_fd, "working"));

  lockExclusive(spool_fd);
  std::vector<std::string> names;
  for (std::string& name : listNames(spool_fd)) {
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".ini") == 0) names.push_back(std::move(name));
  }
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    if (::renameat(spool_fd, name.c_str(), working_fd, name.c_str()) != 0) {
      if (errno == ENOENT) continue;  // claimed by another runner
      fail("could not claim a CI job");
    }
    if (::fsync(working_fd) != 0) fail("could not commit a CI claim");
    try {
      bool gone = false;
      const std::string content = readCappedAt(working_fd, name, kMaximumCiRunRecordBytes, &gone);
      if (gone) continue;
      return parseJob(content);
    } catch (const std::exception&) {
      // An unreadable, unsafe, or malformed job is set aside, never executed.
      ::renameat(working_fd, name.c_str(), working_fd, (name + ".rejected").c_str());
    }
  }
  return std::nullopt;
}

void releaseCiJob(const std::filesystem::path& state_root, std::string_view job_id) {
  if (!isValidCiId(job_id)) fail("refusing to release an invalid CI job id");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  bool missing = false;
  Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
  if (missing) return;
  Descriptor working_fd(openDirAt(ci_fd, "working", &missing));
  if (missing) return;
  const std::string name = std::string(job_id) + ".ini";
  if (::unlinkat(working_fd, name.c_str(), 0) != 0 && errno != ENOENT) fail("could not release a CI job");
}

std::filesystem::path prepareCiRunDirectory(const std::filesystem::path& state_root,
                                            std::string_view project_name, std::string_view run_id) {
  if (!isValidProjectName(project_name)) fail("invalid project name for a CI run");
  if (!isValidCiId(run_id)) fail("invalid CI run id");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor ci_fd(ensureDirAt(root_fd, "ci"));
  Descriptor runs_fd(ensureDirAt(ci_fd, "runs"));
  Descriptor project_fd(ensureDirAt(runs_fd, std::string(project_name)));
  Descriptor run_fd(ensureDirAt(project_fd, std::string(run_id)));
  Descriptor steps_fd(ensureDirAt(run_fd, "steps"));
  return root / "ci" / "runs" / std::string(project_name) / std::string(run_id);
}

void writeCiRunRecord(const std::filesystem::path& state_root, const CiRunRecord& record) {
  if (!isValidCiId(record.run_id) || !isValidProjectName(record.project_name) ||
      !isHexObjectId(record.commit_id)) {
    fail("refusing to write an invalid CI run record");
  }
  if (record.detail.size() > kMaximumCiDetailBytes) fail("a CI run detail exceeds its size limit");
  const std::string content = serializeRun(record);
  if (content.size() > kMaximumCiRunRecordBytes) fail("a CI run record exceeds its size limit");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor ci_fd(ensureDirAt(root_fd, "ci"));
  Descriptor runs_fd(ensureDirAt(ci_fd, "runs"));
  Descriptor project_fd(ensureDirAt(runs_fd, record.project_name));
  Descriptor run_fd(ensureDirAt(project_fd, record.run_id));
  atomicWriteAt(run_fd, "run.ini", content);
}

std::filesystem::path prepareCiArtifactDirectory(const std::filesystem::path& state_root,
                                                 std::string_view project_name, std::string_view run_id) {
  if (!isValidProjectName(project_name)) fail("invalid project name for a CI artifact");
  if (!isValidCiId(run_id)) fail("invalid CI run id");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor ci_fd(ensureDirAt(root_fd, "ci"));
  Descriptor runs_fd(ensureDirAt(ci_fd, "runs"));
  Descriptor project_fd(ensureDirAt(runs_fd, std::string(project_name)));
  Descriptor run_fd(ensureDirAt(project_fd, std::string(run_id)));
  Descriptor artifacts_fd(ensureDirAt(run_fd, "artifacts"));
  return root / "ci" / "runs" / std::string(project_name) / std::string(run_id) / "artifacts";
}

void writeCiArtifactRecord(const std::filesystem::path& state_root, std::string_view project_name,
                           std::string_view run_id, const CiArtifactRecord& record) {
  if (!isValidProjectName(project_name) || !isValidCiId(run_id) || !isValidArtifactName(record.name)) {
    fail("refusing to write an invalid CI artifact record");
  }
  const std::string content = serializeArtifact(record);
  if (content.size() > kMaximumCiArtifactRecordBytes) fail("a CI artifact record exceeds its size limit");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor ci_fd(ensureDirAt(root_fd, "ci"));
  Descriptor runs_fd(ensureDirAt(ci_fd, "runs"));
  Descriptor project_fd(ensureDirAt(runs_fd, std::string(project_name)));
  Descriptor run_fd(ensureDirAt(project_fd, std::string(run_id)));
  Descriptor artifacts_fd(ensureDirAt(run_fd, "artifacts"));
  atomicWriteAt(artifacts_fd, record.name + ".ini", content);
}

// Loads a run's artifact sidecars from an already-open run directory descriptor.
// Never throws: a malformed sidecar is skipped so the dashboard still renders.
static std::vector<CiArtifactRecord> loadArtifactsFor(int run_fd) {
  std::vector<CiArtifactRecord> artifacts;
  try {
    bool missing = false;
    Descriptor artifacts_fd(openDirAt(run_fd, "artifacts", &missing));
    if (missing) return artifacts;
    std::vector<std::string> names;
    for (std::string& name : listNames(artifacts_fd)) {
      if (endsWith(name, ".ini")) names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
      try {
        bool record_missing = false;
        const std::string content = readCappedAt(artifacts_fd, name, kMaximumCiArtifactRecordBytes, &record_missing);
        if (record_missing) continue;
        artifacts.push_back(parseArtifact(content));
      } catch (const std::exception&) {
      }
    }
  } catch (const std::exception&) {
  }
  return artifacts;
}

std::vector<CiRunRecord> loadCiRuns(const std::filesystem::path& state_root,
                                    std::string_view project_name, std::size_t maximum) {
  std::vector<CiRunRecord> runs;
  if (maximum == 0 || !isValidProjectName(project_name)) return runs;
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  bool missing = false;
  Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
  if (missing) return runs;
  Descriptor runs_fd(openDirAt(ci_fd, "runs", &missing));
  if (missing) return runs;
  Descriptor project_fd(openDirAt(runs_fd, std::string(project_name), &missing));
  if (missing) return runs;

  std::vector<std::string> ids;
  for (std::string& name : listNames(project_fd)) {
    if (isValidCiId(name)) ids.push_back(std::move(name));
  }
  std::sort(ids.begin(), ids.end(), std::greater<>());
  for (const std::string& id : ids) {
    if (runs.size() >= maximum) break;
    bool run_missing = false;
    Descriptor run_fd(openDirAt(project_fd, id, &run_missing));
    if (run_missing) continue;
    bool record_missing = false;
    const std::string content = readCappedAt(run_fd, "run.ini", kMaximumCiRunRecordBytes, &record_missing);
    if (record_missing) continue;
    try {
      CiRunRecord run = parseRun(content);
      run.artifacts = loadArtifactsFor(run_fd);
      runs.push_back(std::move(run));
    } catch (const std::exception&) {
      // A malformed run record is skipped rather than failing the dashboard.
    }
  }
  return runs;
}

std::optional<CiRunRecord> loadCiRun(const std::filesystem::path& state_root,
                                     std::string_view project_name, std::string_view run_id) {
  if (!isValidProjectName(project_name) || !isValidCiId(run_id)) return std::nullopt;
  try {
    const std::filesystem::path root = validatedMetadataRoot(state_root);
    Descriptor root_fd(openDir(root));
    bool missing = false;
    Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
    if (missing) return std::nullopt;
    Descriptor runs_fd(openDirAt(ci_fd, "runs", &missing));
    if (missing) return std::nullopt;
    Descriptor project_fd(openDirAt(runs_fd, std::string(project_name), &missing));
    if (missing) return std::nullopt;
    Descriptor run_fd(openDirAt(project_fd, std::string(run_id), &missing));
    if (missing) return std::nullopt;
    bool record_missing = false;
    const std::string content = readCappedAt(run_fd, "run.ini", kMaximumCiRunRecordBytes, &record_missing);
    if (record_missing) return std::nullopt;
    CiRunRecord run = parseRun(content);
    run.artifacts = loadArtifactsFor(run_fd);
    return run;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::string> readCiRunLog(const std::filesystem::path& state_root,
                                        std::string_view project_name, std::string_view run_id,
                                        std::size_t step_index, std::size_t cap) {
  if (!isValidProjectName(project_name) || !isValidCiId(run_id)) return std::nullopt;
  try {
    const std::filesystem::path root = validatedMetadataRoot(state_root);
    Descriptor root_fd(openDir(root));
    bool missing = false;
    Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
    if (missing) return std::nullopt;
    Descriptor runs_fd(openDirAt(ci_fd, "runs", &missing));
    if (missing) return std::nullopt;
    Descriptor project_fd(openDirAt(runs_fd, std::string(project_name), &missing));
    if (missing) return std::nullopt;
    Descriptor run_fd(openDirAt(project_fd, std::string(run_id), &missing));
    if (missing) return std::nullopt;
    Descriptor steps_fd(openDirAt(run_fd, "steps", &missing));
    if (missing) return std::nullopt;
    bool log_missing = false;
    const std::string content = readCappedAt(steps_fd, std::to_string(step_index) + ".log", cap, &log_missing);
    if (log_missing) return std::nullopt;
    return content;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::string> readCiRunLogChunk(const std::filesystem::path& state_root,
                                             std::string_view project_name, std::string_view run_id,
                                             std::size_t step_index, std::uint64_t offset, std::size_t cap) {
  if (!isValidProjectName(project_name) || !isValidCiId(run_id) || cap == 0) return std::nullopt;
  try {
    const std::filesystem::path root = validatedMetadataRoot(state_root);
    Descriptor root_fd(openDir(root));
    bool missing = false;
    Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
    if (missing) return std::nullopt;
    Descriptor runs_fd(openDirAt(ci_fd, "runs", &missing));
    if (missing) return std::nullopt;
    Descriptor project_fd(openDirAt(runs_fd, std::string(project_name), &missing));
    if (missing) return std::nullopt;
    Descriptor run_fd(openDirAt(project_fd, std::string(run_id), &missing));
    if (missing) return std::nullopt;
    Descriptor steps_fd(openDirAt(run_fd, "steps", &missing));
    if (missing) return std::nullopt;
    const std::string name = std::to_string(step_index) + ".log";
    const int raw = ::openat(steps_fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (raw < 0) {
      if (errno == ENOENT) return std::nullopt;  // the step has not begun writing
      fail("could not open a CI step log");
    }
    Descriptor file(raw);
    struct stat status {};
    if (::fstat(file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0077) != 0 || status.st_size < 0) {
      fail("a CI step log is unsafe");
    }
    const std::uint64_t size = static_cast<std::uint64_t>(status.st_size);
    if (offset >= size) return std::string{};  // present, but nothing new past the offset
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size - offset, cap));
    std::string content(want, '\0');
    std::size_t got = 0;
    while (got < want) {
      const ssize_t received =
          ::pread(file, content.data() + got, want - got, static_cast<off_t>(offset + got));
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) break;  // a short read returns what is available so far
      got += static_cast<std::size_t>(received);
    }
    content.resize(got);
    return content;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::string> readCiArtifact(const std::filesystem::path& state_root,
                                          std::string_view project_name, std::string_view run_id,
                                          std::string_view artifact_name, std::size_t cap) {
  if (!isValidProjectName(project_name) || !isValidCiId(run_id) || !isValidArtifactName(artifact_name)) {
    return std::nullopt;
  }
  try {
    const std::filesystem::path root = validatedMetadataRoot(state_root);
    Descriptor root_fd(openDir(root));
    bool missing = false;
    Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
    if (missing) return std::nullopt;
    Descriptor runs_fd(openDirAt(ci_fd, "runs", &missing));
    if (missing) return std::nullopt;
    Descriptor project_fd(openDirAt(runs_fd, std::string(project_name), &missing));
    if (missing) return std::nullopt;
    Descriptor run_fd(openDirAt(project_fd, std::string(run_id), &missing));
    if (missing) return std::nullopt;
    Descriptor artifacts_fd(openDirAt(run_fd, "artifacts", &missing));
    if (missing) return std::nullopt;
    bool blob_missing = false;
    const std::string content =
        readCappedAt(artifacts_fd, std::string(artifact_name) + ".tar", cap, &blob_missing);
    if (blob_missing) return std::nullopt;
    return content;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

namespace {

// Opens an existing ci/runs/<project>/<run> directory, reporting absence via
// `missing` rather than throwing. Returns -1 (and sets missing) when any parent
// or the run directory itself is absent; the caller owns the returned fd.
int openRunDir(const std::filesystem::path& state_root, std::string_view project_name,
               std::string_view run_id, bool* missing) {
  *missing = true;
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  bool absent = false;
  Descriptor ci_fd(openDirAt(root_fd, "ci", &absent));
  if (absent) return -1;
  Descriptor runs_fd(openDirAt(ci_fd, "runs", &absent));
  if (absent) return -1;
  Descriptor project_fd(openDirAt(runs_fd, std::string(project_name), &absent));
  if (absent) return -1;
  const int run_fd = openDirAt(project_fd, std::string(run_id), &absent);
  if (absent) return -1;
  *missing = false;
  return run_fd;
}

constexpr char kCancelMarkerName[] = "cancel";

}  // namespace

bool requestCiCancel(const std::filesystem::path& state_root, std::string_view project_name,
                     std::string_view run_id) {
  if (!isValidProjectName(project_name) || !isValidCiId(run_id)) {
    fail("refusing to cancel an invalid CI run");
  }
  bool missing = false;
  Descriptor run_fd(openRunDir(state_root, project_name, run_id, &missing));
  if (missing) return false;
  atomicWriteAt(run_fd, kCancelMarkerName, "cancel\n");
  return true;
}

bool isCiCancelRequested(const std::filesystem::path& state_root, std::string_view project_name,
                         std::string_view run_id) {
  if (!isValidProjectName(project_name) || !isValidCiId(run_id)) return false;
  try {
    bool missing = false;
    Descriptor run_fd(openRunDir(state_root, project_name, run_id, &missing));
    if (missing) return false;
    bool marker_missing = false;
    static_cast<void>(readCappedAt(run_fd, kCancelMarkerName, 64, &marker_missing));
    return !marker_missing;
  } catch (const std::exception&) {
    return false;
  }
}

void clearCiCancel(const std::filesystem::path& state_root, std::string_view project_name,
                   std::string_view run_id) {
  if (!isValidProjectName(project_name) || !isValidCiId(run_id)) return;
  try {
    bool missing = false;
    Descriptor run_fd(openRunDir(state_root, project_name, run_id, &missing));
    if (missing) return;
    if (::unlinkat(run_fd, kCancelMarkerName, 0) != 0 && errno != ENOENT) {
      fail("could not clear a CI cancel marker");
    }
  } catch (const std::exception&) {
    // Best effort: a leftover marker only affects the run that requested it,
    // and run ids are never reused.
  }
}

void setProjectCiEnabled(const std::filesystem::path& state_root, std::string_view project_name,
                         bool enabled) {
  if (!isValidProjectName(project_name)) fail("invalid project name for CI opt-in");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor ci_fd(ensureDirAt(root_fd, "ci"));
  Descriptor projects_fd(ensureDirAt(ci_fd, "projects"));
  const std::string content =
      std::string("schema_version=1\nci_enabled=") + (enabled ? "true" : "false") + "\n";
  atomicWriteAt(projects_fd, std::string(project_name) + ".ini", content);
}

bool isProjectCiEnabled(const std::filesystem::path& state_root, std::string_view project_name) {
  if (!isValidProjectName(project_name)) return false;
  try {
    const std::filesystem::path root = validatedMetadataRoot(state_root);
    Descriptor root_fd(openDir(root));
    bool missing = false;
    Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
    if (missing) return false;
    Descriptor projects_fd(openDirAt(ci_fd, "projects", &missing));
    if (missing) return false;
    bool record_missing = false;
    const std::string content =
        readCappedAt(projects_fd, std::string(project_name) + ".ini", 4096, &record_missing);
    if (record_missing) return false;
    const std::vector<std::string_view> lines = frame(content);
    if (lines.size() != 2 || lines[0] != "schema_version=1") return false;
    return expectField(lines[1], "ci_enabled=") == "true";
  } catch (const std::exception&) {
    return false;  // absent or malformed configuration reads as disabled
  }
}

std::size_t sweepCiArtifacts(const std::filesystem::path& state_root,
                             const CiArtifactSweepOptions& options) {
  std::size_t removed = 0;
  std::filesystem::path root;
  try {
    root = validatedMetadataRoot(state_root);
  } catch (const std::exception&) {
    return 0;
  }
  const std::filesystem::path runs_root = root / "ci" / "runs";
  std::error_code ec;
  if (!std::filesystem::is_directory(runs_root, ec)) return 0;

  struct Entry {
    std::filesystem::path dir;  // the run's artifacts directory
    std::string project;
    std::string run_id;
    std::string name;
    std::uint64_t bytes = 0;
    std::uint64_t created = 0;
    std::uint64_t expires = 0;
    bool kept = false;      // protected by keep-latest
    bool evicted = false;   // already removed this sweep
  };
  std::vector<Entry> entries;
  std::map<std::string, std::string> newest_run;  // project -> newest run id holding artifacts

  for (const auto& project_entry : std::filesystem::directory_iterator(runs_root, ec)) {
    if (!project_entry.is_directory(ec)) continue;
    const std::string project = project_entry.path().filename().string();
    if (!isValidProjectName(project)) continue;

    std::vector<std::string> ids;
    std::error_code list_ec;
    for (const auto& run_entry : std::filesystem::directory_iterator(project_entry.path(), list_ec)) {
      const std::string id = run_entry.path().filename().string();
      if (run_entry.is_directory(list_ec) && isValidCiId(id)) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end(), std::greater<>());  // newest first

    // Prune run directories beyond runs_keep (oldest first), artifacts and all.
    if (options.runs_keep > 0 && ids.size() > options.runs_keep) {
      for (std::size_t index = options.runs_keep; index < ids.size(); ++index) {
        std::error_code remove_ec;
        std::filesystem::remove_all(project_entry.path() / ids[index], remove_ec);
      }
      ids.resize(options.runs_keep);
    }

    for (const std::string& id : ids) {
      const std::filesystem::path art_dir = project_entry.path() / id / "artifacts";
      std::error_code art_ec;
      if (!std::filesystem::is_directory(art_dir, art_ec)) continue;
      std::set<std::string> tars;
      std::set<std::string> inis;
      for (const auto& file : std::filesystem::directory_iterator(art_dir, art_ec)) {
        const std::string name = file.path().filename().string();
        if (endsWith(name, ".tar")) tars.insert(name.substr(0, name.size() - 4));
        else if (endsWith(name, ".ini")) inis.insert(name.substr(0, name.size() - 4));
      }
      for (const std::string& base : inis) {
        const std::filesystem::path ini_path = art_dir / (base + ".ini");
        std::error_code size_ec;
        const auto size = std::filesystem::file_size(ini_path, size_ec);
        if (size_ec || size > kMaximumCiArtifactRecordBytes) continue;
        std::ifstream in(ini_path, std::ios::binary);
        if (!in) continue;
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        try {
          const CiArtifactRecord record = parseArtifact(content);
          // Durable artifacts (a release asset, expires 0) are not this sweep's
          // concern; only ephemeral CI artifacts expire or are evicted.
          if (record.expires_epoch_seconds == 0) continue;
          entries.push_back(Entry{art_dir, project, id, record.name, record.bytes,
                                  record.created_epoch_seconds, record.expires_epoch_seconds, false, false});
          const auto existing = newest_run.find(project);
          if (existing == newest_run.end() || id > existing->second) newest_run[project] = id;
        } catch (const std::exception&) {
        }
      }
      // A bundle with no sidecar is a crash leftover; drop it.
      for (const std::string& base : tars) {
        if (inis.find(base) == inis.end()) {
          std::error_code remove_ec;
          if (std::filesystem::remove(art_dir / (base + ".tar"), remove_ec)) ++removed;
        }
      }
    }
  }

  if (options.keep_latest) {
    for (Entry& entry : entries) {
      const auto newest = newest_run.find(entry.project);
      if (newest != newest_run.end() && entry.run_id == newest->second) entry.kept = true;
    }
  }

  const auto deleteEntry = [&removed](Entry& entry) {
    std::error_code tar_ec, ini_ec;
    std::filesystem::remove(entry.dir / (entry.name + ".tar"), tar_ec);
    std::filesystem::remove(entry.dir / (entry.name + ".ini"), ini_ec);
    entry.evicted = true;
    ++removed;
  };

  // Timer expiry for ephemeral artifacts (durable ones have expires 0).
  for (Entry& entry : entries) {
    const bool expired = entry.expires != 0 && options.now_epoch_seconds != 0 &&
                         entry.expires <= options.now_epoch_seconds;
    if (expired && !entry.kept) deleteEntry(entry);
  }

  // Budget eviction, oldest first, protected artifacts never evicted.
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.created != b.created) return a.created < b.created;
    return a.run_id < b.run_id;
  });
  const auto liveTotal = [&entries](const std::string* project) {
    std::uint64_t total = 0;
    for (const Entry& entry : entries) {
      if (entry.evicted) continue;
      if (project != nullptr && entry.project != *project) continue;
      total += entry.bytes;
    }
    return total;
  };
  if (options.max_total_bytes != 0) {
    for (Entry& entry : entries) {
      if (liveTotal(nullptr) <= options.max_total_bytes) break;
      if (!entry.evicted && !entry.kept) deleteEntry(entry);
    }
  }
  if (options.max_project_bytes != 0) {
    std::set<std::string> projects;
    for (const Entry& entry : entries) projects.insert(entry.project);
    for (const std::string& project : projects) {
      for (Entry& entry : entries) {
        if (liveTotal(&project) <= options.max_project_bytes) break;
        if (!entry.evicted && !entry.kept && entry.project == project) deleteEntry(entry);
      }
    }
  }
  return removed;
}

bool isValidReleaseTag(std::string_view tag) {
  return !tag.empty() && tag.size() <= kMaximumReleaseTagBytes && tag != "." && tag != ".." &&
         std::all_of(tag.begin(), tag.end(), [](unsigned char character) {
           return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                  character == '-';
         });
}

std::filesystem::path prepareCiReleaseDirectory(const std::filesystem::path& state_root,
                                                std::string_view project_name, std::string_view tag) {
  if (!isValidProjectName(project_name)) fail("invalid project name for a release");
  if (!isValidReleaseTag(tag)) fail("invalid release tag");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor releases_fd(ensureDirAt(root_fd, "releases"));
  Descriptor project_fd(ensureDirAt(releases_fd, std::string(project_name)));
  Descriptor tag_fd(ensureDirAt(project_fd, std::string(tag)));
  return root / "releases" / std::string(project_name) / std::string(tag);
}

void writeCiReleaseArtifactRecord(const std::filesystem::path& state_root, std::string_view project_name,
                                  std::string_view tag, const CiArtifactRecord& record) {
  if (!isValidProjectName(project_name) || !isValidReleaseTag(tag) || !isValidArtifactName(record.name)) {
    fail("refusing to write an invalid release asset record");
  }
  // "release" is reserved for release.ini itself (written by
  // writeCiReleaseRecord in the same directory); the workflow parser already
  // rejects the name, but a direct caller (a test, or future code) must not
  // be able to silently overwrite the release record with an asset sidecar.
  if (record.name == "release") fail("artifact name 'release' is reserved for the release record");
  const std::string content = serializeArtifact(record);
  if (content.size() > kMaximumCiArtifactRecordBytes) fail("a release asset record exceeds its size limit");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor releases_fd(ensureDirAt(root_fd, "releases"));
  Descriptor project_fd(ensureDirAt(releases_fd, std::string(project_name)));
  Descriptor tag_fd(ensureDirAt(project_fd, std::string(tag)));
  atomicWriteAt(tag_fd, record.name + ".ini", content);
}

void writeCiReleaseRecord(const std::filesystem::path& state_root, std::string_view project_name,
                          const CiReleaseRecord& record) {
  if (!isValidProjectName(project_name) || !isValidReleaseTag(record.tag) ||
      !isHexObjectId(record.commit_id)) {
    fail("refusing to write an invalid release record");
  }
  if (record.notes.size() > kMaximumReleaseNotesBytes) fail("release notes exceed the size limit");
  const std::string content = serializeRelease(record);
  if (content.size() > kMaximumReleaseRecordBytes) fail("a release record exceeds its size limit");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor releases_fd(ensureDirAt(root_fd, "releases"));
  Descriptor project_fd(ensureDirAt(releases_fd, std::string(project_name)));
  Descriptor tag_fd(ensureDirAt(project_fd, record.tag));
  atomicWriteAt(tag_fd, "release.ini", content);
}

std::vector<CiReleaseRecord> loadReleases(const std::filesystem::path& state_root,
                                          std::string_view project_name, std::size_t maximum) {
  std::vector<CiReleaseRecord> releases;
  if (maximum == 0 || !isValidProjectName(project_name)) return releases;
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  bool missing = false;
  Descriptor releases_fd(openDirAt(root_fd, "releases", &missing));
  if (missing) return releases;
  Descriptor project_fd(openDirAt(releases_fd, std::string(project_name), &missing));
  if (missing) return releases;

  for (const std::string& tag : listNames(project_fd)) {
    if (!isValidReleaseTag(tag)) continue;
    bool tag_missing = false;
    Descriptor tag_fd(openDirAt(project_fd, tag, &tag_missing));
    if (tag_missing) continue;
    bool record_missing = false;
    try {
      const std::string content = readCappedAt(tag_fd, "release.ini", kMaximumReleaseRecordBytes, &record_missing);
      if (record_missing) continue;
      CiReleaseRecord release = parseRelease(content);
      for (const std::string& name : listNames(tag_fd)) {
        if (!endsWith(name, ".ini") || name == "release.ini") continue;
        try {
          bool asset_missing = false;
          const std::string asset = readCappedAt(tag_fd, name, kMaximumCiArtifactRecordBytes, &asset_missing);
          if (!asset_missing) release.assets.push_back(parseArtifact(asset));
        } catch (const std::exception&) {
        }
      }
      std::sort(release.assets.begin(), release.assets.end(),
                [](const CiArtifactRecord& a, const CiArtifactRecord& b) { return a.name < b.name; });
      releases.push_back(std::move(release));
    } catch (const std::exception&) {
      // A malformed release is skipped rather than failing the dashboard.
    }
  }
  std::sort(releases.begin(), releases.end(), [](const CiReleaseRecord& a, const CiReleaseRecord& b) {
    if (a.created_epoch_seconds != b.created_epoch_seconds) {
      return a.created_epoch_seconds > b.created_epoch_seconds;
    }
    return a.tag > b.tag;
  });
  if (releases.size() > maximum) releases.resize(maximum);
  return releases;
}

std::optional<std::string> readCiReleaseAsset(const std::filesystem::path& state_root,
                                              std::string_view project_name, std::string_view tag,
                                              std::string_view asset_name, std::size_t cap) {
  if (!isValidProjectName(project_name) || !isValidReleaseTag(tag) || !isValidArtifactName(asset_name)) {
    return std::nullopt;
  }
  try {
    const std::filesystem::path root = validatedMetadataRoot(state_root);
    Descriptor root_fd(openDir(root));
    bool missing = false;
    Descriptor releases_fd(openDirAt(root_fd, "releases", &missing));
    if (missing) return std::nullopt;
    Descriptor project_fd(openDirAt(releases_fd, std::string(project_name), &missing));
    if (missing) return std::nullopt;
    Descriptor tag_fd(openDirAt(project_fd, std::string(tag), &missing));
    if (missing) return std::nullopt;
    bool blob_missing = false;
    const std::string content = readCappedAt(tag_fd, std::string(asset_name) + ".tar", cap, &blob_missing);
    if (blob_missing) return std::nullopt;
    return content;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void removeCiRelease(const std::filesystem::path& state_root, std::string_view project_name,
                     std::string_view tag) {
  if (!isValidProjectName(project_name) || !isValidReleaseTag(tag)) fail("invalid release for removal");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  std::error_code error;
  std::filesystem::remove_all(root / "releases" / std::string(project_name) / std::string(tag), error);
}

void removeProjectCi(const std::filesystem::path& state_root, std::string_view project_name) {
  if (!isValidProjectName(project_name)) fail("invalid project name for CI removal");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  std::error_code error;
  std::filesystem::remove_all(root / "ci" / "runs" / std::string(project_name), error);
  std::filesystem::remove(root / "ci" / "projects" / (std::string(project_name) + ".ini"), error);
  std::filesystem::remove_all(root / "releases" / std::string(project_name), error);
}

}  // namespace ckgit
