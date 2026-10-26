// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_results.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <random>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

#include "ckgit/hash.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/pages_store.hpp"
#include "ckgit/process.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("ci results: " + message); }

std::uint64_t nowEpoch() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

bool isValidUploadName(CiUploadKind kind, std::string_view name) {
  if (kind == CiUploadKind::kPages) return name == kCiPagesUploadName;
  return isValidCiArtifactName(name) && name != "release";
}

std::string stem(CiUploadKind kind, std::string_view name) {
  return std::string(ciUploadKindName(kind)) + "-" + std::string(name);
}

// The run's staging directory, without creating anything.
std::filesystem::path incomingPath(const std::filesystem::path& state_root, std::string_view project,
                                   std::string_view run_id) {
  if (!isValidProjectName(project) || !isValidCiId(run_id)) fail("invalid project or run id");
  return validatedMetadataRoot(state_root) / "ci" / "runs" / std::string(project) / std::string(run_id) /
         "incoming";
}

std::string serializeMeta(const CiStagedUpload& upload) {
  return "schema_version=1\n"
         "kind=" + std::string(ciUploadKindName(upload.kind)) + "\n"
         "name=" + upload.name + "\n"
         "total=" + std::to_string(upload.total) + "\n"
         "sha256=" + upload.sha256 + "\n";
}

std::optional<std::uint64_t> parseNumber(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

std::optional<CiStagedUpload> readMeta(const std::filesystem::path& file) {
  std::error_code error;
  const auto size = std::filesystem::file_size(file, error);
  if (error || size > 4096 || std::filesystem::is_symlink(std::filesystem::symlink_status(file, error))) {
    return std::nullopt;
  }
  std::ifstream in(file, std::ios::binary);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(in, line)) lines.push_back(line);
  if (lines.size() != 5 || lines[0] != "schema_version=1") return std::nullopt;
  const auto field = [&lines](std::size_t index, std::string_view key) -> std::optional<std::string> {
    if (lines[index].rfind(key, 0) != 0) return std::nullopt;
    return lines[index].substr(key.size());
  };
  const auto kind_text = field(1, "kind=");
  const auto name = field(2, "name=");
  const auto total_text = field(3, "total=");
  const auto sha = field(4, "sha256=");
  if (!kind_text || !name || !total_text || !sha) return std::nullopt;
  const auto kind = ciUploadKindFromName(*kind_text);
  const auto total = parseNumber(*total_text);
  if (!kind || !total || !isValidUploadName(*kind, *name) || !isValidSha256Hex(*sha)) return std::nullopt;
  CiStagedUpload upload;
  upload.kind = *kind;
  upload.name = *name;
  upload.total = *total;
  upload.sha256 = *sha;
  const std::filesystem::path part = file.parent_path() / (stem(*kind, *name) + ".part");
  const auto held = std::filesystem::file_size(part, error);
  upload.held = error ? 0 : held;
  return upload;
}

void writeFileAtomic(const std::filesystem::path& file, const std::string& content) {
  std::random_device device;
  const std::filesystem::path staging =
      file.parent_path() / (".staging-" + std::to_string(::getpid()) + "-" + std::to_string(device()));
  const int fd = ::open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) fail("could not stage an upload record");
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t written = ::write(fd, content.data() + offset, content.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      ::close(fd);
      ::unlink(staging.c_str());
      fail("could not write an upload record");
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    ::unlink(staging.c_str());
    fail("could not persist an upload record");
  }
  ::close(fd);
  if (::rename(staging.c_str(), file.c_str()) != 0) {
    ::unlink(staging.c_str());
    fail("could not commit an upload record");
  }
  const int parent = ::open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (parent < 0) fail("could not open upload record directory");
  const int persisted = ::fsync(parent);
  ::close(parent);
  if (persisted != 0) fail("could not persist upload record directory");
}

// Takes a complete, verified staged upload into `destination`. Returns an
// empty string on success, else the note to record instead of the bytes.
std::string takeStaged(const std::map<std::string, CiStagedUpload>& staged, const std::filesystem::path& incoming,
                       CiUploadKind kind, const std::string& name, const std::filesystem::path& destination,
                       std::uint64_t* bytes, std::string* sha256) {
  const auto found = staged.find(stem(kind, name));
  if (found == staged.end()) return "the output was never uploaded";
  const CiStagedUpload& upload = found->second;
  if (upload.held != upload.total) return "the upload was incomplete";
  const std::filesystem::path part = incoming / (stem(kind, name) + ".part");
  const std::string actual = sha256HexOfFile(part);
  if (actual != upload.sha256) return "the upload failed its checksum";
  std::error_code error;
  std::filesystem::rename(part, destination, error);
  if (error) return "the upload could not be stored";
  ::chmod(destination.c_str(), 0600);
  *bytes = upload.total;
  *sha256 = actual;
  return {};
}

