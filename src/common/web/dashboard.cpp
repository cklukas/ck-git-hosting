// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/dashboard.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include "ckgit/markdown.hpp"
#include "ckgit/web_renderer.hpp"
#include "tree.hpp"
namespace ckgit {
namespace {
void bounded(const std::string& out) {
  if (out.size() > kMaximumPageBytes) throw WebError(503, "Page exceeds the 8 MiB rendered output limit.");
}
std::string parentPath(const std::string& path) {
  const auto pos = path.rfind('/'); return pos == std::string::npos ? "" : path.substr(0, pos);
}
std::string joinPath(const std::string& dir, const std::string& name) { return dir.empty() ? name : dir + "/" + name; }
std::string link(const std::string& url, const std::string& text) {
  return "<a href=\"" + htmlEscape(url) + "\">" + htmlEscape(text) + "</a>";
}
std::string breadcrumbs(const ProjectSummary& p, const std::string& id, const std::string& path) {
  auto out = link(sourceUrl(p.name, "tree", id), p.name);
  for (std::size_t start = 0; start < path.size();) {
    auto end = path.find('/', start); if (end == std::string::npos) end = path.size();
    out += " / " + (end == path.size() ? htmlEscape(path.substr(start)) :
        link(sourceUrl(p.name, "tree", id, path.substr(0, end)), path.substr(start, end - start)));
    start = end + 1;
  }
  return "<p>" + out + "</p>";
}
std::string sourceLines(const std::string& content) {
  std::string out = "<input class=\"source-wrap\" id=\"wrap-lines\" type=\"checkbox\"><label for=\"wrap-lines\">Wrap lines</label><pre class=\"source\"><code>";
  std::size_t line = 1;
  for (std::size_t start = 0; start < content.size();) {
    auto end = content.find('\n', start); if (end == std::string::npos) end = content.size();
    auto n = std::to_string(line++);
    out += "<span class=\"line\" id=\"L" + n + "\"><a href=\"#L" + n + "\" aria-label=\"Line " + n + "\">" + n +
        "</a><span class=\"line-text\">" + escapePre(std::string_view(content).substr(start, end - start)) + "</span></span>";
    bounded(out);
    start = end + 1;
  }
  return out + "</code></pre>";
}
std::string filePreview(WebRepository& repo, const ProjectSummary& p, const std::string& id,
                         const std::string& ref, const std::string& path, const TreeEntry& e, bool source) {
  auto out = "<h1>" + htmlEscape(e.name) + "</h1><p class=\"muted\">" + formatBytes(e.size);
  if (e.type == "commit") return out + " · submodule</p><code>" + htmlEscape(e.id) + "</code>";
  if (e.type != "blob") throw WebError(404, "This path is a directory; open it in Files.");
  const auto raw = sourceUrl(p.name, "raw", id, path);
  const auto content_type = rawContentType(path);
  out += " · " + htmlEscape(e.mode == "120000" ? "symlink (target is never followed)" : content_type) + "</p>";
  if (e.size <= kMaximumRawBytes) out += "<p>" + link(raw, "Download raw file") + "</p>";
  else out += "<p class=\"notice\">File exceeds the 16 MiB download limit. Use Git to retrieve it.</p>";
  if (e.mode != "120000" && isImageType(content_type)) {
    if (e.size <= kMaximumInlineImageBytes)
      out += "<img src=\"" + htmlEscape(raw) + "\" alt=\"" + htmlEscape(e.name) + "\">";
    else out += "<p class=\"notice\">Image exceeds the 4 MiB inline preview limit.</p>";
  } else if (e.size <= kMaximumPreviewBytes) {
    auto content = repo.blob(e, kMaximumPreviewBytes);
    if (isTextBlob(content)) {
      auto lower = path;
      for (auto& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      const bool markdown = e.mode != "120000" && (lower.ends_with(".md") || lower.ends_with(".markdown"));
      if (markdown) {
        out += "<nav class=\"view-mode\" aria-label=\"File view\"><a href=\"" +
            htmlEscape(sourceUrl(p.name, "blob", ref, path)) + "\"" + (source ? "" : " aria-current=\"page\"") + ">Rendered</a><a href=\"" +
            htmlEscape(sourceUrl(p.name, "source", ref, path)) + "\"" + (source ? " aria-current=\"page\"" : "") + ">Source</a></nav>";
      }
      if (markdown && !source)
        out += "<section class=\"readme\">" + renderMarkdown(content, LinkContext{p.name, id, parentPath(path), ref}) + "</section>";
      else out += sourceLines(content);
    }
    else out += "<p class=\"muted\">Binary file; preview unavailable.</p>";
  } else out += "<p class=\"notice\">File exceeds the 512 KiB text preview limit.</p>";
  return out;
}
std::string renderDiff(const ProcessResult& diff) {
  std::string out;
  if (diff.output_truncated) out += "<p class=\"notice\">Diff truncated at 512 KiB.</p>";
  out += "<div class=\"diff-wrap\"><table class=\"diff\"><tbody>";
  auto text = std::string_view(diff.output);
  if (diff.output_truncated) { auto newline = text.rfind('\n'); text = text.substr(0, newline == std::string_view::npos ? 0 : newline + 1); }
  std::size_t file_index = 0;
  while (!text.empty()) {
    auto end = text.find('\n'); if (end == std::string_view::npos) end = text.size();
    const auto line = text.substr(0, end);
    const bool file_header = line.starts_with("diff --git ");
    const auto kind = line.starts_with("+") ? "add" : line.starts_with("-") ? "remove" : line.starts_with("@@") ? "hunk" : "context";
    out += "<tr class=\"" + std::string(kind) + "\"" +
        (file_header ? " id=\"diff-file-" + std::to_string(file_index++) + "\"" : "") + "><td>" + escapePre(line) + "</td></tr>";
    bounded(out);
    text.remove_prefix(std::min(end + 1, text.size()));
  }
  return out + "</tbody></table></div>";
}
// Stat and patch output list files in the same tree order, so each stat line
// links to its diff section by position rather than re-parsing a renamed path.
std::string renderChangedFiles(const ProcessResult& stat) {
  std::string out;
  if (stat.output_truncated) out += "<p class=\"notice\">Changed-file list truncated at 128 KiB.</p>";
  out += "<pre>";
  std::size_t file_index = 0;
  std::string_view text(stat.output);
  while (!text.empty()) {
    auto end = text.find('\n'); if (end == std::string_view::npos) end = text.size();
    const auto line = text.substr(0, end);
    const auto separator = line.find(" | ");
    if (separator != std::string_view::npos)
      out += link("#diff-file-" + std::to_string(file_index++), std::string(line.substr(0, separator))) + escapePre(line.substr(separator));
    else out += escapePre(line);
    out += "\n";
    bounded(out);
    text.remove_prefix(std::min(end + 1, text.size()));
  }
  return out + "</pre>";
}
std::string dateForRoute(const Route& route) {
  char buffer[16]{}; std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", route.year, route.month, route.day); return buffer;
}
std::string sectionForRoute(const Route& route) {
  switch (route.kind) {
    case RouteKind::kOverview: return "overview";
    case RouteKind::kTree: return "tree";
    case RouteKind::kBlob: return "blob";
    case RouteKind::kSource: return "source";
    case RouteKind::kCommits: return "commits";
    case RouteKind::kGraph: return "graph";
    case RouteKind::kCalendar: return "calendar";
    case RouteKind::kDay: return "day";
    case RouteKind::kCiRuns: return "ci";
    case RouteKind::kReleases: return "releases";
    default: return "commit";
  }
}
std::string latestLink(const ProjectSummary& project, const std::string& ref) {
  if (!project.valid_head || ref == "heads/" + project.default_branch) return {};
  return "<p>" + link(sourceUrl(project.name, "overview", "heads/" + project.default_branch), "Latest default branch") + "</p>";
}
}  // namespace
DashboardResponse renderDashboard(const Route& route, const ProjectSummary& project,
                                   const std::filesystem::path& repository, std::chrono::steady_clock::time_point deadline) {
  DashboardResponse response;
  if (route.kind == RouteKind::kCiRuns) {
    PageContext ci_context{{}, {}, "ci", {}, 0, 0};
    const unsigned refresh = ciAnyActiveRun(project.ci_runs) ? 3u : 0u;
    response.body = pageLayout(project.name + " \xc2\xb7 CI", renderCiRuns(project), &project, &ci_context, refresh);
    return response;
  }
  if (route.kind == RouteKind::kReleases) {
    PageContext releases_context{{}, {}, "releases", {}, 0, 0};
    response.body = pageLayout(project.name + " \xc2\xb7 Releases", renderReleases(project), &project,
                               &releases_context);
    return response;
  }
  const bool empty = !project.indexing && project.index_error.empty() && !project.valid_head &&
      !project.branch_count && !project.tag_count;
  if ((route.kind == RouteKind::kOverview && route.ref.empty()) ||
      (route.kind != RouteKind::kRaw && (empty || (project.indexing && project.head_id.empty())))) {
    response.body = renderProjectDetail(project); return response;
  }
  WebRepository repo(repository, deadline);
  const bool indexed_calendar = route.kind == RouteKind::kCalendar && !project.indexing && project.index_error.empty() &&
      !project.head_id.empty() && (route.ref == project.head_id || route.ref == "heads/" + project.default_branch);
  const auto resolved = indexed_calendar ? ResolvedRef{project.head_id, route.ref, false} : repo.resolve(route.ref);
  const auto& id = resolved.id;
  const auto& ref = resolved.name;
  PageContext context{ref, id, sectionForRoute(route), route.path, route.year, route.month};
  if (route.kind == RouteKind::kRaw) {
    const auto e = repo.entry(id, route.path);
    response.body = repo.blob(e, kMaximumRawBytes);
    response.raw = true; response.content_type = rawContentType(route.path);
    response.filename = e.name;
    return response;
  }
  if (route.kind == RouteKind::kOverview) {
    auto selected = project;
    const auto commit = repo.commit(id);
    selected.last_commit = CommitSummary{id, commit.author, commit.subject, static_cast<std::uint64_t>(std::max<std::int64_t>(0, commit.epoch))};
    selected.last_commit_epoch_seconds = selected.last_commit->epoch_seconds;
    selected.readme_path.clear(); selected.readme_content.clear(); selected.readme_truncated = false;
    const auto entries = repo.tree(id, "");
    selected.readme_path = chooseReadme(entries);
    if (!selected.readme_path.empty()) {
      const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.name == selected.readme_path; });
      selected.readme_truncated = found->size > kMaximumPreviewBytes;
      if (!selected.readme_truncated) selected.readme_content = repo.blob(*found, kMaximumPreviewBytes);
    }
    response.body = renderProjectDetail(selected, &context);
    return response;
  }
  std::string body;
  if (resolved.ambiguous) body += "<p class=\"notice\">A branch and a tag share this name. Showing the branch. " +
      link(sourceUrl(project.name, route.kind == RouteKind::kDay ? "calendar" : context.section,
          "tags/" + route.ref, route.path), "View tag") + "</p>";
  body += latestLink(project, ref);
  if (route.kind == RouteKind::kTree || route.kind == RouteKind::kBlob || route.kind == RouteKind::kSource) {
    const bool is_tree = route.kind == RouteKind::kTree;
    TreeEntry entry;
    if (!is_tree) {
      entry = repo.entry(id, route.path);
      if (entry.type == "tree") {
        response.status = 302; response.location = sourceUrl(project.name, "tree", ref, route.path);
        response.body = pageLayout(project.name, "<p>" + link(response.location, "Open directory") + "</p>", &project, &context);
        return response;
      }
    }
    const auto directory = is_tree ? route.path : parentPath(route.path);
    body += "<div class=\"toolbar\">" + refPicker(project, ref, context.section, route.path) +
        "<span class=\"muted\">" + link(sourceUrl(project.name, context.section, id, route.path), "Permalink") + " · " +
        link(sourceUrl(project.name, "commit", id), id.substr(0, 8)) + "</span></div>" +
        breadcrumbs(project, ref, route.path) + "<p>" + link("#file-preview", "Jump to file") + "</p><div class=\"browser\">"
        "<input class=\"tree-switch\" type=\"checkbox\" id=\"show-files\"><label class=\"tree-toggle\" for=\"show-files\">"
        "<span class=\"show-files\">Show files</span><span class=\"hide-files\">Hide files</span></label>" +
        renderFileTree(repo, project, id, ref, directory, route.path) + "<section class=\"preview\" id=\"file-preview\">";
    if (is_tree) {
      const auto entries = repo.tree(id, route.path);
      const auto readme = chooseReadme(entries);
      if (!readme.empty()) {
        auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.name == readme; });
        if (found->size <= kMaximumPreviewBytes) body += renderReadme(project, id, joinPath(route.path, readme), repo.blob(*found, kMaximumPreviewBytes), ref);
        else body += "<p class=\"notice\">README exceeds the 512 KiB preview limit.</p>";
      } else body += "<h2>" + htmlEscape(route.path.empty() ? project.name : route.path) + "</h2><p class=\"muted\">No README in this directory. Select a file to preview it.</p>";
    } else body += filePreview(repo, project, id, ref, route.path, entry, route.kind == RouteKind::kSource);
    body += "</section></div>";
  } else if (route.kind == RouteKind::kCommits || route.kind == RouteKind::kGraph) {
    // Both routes render the same commit table with its graph column; only
    // Commits is offered from navigation, but an existing /graph/... link
    // keeps working with its own consistent pagination and heading.
    const auto page = repo.commits(id, route.cursor);
    body += "<h1>" + std::string(route.kind == RouteKind::kGraph ? "Commit graph" : "Commits") + "</h1>" +
        refPicker(project, ref, context.section) + renderCommitRows(project.name, page.commits);
    body += "<div class=\"pagination\">" + link(sourceUrl(project.name, context.section, ref), "Newest") +
        (page.has_more ? link(sourceUrl(project.name, context.section, ref) + "/before/" + page.commits.back().id, "Older →") : "<span>End of history</span>") + "</div>";
  } else if (route.kind == RouteKind::kCommit) {
    const auto c = repo.commit(id);
    body += "<h1>" + htmlEscape(c.subject) + "</h1><p class=\"ref-context\">" + htmlEscape(refLabel(ref)) + "</p><p><code class=\"full-object-id\">" + htmlEscape(id) + "</code> · " +
        link(sourceUrl(project.name, "tree", id), "Browse files") + "</p><dl><dt>Author</dt><dd>" + htmlEscape(c.author) +
        " · " + formatUtcTimestamp(c.author_epoch) + "</dd><dt>Committer</dt><dd>" + htmlEscape(c.committer) + " · " +
        formatUtcTimestamp(c.epoch) + "</dd><dt>Parents</dt><dd>";
    if (c.parents.empty()) body += "Root commit";
    for (const auto& parent : c.parents) body += link(sourceUrl(project.name, "commit", parent), parent.substr(0, 8)) + " ";
    body += "</dd></dl><pre>" + escapePre(c.message) + "</pre><h2>Changed files</h2>" + renderChangedFiles(repo.diff(id, true)) + "<h2>Diff</h2>";
    if (c.parents.size() > 1) body += "<p class=\"muted\">Changes compared with the first parent.</p>";
    body += renderDiff(repo.diff(id, false));
  } else if (route.kind == RouteKind::kCalendar) {
    const ActivityData activity = id == project.head_id && !project.indexing && project.index_error.empty()
        ? ActivityData{project.activity, project.history_truncated} : repo.activity(id);
    int y = route.year, m = route.month;
    if (!y) {
      // Open on this ref's own most recent activity rather than today's real-
      // world month, which is usually empty for a dormant project or branch.
      if (!activity.counts.empty()) {
        const auto& latest = activity.counts.rbegin()->first;
        y = std::atoi(latest.substr(0, 4).c_str()); m = std::atoi(latest.substr(5, 2).c_str());
      } else {
        using namespace std::chrono;
        const year_month_day now{floor<days>(system_clock::now())}; y = int(now.year()); m = int(unsigned(now.month()));
      }
    }
    context.year = y; context.month = m;
    body += "<h1>Calendar</h1>" + refPicker(project, ref, "calendar", "", y, m) + renderCalendarGrid(project.name, ref, y, m, activity);
    if (!activity.counts.empty()) {
      body += "<p>Active years: ";
      std::string last;
      for (const auto& [date, count] : activity.counts) { static_cast<void>(count); auto year = date.substr(0, 4);
        // Land on this year's own first active month rather than an
        // unconditional January, which is often empty for that year.
        if (year != last) { body += link(sourceUrl(project.name, "calendar", ref) + "/" + year + "/" + date.substr(5, 2), year) + " "; last = year; }
      }
      body += "</p>";
    }
  } else if (route.kind == RouteKind::kDay) {
    const auto date = dateForRoute(route); const auto commits = repo.day(id, date); const auto asof = repo.asOf(id, date);
    static const char* month_names[]{"", "January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December"};
    const auto human_date = std::to_string(route.day) + " " + month_names[route.month] + " " + std::to_string(route.year);
    const auto count = commits.commits.size();
    const auto count_label = std::to_string(count) + (commits.has_more ? "+" : "") + (count == 1 && !commits.has_more ? " commit" : " commits");
    body += "<h1>" + htmlEscape(count_label + " · " + refLabel(ref) + " · " + human_date + " · UTC") + "</h1>" +
        refPicker(project, ref, "calendar", "", route.year, route.month) + "<p>" + link(sourceUrl(project.name, "calendar", ref) + "/" + date.substr(0, 4) + "/" + date.substr(5, 2), "Back to calendar") + "</p>";
    if (!asof.empty()) body += "<p>" + link(sourceUrl(project.name, "tree", asof), "Browse the tree as of this day") + "</p>";
    else body += "<p class=\"muted\">No tree existed on this branch by the end of this day.</p>";
    if (commits.has_more) body += "<p class=\"notice\">Showing the first 200 commits on this day. " + link(sourceUrl(project.name, "commits", ref), "Browse full history") + "</p>";
    body += renderCommitRows(project.name, commits.commits);
  } else throw WebError(404, "Page was not found.");
  response.body = pageLayout(project.name + " · " + refLabel(ref), body, &project, &context);
  return response;
}
DashboardResponse renderDashboardError(const Route& route, const ProjectSummary& project, int status, const std::string& message) {
  DashboardResponse response; response.status = status;
  PageContext context{route.ref, isObjectId(route.ref) ? route.ref : "", sectionForRoute(route), route.path, route.year, route.month};
  const auto ref = route.ref.empty() && project.valid_head ? "heads/" + project.default_branch : route.ref;
  std::string body = "<h1>" + std::string(status == 503 ? "Temporarily unavailable" : "Content not found") + "</h1>";
  body += "<p class=\"notice\">" + htmlEscape(message) + "</p>";
  if (!route.path.empty()) body += "<p>Requested file or directory: <code>" + htmlEscape(route.path) + "</code></p>";
  body += refPicker(project, ref, route.path.empty() ? "tree" : "blob", route.path);
  body += "<p>" + link("/project/" + project.name, "Project overview");
  if (!ref.empty()) {
    body += " · " + link(sourceUrl(project.name, "tree", ref), "Repository root");
    if (!route.path.empty()) body += " · " + link(sourceUrl(project.name, "tree", ref, parentPath(route.path)), "Parent directory");
  }
  body += "</p><p>" + std::string(status == 503 ? "Reload this page to retry, or choose another branch or tag." :
      "Choose another branch or tag, or return to a parent directory to find the file.") + "</p>";
  body += latestLink(project, ref);
  response.body = pageLayout(project.name + " · " + (status == 503 ? "Temporarily unavailable" : "Content not found"), body, &project, &context);
  return response;
}
}  // namespace ckgit
