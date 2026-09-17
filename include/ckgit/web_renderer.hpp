// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ckgit/project_summary.hpp"

namespace ckgit {

struct PageContext {
  // Named identity is kept independently from the immutable commit used to
  // read this page. A commit ID must never be relabelled as an arbitrary ref.
  std::string ref;
  std::string commit_id;
  std::string section;
  std::string path;
  int year{0};
  int month{0};
};

std::string htmlEscape(std::string_view value);
std::string renderProjectTable(const std::vector<ProjectSummary>& projects, bool sort_by_name = false);
std::string renderProjectDetail(const ProjectSummary& project, const PageContext* context = nullptr);
// The read-only CI page body: the project's recent runs with per-step status
// and links to each step's captured log. Wrapped by pageLayout by the caller.
std::string renderCiRuns(const ProjectSummary& project);
// The live status page for one run: overall status with elapsed/total time,
// per-step results, the running step's log tail, artifacts, and the cancel
// affordances. `live_log` is the current step's captured output while the run
// is active (std::nullopt otherwise); `live_step` is that step's index.
std::string renderCiRunDetail(const ProjectSummary& project, const CiRunRecord& run,
                              const std::optional<std::string>& live_log, std::size_t live_step);
// The step-log page body: the captured output, plus a "Follow live" control that
// tails the running step over Server-Sent Events. Progressive enhancement — the
// static log is the whole page without JavaScript. `nonce` authorizes the small
// inline follow script under the page's scoped CSP; the SSE endpoint is the
// step's `.stream` sibling, carried in a data attribute so the script stays
// static (no interpolation).
std::string renderCiLogView(const ProjectSummary& project, const std::string& run_id, int step,
                            const std::string& log, const std::string& nonce);
std::string renderReleases(const ProjectSummary& project);

// Presentation-ready view of a run at render time, shared by the index table,
// the CI list, and the run page so they agree. A Running record whose heartbeat
// has aged past kCiRunStaleSeconds is reported as interrupted (not live), and
// `seconds` is the elapsed time while active or the total once finished.
struct CiRunDisplay {
  std::string_view icon;
  std::string_view name;   // "running", "success", "interrupted", …
  std::uint64_t seconds;   // elapsed (active) or total (finished); 0 = unknown
  bool active;             // still live: a running run with a fresh heartbeat
};
CiRunDisplay ciRunDisplay(const CiRunRecord& run);
// True when any run is still live (drives whether a page auto-refreshes).
bool ciAnyActiveRun(const std::vector<CiRunRecord>& runs);
// A run's elapsed/total time rendered as "M:SS min" (or "H:MM:SS" past an
// hour); empty when the time is unknown.
std::string ciRunTiming(const CiRunDisplay& display);

}  // namespace ckgit

namespace ckgit {
std::string pageLayout(std::string_view title, std::string_view body, const ProjectSummary* project = nullptr,
                       const PageContext* context = nullptr, unsigned refresh_seconds = 0);
std::string formatUtcTimestamp(std::uint64_t epoch);
std::string relativeTime(std::uint64_t epoch);
std::string formatBytes(std::uint64_t bytes);
std::string escapePre(std::string_view value);
std::string sourceUrl(const std::string& project, const std::string& kind,
                      const std::string& ref, const std::string& path = {});
std::string refPicker(const ProjectSummary& project, const std::string& selected,
                      const std::string& kind, const std::string& path = {}, int year = 0, int month = 0);
std::string refLabel(const std::string& ref);
std::string renderReadme(const ProjectSummary& project, const std::string& id,
                         const std::string& path, const std::string& content, const std::string& ref = {});
}
