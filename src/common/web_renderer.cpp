// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/web_renderer.hpp"

#include <cstdint>
#include <ctime>

namespace {

std::string formatTimestamp(std::uint64_t epoch_seconds) {
  const std::time_t timestamp = static_cast<std::time_t>(epoch_seconds);
  std::tm utc{};
  if (gmtime_r(&timestamp, &utc) == nullptr) {
    return {};
  }
  char buffer[32]{};
  return std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M UTC", &utc) == 0
             ? std::string{}
             : std::string(buffer);
}

std::string renderCheckout(const ckgit::CheckoutMetadata& checkout, bool include_timestamp) {
  std::string rendered = "<strong>" + ckgit::htmlEscape(checkout.client_id) + "</strong> · <code>" +
      ckgit::htmlEscape(checkout.reported_path) + "</code>";
  if (include_timestamp) {
    const std::string timestamp = formatTimestamp(checkout.last_seen_epoch_seconds);
    if (!timestamp.empty()) {
      rendered += " <span class=\"muted\">reported ";
      rendered += timestamp;
      rendered += "</span>";
    }
  }
  return rendered;
}

std::string renderCheckoutSummary(const ckgit::ProjectSummary& project) {
  if (project.checkouts.empty()) {
    return "<span class=\"muted\">none</span>";
  }
  std::string rendered = renderCheckout(project.checkouts.front(), false);
  if (project.checkouts.size() > 1) {
    rendered += " <span class=\"muted\">+ ";
    rendered += std::to_string(project.checkouts.size() - 1);
    rendered += " more</span>";
  }
  return rendered;
}

std::string renderCheckoutList(const ckgit::ProjectSummary& project) {
  if (project.checkouts.empty()) {
    return "<p class=\"muted\">No checkout has reported in.</p>";
  }
  std::string rendered = "<ul>";
  for (const auto& checkout : project.checkouts) {
    rendered += "<li>" + renderCheckout(checkout, true) + "</li>";
  }
  return rendered + "</ul>";
}

std::string renderEventList(const ckgit::ProjectSummary& project) {
  if (project.events.empty()) {
    return "<p class=\"muted\">No recorded events.</p>";
  }
  std::string rendered = "<ol>";
  for (const auto& event : project.events) {
    const std::string label = event.kind == "project-created" ? "Project created"
                            : event.kind == "checkout-registered" ? "Checkout registered"
                            : event.kind == "git-push" ? "Git push"
                                                                      : ckgit::htmlEscape(event.kind);
    rendered += "<li>" + label + " · <strong>" + ckgit::htmlEscape(event.client_id) + "</strong>";
    const std::string timestamp = formatTimestamp(event.epoch_seconds);
    if (!timestamp.empty()) {
      rendered += " <span class=\"muted\">" + timestamp + "</span>";
    }
    rendered += "</li>";
  }
  return rendered + "</ol>";
}

}  // namespace

namespace ckgit {

std::string htmlEscape(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    switch (character) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '"': result += "&quot;"; break;
      case '\'': result += "&#39;"; break;
      default:
        if (character < 0x20 || character == 0x7f) {
          result += "&#xfffd;";
        } else {
          result += static_cast<char>(character);
        }
    }
  }
  return result;
}

std::string renderProjectTable(const std::vector<ProjectSummary>& projects) {
  std::string html =
      "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
      "content=\"width=device-width,initial-scale=1\"><title>ck-git-hosting</title><style>"
      "body{font-family:system-ui,sans-serif;margin:2rem;color:#18212b}table{border-collapse:collapse;"
      "width:100%;max-width:70rem}th,td{text-align:left;padding:.55rem;border-bottom:1px solid #d9e0e7}"
      ".ok{color:#176c3a}.warn{color:#9a5800}.muted{color:#59636d}</style></head><body><h1>Projects</h1>"
      "<table><thead><tr><th>Project</th><th>Default branch</th><th>Branches</th><th>Tags</th>"
      "<th>Last reported checkout</th></tr></thead><tbody>";
  if (projects.empty()) {
    html += "<tr><td colspan=\"5\">No projects</td></tr>";
  }
  for (const auto& project : projects) {
    html += "<tr><td><a href=\"/project/" + htmlEscape(project.name) + "\">" +
            htmlEscape(project.name) + "</a></td><td class=\"" +
            (project.valid_head ? "ok\">" : "warn\">") +
            htmlEscape(project.default_branch.empty() ? "missing" : project.default_branch) + "</td><td>" +
            std::to_string(project.branch_count) + "</td><td>" + std::to_string(project.tag_count) + "</td><td>" +
            renderCheckoutSummary(project) + "</td></tr>";
  }
  html += "</tbody></table></body></html>";
  return html;
}

std::string renderProjectDetail(const ProjectSummary& project) {
  return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
         "content=\"width=device-width,initial-scale=1\"><title>" + htmlEscape(project.name) +
         " · ck-git-hosting</title><style>body{font-family:system-ui,sans-serif;margin:2rem;color:#18212b}"
         "dl{display:grid;grid-template-columns:max-content 1fr;gap:.5rem 1rem}.ok{color:#176c3a}.warn{color:#9a5800}"
         ".muted{color:#59636d}a{color:#145ea8}</style></head><body><p><a href=\"/\">Projects</a></p><h1>" +
         htmlEscape(project.name) + "</h1><dl><dt>Default branch</dt><dd class=\"" +
         (project.valid_head ? "ok\">" : "warn\">") +
         htmlEscape(project.default_branch.empty() ? "missing" : project.default_branch) +
         "</dd><dt>Branches</dt><dd>" + std::to_string(project.branch_count) + "</dd><dt>Tags</dt><dd>" +
         std::to_string(project.tag_count) + "</dd></dl><h2>Checkouts</h2>" +
         renderCheckoutList(project) + "<h2>Recent events</h2>" + renderEventList(project) +
         "</body></html>";
}

}  // namespace ckgit