std::string firstLine(const std::string& text) {
  const std::size_t newline = text.find('\n');
  std::string line = newline == std::string::npos ? text : text.substr(0, newline);
  if (line.size() > kMaximumCiDetailBytes) line.resize(kMaximumCiDetailBytes);
  return line;
}

// The message of an annotated tag object, for release notes. Empty for a
// lightweight tag or on any error.
std::string readTagNotes(const std::filesystem::path& repository, const std::string& id) {
  if (repository.empty()) return {};
  ProcessOptions options;
  options.timeout = std::chrono::seconds(10);
  options.output_limit = 64 * 1024;
  const ProcessResult type = runProcess({"git", "--git-dir", repository.string(), "cat-file", "-t", id}, options);
  if (type.exit_code != 0 || type.timed_out) return {};
  std::string kind = type.output;
  while (!kind.empty() && (kind.back() == '\n' || kind.back() == '\r')) kind.pop_back();
  if (kind != "tag") return {};
  const ProcessResult object = runProcess({"git", "--git-dir", repository.string(), "cat-file", "-p", id}, options);
  if (object.exit_code != 0 || object.timed_out) return {};
  const std::size_t blank = object.output.find("\n\n");
  if (blank == std::string::npos) return {};
  std::string notes = object.output.substr(blank + 2);
  while (!notes.empty() && notes.back() == '\n') notes.pop_back();
  if (notes.size() > kMaximumReleaseNotesBytes) notes.resize(kMaximumReleaseNotesBytes);
  return notes;
}

}  // namespace

std::uint64_t appendCiStagedUpload(const std::filesystem::path& state_root, std::string_view project,
                                   std::string_view run_id, CiUploadKind kind, std::string_view name,
                                   std::uint64_t total, std::string_view sha256, std::uint64_t offset,
                                   std::string_view bytes, std::uint64_t max_total) {
  if (!isValidUploadName(kind, name) || !isValidSha256Hex(sha256)) fail("invalid upload name or checksum");
  if (total > max_total) fail("the upload exceeds the size cap");
  if (bytes.size() > kMaximumCiUploadChunkBytes) fail("an upload chunk is too large");
  if (offset > total || bytes.size() > total - offset) fail("an upload chunk runs past the announced size");
  const std::filesystem::path incoming = prepareCiIncomingDirectory(state_root, project, run_id);
  const std::string base = stem(kind, name);
  const std::filesystem::path meta_path = incoming / (base + ".ini");
  const std::filesystem::path part = incoming / (base + ".part");
  std::optional<CiStagedUpload> meta = readMeta(meta_path);
  const bool same = meta.has_value() && meta->total == total && meta->sha256 == sha256;
  if (!same) {
    // A new upload (or a fresh one replacing an earlier attempt) starts at 0.
    if (offset != 0) fail("an upload must start at offset 0");
    std::error_code error;
    std::filesystem::remove(part, error);
    CiStagedUpload fresh;
    fresh.kind = kind;
    fresh.name = std::string(name);
    fresh.total = total;
    fresh.sha256 = std::string(sha256);
    writeFileAtomic(meta_path, serializeMeta(fresh));
    meta = fresh;
    meta->held = 0;
  }
  // A chunk the server already holds (a retry after a lost reply) is ignored;
  // the caller continues from the returned offset.
  if (offset < meta->held) return meta->held;
  if (offset > meta->held) fail("an upload chunk skips bytes the server does not hold");
  if (bytes.empty()) return meta->held;
  const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) fail("could not open a staged upload");
  std::size_t written_total = 0;
  while (written_total < bytes.size()) {
    const ssize_t written = ::write(fd, bytes.data() + written_total, bytes.size() - written_total);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      ::close(fd);
      fail("could not write a staged upload");
    }
    written_total += static_cast<std::size_t>(written);
  }
  ::close(fd);
  return meta->held + bytes.size();
}

