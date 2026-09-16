// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <string>

#include "ckgit/ci_store.hpp"
#include "ckgit/web_renderer.hpp"

namespace ckgit {
namespace {

std::string branchLabel(const std::string& ref) {
  if (ref.rfind("refs/heads/", 0) == 0) return ref.substr(11);
  if (ref.rfind("refs/tags/", 0) == 0) return ref.substr(10);
  return ref;
}

std::string shortId(const std::string& id) {
  return id.substr(0, std::min<std::size_t>(id.size(), 12));
}

std::string statusBadge(CiRunStatus status) {
  const std::string name(ciRunStatusName(status));
  return "<span class=\"ci-status ci-" + name + "\">" + name + "</span>";
}

std::string duration(const CiRunRecord& run) {
  if (run.finished_epoch_seconds < run.started_epoch_seconds || run.started_epoch_seconds == 0) return {};
  const std::uint64_t seconds = run.finished_epoch_seconds - run.started_epoch_seconds;
  return " · " + std::to_string(seconds) + "s";
}

}  // namespace

std::string renderCiRuns(const ProjectSummary& project) {
  std::string out = "<h1>Continuous integration</h1>";
  if (project.ci_runs.empty()) {
    return out +
           "<p class=\"empty\">No CI runs recorded yet. A push to a CI-enabled branch queues a run "
           "of the repository's <code>.ckgit/ci.yml</code>.</p>";
  }
  out += "<table class=\"ci-runs\"><thead><tr><th>Status</th><th>Ref</th><th>Commit</th><th>When</th>"
         "<th>Steps</th></tr></thead><tbody>";
  for (const CiRunRecord& run : project.ci_runs) {
    out += "<tr><td>" + statusBadge(run.status) + "</td>";
    out += "<td>" + htmlEscape(branchLabel(run.ref)) + "</td>";
    out += "<td><code>" + htmlEscape(shortId(run.commit_id)) + "</code></td>";
    const std::string when = run.started_epoch_seconds
        ? "<span title=\"" + htmlEscape(formatUtcTimestamp(run.started_epoch_seconds)) + "\">" +
              htmlEscape(relativeTime(run.started_epoch_seconds)) + "</span>" + htmlEscape(duration(run))
        : std::string("&mdash;");
    out += "<td>" + when + "</td><td>";
    for (std::size_t index = 0; index < run.steps.size(); ++index) {
      const CiStepResult& step = run.steps[index];
      const std::string label = step.name.empty() ? ("step " + std::to_string(index)) : step.name;
      const std::string url = "/project/" + project.name + "/ci/" + run.run_id + "/" +
                              std::to_string(index) + ".log";
      const std::string state = step.timed_out ? "timeout" : ("exit " + std::to_string(step.exit_code));
      out += "<a href=\"" + htmlEscape(url) + "\">" + htmlEscape(label) + "</a> (" + htmlEscape(state) +
             (step.output_truncated ? ", log truncated" : "") + ") ";
    }
    out += "</td></tr>";
    if (!run.detail.empty()) {
      out += "<tr class=\"ci-detail\"><td colspan=\"5\">" + htmlEscape(run.detail) + "</td></tr>";
    }
    if (!run.artifacts.empty()) {
      out += "<tr class=\"ci-artifacts\"><td colspan=\"5\"><span class=\"muted\">Artifacts:</span> ";
      for (const CiArtifactRecord& artifact : run.artifacts) {
        if (!artifact.note.empty()) {
          out += htmlEscape(artifact.name) + " (" + htmlEscape(artifact.note) + ") ";
          continue;
        }
        const std::string url =
            "/project/" + project.name + "/ci/" + run.run_id + "/artifacts/" + artifact.name;
        out += "<a href=\"" + htmlEscape(url) + "\">" + htmlEscape(artifact.name) + ".tar</a> (" +
               htmlEscape(formatBytes(artifact.bytes));
        if (artifact.expires_epoch_seconds == 0) {
          out += ", kept";
        } else {
          out += ", expires " + htmlEscape(formatUtcTimestamp(artifact.expires_epoch_seconds));
        }
        out += ") ";
      }
      out += "</td></tr>";
    }
  }
  out += "</tbody></table>";
  return out;
}

std::string renderReleases(const ProjectSummary& project) {
  std::string out = "<h1>Releases</h1>";
  if (project.releases.empty()) {
    return out +
           "<p class=\"empty\">No releases yet. Push a tag whose build declares an "
           "<code>artifacts:</code> bundle to publish one; it is kept until the tag is deleted.</p>";
  }
  for (const CiReleaseRecord& release : project.releases) {
    out += "<section class=\"release\"><h2>" + htmlEscape(release.tag) + "</h2>";
    out += "<p class=\"muted\"><code>" + htmlEscape(shortId(release.commit_id)) + "</code>";
    if (release.created_epoch_seconds != 0) {
      out += " · <span title=\"" + htmlEscape(formatUtcTimestamp(release.created_epoch_seconds)) + "\">" +
             htmlEscape(relativeTime(release.created_epoch_seconds)) + "</span>";
    }
    out += "</p>";
    if (!release.notes.empty()) {
      out += "<pre class=\"release-notes\">" + escapePre(release.notes) + "</pre>";
    }
    if (release.assets.empty()) {
      out += "<p class=\"empty\">No assets.</p>";
    } else {
      out += "<ul class=\"release-assets\">";
      for (const CiArtifactRecord& asset : release.assets) {
        if (!asset.note.empty()) {
          out += "<li>" + htmlEscape(asset.name) + " (" + htmlEscape(asset.note) + ")</li>";
          continue;
        }
        const std::string url =
            "/project/" + project.name + "/releases/" + release.tag + "/" + asset.name;
        out += "<li><a href=\"" + htmlEscape(url) + "\">" + htmlEscape(asset.name) + ".tar</a> (" +
               htmlEscape(formatBytes(asset.bytes)) + ")";
        if (!asset.sha256.empty()) {
          out += " <code class=\"muted\">" + htmlEscape(asset.sha256.substr(0, 12)) + "</code>";
        }
        out += "</li>";
      }
      out += "</ul>";
    }
    out += "</section>";
  }
  return out;
}

}  // namespace ckgit
