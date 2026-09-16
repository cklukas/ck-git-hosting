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
#include <functional>
#include <optional>
#include <random>
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
  std::string out =
      "schema_version=1\n"
      "run_id=" + run.run_id + "\n"
      "project=" + run.project_name + "\n"
      "ref_hex=" + toHex(run.ref) + "\n"
      "commit=" + run.commit_id + "\n"
      "status=" + std::string(ciRunStatusName(run.status)) + "\n"
      "started_epoch=" + std::to_string(run.started_epoch_seconds) + "\n"
      "finished_epoch=" + std::to_string(run.finished_epoch_seconds) + "\n"
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
  if (lines.size() < 10 || lines[0] != "schema_version=1") fail("a CI run record has the wrong shape");
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
  run.detail = fromHex(expectField(lines[8], "detail_hex="), kMaximumCiDetailBytes);
  const std::string_view count_text = expectField(lines[9], "step_count=");
  std::size_t count = 0;
  const auto [end, error] = std::from_chars(count_text.data(), count_text.data() + count_text.size(), count);
  if (error != std::errc{} || end != count_text.data() + count_text.size()) fail("a CI step count is malformed");
  if (count > 4096 || lines.size() != 10 + count) fail("a CI run record has a mismatched step count");
  for (std::size_t index = 0; index < count; ++index) {
    const std::string_view step_line = expectField(lines[10 + index], "step=");
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
  return std::nullopt;
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
      runs.push_back(parseRun(content));
    } catch (const std::exception&) {
      // A malformed run record is skipped rather than failing the dashboard.
    }
  }
  return runs;
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

void removeProjectCi(const std::filesystem::path& state_root, std::string_view project_name) {
  if (!isValidProjectName(project_name)) fail("invalid project name for CI removal");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  std::error_code error;
  std::filesystem::remove_all(root / "ci" / "runs" / std::string(project_name), error);
  std::filesystem::remove(root / "ci" / "projects" / (std::string(project_name) + ".ini"), error);
}

}  // namespace ckgit
