// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_control.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ckgit/control_rpc.hpp"
#include "ckgit/text.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

// Decoded bounds for the hex-encoded text fields, matching what the CI store
// itself accepts for each field.
constexpr std::size_t kMaximumRefBytes = 512;
constexpr std::size_t kMaximumStepNameBytes = 256;
constexpr std::size_t kMaximumNoteBytes = 256;
// Counts no real server approaches, bounding a hostile header.
constexpr std::size_t kMaximumProjects = 65536;
constexpr std::size_t kMaximumArtifacts = 4096;

[[noreturn]] void invalid(std::string_view what, std::string_view message) {
  throw std::invalid_argument(std::string(what) + " response " + std::string(message));
}

// Empty text is sent as "-" so every field stays a non-empty token.
std::string encodeText(std::string_view text) {
  return text.empty() ? std::string("-") : hexEncode(text);
}

std::string decodeText(std::string_view field, std::size_t maximum_bytes, std::string_view what) {
  if (field == "-") return {};
  std::optional<std::string> decoded = hexDecode(field, maximum_bytes);
  if (!decoded.has_value() || decoded->empty()) invalid(what, "has an invalid text field");
  return std::move(*decoded);
}

std::uint64_t parseNumber(std::string_view field, std::string_view what) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
  if (field.empty() || error != std::errc{} || end != field.data() + field.size() ||
      (field.size() > 1 && field.front() == '0')) {
    invalid(what, "has an invalid number");
  }
  return value;
}

bool parseFlag(std::string_view field, std::string_view what) {
  if (field != "0" && field != "1") invalid(what, "has an invalid flag");
  return field == "1";
}

// Walks a response's newline-terminated lines after checking its framing: at
// most kMaximumControlResponseBytes, a final newline, no CR or NUL, printable
// ASCII only, and no empty line.
class ResponseLines {
 public:
  ResponseLines(std::string_view response, std::string_view what) : response_(response), what_(what) {
    if (response.empty() || response.size() > kMaximumControlResponseBytes || response.back() != '\n') {
      invalid(what_, "has invalid framing");
    }
    for (const unsigned char character : response) {
      if (character != '\n' && (character < 0x20 || character > 0x7e)) invalid(what_, "contains unsafe bytes");
    }
  }

  bool done() const { return position_ == response_.size(); }

  std::string_view next() {
    if (done()) invalid(what_, "ends before its declared records");
    const std::size_t newline = response_.find('\n', position_);
    const std::string_view line = response_.substr(position_, newline - position_);
    if (line.empty()) invalid(what_, "has an empty line");
    position_ = newline + 1;
    return line;
  }

  // The line's space-separated fields; exactly `count`, none empty, and the
  // first equal to `keyword`.
  std::vector<std::string_view> fields(std::string_view keyword, std::size_t count) {
    const std::string_view line = next();
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (true) {
      const std::size_t space = line.find(' ', start);
      result.push_back(line.substr(start, space == std::string_view::npos ? std::string_view::npos : space - start));
      if (space == std::string_view::npos) break;
      start = space + 1;
    }
    if (result.size() != count || result.front() != keyword ||
        std::any_of(result.begin(), result.end(), [](std::string_view field) { return field.empty(); })) {
      invalid(what_, "has a malformed " + std::string(keyword) + " record");
    }
    return result;
  }

 private:
  std::string_view response_;
  std::string_view what_;
  std::size_t position_{0};
};

void requireRepresentable(const CiRunRecord& run) {
  if (!isValidCiId(run.run_id) || !isValidObjectId(run.commit_id) || run.ref.size() > kMaximumRefBytes ||
      run.detail.size() > kMaximumCiDetailBytes || run.steps.size() > kMaximumCiSteps) {
    throw std::invalid_argument("a CI run cannot be represented in a control response");
  }
}

std::string runLine(const CiRunRecord& run, std::size_t completed_steps) {
  requireRepresentable(run);
  return "run " + run.run_id + " " + std::string(ciRunStatusName(run.status)) + " " +
         std::to_string(run.started_epoch_seconds) + " " + std::to_string(run.finished_epoch_seconds) + " " +
         std::to_string(run.heartbeat_epoch_seconds) + " " + std::to_string(completed_steps) + " " +
         run.commit_id + " " + encodeText(run.ref) + "\n";
}

// Parses a `run` line into `summary`, validating every field.
CiRunSummary parseRunLine(ResponseLines& lines, std::string_view project_name, std::string_view what) {
  const auto fields = lines.fields("run", 9);
  CiRunSummary summary;
  CiRunRecord& run = summary.run;
  run.run_id = std::string(fields[1]);
  run.project_name = std::string(project_name);
  const auto status = ciRunStatusFromName(fields[2]);
  if (!isValidCiId(run.run_id) || !status.has_value()) invalid(what, "has an invalid run id or status");
  run.status = *status;
  run.started_epoch_seconds = parseNumber(fields[3], what);
  run.finished_epoch_seconds = parseNumber(fields[4], what);
  run.heartbeat_epoch_seconds = parseNumber(fields[5], what);
  const std::uint64_t completed = parseNumber(fields[6], what);
  if (completed > kMaximumCiSteps) invalid(what, "has an invalid step count");
  summary.completed_steps = static_cast<std::size_t>(completed);
  if (!isValidObjectId(fields[7])) invalid(what, "has an invalid commit id");
  run.commit_id = std::string(fields[7]);
  run.ref = decodeText(fields[8], kMaximumRefBytes, what);
  return summary;
}

