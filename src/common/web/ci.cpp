// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <ctime>
#include <optional>
#include <string>

#include "ckgit/ci_store.hpp"
#include "ckgit/text.hpp"
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

// Icon + name badge, linking to the run's live status page so it is an obvious
// place to open progress and (while active) cancel.
std::string statusBadge(const std::string& project, const CiRunRecord& run, const CiRunDisplay& display) {
  return "<a class=\"ci-status ci-" + std::string(display.name) + "\" href=\"/project/" + htmlEscape(project) +
         "/ci/" + htmlEscape(run.run_id) + "\"><span class=\"ci-icon\" aria-hidden=\"true\">" +
         std::string(display.icon) + "</span> " + std::string(display.name) + "</a>";
}

// The completed step's own glyph, from its exit disposition.
std::string_view stepIcon(const CiStepResult& step) {
  if (step.timed_out) return ciRunStatusIcon(CiRunStatus::Timeout);
  return step.exit_code == 0 ? ciRunStatusIcon(CiRunStatus::Success) : ciRunStatusIcon(CiRunStatus::Failure);
}

}  // namespace

CiRunDisplay ciRunDisplay(const CiRunRecord& run) {
  const std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr));
  CiRunDisplay display;
  display.icon = ciRunStatusIcon(run.status);
  display.name = ciRunStatusName(run.status);
  display.seconds = 0;
  display.active = false;
  if (run.status == CiRunStatus::Running) {
    const std::uint64_t beat =
        run.heartbeat_epoch_seconds ? run.heartbeat_epoch_seconds : run.started_epoch_seconds;
    if (beat != 0 && now > beat + kCiRunStaleSeconds) {
      // The runner stopped reporting: show it as interrupted, not a live clock.
      display.icon = "\xf0\x9f\x92\xa4";  // 💤
      display.name = "interrupted";
      display.seconds = beat > run.started_epoch_seconds ? beat - run.started_epoch_seconds : 0;
    } else {
      display.name = "running";
      display.seconds =
          run.started_epoch_seconds && now >= run.started_epoch_seconds ? now - run.started_epoch_seconds : 0;
      display.active = true;
    }
    return display;
  }
  if (run.status == CiRunStatus::Pending) {
    display.active = true;  // queued: keep the view live until it starts
    return display;
  }
  if (run.finished_epoch_seconds >= run.started_epoch_seconds && run.started_epoch_seconds != 0) {
    display.seconds = run.finished_epoch_seconds - run.started_epoch_seconds;
  }
  return display;
}

bool ciAnyActiveRun(const std::vector<CiRunRecord>& runs) {
  return std::any_of(runs.begin(), runs.end(), [](const CiRunRecord& run) { return ciRunDisplay(run).active; });
}

std::string ciRunTiming(const CiRunDisplay& display) {
  if (display.name == "pending") return {};
  if (display.seconds == 0 && !display.active) return {};
  return formatDuration(display.seconds) + (display.seconds < 3600 ? " min" : "");
}

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
    const CiRunDisplay display = ciRunDisplay(run);
    out += "<tr><td>" + statusBadge(project.name, run, display) + "</td>";
    out += "<td>" + htmlEscape(branchLabel(run.ref)) + "</td>";
    out += "<td><code>" + htmlEscape(shortId(run.commit_id)) + "</code></td>";
    const std::string timing = ciRunTiming(display);
    const std::string when = run.started_epoch_seconds
        ? "<span title=\"" + htmlEscape(formatUtcTimestamp(run.started_epoch_seconds)) + "\">" +
              htmlEscape(relativeTime(run.started_epoch_seconds)) + "</span>" +
              (timing.empty() ? std::string() : " · " + htmlEscape(timing))
        : std::string("&mdash;");
    out += "<td>" + when + "</td><td>";
    for (std::size_t index = 0; index < run.steps.size(); ++index) {
      if (index > 0) out += " \xe2\x86\x92 ";  // → between steps
      const CiStepResult& step = run.steps[index];
      const std::string label = step.name.empty() ? ("step " + std::to_string(index)) : step.name;
      const std::string url = "/project/" + project.name + "/ci/" + run.run_id + "/" +
                              std::to_string(index) + ".log";
      const std::string state = step.timed_out ? "timeout" : ("exit " + std::to_string(step.exit_code));
      out += "<a href=\"" + htmlEscape(url) + "\">" + htmlEscape(label) + "</a> (" + htmlEscape(state) +
             (step.output_truncated ? ", log truncated" : "") + ")";
    }
    out += "</td></tr>";
    // The per-run detail line is intentionally omitted here: the status and
    // step columns already convey it for runs that ran steps, and the reason a
    // step-less run was skipped or errored is shown on the run's own page.
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

