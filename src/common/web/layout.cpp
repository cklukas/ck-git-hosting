// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/web_renderer.hpp"
#include <ctime>
#include <iomanip>
#include <sstream>
#include "ckgit/cli_help.hpp"
#include "ckgit/http_router.hpp"
#include "ckgit/markdown.hpp"
#include "ckgit/runtime_status.hpp"
#include "ckgit/web_repository.hpp"
#include "style.hpp"
namespace ckgit {
namespace {
constexpr const char* kTextBrand =
    "<span class=\"brand-monogram\">ck</span>"
    "<span class=\"brand-wordmark\">git<span class=\"brand-caption\">hosting</span></span>";

// The versions running on this server, refreshed per request by the daemon
// serving the page (the About dialog is on every page). Empty on a page not
// rendered by the hosting daemon, in which case the dialog shows only this
// process's own build. Per-thread because each request is handled start to
// finish on one worker thread, so no lock is needed.
thread_local std::vector<RuntimeComponent> t_about_components;

std::string aboutDialog() {
  std::string components;
  if (!t_about_components.empty()) {
    components = "<ul class=\"about-components\">";
    for (const auto& component : t_about_components) {
      components += "<li>" + htmlEscape(component.name) + " <code>" +
          htmlEscape(component.version.empty() ? "unknown" : component.version) + "</code>" +
          (component.running ? "" : " <span class=\"muted\">(not running)</span>") + "</li>";
    }
    components += "</ul>";
  }
  return "<div id=\"about-dialog\" class=\"about-dialog\" popover=\"auto\" role=\"dialog\" aria-labelledby=\"about-title\">"
      "<div class=\"about-heading\"><div class=\"brand about-brand\" aria-hidden=\"true\">" + std::string(kTextBrand) +
      "</div><button type=\"button\" class=\"about-close\" popovertarget=\"about-dialog\" popovertargetaction=\"hide\" autofocus>Close</button></div>"
      "<h2 id=\"about-title\">About ck-git-hosting</h2>"
      "<p class=\"about-version\">Version <code>" + htmlEscape(buildVersion()) + "</code></p>" + components +
      "<p>Publish Git projects and browse their files and history.</p>"
      "<p>© 2026 C. Klukas</p><p class=\"muted\">Licensed under the MIT License.</p></div>";
}
}  // namespace

void setAboutServerComponents(std::vector<RuntimeComponent> components) {
  t_about_components = std::move(components);
}

std::string formatUtcTimestamp(std::uint64_t epoch) {
  const auto timestamp = static_cast<std::time_t>(epoch); std::tm utc{}; char buffer[32]{};
  if (!gmtime_r(&timestamp, &utc) || !std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M UTC", &utc)) return {};
  return buffer;
}
std::string relativeTime(std::uint64_t epoch) {
  auto now = static_cast<std::uint64_t>(std::time(nullptr));
  if (epoch > now) return "in the future";
  auto delta = now - epoch;
  if (delta < 60) return "just now";
  for (const auto& unit : {std::pair<std::uint64_t, const char*>{31536000, "year"}, {2592000, "month"},
                          {86400, "day"}, {3600, "hour"}, {60, "minute"}})
    if (delta >= unit.first) { auto n = delta / unit.first; return std::to_string(n) + " " + unit.second + (n == 1 ? " ago" : "s ago"); }
  return "just now";
}
std::string formatBytes(std::uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  double n = bytes; std::size_t unit = 0;
  while (n >= 1024 && unit < 3) { n /= 1024; ++unit; }
  const char* units[]{"B", "KiB", "MiB", "GiB"};
  std::ostringstream text; text << std::fixed << std::setprecision(1) << n << " " << units[unit]; return text.str();
}
std::string escapePre(std::string_view value) {
  std::string result;
  std::size_t start = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\n' || value[i] == '\t') {
      result += htmlEscape(value.substr(start, i - start)); result += value[i]; start = i + 1;
    }
  }
  return result + htmlEscape(value.substr(start));
}
std::string sourceUrl(const std::string& project, const std::string& kind,
                      const std::string& ref, const std::string& path) {
  auto result = "/project/" + project + "/" + kind + "/" + encodePathSegment(ref);
  if (kind == "tree" || kind == "blob" || kind == "source" || kind == "raw") result += ":" + encodePathSegment(path);
  else if ((kind == "calendar" || kind == "commits" || kind == "graph") &&
           parseHttpRoute(result).ref != ref) result += ':';
  return result;
}
std::string refLabel(const std::string& ref) {
  if (ref.starts_with("heads/")) return "Branch " + ref.substr(6);
  if (ref.starts_with("tags/")) return "Tag " + ref.substr(5);
  if (isObjectId(ref)) return "Commit " + ref.substr(0, 8);
  return ref.empty() ? "Choose a branch or tag" : "Ref " + ref;
}
std::string refPicker(const ProjectSummary& p, const std::string& selected,
                      const std::string& kind, const std::string& path, int year, int month) {
  std::string suffix;
  if (kind == "calendar" && year && month) {
    std::ostringstream date; date << '/' << std::setfill('0') << std::setw(4) << year << '/' << std::setw(2) << month;
    suffix = date.str();
  }
  std::string out = "<details class=\"picker\"><summary title=\"" + htmlEscape(selected) + "\">" + htmlEscape(refLabel(selected)) + " ▾</summary>";
  for (bool tags : {false, true}) {
    out += tags ? "<h3>Tags</h3><ul>" : "<h3>Branches</h3><ul>";
    for (const auto& ref : p.refs) if (ref.is_tag == tags) {
      const auto target = (tags ? "tags/" : "heads/") + ref.name;
      out += "<li><a" + std::string(target == selected ? " aria-current=\"true\"" : "") + " href=\"" +
          htmlEscape(sourceUrl(p.name, kind, target, path) + suffix) + "\">" + htmlEscape(ref.name) + "</a></li>";
    }
    out += "</ul>";
  }
  if (p.refs_truncated) out += "<p class=\"muted\">Showing the newest 500 refs.</p>";
  return out + "</details>";
}
std::string renderReadme(const ProjectSummary& p, const std::string& id,
                         const std::string& path, const std::string& content, const std::string& ref) {
  if (!isTextBlob(content)) return "<p class=\"notice\">README is binary or is not valid UTF-8; preview unavailable.</p>";
  auto lower = path; for (char& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  const auto slash = path.rfind('/');
  std::string out = "<section class=\"readme\"><h2>" + htmlEscape(path) + "</h2>";
  if (lower.ends_with(".md") || lower.ends_with(".markdown"))
    out += renderMarkdown(content, LinkContext{p.name, id, slash == std::string::npos ? "" : path.substr(0, slash), ref});
  else out += "<pre>" + escapePre(content) + "</pre>";
  return out + "</section>";
}
std::string pageLayout(std::string_view title, std::string_view body, const ProjectSummary* p, const PageContext* context, unsigned refresh_seconds) {
  if (body.size() > kMaximumPageBytes) throw WebError(503, "Page exceeds the 8 MiB rendered output limit.");
  std::string out = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  // A no-JS live refresh for pages that show an in-progress CI run. It fits the
  // strict Content-Security-Policy (no scripts) and simply re-fetches the same
  // canonical GET URL; the page stops emitting it once the run is terminal.
  if (refresh_seconds > 0) out += "<meta http-equiv=\"refresh\" content=\"" + std::to_string(refresh_seconds) + "\">";
  out += "<title>" + htmlEscape(title) + " · ck-git-hosting</title><style>";
  out += kWebStyles;
  out += "</style></head><body><a class=\"skip-link\" href=\"#main-content\">Skip to content</a><header>"
      "<a class=\"brand\" href=\"/\" aria-label=\"ck-git-hosting\">" + std::string(kTextBrand) + "</a><a href=\"/\">Projects</a>";
  if (p) {
    const auto ref = context && !context->ref.empty() ? context->ref :
        p->valid_head && !p->default_branch.empty() ? "heads/" + p->default_branch : std::string{};
    out += "<a href=\"" + htmlEscape(ref.empty() ? "/project/" + p->name : sourceUrl(p->name, "overview", ref)) + "\">" + htmlEscape(p->name) + "</a><nav aria-label=\"Project sections\">";
    out += "<a href=\"" + htmlEscape(ref.empty() ? "/project/" + p->name : sourceUrl(p->name, "overview", ref)) +
        "\"" + (context == nullptr || context->section == "overview" ? " aria-current=\"page\"" : "") + ">Overview</a>";
    out += "<a href=\"/project/" + htmlEscape(p->name) + "/ci\"" +
        (context && context->section == "ci" ? " aria-current=\"page\"" : "") + ">CI</a>";
    out += "<a href=\"/project/" + htmlEscape(p->name) + "/releases\"" +
        (context && context->section == "releases" ? " aria-current=\"page\"" : "") + ">Releases</a>";
    if (!ref.empty() && (p->branch_count || p->tag_count || p->valid_head || (context && !context->commit_id.empty()))) {
      for (const auto& item : {std::pair<const char*, const char*>{"commits", "Commits"}, {"tree", "Files"}, {"calendar", "Calendar"}}) {
        std::string path;
        if (std::string_view(item.first) == "tree" && context) {
          if (context->section == "tree") path = context->path;
          else if (context->section == "blob" || context->section == "source") {
            const auto slash = context->path.rfind('/');
            if (slash != std::string::npos) path = context->path.substr(0, slash);
          }
        }
        auto url = sourceUrl(p->name, item.first, ref, path);
        if (std::string_view(item.first) == "calendar" && context && context->year && context->month) {
          std::ostringstream date; date << '/' << std::setfill('0') << std::setw(4) << context->year << '/' << std::setw(2) << context->month;
          url += date.str();
        }
        const bool active = context && (context->section == item.first ||
            (std::string_view(item.first) == "tree" && (context->section == "blob" || context->section == "source")) ||
            (std::string_view(item.first) == "calendar" && context->section == "day"));
        out += "<a href=\"" + htmlEscape(url) + "\"" + (active ? " aria-current=\"page\"" : "") + ">" + item.second + "</a>";
      }
    }
    out += "</nav>";
  }
  out += "<button type=\"button\" class=\"about-trigger\" popovertarget=\"about-dialog\" popovertargetaction=\"show\" aria-haspopup=\"dialog\">About</button></header>";
  return out + aboutDialog() + "<main id=\"main-content\">" + std::string(body) + "</main><footer>Read-only Git dashboard · All dates in UTC</footer></body></html>";
}
}  // namespace ckgit
