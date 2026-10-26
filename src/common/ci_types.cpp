// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_store.hpp"
#include <algorithm>
namespace ckgit {
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