// Appends `line` unless the response would exceed its bound.
void append(std::string& response, const std::string& line, std::string_view what) {
  if (response.size() + line.size() > kMaximumControlResponseBytes) {
    throw std::length_error(std::string(what) + " exceeds the control response limit");
  }
  response += line;
}

}  // namespace

CiRunSummary summarizeCiRun(const CiRunRecord& run) {
  CiRunSummary summary;
  summary.run.run_id = run.run_id;
  summary.run.project_name = run.project_name;
  summary.run.ref = run.ref;
  summary.run.commit_id = run.commit_id;
  summary.run.status = run.status;
  summary.run.started_epoch_seconds = run.started_epoch_seconds;
  summary.run.finished_epoch_seconds = run.finished_epoch_seconds;
  summary.run.heartbeat_epoch_seconds = run.heartbeat_epoch_seconds;
  summary.completed_steps = run.steps.size();
  return summary;
}

std::string formatCiStatusControlResponse(const CiStatusReport& report) {
  constexpr std::string_view kWhat = "CI status";
  std::string response = "ok " + std::to_string(report.server_epoch_seconds) + " " +
                         std::to_string(report.projects.size()) + "\n";
  for (const auto& project : report.projects) {
    if (!isValidProjectName(project.project_name) || project.runs.size() > kMaximumControlCiRuns) {
      throw std::invalid_argument("a CI project status cannot be represented in a control response");
    }
    append(response,
           "project " + project.project_name + " " + (project.ci_enabled ? "enabled" : "disabled") + " " +
               std::to_string(project.runs.size()) + "\n",
           kWhat);
    for (const auto& summary : project.runs) append(response, runLine(summary.run, summary.completed_steps), kWhat);
  }
  return response;
}

CiStatusReport parseCiStatusControlResponse(std::string_view response) {
  constexpr std::string_view kWhat = "CI status";
  ResponseLines lines(response, kWhat);
  const auto header = lines.fields("ok", 3);
  CiStatusReport report;
  report.server_epoch_seconds = parseNumber(header[1], kWhat);
  const std::uint64_t project_count = parseNumber(header[2], kWhat);
  if (project_count > kMaximumProjects) invalid(kWhat, "has an invalid project count");
  for (std::uint64_t project_index = 0; project_index < project_count; ++project_index) {
    const auto fields = lines.fields("project", 4);
    CiProjectStatus project;
    project.project_name = std::string(fields[1]);
    if (!isValidProjectName(project.project_name) || (fields[2] != "enabled" && fields[2] != "disabled")) {
      invalid(kWhat, "has an invalid project record");
    }
    if (std::any_of(report.projects.begin(), report.projects.end(),
                    [&](const CiProjectStatus& seen) { return seen.project_name == project.project_name; })) {
      invalid(kWhat, "repeats a project");
    }
    project.ci_enabled = fields[2] == "enabled";
    const std::uint64_t run_count = parseNumber(fields[3], kWhat);
    if (run_count > kMaximumControlCiRuns) invalid(kWhat, "has an invalid run count");
    for (std::uint64_t run_index = 0; run_index < run_count; ++run_index) {
      CiRunSummary summary = parseRunLine(lines, project.project_name, kWhat);
      // Run ids sort chronologically, and the listing is newest first.
      if (!project.runs.empty() && !(summary.run.run_id < project.runs.back().run.run_id)) {
        invalid(kWhat, "lists runs out of order or twice");
      }
      project.runs.push_back(std::move(summary));
    }
    report.projects.push_back(std::move(project));
  }
  if (!lines.done()) invalid(kWhat, "has records beyond its declared counts");
  return report;
}

std::string formatCiRunControlResponse(const CiRunReport& report) {
  constexpr std::string_view kWhat = "CI run";
  const CiRunRecord& run = report.run;
  std::string response = "ok " + std::to_string(report.server_epoch_seconds) + " " +
                         std::to_string(run.artifacts.size()) + "\n";
  append(response, runLine(run, run.steps.size()), kWhat);
  append(response, "detail " + encodeText(run.detail) + "\n", kWhat);
  for (std::size_t index = 0; index < run.steps.size(); ++index) {
    const CiStepResult& step = run.steps[index];
    if (step.name.size() > kMaximumStepNameBytes) {
      throw std::invalid_argument("a CI step name cannot be represented in a control response");
    }
    append(response,
           "step " + std::to_string(index) + " " + std::to_string(step.exit_code) + " " +
               (step.timed_out ? "1" : "0") + " " + (step.output_truncated ? "1" : "0") + " " +
               encodeText(step.name) + "\n",
           kWhat);
  }
  for (const CiArtifactRecord& artifact : run.artifacts) {
    if (!isValidCiArtifactName(artifact.name) || (!artifact.sha256.empty() && !isValidSha256Hex(artifact.sha256)) ||
        artifact.note.size() > kMaximumNoteBytes) {
      throw std::invalid_argument("a CI artifact cannot be represented in a control response");
    }
    append(response,
           "artifact " + artifact.name + " " + std::to_string(artifact.bytes) + " " +
               (artifact.sha256.empty() ? std::string("-") : artifact.sha256) + " " +
               std::to_string(artifact.created_epoch_seconds) + " " +
               std::to_string(artifact.expires_epoch_seconds) + " " + encodeText(artifact.note) + "\n",
           kWhat);
  }
  return response;
}

