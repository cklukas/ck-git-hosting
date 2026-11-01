// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_store.hpp"
#include "ckgit/ci_results.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
namespace ckgit {
CiExecutionReport parseCiExecutionReport(std::string_view text) {
  if (text.size() > 256) throw std::invalid_argument("execution report too large");
  std::array<std::string_view, 8> fields{};
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const auto comma = text.find(',');
    if ((i + 1 < fields.size()) != (comma != std::string_view::npos))
      throw std::invalid_argument("malformed execution report");
    fields[i] = text.substr(0, comma);
    if (comma != std::string_view::npos) text.remove_prefix(comma + 1);
  }
  if (fields[0] != "1" ||
      (fields[1] != "windows" && fields[1] != "linux" && fields[1] != "macos") ||
      (fields[2] != "x64" && fields[2] != "arm64" && fields[2] != "x86") ||
      (fields[3] != "lpac" && fields[3] != "linux-namespaces" && fields[3] != "seatbelt" && fields[3] != "none") ||
      (fields[4] != "denied" && fields[4] != "loopback-only" && fields[4] != "unrestricted"))
    throw std::invalid_argument("unknown execution report value");
  if ((fields[3] == "lpac" && fields[1] != "windows") ||
      (fields[3] == "linux-namespaces" && fields[1] != "linux") ||
      (fields[3] == "seatbelt" && fields[1] != "macos"))
    throw std::invalid_argument("execution isolation does not match platform");
  const auto number = [](std::string_view value, std::uint64_t maximum) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() || result > maximum)
      throw std::invalid_argument("invalid execution resource limit");
    return result;
  };
  return {std::string(fields[1]), std::string(fields[2]), std::string(fields[3]), std::string(fields[4]),
      number(fields[5], std::numeric_limits<std::uint64_t>::max()),
      static_cast<unsigned>(number(fields[6], std::numeric_limits<unsigned>::max())),
      static_cast<unsigned>(number(fields[7], std::numeric_limits<unsigned>::max()))};
}

std::string formatCiExecutionReport(const CiExecutionReport& report) {
  const auto result = "1," + report.platform + "," + report.architecture + "," + report.isolation + "," +
      report.network + "," + std::to_string(report.memory_bytes) + "," + std::to_string(report.process_limit) +
      "," + std::to_string(report.timeout_seconds);
  static_cast<void>(parseCiExecutionReport(result));
  return result;
}

std::string_view ciUploadKindName(CiUploadKind kind) {
  switch (kind) {
    case CiUploadKind::kArtifact: return "artifact";
    case CiUploadKind::kAsset: return "asset";
    case CiUploadKind::kPages: return "pages";
  }
  return "artifact";
}

std::optional<CiUploadKind> ciUploadKindFromName(std::string_view name) {
  if (name == "artifact") return CiUploadKind::kArtifact;
  if (name == "asset") return CiUploadKind::kAsset;
  if (name == "pages") return CiUploadKind::kPages;
  return std::nullopt;
}


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

bool isValidReleaseTag(std::string_view tag) {
  return !tag.empty() && tag.size() <= kMaximumReleaseTagBytes && tag != "." && tag != ".." &&
         std::all_of(tag.begin(), tag.end(), [](unsigned char character) {
           return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                  character == '-';
         });
}

}