void stageCiFile(const std::filesystem::path& state_root, std::string_view project, std::string_view run_id,
                 CiUploadKind kind, std::string_view name, const std::filesystem::path& file) {
  if (!isValidUploadName(kind, name)) fail("invalid upload name");
  const std::filesystem::path incoming = prepareCiIncomingDirectory(state_root, project, run_id);
  const std::string base = stem(kind, name);
  const std::filesystem::path part = incoming / (base + ".part");
  std::filesystem::copy_file(file, part, std::filesystem::copy_options::overwrite_existing);
  ::chmod(part.c_str(), 0600);
  CiStagedUpload upload;
  upload.kind = kind;
  upload.name = std::string(name);
  upload.total = std::filesystem::file_size(part);
  upload.sha256 = sha256HexOfFile(part);
  writeFileAtomic(incoming / (base + ".ini"), serializeMeta(upload));
}

std::vector<CiStagedUpload> listCiStagedUploads(const std::filesystem::path& state_root, std::string_view project,
                                                std::string_view run_id) {
  std::vector<CiStagedUpload> uploads;
  const std::filesystem::path incoming = incomingPath(state_root, project, run_id);
  std::error_code error;
  if (!std::filesystem::is_directory(incoming, error)) return uploads;
  for (const auto& entry : std::filesystem::directory_iterator(incoming, error)) {
    const std::string file = entry.path().filename().string();
    if (file.size() <= 4 || file.compare(file.size() - 4, 4, ".ini") != 0) continue;
    if (std::optional<CiStagedUpload> upload = readMeta(entry.path())) uploads.push_back(std::move(*upload));
  }
  std::sort(uploads.begin(), uploads.end(), [](const CiStagedUpload& a, const CiStagedUpload& b) {
    return std::make_pair(a.kind, a.name) < std::make_pair(b.kind, b.name);
  });
  return uploads;
}

void discardCiStagedUploads(const std::filesystem::path& state_root, std::string_view project,
                            std::string_view run_id) {
  std::error_code error;
  std::filesystem::remove_all(incomingPath(state_root, project, run_id), error);
}

