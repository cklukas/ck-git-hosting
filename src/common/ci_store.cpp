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

#include "ckgit/control_rpc.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/text.hpp"
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

std::string fromHex(std::string_view token, std::size_t byte_cap) {
  std::optional<std::string> decoded = hexDecode(token, byte_cap);
  if (!decoded.has_value()) fail("a CI field is not valid hex");
  return std::move(*decoded);
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

// schema_version 2 adds the attempt counter after queued_epoch; parseJob still
// reads a version-1 job (attempt 1) queued by an earlier build.
std::string serializeJob(const CiJobRequest& job) {
  return "schema_version=2\n"
         "job_id=" + job.job_id + "\n"
         "project=" + job.project_name + "\n"
         "ref_hex=" + hexEncode(job.ref) + "\n"
         "commit=" + job.commit_id + "\n"
         "client_id=" + job.client_id + "\n"
         "queued_epoch=" + std::to_string(job.queued_epoch_seconds) + "\n"
         "attempt=" + std::to_string(job.attempt) + "\n";
}

CiJobRequest parseJob(std::string_view content) {
  const std::vector<std::string_view> lines = frame(content);
  const bool v2 = !lines.empty() && lines[0] == "schema_version=2";
  if (!((v2 && lines.size() == 8) || (lines.size() == 7 && lines[0] == "schema_version=1"))) {
    fail("a CI job record has the wrong shape");
  }
  CiJobRequest job;
  job.job_id = std::string(expectField(lines[1], "job_id="));
  job.project_name = std::string(expectField(lines[2], "project="));
  job.ref = fromHex(expectField(lines[3], "ref_hex="), 512);
  job.commit_id = std::string(expectField(lines[4], "commit="));
  job.client_id = std::string(expectField(lines[5], "client_id="));
  job.queued_epoch_seconds = parseEpoch(expectField(lines[6], "queued_epoch="));
  if (v2) {
    const std::uint64_t attempt = parseEpoch(expectField(lines[7], "attempt="));
    if (attempt < 1 || attempt > 1000) fail("a CI job record has an invalid attempt");
    job.attempt = static_cast<unsigned>(attempt);
  }
  if (!isValidCiId(job.job_id) || !isValidProjectName(job.project_name) ||
      !isValidClientId(job.client_id) || !isValidObjectId(job.commit_id)) {
    fail("a CI job record contains invalid data");
  }
  return job;
}

std::string serializeRun(const CiRunRecord& run) {
  // schema_version 2 added heartbeat_epoch after finished_epoch, and 3 adds
  // runner and attempt after it; parseRun still reads every earlier layout
  // written by earlier builds so an upgrade never discards a project's run
  // history.
  std::string out =
      "schema_version=3\n"
      "run_id=" + run.run_id + "\n"
      "project=" + run.project_name + "\n"
      "ref_hex=" + hexEncode(run.ref) + "\n"
      "commit=" + run.commit_id + "\n"
      "status=" + std::string(ciRunStatusName(run.status)) + "\n"
      "started_epoch=" + std::to_string(run.started_epoch_seconds) + "\n"
      "finished_epoch=" + std::to_string(run.finished_epoch_seconds) + "\n"
      "heartbeat_epoch=" + std::to_string(run.heartbeat_epoch_seconds) + "\n"
      "runner=" + (run.runner.empty() ? std::string("-") : run.runner) + "\n"
      "attempt=" + std::to_string(run.attempt) + "\n"
      "detail_hex=" + hexEncode(run.detail) + "\n"
      "step_count=" + std::to_string(run.steps.size()) + "\n";
  for (const CiStepResult& step : run.steps) {
    out += "step=" + std::to_string(step.exit_code) + "," + (step.timed_out ? "1" : "0") + "," +
           (step.output_truncated ? "1" : "0") + "," + hexEncode(step.name) + "\n";
  }
  return out;
}

CiRunRecord parseRun(std::string_view content) {
  const std::vector<std::string_view> lines = frame(content);
  if (lines.empty()) fail("a CI run record has the wrong shape");
  // Version 1 has no heartbeat line and version 2 no runner and attempt lines,
  // so their headers are shorter and the later fields sit earlier.
  const bool v3 = lines[0] == "schema_version=3";
  const bool v2 = v3 || lines[0] == "schema_version=2";
  if (!v2 && lines[0] != "schema_version=1") fail("a CI run record has the wrong shape");
  const std::size_t header = v3 ? 13 : v2 ? 11 : 10;
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
  if (v3) {
    const std::string_view runner = expectField(lines[next++], "runner=");
    if (runner != "-") {
      if (!isValidClientId(runner)) fail("a CI run record names an invalid runner");
      run.runner = std::string(runner);
    }
    const std::uint64_t attempt = parseEpoch(expectField(lines[next++], "attempt="));
    if (attempt < 1 || attempt > 1000) fail("a CI run record has an invalid attempt");
    run.attempt = static_cast<unsigned>(attempt);
  }
  run.detail = fromHex(expectField(lines[next++], "detail_hex="), kMaximumCiDetailBytes);
  const std::string_view count_text = expectField(lines[next], "step_count=");
  std::size_t count = 0;
  const auto [end, error] = std::from_chars(count_text.data(), count_text.data() + count_text.size(), count);
  if (error != std::errc{} || end != count_text.data() + count_text.size()) fail("a CI step count is malformed");
  if (count > kMaximumCiSteps || lines.size() != header + count) fail("a CI run record has a mismatched step count");
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
  if (!isValidCiId(run.run_id) || !isValidProjectName(run.project_name) || !isValidObjectId(run.commit_id)) {
    fail("a CI run record contains invalid data");
  }
  return run;
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
         "note_hex=" + hexEncode(artifact.note) + "\n";
}

CiArtifactRecord parseArtifact(std::string_view content) {
  const std::vector<std::string_view> lines = frame(content);
  if (lines.size() != 7 || lines[0] != "schema_version=1") fail("a CI artifact record has the wrong shape");
  CiArtifactRecord artifact;
  artifact.name = std::string(expectField(lines[1], "name="));
  artifact.bytes = parseEpoch(expectField(lines[2], "bytes="));
  const std::string_view sha = expectField(lines[3], "sha256=");
  if (!sha.empty() && !isValidSha256Hex(sha)) fail("a CI artifact sha256 is malformed");
  artifact.sha256 = std::string(sha);
  artifact.created_epoch_seconds = parseEpoch(expectField(lines[4], "created_epoch="));
  artifact.expires_epoch_seconds = parseEpoch(expectField(lines[5], "expires_epoch="));
  artifact.note = fromHex(expectField(lines[6], "note_hex="), 256);
  if (!isValidCiArtifactName(artifact.name)) fail("a CI artifact record has an invalid name");
  return artifact;
}

std::string serializeRelease(const CiReleaseRecord& release) {
  return "schema_version=1\n"
         "tag=" + release.tag + "\n"
         "commit=" + release.commit_id + "\n"
         "created_epoch=" + std::to_string(release.created_epoch_seconds) + "\n"
         "notes_hex=" + hexEncode(release.notes) + "\n";
}

CiReleaseRecord parseRelease(std::string_view content) {
  const std::vector<std::string_view> lines = frame(content);
  if (lines.size() != 5 || lines[0] != "schema_version=1") fail("a release record has the wrong shape");
  CiReleaseRecord release;
  release.tag = std::string(expectField(lines[1], "tag="));
  release.commit_id = std::string(expectField(lines[2], "commit="));
  release.created_epoch_seconds = parseEpoch(expectField(lines[3], "created_epoch="));
  release.notes = fromHex(expectField(lines[4], "notes_hex="), kMaximumReleaseNotesBytes);
  if (!isValidReleaseTag(release.tag) || !isValidObjectId(release.commit_id)) {
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

bool isValidCiArtifactName(std::string_view name) {
  return !name.empty() && name.size() <= kMaximumCiArtifactNameBytes &&
         std::all_of(name.begin(), name.end(), [](unsigned char character) {
           return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                  character == '-';
         });
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
      !isValidClientId(request.client_id) || !isValidObjectId(request.commit_id)) {
    fail("refusing to queue an invalid CI job");
  }
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor ci_fd(ensureDirAt(root_fd, "ci"));
  Descriptor spool_fd(ensureDirAt(ci_fd, "spool"));
  lockExclusive(spool_fd);
  atomicWriteAt(spool_fd, request.job_id + ".ini", serializeJob(request));

  // Publish the run as Pending immediately, reusing the job id as the run id
  // (the runner reuses it too — see CiRunnerOptions::run_id), so the dashboard
  // lists it and its Cancel button works from the moment it is queued rather
  // than only once a runner claims it. Best-effort: the job is already
  // durably queued above and must run whether or not this preview record can
  // be written.
  try {
    CiRunRecord pending;
    pending.run_id = request.job_id;
    pending.project_name = request.project_name;
    pending.ref = request.ref;
    pending.commit_id = request.commit_id;
    pending.status = CiRunStatus::Pending;
    prepareCiRunDirectory(state_root, request.project_name, request.job_id);
    writeCiRunRecord(state_root, pending);
  } catch (const std::exception&) {
  }
}

CiQueuePaths ensureCiQueueDirectories(const std::filesystem::path& state_root) {
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor ci_fd(ensureDirAt(root_fd, "ci"));
  Descriptor spool_fd(ensureDirAt(ci_fd, "spool"));
  Descriptor working_fd(ensureDirAt(ci_fd, "working"));
  return CiQueuePaths{root / "ci" / "spool", root / "ci" / "working"};
}

std::optional<CiJobRequest> claimNextCiJob(const std::filesystem::path& state_root) {
  // The plain FIFO claim: no per-ref serialization and no delay.
  CiClaimOptions options;
  options.one_per_ref = false;
  const std::optional<CiClaim> claim = claimCiJob(state_root, options);
  if (!claim.has_value()) return std::nullopt;
  return claim->job;
}

namespace {

std::uint64_t currentEpoch() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string generateLeaseToken() {
  std::random_device device;
  static constexpr char hex[] = "0123456789abcdef";
  std::string token;
  token.reserve(32);
  for (int index = 0; index < 32; ++index) token += hex[device() & 0x0f];
  return token;
}

bool isValidLeaseToken(std::string_view token) {
  return token.size() == 32 && std::all_of(token.begin(), token.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

std::string serializeLease(const CiLease& lease) {
  return "schema_version=1\n"
         "runner=" + lease.runner + "\n"
         "token=" + lease.token + "\n"
         "expires_epoch=" + std::to_string(lease.expires_epoch_seconds) + "\n";
}

CiLease parseLease(std::string_view content) {
  const std::vector<std::string_view> lines = frame(content);
  if (lines.size() != 4 || lines[0] != "schema_version=1") fail("a CI lease has the wrong shape");
  CiLease lease;
  lease.runner = std::string(expectField(lines[1], "runner="));
  lease.token = std::string(expectField(lines[2], "token="));
  lease.expires_epoch_seconds = parseEpoch(expectField(lines[3], "expires_epoch="));
  if (!isValidClientId(lease.runner) || !isValidLeaseToken(lease.token)) fail("a CI lease contains invalid data");
  return lease;
}

// The queue's directories, opened once for an operation that holds the spool
// lock across all of them.
struct QueueDirs {
  std::filesystem::path root;
  int spool = -1;
  int working = -1;
  int leases = -1;
};

std::optional<CiLease> readLeaseAt(int leases_fd, const std::string& job_id) {
  try {
    bool missing = false;
    const std::string content = readCappedAt(leases_fd, job_id + ".ini", 4096, &missing);
    if (missing) return std::nullopt;
    return parseLease(content);
  } catch (const std::exception&) {
    return std::nullopt;  // an unreadable lease is treated like a missing one: lapsed
  }
}

std::optional<CiJobRequest> readJobAt(int directory_fd, const std::string& name) {
  try {
    bool missing = false;
    const std::string content = readCappedAt(directory_fd, name, kMaximumCiRunRecordBytes, &missing);
    if (missing) return std::nullopt;
    return parseJob(content);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// Returns a lost attempt's run to Pending so the dashboard shows it waiting
// again: its step logs move aside to attempt-<n>/ (kept for diagnosis), a
// partial artifact or upload set is dropped, and the record is rewritten with
// no steps. Best-effort: the job itself is requeued whether or not this works.
void resetRunForRequeue(const std::filesystem::path& state_root, const CiJobRequest& job, unsigned lost_attempt,
                        const std::string& detail) {
  try {
    const std::filesystem::path run_dir = prepareCiRunDirectory(state_root, job.project_name, job.job_id);
    std::error_code error;
    const std::filesystem::path aside = run_dir / ("attempt-" + std::to_string(lost_attempt));
    std::filesystem::remove_all(aside, error);
    std::filesystem::rename(run_dir / "steps", aside, error);
    std::filesystem::remove_all(run_dir / "artifacts", error);
    std::filesystem::remove_all(run_dir / "incoming", error);
    prepareCiRunDirectory(state_root, job.project_name, job.job_id);  // a fresh, empty steps/
    CiRunRecord pending;
    pending.run_id = job.job_id;
    pending.project_name = job.project_name;
    pending.ref = job.ref;
    pending.commit_id = job.commit_id;
    pending.status = CiRunStatus::Pending;
    pending.attempt = job.attempt;
    pending.detail = detail.substr(0, kMaximumCiDetailBytes);
    writeCiRunRecord(state_root, pending);
  } catch (const std::exception&) {
  }
}

// Puts a claimed job back into the spool (or, past the attempt limit, records
// its run as an error). The caller holds the spool lock.
enum class RequeueOutcome { kRequeued, kFailed, kGone };
RequeueOutcome requeueLocked(const QueueDirs& dirs, const std::string& job_id, const std::string& runner,
                             bool enforce_limit, unsigned max_attempts) {
  const std::string name = job_id + ".ini";
  std::optional<CiJobRequest> job = readJobAt(dirs.working, name);
  if (!job.has_value()) {
    ::renameat(dirs.working, name.c_str(), dirs.working, (name + ".rejected").c_str());
    ::unlinkat(dirs.leases, name.c_str(), 0);
    return RequeueOutcome::kGone;
  }
  const unsigned lost_attempt = job->attempt;
  const std::string who = runner.empty() ? std::string("its runner") : "runner " + runner;
  if (enforce_limit && lost_attempt >= max_attempts) {
    try {
      CiRunRecord record;
      if (std::optional<CiRunRecord> existing = loadCiRun(dirs.root, job->project_name, job->job_id)) {
        record = *existing;
      } else {
        record.run_id = job->job_id;
        record.project_name = job->project_name;
        record.ref = job->ref;
        record.commit_id = job->commit_id;
      }
      record.status = CiRunStatus::Error;
      record.finished_epoch_seconds = currentEpoch();
      record.heartbeat_epoch_seconds = record.finished_epoch_seconds;
      record.detail = (who + " was lost; gave up after " + std::to_string(lost_attempt) + " attempt" +
                       (lost_attempt == 1 ? "" : "s")).substr(0, kMaximumCiDetailBytes);
      writeCiRunRecord(dirs.root, record);
    } catch (const std::exception&) {
    }
    ::unlinkat(dirs.working, name.c_str(), 0);
    ::unlinkat(dirs.leases, name.c_str(), 0);
    ::fsync(dirs.working);
    return RequeueOutcome::kFailed;
  }
  job->attempt = lost_attempt + 1;
  resetRunForRequeue(dirs.root, *job,
                     lost_attempt,
                     "requeued: " + who + (enforce_limit ? " was lost" : " gave the job back") + " (attempt " +
                         std::to_string(job->attempt) + ")");
  // Into the spool first, then out of working/: a crash in between leaves the
  // job queued twice under one id, never lost; the second claim of it finds the
  // working copy and simply replaces it.
  atomicWriteAt(dirs.spool, name, serializeJob(*job));
  ::unlinkat(dirs.working, name.c_str(), 0);
  ::unlinkat(dirs.leases, name.c_str(), 0);
  ::fsync(dirs.working);
  return RequeueOutcome::kRequeued;
}

}  // namespace

std::optional<CiClaim> claimCiJob(const std::filesystem::path& state_root, const CiClaimOptions& options,
                                  std::optional<std::chrono::milliseconds>* retry_in) {
  if (retry_in != nullptr) retry_in->reset();
  if (!isValidClientId(options.runner) || options.lease_seconds == 0) fail("invalid CI claim options");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  bool missing = false;
  Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
  if (missing) return std::nullopt;
  Descriptor spool_fd(openDirAt(ci_fd, "spool", &missing));
  if (missing) return std::nullopt;
  Descriptor working_fd(ensureDirAt(ci_fd, "working"));
  Descriptor leases_fd(ensureDirAt(ci_fd, "leases"));

  lockExclusive(spool_fd);
  std::vector<std::string> names;
  for (std::string& name : listNames(spool_fd)) {
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".ini") == 0) names.push_back(std::move(name));
  }
  std::sort(names.begin(), names.end());

  // The project/ref pairs that already have a run in progress.
  std::set<std::pair<std::string, std::string>> busy;
  if (options.one_per_ref) {
    for (const std::string& name : listNames(working_fd)) {
      if (!endsWith(name, ".ini")) continue;
      if (std::optional<CiJobRequest> running = readJobAt(working_fd, name)) {
        busy.emplace(running->project_name, running->ref);
      }
    }
  }
  const auto now = std::chrono::system_clock::now();
  for (const std::string& name : names) {
    // Eligibility is decided on the spool copy before the claim; a job that
    // cannot be read here goes through the claim below, which sets it aside.
    if (options.one_per_ref || options.min_age.count() > 0) {
      if (std::optional<CiJobRequest> queued = readJobAt(spool_fd, name)) {
        if (busy.count({queued->project_name, queued->ref}) != 0) continue;
        if (options.min_age.count() > 0) {
          struct stat status {};
          if (::fstatat(spool_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
#if defined(__APPLE__)
            const auto modified = std::chrono::seconds(status.st_mtimespec.tv_sec) +
                                  std::chrono::nanoseconds(status.st_mtimespec.tv_nsec);
#else
            const auto modified =
                std::chrono::seconds(status.st_mtim.tv_sec) + std::chrono::nanoseconds(status.st_mtim.tv_nsec);
#endif
            const std::chrono::system_clock::time_point queued_at(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(modified));
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - queued_at);
            if (age < options.min_age) {
              const auto wait = options.min_age - age;
              if (retry_in != nullptr && (!retry_in->has_value() || wait < **retry_in)) *retry_in = wait;
              continue;
            }
          }
        }
      }
    }
    if (::renameat(spool_fd, name.c_str(), working_fd, name.c_str()) != 0) {
      if (errno == ENOENT) continue;  // claimed by another runner
      fail("could not claim a CI job");
    }
    if (::fsync(working_fd) != 0) fail("could not commit a CI claim");
    CiJobRequest job;
    try {
      bool gone = false;
      const std::string content = readCappedAt(working_fd, name, kMaximumCiRunRecordBytes, &gone);
      if (gone) continue;
      job = parseJob(content);
    } catch (const std::exception&) {
      // An unreadable, unsafe, or malformed job is set aside, never executed.
      ::renameat(working_fd, name.c_str(), working_fd, (name + ".rejected").c_str());
      continue;
    }
    CiLease lease;
    lease.runner = options.runner;
    lease.token = generateLeaseToken();
    lease.expires_epoch_seconds = currentEpoch() + options.lease_seconds;
    atomicWriteAt(leases_fd, name, serializeLease(lease));
    return CiClaim{std::move(job), std::move(lease)};
  }
  return std::nullopt;
}

bool renewCiLease(const std::filesystem::path& state_root, std::string_view job_id, std::string_view token,
                  unsigned lease_seconds) {
  if (!isValidCiId(job_id) || !isValidLeaseToken(token)) return false;
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  bool missing = false;
  Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
  if (missing) return false;
  Descriptor spool_fd(ensureDirAt(ci_fd, "spool"));
  Descriptor working_fd(ensureDirAt(ci_fd, "working"));
  Descriptor leases_fd(ensureDirAt(ci_fd, "leases"));
  lockExclusive(spool_fd);
  const std::string id(job_id);
  struct stat status {};
  if (::fstatat(working_fd, (id + ".ini").c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) return false;
  std::optional<CiLease> lease = readLeaseAt(leases_fd, id);
  if (!lease.has_value() || lease->token != token) return false;
  lease->expires_epoch_seconds = currentEpoch() + lease_seconds;
  atomicWriteAt(leases_fd, id + ".ini", serializeLease(*lease));
  return true;
}

std::optional<CiClaim> loadCiClaim(const std::filesystem::path& state_root, std::string_view job_id) {
  if (!isValidCiId(job_id)) return std::nullopt;
  try {
    const std::filesystem::path root = validatedMetadataRoot(state_root);
    Descriptor root_fd(openDir(root));
    bool missing = false;
    Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
    if (missing) return std::nullopt;
    Descriptor working_fd(openDirAt(ci_fd, "working", &missing));
    if (missing) return std::nullopt;
    Descriptor leases_fd(openDirAt(ci_fd, "leases", &missing));
    if (missing) return std::nullopt;
    const std::string id(job_id);
    std::optional<CiJobRequest> job = readJobAt(working_fd, id + ".ini");
    std::optional<CiLease> lease = readLeaseAt(leases_fd, id);
    if (!job.has_value() || !lease.has_value()) return std::nullopt;
    return CiClaim{std::move(*job), std::move(*lease)};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

CiReapResult reapCiLeases(const std::filesystem::path& state_root, const CiReapOptions& options) {
  CiReapResult result;
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  bool missing = false;
  Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
  if (missing) return result;
  Descriptor working_fd(openDirAt(ci_fd, "working", &missing));
  if (missing) return result;
  Descriptor spool_fd(ensureDirAt(ci_fd, "spool"));
  Descriptor leases_fd(ensureDirAt(ci_fd, "leases"));
  lockExclusive(spool_fd);
  const QueueDirs dirs{root, spool_fd, working_fd, leases_fd};
  const std::uint64_t now = options.now_epoch_seconds != 0 ? options.now_epoch_seconds : currentEpoch();
  for (const std::string& name : listNames(working_fd)) {
    if (!endsWith(name, ".ini")) continue;
    const std::string job_id = name.substr(0, name.size() - 4);
    if (!isValidCiId(job_id)) continue;
    const std::optional<CiLease> lease = readLeaseAt(leases_fd, job_id);
    const bool lapsed = !lease.has_value() || lease->expires_epoch_seconds <= now ||
                        (!options.restarted_runner.empty() && lease->runner == options.restarted_runner);
    if (!lapsed) {
      if (!result.next_expiry_epoch_seconds.has_value() ||
          lease->expires_epoch_seconds < *result.next_expiry_epoch_seconds) {
        result.next_expiry_epoch_seconds = lease->expires_epoch_seconds;
      }
      continue;
    }
    switch (requeueLocked(dirs, job_id, lease.has_value() ? lease->runner : std::string(), true,
                          options.max_attempts == 0 ? 1 : options.max_attempts)) {
      case RequeueOutcome::kRequeued: result.requeued.push_back(job_id); break;
      case RequeueOutcome::kFailed: result.failed.push_back(job_id); break;
      case RequeueOutcome::kGone: break;
    }
  }
  return result;
}

bool abandonCiJob(const std::filesystem::path& state_root, std::string_view job_id, std::string_view token,
                  std::string_view runner) {
  if (!isValidCiId(job_id) || !isValidLeaseToken(token)) return false;
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  bool missing = false;
  Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
  if (missing) return false;
  Descriptor working_fd(openDirAt(ci_fd, "working", &missing));
  if (missing) return false;
  Descriptor spool_fd(ensureDirAt(ci_fd, "spool"));
  Descriptor leases_fd(ensureDirAt(ci_fd, "leases"));
  lockExclusive(spool_fd);
  const std::string id(job_id);
  const std::optional<CiLease> lease = readLeaseAt(leases_fd, id);
  if (!lease.has_value() || lease->token != token) return false;
  const QueueDirs dirs{root, spool_fd, working_fd, leases_fd};
  return requeueLocked(dirs, id, std::string(runner), false, 0) == RequeueOutcome::kRequeued;
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
  Descriptor leases_fd(openDirAt(ci_fd, "leases", &missing));
  if (missing) return;
  if (::unlinkat(leases_fd, name.c_str(), 0) != 0 && errno != ENOENT) fail("could not release a CI lease");
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

std::filesystem::path prepareCiIncomingDirectory(const std::filesystem::path& state_root,
                                                 std::string_view project_name, std::string_view run_id) {
  if (!isValidProjectName(project_name)) fail("invalid project name for CI staging");
  if (!isValidCiId(run_id)) fail("invalid CI run id");
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor ci_fd(ensureDirAt(root_fd, "ci"));
  Descriptor runs_fd(ensureDirAt(ci_fd, "runs"));
  Descriptor project_fd(ensureDirAt(runs_fd, std::string(project_name)));
  Descriptor run_fd(ensureDirAt(project_fd, std::string(run_id)));
  Descriptor incoming_fd(ensureDirAt(run_fd, "incoming"));
  return root / "ci" / "runs" / std::string(project_name) / std::string(run_id) / "incoming";
}

void writeCiRunRecord(const std::filesystem::path& state_root, const CiRunRecord& record) {
  if (!isValidCiId(record.run_id) || !isValidProjectName(record.project_name) ||
      !isValidObjectId(record.commit_id)) {
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
  if (!isValidProjectName(project_name) || !isValidCiId(run_id) || !isValidCiArtifactName(record.name)) {
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

std::optional<CiLogTail> readCiRunLogTail(const std::filesystem::path& state_root, std::string_view project_name,
                                          std::string_view run_id, std::size_t step_index, std::size_t max_bytes) {
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
    const std::string name = std::to_string(step_index) + ".log";
    const int raw = ::openat(steps_fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (raw < 0) return std::nullopt;
    Descriptor file(raw);
    struct stat status {};
    if (::fstat(file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0077) != 0 || status.st_size < 0) {
      return std::nullopt;
    }
    CiLogTail tail;
    tail.total = static_cast<std::uint64_t>(status.st_size);
    const std::uint64_t start = tail.total > max_bytes ? tail.total - max_bytes : 0;
    tail.bytes.resize(static_cast<std::size_t>(tail.total - start));
    std::size_t got = 0;
    while (got < tail.bytes.size()) {
      const ssize_t received =
          ::pread(file, tail.bytes.data() + got, tail.bytes.size() - got, static_cast<off_t>(start + got));
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) break;
      got += static_cast<std::size_t>(received);
    }
    tail.bytes.resize(got);
    return tail;
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
  if (!isValidProjectName(project_name) || !isValidCiId(run_id) || !isValidCiArtifactName(artifact_name)) {
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

namespace {

std::string serializeRunnerRecord(const CiRunnerRecord& record) {
  return "schema_version=1\n"
         "enabled=" + std::string(record.enabled ? "true" : "false") + "\n"
         "last_seen_epoch=" + std::to_string(record.last_seen_epoch_seconds) + "\n"
         "version_hex=" + hexEncode(record.version) + "\n";
}

CiRunnerRecord parseRunnerRecord(std::string_view name, std::string_view content) {
  const std::vector<std::string_view> lines = frame(content);
  if (lines.size() != 4 || lines[0] != "schema_version=1") fail("a runner record has the wrong shape");
  CiRunnerRecord record;
  record.name = std::string(name);
  const std::string_view enabled = expectField(lines[1], "enabled=");
  if (enabled != "true" && enabled != "false") fail("a runner record has an invalid flag");
  record.enabled = enabled == "true";
  record.last_seen_epoch_seconds = parseEpoch(expectField(lines[2], "last_seen_epoch="));
  record.version = fromHex(expectField(lines[3], "version_hex="), 128);
  return record;
}

}  // namespace

void writeCiRunnerRecord(const std::filesystem::path& state_root, const CiRunnerRecord& record) {
  if (!isValidClientId(record.name) || record.name == kCiPrimaryRunnerName || record.version.size() > 128) {
    fail("refusing to write an invalid runner record");
  }
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  Descriptor root_fd(openDir(root));
  Descriptor ci_fd(ensureDirAt(root_fd, "ci"));
  Descriptor runners_fd(ensureDirAt(ci_fd, "runners"));
  atomicWriteAt(runners_fd, record.name + ".ini", serializeRunnerRecord(record));
}

std::optional<CiRunnerRecord> loadCiRunnerRecord(const std::filesystem::path& state_root, std::string_view name) {
  if (!isValidClientId(name)) return std::nullopt;
  try {
    const std::filesystem::path root = validatedMetadataRoot(state_root);
    Descriptor root_fd(openDir(root));
    bool missing = false;
    Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
    if (missing) return std::nullopt;
    Descriptor runners_fd(openDirAt(ci_fd, "runners", &missing));
    if (missing) return std::nullopt;
    bool absent = false;
    const std::string content = readCappedAt(runners_fd, std::string(name) + ".ini", 4096, &absent);
    if (absent) return std::nullopt;
    return parseRunnerRecord(name, content);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::vector<CiRunnerRecord> loadCiRunnerRecords(const std::filesystem::path& state_root) {
  std::vector<CiRunnerRecord> records;
  try {
    const std::filesystem::path root = validatedMetadataRoot(state_root);
    Descriptor root_fd(openDir(root));
    bool missing = false;
    Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
    if (missing) return records;
    Descriptor runners_fd(openDirAt(ci_fd, "runners", &missing));
    if (missing) return records;
    for (const std::string& file : listNames(runners_fd)) {
      if (!endsWith(file, ".ini")) continue;
      const std::string name = file.substr(0, file.size() - 4);
      if (std::optional<CiRunnerRecord> record = loadCiRunnerRecord(state_root, name)) records.push_back(*record);
    }
  } catch (const std::exception&) {
  }
  std::sort(records.begin(), records.end(),
            [](const CiRunnerRecord& a, const CiRunnerRecord& b) { return a.name < b.name; });
  return records;
}

bool removeCiRunnerRecord(const std::filesystem::path& state_root, std::string_view name) {
  if (!isValidClientId(name)) return false;
  const std::filesystem::path root = validatedMetadataRoot(state_root);
  std::error_code error;
  return std::filesystem::remove(root / "ci" / "runners" / (std::string(name) + ".ini"), error);
}

void noteCiRunnerSeen(const std::filesystem::path& state_root, std::string_view name, std::string_view version) {
  try {
    CiRunnerRecord record;
    if (std::optional<CiRunnerRecord> existing = loadCiRunnerRecord(state_root, name)) record = *existing;
    record.name = std::string(name);
    record.last_seen_epoch_seconds = currentEpoch();
    if (!version.empty()) record.version = std::string(version.substr(0, 128));
    writeCiRunnerRecord(state_root, record);
  } catch (const std::exception&) {
  }
}

std::vector<CiClaim> listCiClaims(const std::filesystem::path& state_root) {
  std::vector<CiClaim> claims;
  try {
    const std::filesystem::path root = validatedMetadataRoot(state_root);
    Descriptor root_fd(openDir(root));
    bool missing = false;
    Descriptor ci_fd(openDirAt(root_fd, "ci", &missing));
    if (missing) return claims;
    Descriptor working_fd(openDirAt(ci_fd, "working", &missing));
    if (missing) return claims;
    for (const std::string& file : listNames(working_fd)) {
      if (!endsWith(file, ".ini")) continue;
      if (std::optional<CiClaim> claim = loadCiClaim(state_root, file.substr(0, file.size() - 4))) {
        claims.push_back(std::move(*claim));
      }
    }
  } catch (const std::exception&) {
  }
  std::sort(claims.begin(), claims.end(),
            [](const CiClaim& a, const CiClaim& b) { return a.job.job_id < b.job.job_id; });
  return claims;
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
      // Staging left behind by a run that has ended (a crash between upload
      // and commit) is never committed any more.
      const std::filesystem::path incoming = project_entry.path() / id / "incoming";
      std::error_code incoming_ec;
      if (std::filesystem::is_directory(incoming, incoming_ec)) {
        const std::optional<CiRunRecord> run = loadCiRun(root, project, id);
        if (!run.has_value() || !ciRunStatusIsActive(run->status)) {
          std::filesystem::remove_all(incoming, incoming_ec);
        }
      }
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
  if (!isValidProjectName(project_name) || !isValidReleaseTag(tag) || !isValidCiArtifactName(record.name)) {
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
      !isValidObjectId(record.commit_id)) {
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

namespace {

std::vector<std::string_view> splitReleaseFields(std::string_view line, std::size_t expected_fields) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t space = line.find(' ', start);
    if (space == std::string_view::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, space - start));
    start = space + 1;
  }
  if (fields.size() != expected_fields ||
      std::any_of(fields.begin(), fields.end(), [](std::string_view field) { return field.empty(); })) {
    throw std::invalid_argument("releases response has a malformed record");
  }
  return fields;
}

std::uint64_t parseReleaseCounter(std::string_view field, const char* what) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
  if (error != std::errc{} || end != field.data() + field.size()) {
    throw std::invalid_argument(std::string("releases response has an invalid ") + what);
  }
  return value;
}

}  // namespace

std::vector<CiReleaseRecord> parseReleasesControlResponse(std::string_view response) {
  if (response.empty() || response.size() > kMaximumControlResponseBytes || response.back() != '\n' ||
      response.find('\r') != std::string_view::npos || response.find('\0') != std::string_view::npos) {
    throw std::invalid_argument("invalid releases response framing");
  }
  const std::size_t first_newline = response.find('\n');
  const std::string_view first_line = response.substr(0, first_newline);
  constexpr std::string_view prefix{"ok "};
  if (first_line.rfind(prefix, 0) != 0 || first_line.size() == prefix.size()) {
    throw std::invalid_argument("releases response has no valid ok count");
  }
  const std::size_t expected = parseReleaseCounter(first_line.substr(prefix.size()), "count");
  if (expected > kMaximumControlReleases) {
    throw std::invalid_argument("releases response has an invalid count");
  }

  std::vector<CiReleaseRecord> releases;
  CiReleaseRecord* current = nullptr;
  std::size_t start = first_newline + 1;
  while (start < response.size()) {
    const std::size_t newline = response.find('\n', start);
    if (newline == std::string_view::npos || newline == start) {
      throw std::invalid_argument("releases response has an empty or unterminated record");
    }
    const std::string_view line = response.substr(start, newline - start);
    start = newline + 1;
    if (line.rfind("release ", 0) == 0) {
      const auto fields = splitReleaseFields(line, 4);
      const std::string_view tag = fields[1];
      const std::string_view commit = fields[2];
      if (!isValidReleaseTag(tag) || !isValidObjectId(commit)) {
        throw std::invalid_argument("releases response has an invalid release record");
      }
      if (std::any_of(releases.begin(), releases.end(),
                       [&](const CiReleaseRecord& existing) { return existing.tag == tag; })) {
        throw std::invalid_argument("releases response repeats a release tag");
      }
      const std::uint64_t created = parseReleaseCounter(fields[3], "release timestamp");
      releases.push_back(CiReleaseRecord{std::string(tag), std::string(commit), created, {}, {}});
      current = &releases.back();
    } else if (line.rfind("asset ", 0) == 0) {
      if (current == nullptr) {
        throw std::invalid_argument("releases response has an asset before any release");
      }
      const auto fields = splitReleaseFields(line, 5);
      if (fields[1] != current->tag) {
        throw std::invalid_argument("releases response asset tag does not match its release");
      }
      const std::string_view name = fields[2];
      if (!isValidCiArtifactName(name)) {
        throw std::invalid_argument("releases response has an invalid asset name");
      }
      const std::uint64_t bytes = parseReleaseCounter(fields[3], "asset size");
      const std::string_view sha_field = fields[4];
      std::string sha256;
      if (sha_field != "-") {
        if (!isValidSha256Hex(sha_field)) {
          throw std::invalid_argument("releases response has an invalid asset checksum");
        }
        sha256 = std::string(sha_field);
      }
      current->assets.push_back(CiArtifactRecord{std::string(name), bytes, sha256, 0, 0, {}});
    } else {
      throw std::invalid_argument("releases response has an unrecognized record");
    }
  }
  if (releases.size() != expected) {
    throw std::invalid_argument("releases response count does not match records");
  }
  return releases;
}

std::optional<std::string> readCiReleaseAsset(const std::filesystem::path& state_root,
                                              std::string_view project_name, std::string_view tag,
                                              std::string_view asset_name, std::size_t cap) {
  if (!isValidProjectName(project_name) || !isValidReleaseTag(tag) || !isValidCiArtifactName(asset_name)) {
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
