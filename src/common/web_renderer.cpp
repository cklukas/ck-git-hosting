// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/web_renderer.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include "ckgit/http_router.hpp"
#include "ckgit/server_config.hpp"
#include "ckgit/server_identity.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/web_repository.hpp"

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

std::string renderLastCommit(const ckgit::ProjectSummary& project) {
  if (project.indexing) return "<span class=\"muted\">Indexing…</span>";
  if (!project.index_error.empty()) return "<span class=\"warn\">Index unavailable</span>";
  const auto epoch = project.last_commit ? project.last_commit->epoch_seconds : project.last_commit_epoch_seconds;
  if (!epoch) return "<span class=\"muted\">none</span>";
  std::string out = ckgit::htmlEscape(formatTimestamp(epoch));
  if (project.last_commit) {
    const auto& commit = *project.last_commit;
    out += " · <a href=\"/project/" + ckgit::htmlEscape(project.name) + "/commit/" +
        ckgit::htmlEscape(commit.id) + "\"><code class=\"object-id\">" + ckgit::htmlEscape(commit.id.substr(0, 8)) +
        "</code> · " + ckgit::htmlEscape(commit.subject) + "</a>";
  }
  return out + " <span class=\"muted\" title=\"" + ckgit::htmlEscape(formatTimestamp(epoch)) + "\">(" + ckgit::relativeTime(epoch) + ")</span>";
}