CiRunRecord commitCiRun(const CiCommitPolicy& policy, const CiPreparedJob& prepared, CiRunRecord record,
                        const std::vector<CiDeclaredOutput>& outputs,
                        const std::optional<std::filesystem::path>& site_dir) {
  const std::string& project = prepared.job.project_name;
  const std::string& run_id = prepared.job.job_id;
  const bool child = !prepared.job.parent_run_id.empty();
  if (child) {
    const auto parent_dir = prepareCiRunDirectory(policy.state_root, project, prepared.job.parent_run_id);
    if (std::filesystem::exists(parent_dir / "outputs-committed")) fail("workflow outputs are already finalized");
    writeFileAtomic(parent_dir / "outputs-pending", "pending\n");
  }
  record.parent_run_id = prepared.job.parent_run_id;
  record.workflow_job = prepared.job.workflow_job;
  if (child && (prepared.workflow.version != 2 || prepared.workflow.jobs.size() != 1 ||
                prepared.workflow.jobs.front().name != prepared.job.workflow_job))
    fail("child result must belong to exactly one assigned workflow job");
  const std::filesystem::path incoming = incomingPath(policy.state_root, project, run_id);
  // A local child's source tree disappears when its job ends. Preserve its
  // site as the same staged archive a remote runner uploads, for the parent.
  if (child && record.status == CiRunStatus::Success && site_dir && prepared.workflow.pages_path &&
      !prepared.is_release && prepared.is_default_branch) {
    prepareCiIncomingDirectory(policy.state_root, project, run_id);
    const auto archive = incoming / "site-local.tar";
    ProcessOptions options;
    options.timeout = std::chrono::seconds(600);
    options.output_limit = 16 * 1024;
    const auto packed = runProcess({"tar", "-cf", archive.string(), "-C", site_dir->string(), "."}, options);
    if (packed.timed_out || packed.exit_code != 0) fail("child Pages site could not be packed");
    stageCiFile(policy.state_root, project, run_id, CiUploadKind::kPages, kCiPagesUploadName, archive);
    std::filesystem::remove(archive);
  }
  std::map<std::string, CiStagedUpload> staged;
  for (CiStagedUpload& upload : listCiStagedUploads(policy.state_root, project, run_id)) {
    const std::string key = stem(upload.kind, upload.name);
    staged.emplace(key, std::move(upload));
  }
  // Honour a cancellation received after the last step, including remote
  // runners that report success before receiving their next heartbeat reply.
  if (isCiCancelRequested(policy.state_root, project, run_id)) {
    record.status = CiRunStatus::Cancelled;
    record.detail = "cancelled before publishing results";
  }
  const bool success = record.status == CiRunStatus::Success;
  record.artifacts.clear();

  // CI artifacts: each was the output of a job whose steps all passed, so it
  // is kept whatever a later job did.
  for (const CiDeclaredOutput& output : outputs) {
    if ((output.release && !child) || (child && !success) ||
        !isValidCiArtifactName(output.name) || output.name == "release") continue;
    if (child) {
      const auto& artifact = prepared.workflow.jobs.front().artifact;
      if (!artifact || artifact->name != output.name || output.release != prepared.is_release)
        fail("child reported an undeclared output");
    }
    try {
      CiArtifactRecord stored;
      stored.name = output.name;
      stored.created_epoch_seconds = nowEpoch();
      unsigned days = output.retention_days == 0 ? policy.artifact_retention_days : output.retention_days;
      if (policy.artifact_max_retention_days != 0 && days > policy.artifact_max_retention_days) {
        days = policy.artifact_max_retention_days;
      }
      stored.expires_epoch_seconds = output.release ? 0 :
          stored.created_epoch_seconds + static_cast<std::uint64_t>(days) * 86400ull;
      stored.note = output.note;
      if (stored.note.empty()) {
        const std::filesystem::path dir = prepareCiArtifactDirectory(policy.state_root, project, run_id);
        stored.note = takeStaged(staged, incoming, output.release ? CiUploadKind::kAsset : CiUploadKind::kArtifact, output.name,
                                 dir / (output.name + ".tar"), &stored.bytes, &stored.sha256);
      }
      writeCiArtifactRecord(policy.state_root, project, run_id, stored);
      record.artifacts.push_back(stored);
    } catch (const std::exception&) {
      // A failed artifact never fails the run; it simply is not listed.
    }
  }

  // A tag build publishes a release on success, and publishes nothing
  // (removing any earlier release under the tag) when it fails, so a release
  // is always complete.
  if (prepared.is_release && !child) {
    if (success) {
      try {
        if (prepared.workflow.version == 2) {
          removeCiRelease(policy.state_root, project, prepared.release_tag);
          const auto release_dir = validatedMetadataRoot(policy.state_root) / "releases" / project / prepared.release_tag;
          if (std::filesystem::exists(release_dir)) fail("could not replace prior distributed release");
        }
        for (const CiDeclaredOutput& output : outputs) {
          if (!output.release || !isValidCiArtifactName(output.name) || output.name == "release") continue;
          CiArtifactRecord stored;
          stored.name = output.name;
          stored.created_epoch_seconds = nowEpoch();
          stored.expires_epoch_seconds = 0;  // durable
          stored.note = output.note;
          if (stored.note.empty()) {
            const std::filesystem::path dir = prepareCiReleaseDirectory(policy.state_root, project, prepared.release_tag);
            stored.note = takeStaged(staged, incoming, CiUploadKind::kAsset, output.name,
                                     dir / (output.name + ".tar"), &stored.bytes, &stored.sha256);
          }
          if (prepared.workflow.version == 2 && !stored.note.empty()) fail("incomplete distributed release: " + stored.note);
          writeCiReleaseArtifactRecord(policy.state_root, project, prepared.release_tag, stored);
          record.artifacts.push_back(stored);
        }
        CiReleaseRecord release;
        release.tag = prepared.release_tag;
        release.commit_id = prepared.job.commit_id;
        release.created_epoch_seconds = nowEpoch();
        release.notes = readTagNotes(policy.repository, prepared.job.commit_id);
        writeCiReleaseRecord(policy.state_root, project, release);
      } catch (const std::exception&) {
        if (prepared.workflow.version == 2) throw;
      }
    } else {
      try {
        removeCiRelease(policy.state_root, project, prepared.release_tag);
      } catch (const std::exception&) {
      }
    }
  }

  // The Pages site of a successful default-branch build.
  if (success && !child && !prepared.is_release && prepared.is_default_branch && prepared.workflow.pages_path.has_value() &&
      !policy.pages_root.empty()) {
    try {
      std::optional<std::filesystem::path> site = site_dir;
      if (!site.has_value() && staged.count(stem(CiUploadKind::kPages, kCiPagesUploadName)) != 0) {
        // A remote runner's site arrives as one tar: verify and unpack it
        // inside the private staging directory, then publish from there.
        const std::filesystem::path bundle = incoming / "site.tar";
        std::uint64_t bytes = 0;
        std::string sha;
        const std::string note = takeStaged(staged, incoming, CiUploadKind::kPages, kCiPagesUploadName, bundle,
                                            &bytes, &sha);
        if (!note.empty()) throw std::runtime_error(note);
        const std::filesystem::path tree = incoming / "site";
        std::error_code error;
        std::filesystem::remove_all(tree, error);
        std::filesystem::create_directories(tree);
        ProcessOptions options;
        options.timeout = std::chrono::seconds(600);
        options.output_limit = 16 * 1024;
        const ProcessResult unpacked = runProcess({"tar", "-xf", bundle.string(), "-C", tree.string()}, options);
        if (unpacked.timed_out || unpacked.exit_code != 0) throw std::runtime_error("the site archive could not be unpacked");
        site = tree;
      }
      if (site.has_value()) {
        // A newer run's site is already live: a slow or requeued older run
        // must never roll the site back. Run ids sort by queue time.
        const std::optional<std::string> current = currentPagesVersion(policy.pages_root, project);
        if (current.has_value() && *current > run_id) {
          record.detail = firstLine(record.detail + " (a newer Pages site is already live)");
        } else {
          publishPagesSite(policy.pages_root, project, run_id, *site, policy.pages_keep_versions);
        }
      }
    } catch (const std::exception& publish_error) {
      // A build whose steps all passed but whose site failed to publish would
      // otherwise report success while the live site silently stayed on its
      // previous version. Fail the run so the broken publish is never hidden.
      record.status = CiRunStatus::Error;
      record.detail =
          firstLine(std::string("steps passed but the Pages site failed to publish: ") + publish_error.what());
    }
  }

  if (!child || !success || prepared.is_release || !prepared.workflow.pages_path)
    discardCiStagedUploads(policy.state_root, project, run_id);
  writeCiRunRecord(policy.state_root, record);
  // A cancel marker never outlives the run it targeted.
  try {
    clearCiCancel(policy.state_root, project, run_id);
  } catch (const std::exception&) {
  }
  return record;
}