std::string renderCiRunDetail(const ProjectSummary& project, const CiRunRecord& run,
                              const std::optional<std::string>& live_log, std::size_t live_step) {
  const CiRunDisplay display = ciRunDisplay(run);
  const std::string project_url = "/project/" + htmlEscape(project.name);
  std::string out = "<p><a href=\"" + project_url + "/ci\">\xe2\x86\x90 Back to CI</a></p>";
  out += "<h1 class=\"ci-run-head\"><span class=\"ci-icon-lg\" aria-hidden=\"true\">" + std::string(display.icon) +
         "</span> <span class=\"ci-status ci-" + std::string(display.name) + "\">" + std::string(display.name) +
         "</span></h1>";

  out += "<dl class=\"ci-run-meta\"><dt>Ref</dt><dd>" + htmlEscape(branchLabel(run.ref)) + "</dd>";
  out += "<dt>Commit</dt><dd><a href=\"" + project_url + "/commit/" + htmlEscape(run.commit_id) + "\"><code>" +
         htmlEscape(shortId(run.commit_id)) + "</code></a></dd>";
  if (run.started_epoch_seconds) {
    out += "<dt>Started</dt><dd><span title=\"" + htmlEscape(formatUtcTimestamp(run.started_epoch_seconds)) +
           "\">" + htmlEscape(relativeTime(run.started_epoch_seconds)) + "</span></dd>";
  }
  const std::string timing = ciRunTiming(display);
  if (!timing.empty()) {
    out += "<dt>" + std::string(display.active ? "Elapsed" : "Duration") + "</dt><dd>" + htmlEscape(timing) + "</dd>";
  }
  if (!run.detail.empty()) out += "<dt>Detail</dt><dd>" + htmlEscape(run.detail) + "</dd>";
  out += "</dl>";

  // Cancel affordances, mirrored across channels: a loopback POST button and the
  // equivalent CLI command (which drives the same cancel marker via the socket).
  if (display.active) {
    out += "<div class=\"ci-cancel\"><form method=\"post\" action=\"" + project_url + "/ci/" +
           htmlEscape(run.run_id) + "/cancel\"><button type=\"submit\" class=\"ci-cancel-button\">\xe2\x9b\x94 "
           "Cancel run</button></form>";
    out += "<details class=\"ci-cancel-cli\"><summary>Cancel from the command line</summary><pre><code>"
           "ckgit-admin ci cancel " + htmlEscape(project.name) + " " + htmlEscape(run.run_id) +
           "</code></pre></details></div>";
  } else if (display.name == "interrupted") {
    out += "<p class=\"notice\">This run stopped reporting progress; its runner may have been interrupted. "
           "The recorded status settles at the next runner sweep.</p>";
  }

  out += "<h2>Steps</h2><ol class=\"ci-steps\">";
  for (std::size_t index = 0; index < run.steps.size(); ++index) {
    const CiStepResult& step = run.steps[index];
    const std::string label = step.name.empty() ? ("step " + std::to_string(index)) : step.name;
    const std::string url = project_url + "/ci/" + htmlEscape(run.run_id) + "/" + std::to_string(index) + ".log";
    const std::string state = step.timed_out ? "timeout" : ("exit " + std::to_string(step.exit_code));
    out += "<li><span class=\"ci-icon\" aria-hidden=\"true\">" + std::string(stepIcon(step)) + "</span> <a href=\"" +
           url + "\">" + htmlEscape(label) + "</a> <span class=\"muted\">(" + htmlEscape(state) +
           (step.output_truncated ? ", log truncated" : "") + ")</span></li>";
  }
  const bool show_live = display.active && live_log.has_value();
  if (show_live) {
    out += "<li class=\"ci-step-running\"><span class=\"ci-icon\" aria-hidden=\"true\">" +
           std::string(ciRunStatusIcon(CiRunStatus::Running)) + "</span> <a href=\"" + project_url + "/ci/" +
           htmlEscape(run.run_id) + "/" + std::to_string(live_step) + ".log\">step " + std::to_string(live_step) +
           "</a> <span class=\"muted\">(running\xe2\x80\xa6)</span></li>";
  }
  out += "</ol>";

  if (show_live) {
    out += "<h2>Live output <span class=\"muted\">\xc2\xb7 step " + std::to_string(live_step) + "</span></h2>";
    constexpr std::size_t kTailBytes = 16 * 1024;
    std::string_view view(*live_log);
    const bool trimmed = view.size() > kTailBytes;
    if (trimmed) view = view.substr(view.size() - kTailBytes);
    out += "<pre class=\"ci-log ci-log-live\">";
    if (trimmed) {
      out += "<span class=\"muted\">\xe2\x80\xa6 showing the last 16 KiB; open the step log for the full "
             "output\n</span>";
    }
    out += escapePre(view);
    out += "</pre><p class=\"muted\">This page refreshes automatically while the run is active.</p>";
  }

  if (!run.artifacts.empty()) {
    out += "<h2>Artifacts</h2><ul class=\"ci-artifacts\">";
    for (const CiArtifactRecord& artifact : run.artifacts) {
      if (!artifact.note.empty()) {
        out += "<li>" + htmlEscape(artifact.name) + " <span class=\"muted\">(" + htmlEscape(artifact.note) +
               ")</span></li>";
        continue;
      }
      out += "<li><a href=\"" + project_url + "/ci/" + htmlEscape(run.run_id) + "/artifacts/" +
             htmlEscape(artifact.name) + "\">" + htmlEscape(artifact.name) + ".tar</a> <span class=\"muted\">(" +
             htmlEscape(formatBytes(artifact.bytes)) + ")</span></li>";
    }
    out += "</ul>";
  }
  return out;
}

