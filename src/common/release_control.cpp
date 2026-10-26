// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_store.hpp"
#include "ckgit/control_rpc.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/validation.hpp"
#include <algorithm>
#include <charconv>
#include <stdexcept>
namespace ckgit {
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

}
