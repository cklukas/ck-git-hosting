// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_runner_protocol.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>

#include "ckgit/ci_results.hpp"
#include "ckgit/text.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

void reject(std::string_view message, std::string* reason) {
  if (reason != nullptr) *reason = std::string(message);
}

bool isToken(std::string_view token) {
  return !token.empty() && token.size() <= 256 && std::all_of(token.begin(), token.end(), [](unsigned char c) {
           return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '_' || c == '-';
         });
}

// Canonical decimal: digits only, no leading zero unless the value is 0.
std::optional<std::uint64_t> canonicalNumber(std::string_view token) {
  if (token.empty() || token.size() > 20 || (token.size() > 1 && token.front() == '0')) return std::nullopt;
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
  if (error != std::errc{} || end != token.data() + token.size()) return std::nullopt;
  return value;
}

bool isLeaseToken(std::string_view token) {
  return token.size() == 32 && std::all_of(token.begin(), token.end(), [](unsigned char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::vector<std::string_view> splitLines(std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string_view::npos) throw std::invalid_argument("an unterminated line");
    lines.push_back(text.substr(start, newline - start));
    start = newline + 1;
  }
  return lines;
}

std::string_view field(std::string_view line, std::string_view key) {
  if (line.rfind(key, 0) != 0) throw std::invalid_argument("expected field " + std::string(key));
  return line.substr(key.size());
}

std::uint64_t number(std::string_view text, const char* what) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
    throw std::invalid_argument(std::string("malformed ") + what);
  }
  return value;
}

std::string decodeHex(std::string_view text, std::size_t cap, const char* what) {
  std::optional<std::string> decoded = hexDecode(text, cap);
  if (!decoded.has_value()) throw std::invalid_argument(std::string("malformed ") + what);
  return *decoded;
}

std::vector<std::string_view> splitFields(std::string_view text, char separator) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  for (;;) {
    const std::size_t at = text.find(separator, start);
    fields.push_back(text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
    if (at == std::string_view::npos) break;
    start = at + 1;
  }
  return fields;
}

}  // namespace

std::string hexOrDash(std::string_view text) { return text.empty() ? std::string("-") : hexEncode(text); }

std::optional<std::string> fromHexOrDash(std::string_view token, std::size_t cap) {
  if (token == "-") return std::string();
  return hexDecode(token, cap);
}

std::optional<CiRunnerCommand> parseCiRunnerCommand(std::string_view command, std::string* reason) {
  if (reason != nullptr) reason->clear();
  if (command.empty() || command.size() > 1024) {
    reject("command is empty or exceeds 1024 bytes", reason);
    return std::nullopt;
  }
  std::vector<std::string_view> tokens;
  std::size_t start = 0;
  for (;;) {
    const std::size_t space = command.find(' ', start);
    const std::string_view token =
        command.substr(start, space == std::string_view::npos ? std::string_view::npos : space - start);
    if (!isToken(token)) {
      reject("command contains an empty or unsupported token", reason);
      return std::nullopt;
    }
    tokens.push_back(token);
    if (tokens.size() > 10) {
      reject("command has too many arguments", reason);
      return std::nullopt;
    }
    if (space == std::string_view::npos) break;
    start = space + 1;
  }
  if (tokens.size() < 3 || tokens[0] != "ckgit-runner") {
    reject("command is not a ckgit-runner operation", reason);
    return std::nullopt;
  }
  if (tokens[1] != "1" && tokens[1] != "2") {
    reject("unsupported ckgit-runner protocol version", reason);
    return std::nullopt;
  }
  const std::string_view op = tokens[2];
  const std::size_t arguments = tokens.size() - 3;
  CiRunnerCommand parsed;
  parsed.protocol_version = tokens[1] == "2" ? 2 : 1;
  const auto jobAndToken = [&](std::size_t first) {
    if (!isValidCiId(tokens[first]) || !isLeaseToken(tokens[first + 1])) return false;
    parsed.job_id = std::string(tokens[first]);
    parsed.token = std::string(tokens[first + 1]);
    return true;
  };
  bool ok = false;
  if (op == "hello" && arguments == 1) {
    parsed.op = CiRunnerOp::kHello;
    ok = hexDecode(tokens[3], 128).has_value();
    parsed.version_hex = std::string(tokens[3]);
  } else if (op == "claim" && arguments == 1) {
    parsed.op = CiRunnerOp::kClaim;
    const auto wait = canonicalNumber(tokens[3]);
    ok = wait.has_value() && *wait <= kMaximumCiClaimWaitSeconds;
    if (ok) parsed.wait_seconds = static_cast<unsigned>(*wait);
  } else if (op == "upload-pack" && arguments == 3) {
    parsed.op = CiRunnerOp::kUploadPack;
    ok = jobAndToken(3) && isValidProjectName(tokens[5]);
    parsed.project = std::string(tokens[5]);
  } else if ((op == "stream" || op == "heartbeat" || op == "finish" || op == "abandon") && arguments == 2) {
    parsed.op = op == "stream" ? CiRunnerOp::kStream
              : op == "heartbeat" ? CiRunnerOp::kHeartbeat
              : op == "finish" ? CiRunnerOp::kFinish
                               : CiRunnerOp::kAbandon;
    ok = jobAndToken(3);
  } else if (op == "put" && arguments == 7) {
    parsed.op = CiRunnerOp::kPut;
    const auto offset = canonicalNumber(tokens[7]);
    const auto total = canonicalNumber(tokens[8]);
    const auto kind = ciUploadKindFromName(tokens[5]);
    ok = jobAndToken(3) && kind.has_value() && isValidCiArtifactName(tokens[6]) && offset.has_value() &&
         total.has_value() && *offset <= *total && isValidSha256Hex(tokens[9]);
    if (ok) {
      parsed.kind = std::string(tokens[5]);
      parsed.name = std::string(tokens[6]);
      parsed.offset = *offset;
      parsed.total = *total;
      parsed.sha256 = std::string(tokens[9]);
    }
  }
  if (!ok) {
    reject("command is not an allowed ckgit-runner operation", reason);
    return std::nullopt;
  }
  return parsed;
}