std::optional<CiRunRecord> finalizeCiWorkflowResults(const CiCommitPolicy& policy,
                                                    const CiPreparedJob& parent) {
  if (parent.workflow.version != 2 || !parent.job.parent_run_id.empty())
    fail("only a distributed parent can finalize its graph");
  refreshCiWorkflowGroups(policy.state_root);
  const auto& project = parent.job.project_name;
  const auto& id = parent.job.job_id;
  const auto directory = prepareCiRunDirectory(policy.state_root, project, id);
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) fail("could not open workflow result lock");
  struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
  if (::flock(fd, LOCK_EX) != 0) fail("could not lock workflow publication");
  auto summary = loadCiRun(policy.state_root, project, id);
  if (!summary || summary->commit_id != parent.job.commit_id || summary->ref != parent.job.ref)
    fail("workflow parent identity mismatch");
  if (std::filesystem::exists(directory / "outputs-committed")) return summary;
  const auto children = loadCiWorkflowJobs(policy.state_root, project, id);
  if (children.size() != parent.workflow.jobs.size()) fail("workflow graph does not match its definition");
  std::map<std::string, CiRunRecord> results;
  for (const auto& child : children) {
    const auto result = loadCiRun(policy.state_root, project, child.job_id);
    if (!result || result->commit_id != parent.job.commit_id || result->ref != parent.job.ref ||
        result->parent_run_id != id || result->workflow_job != child.workflow_job)
      fail("workflow child identity mismatch");
    if (ciRunStatusIsActive(result->status)) return std::nullopt;
    if (!results.emplace(child.workflow_job, *result).second) fail("duplicate workflow child");
  }
  if (ciRunStatusIsActive(summary->status)) return std::nullopt;
  const auto inputFailure = [&](const std::string& detail) {
    if (summary->status == CiRunStatus::Success) {
      summary->status = CiRunStatus::Error;
      summary->detail = firstLine(detail);
    }
  };
  std::vector<CiDeclaredOutput> outputs;
  for (const auto& job : parent.workflow.jobs) {
    const auto found = results.find(job.name);
    if (found == results.end()) fail("workflow is missing a declared child");
    const auto& child = found->second;
    if (!job.artifact || child.status != CiRunStatus::Success) continue;
    CiDeclaredOutput output;
    output.name = job.artifact->name;
    output.release = parent.is_release;
    output.retention_days = job.artifact->retention_days;
    const auto artifact = std::find_if(child.artifacts.begin(), child.artifacts.end(),
                                     [&](const auto& item) { return item.name == output.name; });
    if (artifact == child.artifacts.end()) output.note = "child did not produce its declared artifact";
    else if (!artifact->note.empty()) output.note = artifact->note;
    else {
      try {
        if (artifact->bytes > policy.artifact_max_bytes) fail("child artifact exceeds the server cap");
        const auto source = directory.parent_path() / child.run_id / "artifacts" / (output.name + ".tar");
        if (std::filesystem::file_size(source) != artifact->bytes || sha256HexOfFile(source) != artifact->sha256)
          fail("child artifact no longer matches its recorded checksum");
        stageCiFile(policy.state_root, project, id,
                    output.release ? CiUploadKind::kAsset : CiUploadKind::kArtifact, output.name, source);
        const auto staged = listCiStagedUploads(policy.state_root, project, id);
        const auto copy = std::find_if(staged.begin(), staged.end(), [&](const auto& item) { return item.name == output.name; });
        if (copy == staged.end() || copy->sha256 != artifact->sha256 || copy->held != artifact->bytes)
          fail("collected artifact does not match the child result");
      } catch (const std::exception& error) { output.note = firstLine(error.what()); }
    }
    if (parent.is_release && !output.note.empty()) inputFailure("incomplete release: " + output.note);
    outputs.push_back(std::move(output));
  }
  if (!parent.is_release && parent.is_default_branch && parent.workflow.pages_path &&
      summary->status == CiRunStatus::Success) {
    try {
      const auto producer = results.find(parent.workflow.pages_job);
      if (producer == results.end()) fail("missing Pages producer");
      const auto staged = listCiStagedUploads(policy.state_root, project, producer->second.run_id);
      const auto site = std::find_if(staged.begin(), staged.end(), [](const auto& item) { return item.kind == CiUploadKind::kPages; });
      if (site == staged.end() || site->held != site->total) fail("Pages producer did not supply a complete site");
      const auto source = incomingPath(policy.state_root, project, producer->second.run_id) / "pages-site.part";
      if (sha256HexOfFile(source) != site->sha256) fail("Pages output failed its checksum");
      stageCiFile(policy.state_root, project, id, CiUploadKind::kPages, kCiPagesUploadName, source);
    } catch (const std::exception& error) { inputFailure(error.what()); }
  }
  auto stored = commitCiRun(policy, parent, *summary, outputs);
  writeFileAtomic(directory / "outputs-committed", parent.job.commit_id + "\n");
  std::error_code error;
  std::filesystem::remove(directory / "outputs-pending", error);
  for (const auto& child : children) discardCiStagedUploads(policy.state_root, project, child.job_id);
  return stored;
}