std::string renderCheckoutSummary(const ckgit::ProjectSummary& project) {
  if (project.checkouts.empty()) {
    return "<span class=\"muted\">none</span>";
  }
  const auto& first = project.checkouts.front();
  std::string rendered = renderCheckout(first, false);
  if (first.last_seen_epoch_seconds) {
    const auto timestamp = formatTimestamp(first.last_seen_epoch_seconds);
    rendered += " <span class=\"muted\" title=\"reported " + timestamp + "\">(" +
        ckgit::relativeTime(first.last_seen_epoch_seconds) + ")</span>";
  }
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

std::string renderCloneInfo(const ckgit::ProjectSummary& project) {
  if (!ckgit::isValidProjectName(project.name)) return {};
  std::string out = "<section class=\"clone-info\"><h2>Clone this project</h2>"
      "<p class=\"muted\">With ckgit, using your configured server:</p><pre><code>ckgit clone " +
      std::string(project.name.front() == '-' ? "-- " : "") +
      ckgit::htmlEscape(project.name) + "</code></pre>";
  if (ckgit::isValidSshCloneTarget(project.ssh_clone_target)) {
    const auto url = ckgit::hostedRepositoryUrl(project.ssh_clone_target, project.name);
    out += "<p class=\"muted\">With Git, using your existing SSH access:</p><pre><code>git clone " +
        ckgit::htmlEscape(url) + "</code></pre>";
  } else {
    out += "<p class=\"muted\">The Git clone address is not configured. The server administrator can set "
        "<code>ssh_clone_target=user@host</code> in <code>/etc/ck-git-hosting/server.ini</code> and restart the service.</p>";
  }
  return out + "</section>";
}

std::string renderLastCi(const ckgit::ProjectSummary& project) {
  if (!project.last_ci_run.has_value()) return "<span class=\"muted\">none</span>";
  const auto& run = *project.last_ci_run;
  const ckgit::CiRunDisplay display = ckgit::ciRunDisplay(run);
  const std::string timing = ckgit::ciRunTiming(display);
  std::string cell = "<a class=\"ci-status ci-" + std::string(display.name) + "\" href=\"/project/" +
      ckgit::htmlEscape(project.name) + "/ci/" + ckgit::htmlEscape(run.run_id) +
      "\"><span class=\"ci-icon\" aria-hidden=\"true\">" + std::string(display.icon) + "</span> " +
      std::string(display.name) + "</a>";
  if (!timing.empty()) cell += " <span class=\"muted\">(" + ckgit::htmlEscape(timing) + ")</span>";
  return cell;
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

std::string renderProjectTable(const std::vector<ProjectSummary>& projects, bool sort_by_name) {
  std::string html = "<h1>Projects</h1><p class=\"muted\">Sort <a href=\"/\"" +
      std::string(sort_by_name ? "" : " aria-current=\"true\"") + ">by last commit</a> · <a href=\"/by-name\"" +
      std::string(sort_by_name ? " aria-current=\"true\"" : "") + ">by name</a></p>"
      "<div class=\"project-table\"><table><thead><tr><th>Project</th><th>Default branch</th><th>Branches</th><th>Tags</th>"
      "<th title=\"Latest commit across every published branch and tag\">Last commit</th>"
      "<th title=\"Newest CI run: status and how long it took, or has been running\">Last CI</th>"
      "<th>Last reported checkout</th></tr></thead><tbody>";
  if (projects.empty()) html += "<tr><td colspan=\"7\">No projects</td></tr>";
  bool any_live = false;
  for (const auto& project : projects) {
    if (project.last_ci_run.has_value() && ciRunDisplay(*project.last_ci_run).active) any_live = true;
    html += "<tr><td class=\"project-name\"><a href=\"/project/" + htmlEscape(project.name) + "\">" +
            htmlEscape(project.name) + "</a></td><td class=\"project-default " +
            (project.valid_head ? "ok\">" : "warn\">") +
            htmlEscape(project.default_branch.empty() ? "missing" : project.default_branch) + "</td><td class=\"project-branches\">" +
            "<a href=\"/project/" + htmlEscape(project.name) + "#branches\">" + std::to_string(project.branch_count) + "</a>" +
            "</td><td class=\"project-tags\"><a href=\"/project/" + htmlEscape(project.name) + "#tags\">" + std::to_string(project.tag_count) + "</a>" +
            "</td><td class=\"project-last-commit\">" +
            renderLastCommit(project) + "</td><td class=\"project-ci\">" + renderLastCi(project) +
            "</td><td class=\"project-checkout\">" + renderCheckoutSummary(project) + "</td></tr>";
    if (html.size() > kMaximumPageBytes) throw WebError(503, "Project table exceeds the output limit.");
  }
  // Keep the index page live while any project has a run in progress.
  return pageLayout("Projects", html + "</tbody></table></div>", nullptr, nullptr, any_live ? 5u : 0u);
}

std::string renderProjectDetail(const ProjectSummary& project, const PageContext* requested_context) {
  const PageContext default_context{project.valid_head ? "heads/" + project.default_branch : "", project.head_id, "overview", "", 0, 0};
  const auto& context = requested_context ? *requested_context : default_context;
  std::string body = "<h1>" + htmlEscape(project.name) + "</h1>";
  if (project.branch_count || project.tag_count || !context.commit_id.empty())
    body += refPicker(project, context.ref, "overview");
  if (isObjectId(context.commit_id))
    body += "<p><a href=\"" + htmlEscape(sourceUrl(project.name, "overview", context.commit_id)) + "\">Permalink</a>" +
        (project.valid_head && context.ref != "heads/" + project.default_branch ?
            " · <a href=\"" + htmlEscape(sourceUrl(project.name, "overview", "heads/" + project.default_branch)) + "\">Latest default branch</a>" : "") + "</p>";
  if (project.indexing) body += "<p class=\"notice\">Indexing this project. Reload shortly.</p>";
  if (!project.index_error.empty()) body += "<p class=\"notice\">Index temporarily unavailable. " + htmlEscape(project.index_error) + " Reload to retry.</p>";
  if (!project.indexing && project.index_error.empty() && project.branch_count == 0 && project.tag_count == 0 && !project.valid_head) {
    body += "<section class=\"notice\"><h2>No commits published yet</h2><p>This project exists on the server and is ready for its first commit.</p>"
        "<p>Clone it below, commit your files, then run <code>ckgit publish --yes</code> "
        "from its connected checkout.</p></section>" + renderCloneInfo(project) +
        "<h2>Checkouts</h2>" + renderCheckoutList(project) + "<h2>Recent events</h2>" + renderEventList(project);
    return pageLayout(project.name, body, &project, &context);
  }
  if (!project.valid_head && !project.indexing && project.index_error.empty())
    body += "<p class=\"notice\">The default branch is unavailable. Choose an existing branch or tag above to browse its files and history.</p>";
  body += "<dl><dt>Default branch</dt><dd class=\"" + std::string(project.valid_head ? "ok\">" : "warn\">") +
      htmlEscape(project.default_branch.empty() ? "missing" : project.default_branch) +
      "</dd><dt>Branches</dt><dd>" + std::to_string(project.branch_count) + "</dd><dt>Tags</dt><dd>" +
      std::to_string(project.tag_count) + "</dd><dt>" + (requested_context ? std::string("Selected commit") : std::string("Last commit across published refs")) + "</dt><dd>" + renderLastCommit(project) +
      "</dd><dt>Author</dt><dd>" + htmlEscape(project.last_commit ? project.last_commit->author : "none") +
      "</dd><dt>Size</dt><dd>" + formatBytes(project.size_bytes) + " in " + std::to_string(project.object_count) +
      " objects</dd><dt>Indexed</dt><dd><span title=\"" + htmlEscape(formatUtcTimestamp(project.generated_epoch_seconds)) +
      "\">" + htmlEscape(relativeTime(project.generated_epoch_seconds)) + "</span></dd></dl>";
  body += renderCloneInfo(project);
  if (!project.readme_path.empty() && !project.readme_truncated)
    body += renderReadme(project, context.commit_id, project.readme_path, project.readme_content, context.ref);
  else body += "<p class=\"muted\">" + std::string(project.readme_truncated ? "README exceeds the 512 KiB preview limit." : "No README at this revision.") + "</p>";
  body += "<div class=\"columns\">";
  for (bool tags : {false, true}) {
    body += tags ? "<section id=\"tags\"><h2>Tags</h2><ul>" : "<section id=\"branches\"><h2>Branches</h2><ul>";
    bool found = false;
    for (const auto& ref : project.refs) if (ref.is_tag == tags) {
      found = true;
      body += "<li><a href=\"" + htmlEscape(sourceUrl(project.name, "tree", (tags ? "tags/" : "heads/") + ref.name)) + "\">" + htmlEscape(ref.name) +
          "</a> <span class=\"muted\">" + formatUtcTimestamp(ref.epoch_seconds) + "</span></li>";
    }
    if (!found) body += "<li class=\"muted\">none</li>";
    body += "</ul></section>";
  }
  body += "</div>";
  if (project.refs_truncated) body += "<p class=\"notice\">Showing the newest 500 refs.</p>";
  body += "<div class=\"columns\"><section><h2>Checkouts</h2>" + renderCheckoutList(project) +
      "</section><section><h2>Recent events</h2>" + renderEventList(project) + "</section></div>";
  return pageLayout(project.name + (requested_context ? " · " + refLabel(context.ref) : ""), body, &project, &context);
}

}  // namespace ckgit