std::string formatCiRunnerCommand(const CiRunnerCommand& command) {
  if (command.protocol_version != 1 && command.protocol_version != 2)
    throw std::invalid_argument("unsupported runner protocol version");
  std::string line = "ckgit-runner " + std::to_string(command.protocol_version) + " ";
  switch (command.op) {
    case CiRunnerOp::kHello: return line + "hello " + command.version_hex;
    case CiRunnerOp::kClaim: return line + "claim " + std::to_string(command.wait_seconds);
    case CiRunnerOp::kUploadPack:
      return line + "upload-pack " + command.job_id + " " + command.token + " " + command.project;
    case CiRunnerOp::kStream: return line + "stream " + command.job_id + " " + command.token;
    case CiRunnerOp::kHeartbeat: return line + "heartbeat " + command.job_id + " " + command.token;
    case CiRunnerOp::kFinish: return line + "finish " + command.job_id + " " + command.token;
    case CiRunnerOp::kAbandon: return line + "abandon " + command.job_id + " " + command.token;
    case CiRunnerOp::kPut:
      return line + "put " + command.job_id + " " + command.token + " " + command.kind + " " + command.name + " " +
             std::to_string(command.offset) + " " + std::to_string(command.total) + " " + command.sha256;
  }
  return line;
}

std::string formatCiClaimResponse(const CiPreparedJob& prepared, const CiLease& lease, const CiRemotePolicy& policy,
                                  unsigned protocol_version) {
  if (protocol_version != 1 && protocol_version != 2)
    throw std::invalid_argument("unsupported runner claim protocol");
  const CiJobRequest& job = prepared.job;
  if (protocol_version == 1 && !job.parent_run_id.empty())
    throw std::invalid_argument("protocol 1 cannot carry a child assignment");
  std::string out = protocol_version == 2 ? "job2\n" : "job\n";
  out += "job_id=" + job.job_id + "\n";
  out += "project=" + job.project_name + "\n";
  out += "ref_hex=" + hexEncode(job.ref) + "\n";
  out += "commit=" + job.commit_id + "\n";
  out += "attempt=" + std::to_string(job.attempt) + "\n";
  out += "token=" + lease.token + "\n";
  out += "lease_seconds=" + std::to_string(policy.lease_seconds) + "\n";
  out += "release_tag=" + (prepared.is_release ? prepared.release_tag : std::string()) + "\n";
  out += std::string("default_branch=") + (prepared.is_default_branch ? "1" : "0") + "\n";
  out += "timeout_seconds=" + std::to_string(policy.timeout_seconds) + "\n";
  out += "max_log_bytes=" + std::to_string(policy.max_log_bytes) + "\n";
  out += "artifact_max_bytes=" + std::to_string(policy.artifact_max_bytes) + "\n";
  out += std::string("allow_network=") + (policy.allow_network ? "1" : "0") + "\n";
  out += "sister_count=" + std::to_string(prepared.sisters.size()) + "\n";
  for (const CiResolvedSister& sister : prepared.sisters) out += "sister=" + sister.name + " " + sister.commit + "\n";
  out += "workflow_hex=" + hexEncode(prepared.workflow_text) + "\n";
  if (protocol_version == 2) {
    out += "parent_run_id=" + job.parent_run_id + "\n";
    out += "workflow_job=" + job.workflow_job + "\n";
  }
  return out;
}