std::optional<CiRunRecord> finalizeCiParentForChild(const CiCommitPolicy& policy, const CiJobRequest& child) {
  if (child.parent_run_id.empty()) return std::nullopt;
  auto parent = child;
  parent.job_id = child.parent_run_id;
  parent.parent_run_id.clear();
  parent.workflow_job.clear();
  parent.needs.clear();
  parent.runs_on.clear();
  parent.attempt = 1;
  const auto prepared = prepareCiJob(policy.repository, parent, true);
  if (!prepared.prepared) fail("cannot prepare workflow publication: " + prepared.detail);
  return finalizeCiWorkflowResults(policy, *prepared.prepared);
}

std::vector<std::string> recoverCiWorkflowResults(const CiCommitPolicy& defaults,
                                                const std::filesystem::path& repository_root) {
  std::vector<std::string> errors;
  refreshCiWorkflowGroups(defaults.state_root);
  for (const auto& parent : listCiUnpublishedWorkflows(defaults.state_root)) {
    try {
      auto policy = defaults;
      policy.repository = repository_root / (parent.project_name + ".git");
      const auto prepared = prepareCiJob(policy.repository, parent, true);
      if (!prepared.prepared) fail(prepared.detail);
      finalizeCiWorkflowResults(policy, *prepared.prepared);
    } catch (const std::exception& error) {
      errors.push_back(parent.project_name + "/" + parent.job_id + ": " + firstLine(error.what()));
    }
  }
  return errors;
}

}  // namespace ckgit
