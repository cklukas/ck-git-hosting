// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "ckgit/text.hpp"

namespace ckgit {

inline constexpr std::size_t kMaximumRouteBytes = 8192;
inline constexpr std::size_t kMaximumRouteComponentBytes = 255;
inline constexpr std::size_t kMaximumRoutePathDepth = 32;

enum class RouteKind {
  kNotFound, kTable, kOverview, kCommits, kCommit, kTree, kBlob, kSource, kRaw,
  kCalendar, kDay, kGraph, kCiRuns, kCiLog, kCiArtifact
};

struct Route {
  RouteKind kind = RouteKind::kNotFound;
  std::string project;
  std::string ref;
  std::string path;
  std::string cursor;
  int year = 0;
  int month = 0;
  int day = 0;
  bool sort_by_name = false;
  // CI log route: which run and which step's log to serve.
  std::string run_id;
  int step = -1;
};

bool isObjectId(std::string_view value);
// Encode a ref or repository path; slash separators are deliberately retained.
std::string encodePathSegment(std::string_view value);
// Decode only the canonical uppercase escape spelling; encoded slashes,
// controls, invalid UTF-8 and backslashes are rejected.
std::optional<std::string> decodePathSegment(std::string_view value);
Route parseHttpRoute(std::string_view target);

}  // namespace ckgit