std::optional<CiRemoteJob> parseCiClaimResponse(std::string_view response) {
  if (response == "none\n") return std::nullopt;
  if (response.size() > kMaximumCiClaimResponseBytes) throw std::invalid_argument("claim response too large");
  const std::vector<std::string_view> lines = splitLines(response);
  const bool v2 = !lines.empty() && lines[0] == "job2";
  if (lines.size() < (v2 ? 18u : 16u) || (!v2 && lines[0] != "job")) throw std::invalid_argument("malformed claim response");
  CiRemoteJob remote;
  CiPreparedJob& prepared = remote.prepared;
  CiJobRequest& job = prepared.job;
  job.job_id = std::string(field(lines[1], "job_id="));
  job.project_name = std::string(field(lines[2], "project="));
  job.ref = decodeHex(field(lines[3], "ref_hex="), 512, "ref");
  job.commit_id = std::string(field(lines[4], "commit="));
  job.attempt = static_cast<unsigned>(number(field(lines[5], "attempt="), "attempt"));
  job.client_id = "remote";
  remote.token = std::string(field(lines[6], "token="));
  remote.policy.lease_seconds = static_cast<unsigned>(number(field(lines[7], "lease_seconds="), "lease"));
  const std::string_view tag = field(lines[8], "release_tag=");
  prepared.is_release = !tag.empty();
  prepared.release_tag = std::string(tag);
  prepared.is_default_branch = field(lines[9], "default_branch=") == "1";
  remote.policy.timeout_seconds = static_cast<unsigned>(number(field(lines[10], "timeout_seconds="), "timeout"));
  remote.policy.max_log_bytes = number(field(lines[11], "max_log_bytes="), "log cap");
  remote.policy.artifact_max_bytes = number(field(lines[12], "artifact_max_bytes="), "artifact cap");
  remote.policy.allow_network = field(lines[13], "allow_network=") == "1";
  const std::uint64_t sisters = number(field(lines[14], "sister_count="), "sister count");
  if (sisters > kMaximumCiSisters || lines.size() != (v2 ? 18u : 16u) + sisters) throw std::invalid_argument("malformed sister list");
  std::vector<CiResolvedSister> resolved;
  for (std::uint64_t index = 0; index < sisters; ++index) {
    const std::string_view value = field(lines[15 + index], "sister=");
    const std::size_t space = value.find(' ');
    if (space == std::string_view::npos) throw std::invalid_argument("malformed sister");
    CiResolvedSister sister;
    sister.name = std::string(value.substr(0, space));
    sister.commit = std::string(value.substr(space + 1));
    if (!isValidProjectName(sister.name) || !isValidObjectId(sister.commit)) throw std::invalid_argument("invalid sister");
    resolved.push_back(std::move(sister));
  }
  prepared.workflow_text =
      decodeHex(field(lines[15 + sisters], "workflow_hex="), kMaximumCiWorkflowBytes, "workflow");
  if (!isValidCiId(job.job_id) || !isValidProjectName(job.project_name) || !isValidObjectId(job.commit_id) ||
      !isLeaseToken(remote.token) || (prepared.is_release && !isValidReleaseTag(prepared.release_tag))) {
    throw std::invalid_argument("claim response contains invalid data");
  }
  prepared.workflow = parseCiWorkflow(prepared.workflow_text);
  if (v2) {
    job.parent_run_id = std::string(field(lines[16 + sisters], "parent_run_id="));
    job.workflow_job = std::string(field(lines[17 + sisters], "workflow_job="));
  }
  if (prepared.workflow.version == 2) {
    if (!v2 || !isValidCiId(job.parent_run_id) || job.parent_run_id == job.job_id || job.workflow_job.empty())
      throw std::invalid_argument("distributed workflow requires a protocol-2 child assignment");
    prepared.workflow = selectCiWorkflowJob(std::move(prepared.workflow), job.workflow_job);
    job.runs_on = prepared.workflow.runs_on;
  } else if (!job.parent_run_id.empty() || !job.workflow_job.empty()) {
    throw std::invalid_argument("legacy workflow cannot carry a child assignment");
  }
  // Each sister's history settings come from the workflow itself.
  for (CiResolvedSister& sister : resolved) {
    for (const CiSister& declared : prepared.workflow.sisters) {
      if (declared.name == sister.name) sister.checkout = declared.checkout;
    }
  }
  prepared.sisters = std::move(resolved);
  return remote;
}