std::string renderCiLogView(const ProjectSummary& project, const std::string& run_id, int step,
                            const std::string& log, const std::string& nonce) {
  const std::string project_url = "/project/" + htmlEscape(project.name) + "/ci";
  // project.name, run_id and step are all validated tokens ([A-Za-z0-9._-] etc.),
  // so this URL carries no character that could escape the data attribute or the
  // script; the follow script reads it from the attribute and interpolates
  // nothing itself.
  const std::string stream_url =
      "/project/" + project.name + "/ci/" + run_id + "/" + std::to_string(step) + ".stream";

  std::string out = "<p><a href=\"" + project_url + "\">\xe2\x86\x90 Back to CI</a> \xc2\xb7 <a href=\"" +
      project_url + "/" + htmlEscape(run_id) + "\">run</a></p>";
  out += "<div class=\"ci-log-controls\"><button type=\"button\" id=\"ci-follow-btn\" data-stream=\"" +
      htmlEscape(stream_url) + "\">Follow live</button> <span id=\"ci-follow-note\" class=\"muted\"></span></div>";
  out += "<pre class=\"ci-log\" id=\"ci-log\">" + escapePre(log) + "</pre>";
  // A static, self-contained follow script: it reads the SSE endpoint from the
  // button's data attribute, so nothing is interpolated into the script body.
  out += "<script nonce=\"" + nonce + "\">";
  out += "(function(){"
         "var b=document.getElementById('ci-follow-btn'),"
         "n=document.getElementById('ci-follow-note'),"
         "x=document.getElementById('ci-log'),"
         "u=b.getAttribute('data-stream'),e=null;"
         "function stop(m){if(e){e.close();e=null;}b.textContent='Follow live';if(m)n.textContent=m;}"
         "function start(){b.textContent='Stop';n.textContent='Connecting\\u2026';e=new EventSource(u);"
         "e.onopen=function(){x.textContent='';n.textContent='Following live\\u2026';};"
         "e.onmessage=function(ev){x.textContent+=ev.data+'\\n';window.scrollTo(0,document.body.scrollHeight);};"
         "e.addEventListener('done',function(){stop('Run finished.');});"
         "e.onerror=function(){n.textContent='Reconnecting\\u2026';};}"
         "b.addEventListener('click',function(){e?stop(''):start();});"
         "})();";
  out += "</script>";
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
