// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <string>
#include "ckgit/http_router.hpp"
#include "ckgit/project_summary.hpp"
#include "ckgit/web_repository.hpp"
namespace ckgit {
struct DashboardResponse {
  int status{200};
  std::string body;
  std::string content_type{"text/html; charset=utf-8"};
  bool raw{false};
  std::string filename;
  std::string location;
  // Optional Content-Security-Policy override. Empty keeps the default strict
  // policy; the log-follow page sets a scoped policy that additionally permits
  // its nonce'd inline script and a same-origin EventSource connection.
  std::string csp;
};
DashboardResponse renderDashboard(const Route& route, const ProjectSummary& project,
                                   const std::filesystem::path& repository,
                                   std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25));
DashboardResponse renderDashboardError(const Route& route, const ProjectSummary& project,
                                        int status, const std::string& message);
std::vector<std::string> renderGraphRows(const std::vector<WebCommit>& commits);
std::string renderCommitRows(const std::string& project, const std::vector<WebCommit>& commits);
std::string renderCalendarGrid(const std::string& project, const std::string& ref,
                               int year, int month, const ActivityData& activity);
}  // namespace ckgit
