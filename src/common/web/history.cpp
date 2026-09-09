// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/dashboard.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include "ckgit/web_renderer.hpp"
namespace ckgit {
std::vector<std::string> renderGraphRows(const std::vector<WebCommit>& commits) {
  constexpr std::size_t maximum_lanes = 128;
  std::vector<std::string> lanes, rows;
  for (const auto& commit : commits) {
    auto found = std::find(lanes.begin(), lanes.end(), commit.id);
    const bool has_incoming = found != lanes.end();
    if (found == lanes.end()) { lanes.push_back(commit.id); found = lanes.end() - 1; }
    if (lanes.size() > maximum_lanes || commit.parents.size() > maximum_lanes) {
      rows.resize(commits.size(), "<span class=\"muted\">Graph exceeds 128 lanes</span>"); return rows;
    }
    const auto lane = static_cast<std::size_t>(found - lanes.begin());
    const auto before = lanes;
    if (commit.parents.empty()) lanes.erase(lanes.begin() + static_cast<std::ptrdiff_t>(lane));
    else {
      lanes[lane] = commit.parents.front();
      for (std::size_t p = 1; p < commit.parents.size(); ++p)
        if (std::find(lanes.begin(), lanes.end(), commit.parents[p]) == lanes.end())
          lanes.insert(lanes.begin() + static_cast<std::ptrdiff_t>(std::min(lane + p, lanes.size())), commit.parents[p]);
    }
    std::vector<std::string> unique;
    for (const auto& id : lanes) if (std::find(unique.begin(), unique.end(), id) == unique.end()) unique.push_back(id);
    lanes = std::move(unique);
    if (lanes.size() > maximum_lanes) {
      rows.resize(commits.size(), "<span class=\"muted\">Graph exceeds 128 lanes</span>");
      return rows;
    }
    auto x = [](std::size_t n) { return std::to_string(n * 14 + 8); };
    auto color = [](std::size_t n) { return "lane" + std::to_string(n % 6); };
    const auto width = std::max(before.size(), lanes.size()) * 14 + 4;
    // Only the edge layer stretches with the table row. The normal-flow node
    // layer reserves the graph's width and minimum height; the cell vertically
    // centers it without turning circles into ellipses in wrapped commit rows.
    const auto coordinates = " viewBox=\"0 0 " + std::to_string(width) + " 48\"";
    std::string svg = "<svg class=\"graph graph-lines\" aria-hidden=\"true\" focusable=\"false\" width=\"" +
        std::to_string(width) + "\" height=\"100%\"" + coordinates + " preserveAspectRatio=\"none\">";
    for (std::size_t i = 0; i < before.size(); ++i) {
      if (i == lane) {
        // A newly introduced tip has no edge arriving from the previous row.
        if (has_incoming) svg += "<path class=\"" + color(i) + "\" vector-effect=\"non-scaling-stroke\" d=\"M" + x(i) + " 0V24\"/>";
      }
      else {
        auto next = std::find(lanes.begin(), lanes.end(), before[i]);
        if (next != lanes.end()) svg += "<path class=\"" + color(i) + "\" vector-effect=\"non-scaling-stroke\" d=\"M" + x(i) + " 0L" +
            x(static_cast<std::size_t>(next - lanes.begin())) + " 48\"/>";
      }
    }
    for (const auto& parent : commit.parents) {
      auto next = std::find(lanes.begin(), lanes.end(), parent);
      if (next != lanes.end()) svg += "<path class=\"" + color(lane) + "\" vector-effect=\"non-scaling-stroke\" d=\"M" + x(lane) + " 24L" +
          x(static_cast<std::size_t>(next - lanes.begin())) + " 48\"/>";
    }
    svg += "</svg><svg class=\"graph graph-node\" aria-label=\"" +
        htmlEscape("Commit " + commit.id.substr(0, 8) + ": " + commit.subject) +
        "\" role=\"img\" focusable=\"false\" width=\"" +
        std::to_string(width) + "\" height=\"48\"" + coordinates + "><circle class=\"" + color(lane) +
        "\" cx=\"" + x(lane) + "\" cy=\"24\" r=\"4\"/></svg>";
    rows.push_back(std::move(svg));
  }
  return rows;
}
std::string renderCommitRows(const std::string& project, const std::vector<WebCommit>& commits) {
  const auto graph = renderGraphRows(commits);
  std::string out = "<table class=\"commit-table\"><thead><tr><th>Graph</th><th>Commit</th><th>Subject</th><th>Author</th><th>Time (UTC)</th></tr></thead><tbody>";
  for (std::size_t i = 0; i < commits.size(); ++i) {
    const auto& c = commits[i];
    const auto link = htmlEscape(sourceUrl(project, "commit", c.id));
    out += "<tr><td class=\"graph-cell\">" + graph[i] + "</td><td class=\"commit-id\"><a href=\"" + link + "\"><code>" +
        htmlEscape(c.id.substr(0, 8)) + "</code></a></td><td class=\"commit-subject\"><a href=\"" + link + "\">" + htmlEscape(c.subject) +
        "</a></td><td class=\"commit-author\">" + htmlEscape(c.author) + "</td><td class=\"commit-time\"><time title=\"" + formatUtcTimestamp(c.epoch) + "\">" +
        relativeTime(c.epoch) + "</time><br><small class=\"muted\">" + formatUtcTimestamp(c.epoch) + "</small></td></tr>";
  }
  if (commits.empty()) out += "<tr><td colspan=\"5\">No commits.</td></tr>";
  return out + "</tbody></table>";
}
namespace {
std::string dateString(int y, unsigned m, unsigned d) {
  char buf[16]{}; std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d); return buf;
}
std::string monthLink(const std::string& project, const std::string& ref, int y, unsigned m) {
  char suffix[16]{}; std::snprintf(suffix, sizeof(suffix), "/%04d/%02u", y, m);
  return htmlEscape(sourceUrl(project, "calendar", ref) + suffix);
}
std::string heat(std::size_t count) {
  return count == 0 ? "empty" : count < 3 ? "heat1" : count < 6 ? "heat2" : count < 10 ? "heat3" : "heat4";
}
}
std::string renderCalendarGrid(const std::string& project, const std::string& ref,
                               int y, int m, const ActivityData& activity) {
  using namespace std::chrono;
  const year_month ym{year{y}, month{static_cast<unsigned>(m)}};
  if (!ym.ok() || y < 1 || y > 9999) throw WebError(404, "Invalid calendar month.");
  const sys_days first{ym / day{1}};
  const unsigned offset = weekday{first}.iso_encoding() - 1;
  const unsigned days = unsigned(year_month_day_last{ym / last}.day());
  const auto prev = ym - months{1}, next = ym + months{1};
  const char* month_names[]{"", "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  std::string out = "<div class=\"toolbar calendar-nav\">";
  if (y > 1 || m > 1) out += "<a href=\"" + monthLink(project, ref, int(prev.year()), unsigned(prev.month())) + "\">← Previous month</a>";
  out += "<h2>" + std::string(month_names[m]) + " " + std::to_string(y) + "</h2>";
  if (y < 9999 || m < 12) out += "<a href=\"" + monthLink(project, ref, int(next.year()), unsigned(next.month())) + "\">Next month →</a>";
  out += "</div><table class=\"calendar\"><thead><tr>";
  for (auto name : {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"}) out += "<th>" + std::string(name) + "</th>";
  out += "</tr></thead><tbody>";
  for (unsigned cell = 0; cell < ((offset + days + 6) / 7) * 7; ++cell) {
    if (cell % 7 == 0) out += "<tr>";
    if (cell < offset || cell >= offset + days) out += "<td></td>";
    else {
      const auto d = cell - offset + 1; const auto date = dateString(y, m, d);
      auto found = activity.counts.find(date); const auto count = found == activity.counts.end() ? 0 : found->second;
      out += "<td class=\"" + heat(count) + "\"><a href=\"" + htmlEscape(sourceUrl(project, "day", ref) + "/" + date) +
          "\">" + std::to_string(d) + "<small>" + (count ? std::to_string(count) + " <span class=\"calendar-count-label\">" + (count == 1 ? "commit" : "commits") + "</span>" : "") + "</small></a></td>";
    }
    if (cell % 7 == 6) out += "</tr>";
  }
  out += "</tbody></table><h2>" + std::to_string(y) + " activity</h2><div class=\"year-strip\"><table aria-label=\"Year activity by day\"><tbody>";
  const sys_days jan{year{y}/January/1};
  const sys_days dec{year{y}/December/31};
  const auto start = jan - std::chrono::days{weekday{jan}.iso_encoding() - 1};
  for (int row = 0; row < 7; ++row) {
    out += "<tr>";
    for (auto day = start + std::chrono::days{row}; day <= dec + std::chrono::days{6}; day += weeks{1}) {
      year_month_day date{day};
      if (day < jan || day > dec) { out += "<td></td>"; continue; }
      const auto key = dateString(int(date.year()), unsigned(date.month()), unsigned(date.day()));
      auto found = activity.counts.find(key); auto count = found == activity.counts.end() ? 0 : found->second;
      out += "<td class=\"" + heat(count) + "\"><a title=\"" + key + ": " + std::to_string(count) + " commits\" aria-label=\"" +
          key + ": " + std::to_string(count) + " commits\" href=\"" + htmlEscape(sourceUrl(project, "day", ref) + "/" + key) + "\"></a></td>";
    }
    out += "</tr>";
  }
  out += "</tbody></table><p class=\"heat-legend\">Less <span class=\"heat-swatch empty\"></span><span class=\"heat-swatch heat1\"></span>"
      "<span class=\"heat-swatch heat2\"></span><span class=\"heat-swatch heat3\"></span><span class=\"heat-swatch heat4\"></span> More</p></div>";
  if (activity.truncated) out += "<p class=\"notice\">Activity is limited to the newest 200000 commits.</p>";
  return out;
}
}  // namespace ckgit