std::string formatCiFinishReport(const CiFinishReport& report) {
  std::string out = "status=" + std::string(ciRunStatusName(report.status)) + "\n";
  out += "started_epoch=" + std::to_string(report.started_epoch_seconds) + "\n";
  out += "detail_hex=" + hexEncode(report.detail) + "\n";
  if (report.execution) out += "execution=" + formatCiExecutionReport(*report.execution) + "\n";
  for (const CiStepResult& step : report.steps) {
    out += "step=" + std::to_string(step.exit_code) + "," + (step.timed_out ? "1" : "0") + "," +
           (step.output_truncated ? "1" : "0") + "," + hexOrDash(step.name) + "\n";
  }
  for (const CiDeclaredOutput& output : report.outputs) {
    out += "output=" + output.name + "," + (output.release ? "1" : "0") + "," + std::to_string(output.retention_days) +
           "," + hexOrDash(output.note) + "\n";
  }
  return out;
}

CiFinishReport parseCiFinishReport(std::string_view payload) {
  if (payload.size() > kMaximumCiFinishPayloadBytes) throw std::invalid_argument("finish payload too large");
  const std::vector<std::string_view> lines = splitLines(payload);
  if (lines.size() < 3) throw std::invalid_argument("malformed finish payload");
  CiFinishReport report;
  const std::optional<CiRunStatus> status = ciRunStatusFromName(field(lines[0], "status="));
  if (!status.has_value() || ciRunStatusIsActive(*status)) throw std::invalid_argument("invalid final status");
  report.status = *status;
  report.started_epoch_seconds = number(field(lines[1], "started_epoch="), "start time");
  report.detail = decodeHex(field(lines[2], "detail_hex="), kMaximumCiDetailBytes, "detail");
  for (std::size_t index = 3; index < lines.size(); ++index) {
    const std::string_view line = lines[index];
    if (line.rfind("execution=", 0) == 0) {
      if (report.execution) throw std::invalid_argument("duplicate execution report");
      report.execution = parseCiExecutionReport(line.substr(10));
    } else if (line.rfind("step=", 0) == 0) {
      const auto parts = splitFields(line.substr(5), ',');
      if (parts.size() != 4 || report.steps.size() >= kMaximumCiSteps) throw std::invalid_argument("malformed step");
      CiStepResult step;
      const std::string_view exit_text = parts[0];
      const bool negative = !exit_text.empty() && exit_text.front() == '-';
      const std::uint64_t magnitude = number(negative ? exit_text.substr(1) : exit_text, "exit code");
      if (magnitude > 1000) throw std::invalid_argument("malformed exit code");
      step.exit_code = negative ? -static_cast<int>(magnitude) : static_cast<int>(magnitude);
      step.timed_out = parts[1] == "1";
      step.output_truncated = parts[2] == "1";
      const std::optional<std::string> name = fromHexOrDash(parts[3], 256);
      if (!name.has_value()) throw std::invalid_argument("malformed step name");
      step.name = *name;
      report.steps.push_back(std::move(step));
    } else if (line.rfind("output=", 0) == 0) {
      const auto parts = splitFields(line.substr(7), ',');
      if (parts.size() != 4 || report.outputs.size() >= kMaximumCiJobs) throw std::invalid_argument("malformed output");
      CiDeclaredOutput output;
      output.name = std::string(parts[0]);
      output.release = parts[1] == "1";
      output.retention_days = static_cast<unsigned>(number(parts[2], "retention"));
      const std::optional<std::string> note = fromHexOrDash(parts[3], 256);
      if (!isValidCiArtifactName(output.name) || !note.has_value() || output.retention_days > kMaximumCiRetentionDaysCap) {
        throw std::invalid_argument("invalid output");
      }
      output.note = *note;
      report.outputs.push_back(std::move(output));
    } else {
      throw std::invalid_argument("unknown finish field");
    }
  }
  return report;
}

}  // namespace ckgit