CiRunReport parseCiRunControlResponse(std::string_view response, std::string_view project_name) {
  constexpr std::string_view kWhat = "CI run";
  ResponseLines lines(response, kWhat);
  const auto header = lines.fields("ok", 3);
  CiRunReport report;
  report.server_epoch_seconds = parseNumber(header[1], kWhat);
  const std::uint64_t artifact_count = parseNumber(header[2], kWhat);
  if (artifact_count > kMaximumArtifacts) invalid(kWhat, "has an invalid artifact count");
  CiRunSummary summary = parseRunLine(lines, project_name, kWhat);
  report.run = std::move(summary.run);
  report.run.detail = decodeText(lines.fields("detail", 2)[1], kMaximumCiDetailBytes, kWhat);
  for (std::size_t index = 0; index < summary.completed_steps; ++index) {
    const auto fields = lines.fields("step", 6);
    if (parseNumber(fields[1], kWhat) != index) invalid(kWhat, "lists steps out of order");
    CiStepResult step;
    const auto [end, error] = std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), step.exit_code);
    if (error != std::errc{} || end != fields[2].data() + fields[2].size()) {
      invalid(kWhat, "has an invalid exit code");
    }
    step.timed_out = parseFlag(fields[3], kWhat);
    step.output_truncated = parseFlag(fields[4], kWhat);
    step.name = decodeText(fields[5], kMaximumStepNameBytes, kWhat);
    report.run.steps.push_back(std::move(step));
  }
  for (std::uint64_t index = 0; index < artifact_count; ++index) {
    const auto fields = lines.fields("artifact", 7);
    CiArtifactRecord artifact;
    artifact.name = std::string(fields[1]);
    if (!isValidCiArtifactName(artifact.name) ||
        std::any_of(report.run.artifacts.begin(), report.run.artifacts.end(),
                    [&](const CiArtifactRecord& seen) { return seen.name == artifact.name; })) {
      invalid(kWhat, "has an invalid or repeated artifact name");
    }
    artifact.bytes = parseNumber(fields[2], kWhat);
    if (fields[3] != "-") {
      if (!isValidSha256Hex(fields[3])) invalid(kWhat, "has an invalid artifact checksum");
      artifact.sha256 = std::string(fields[3]);
    }
    artifact.created_epoch_seconds = parseNumber(fields[4], kWhat);
    artifact.expires_epoch_seconds = parseNumber(fields[5], kWhat);
    artifact.note = decodeText(fields[6], kMaximumNoteBytes, kWhat);
    report.run.artifacts.push_back(std::move(artifact));
  }
  if (!lines.done()) invalid(kWhat, "has records beyond its declared counts");
  return report;
}

std::string formatCiLogControlResponse(const CiLogChunk& chunk) {
  if (chunk.bytes.size() > kMaximumControlLogChunkBytes) {
    throw std::length_error("a CI log chunk exceeds the control response limit");
  }
  std::string response = "ok " + std::string(chunk.complete ? "end" : "more") + " " +
                         std::to_string(chunk.offset) + " " + std::to_string(chunk.bytes.size()) + "\n";
  if (!chunk.bytes.empty()) response += hexEncode(chunk.bytes) + "\n";
  return response;
}

CiLogChunk parseCiLogControlResponse(std::string_view response, std::uint64_t requested_offset) {
  constexpr std::string_view kWhat = "CI log";
  ResponseLines lines(response, kWhat);
  const auto header = lines.fields("ok", 4);
  if (header[1] != "more" && header[1] != "end") invalid(kWhat, "has an invalid state");
  CiLogChunk chunk;
  chunk.complete = header[1] == "end";
  chunk.offset = parseNumber(header[2], kWhat);
  if (chunk.offset != requested_offset) invalid(kWhat, "answers a different offset");
  const std::uint64_t size = parseNumber(header[3], kWhat);
  if (size > kMaximumControlLogChunkBytes) invalid(kWhat, "has an invalid chunk size");
  if (size != 0) {
    std::optional<std::string> bytes = hexDecode(lines.next(), static_cast<std::size_t>(size));
    if (!bytes.has_value() || bytes->size() != size) invalid(kWhat, "has a malformed chunk");
    chunk.bytes = std::move(*bytes);
  }
  if (!lines.done()) invalid(kWhat, "has records beyond its chunk");
  return chunk;
}

}  // namespace ckgit
