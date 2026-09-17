// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_store.hpp"
#include "ckgit/http_router.hpp"
#include "ckgit/project_summary.hpp"
#include "ckgit/web_renderer.hpp"

#include <cstdint>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("ci web: " + message);
}

void testRoutes() {
  const auto list = ckgit::parseHttpRoute("/project/demo/ci");
  require(list.kind == ckgit::RouteKind::kCiRuns && list.project == "demo", "the CI list route parses");
  const auto run = ckgit::parseHttpRoute("/project/demo/ci/00000000000000000001-abcdabcd");
  require(run.kind == ckgit::RouteKind::kCiRun && run.project == "demo" &&
              run.run_id == "00000000000000000001-abcdabcd",
          "the live run status route parses");
  const auto cancel = ckgit::parseHttpRoute("/project/demo/ci/00000000000000000001-abcdabcd/cancel");
  require(cancel.kind == ckgit::RouteKind::kCiCancel && cancel.project == "demo" &&
              cancel.run_id == "00000000000000000001-abcdabcd",
          "the cancel route parses");
  require(ckgit::parseHttpRoute("/project/demo/ci/00000000000000000001-abcdabcd/cancel/x").kind ==
              ckgit::RouteKind::kNotFound,
          "a route past cancel is rejected");

  const auto log = ckgit::parseHttpRoute("/project/demo/ci/00000000000000000001-abcdabcd/2.log");
  require(log.kind == ckgit::RouteKind::kCiLog && log.project == "demo" &&
              log.run_id == "00000000000000000001-abcdabcd" && log.step == 2,
          "the CI log route parses run id and step");

  require(ckgit::parseHttpRoute("/project/demo/ci/00000000000000000001-abcdabcd/2.txt").kind ==
              ckgit::RouteKind::kNotFound,
          "a non-.log tail is rejected");
  require(ckgit::parseHttpRoute("/project/demo/ci/../secret/0.log").kind == ckgit::RouteKind::kNotFound,
          "a traversal run id is rejected");
  require(ckgit::parseHttpRoute("/project/demo/ci/run/x.log").kind == ckgit::RouteKind::kNotFound,
          "a non-numeric step is rejected");

  const auto artifact = ckgit::parseHttpRoute("/project/demo/ci/00000000000000000001-abcdabcd/artifacts/bundle");
  require(artifact.kind == ckgit::RouteKind::kCiArtifact && artifact.project == "demo" &&
              artifact.run_id == "00000000000000000001-abcdabcd" && artifact.path == "bundle",
          "the CI artifact route parses the run id and name");
  require(ckgit::parseHttpRoute("/project/demo/ci/00000000000000000001-abcdabcd/artifacts/../x").kind ==
              ckgit::RouteKind::kNotFound,
          "a slashed traversal artifact name is rejected");
  require(ckgit::parseHttpRoute("/project/demo/ci/00000000000000000001-abcdabcd/artifacts/").kind ==
              ckgit::RouteKind::kNotFound,
          "an empty artifact name is rejected");
}

void testRender() {
  ckgit::ProjectSummary empty;
  empty.name = "demo";
  require(ckgit::renderCiRuns(empty).find("No CI runs") != std::string::npos, "empty state renders");

  ckgit::ProjectSummary project;
  project.name = "demo";
  ckgit::CiRunRecord run;
  run.run_id = "00000000000000000001-abcdabcd";
  run.project_name = "demo";
  run.ref = "refs/heads/main";
  run.commit_id = std::string(40, 'a');
  run.status = ckgit::CiRunStatus::Failure;
  run.detail = "step 'tests' failed";
  run.steps.push_back({"build", 0, false, false});
  run.steps.push_back({"tests", 2, false, true});
  ckgit::CiArtifactRecord artifact;
  artifact.name = "bundle";
  artifact.bytes = 2048;
  artifact.expires_epoch_seconds = 2000;
  run.artifacts.push_back(artifact);
  project.ci_runs.push_back(run);
  const std::string html = ckgit::renderCiRuns(project);
  require(html.find("failure") != std::string::npos, "shows the run status");
  require(html.find("main") != std::string::npos, "shows the branch");
  require(html.find("/project/demo/ci/00000000000000000001-abcdabcd/1.log") != std::string::npos,
          "links each step to its log");
  require(html.find("step &#39;tests&#39; failed") != std::string::npos,
          "shows the failure detail, HTML-escaped");
  require(html.find("/project/demo/ci/00000000000000000001-abcdabcd/artifacts/bundle") != std::string::npos,
          "links the artifact bundle for download");
  require(html.find("bundle.tar") != std::string::npos, "shows the artifact filename");

  ckgit::ProjectSummary hostile;
  hostile.name = "demo";
  ckgit::CiRunRecord evil;
  evil.run_id = "00000000000000000002-abcdabcd";
  evil.project_name = "demo";
  evil.commit_id = std::string(40, 'b');
  evil.status = ckgit::CiRunStatus::Error;
  evil.detail = "<script>alert(1)</script>";
  evil.steps.push_back({"<img src=x>", 1, false, false});
  hostile.ci_runs.push_back(evil);
  const std::string escaped = ckgit::renderCiRuns(hostile);
  require(escaped.find("<script>") == std::string::npos && escaped.find("<img src=x>") == std::string::npos,
          "run detail and step names are HTML-escaped");
}

void testReleaseRoutes() {
  require(ckgit::parseHttpRoute("/project/demo/releases").kind == ckgit::RouteKind::kReleases,
          "the releases list route parses");
  const auto asset = ckgit::parseHttpRoute("/project/demo/releases/v1.0.0/app");
  require(asset.kind == ckgit::RouteKind::kReleaseAsset && asset.project == "demo" &&
              asset.run_id == "v1.0.0" && asset.path == "app",
          "the release asset route parses the tag and name");
  require(ckgit::parseHttpRoute("/project/demo/releases/../secret/app").kind == ckgit::RouteKind::kNotFound,
          "a traversal tag is rejected");
  require(ckgit::parseHttpRoute("/project/demo/releases/v1/a/b").kind == ckgit::RouteKind::kNotFound,
          "a slashed asset name is rejected");
}

void testReleaseRender() {
  ckgit::ProjectSummary empty;
  empty.name = "demo";
  require(ckgit::renderReleases(empty).find("No releases") != std::string::npos, "empty state renders");

  ckgit::ProjectSummary project;
  project.name = "demo";
  ckgit::CiReleaseRecord release;
  release.tag = "v1.0.0";
  release.commit_id = std::string(40, 'a');
  release.created_epoch_seconds = 1700000000;
  release.notes = "<b>notes</b>";
  ckgit::CiArtifactRecord asset;
  asset.name = "app";
  asset.bytes = 2048;
  asset.sha256 = std::string(64, 'a');
  release.assets.push_back(asset);
  project.releases.push_back(release);
  const std::string html = ckgit::renderReleases(project);
  require(html.find("v1.0.0") != std::string::npos, "shows the tag");
  require(html.find("/project/demo/releases/v1.0.0/app") != std::string::npos, "links the asset download");
  require(html.find("app.tar") != std::string::npos, "shows the asset filename");
  require(html.find("<b>notes</b>") == std::string::npos && html.find("&lt;b&gt;notes&lt;/b&gt;") != std::string::npos,
          "release notes are HTML-escaped");
}

}  // namespace

void testRunDetailAndDisplay() {
  const std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr));

  ckgit::CiRunRecord done;
  done.run_id = "00000000000000000001-abcdabcd";
  done.project_name = "demo";
  done.ref = "refs/heads/main";
  done.commit_id = std::string(40, 'a');
  done.status = ckgit::CiRunStatus::Success;
  done.started_epoch_seconds = 1700000000;
  done.finished_epoch_seconds = 1700000000 + 200;  // 3:20
  const auto done_display = ckgit::ciRunDisplay(done);
  require(!done_display.active, "a finished run is not active");
  require(ckgit::ciRunTiming(done_display) == "3:20 min", "finished timing renders as M:SS min");

  ckgit::CiRunRecord live;
  live.run_id = "00000000000000000002-abcdabcd";
  live.project_name = "demo";
  live.ref = "refs/heads/main";
  live.commit_id = std::string(40, 'b');
  live.status = ckgit::CiRunStatus::Running;
  live.started_epoch_seconds = now - 5;
  live.heartbeat_epoch_seconds = now;
  const auto live_display = ckgit::ciRunDisplay(live);
  require(live_display.active && live_display.name == "running", "a fresh running run is active");

  ckgit::CiRunRecord stale = live;
  stale.started_epoch_seconds = now - (ckgit::kCiRunStaleSeconds + 60);
  stale.heartbeat_epoch_seconds = now - (ckgit::kCiRunStaleSeconds + 30);
  const auto stale_display = ckgit::ciRunDisplay(stale);
  require(!stale_display.active && stale_display.name == "interrupted",
          "a running run with a dead heartbeat is interrupted, not live");

  require(ckgit::ciAnyActiveRun({done, live}), "ciAnyActiveRun sees a live run");
  require(!ckgit::ciAnyActiveRun({done, stale}), "ciAnyActiveRun ignores interrupted runs");

  ckgit::ProjectSummary project;
  project.name = "demo";
  const std::string detail =
      ckgit::renderCiRunDetail(project, live, std::optional<std::string>("integ-live-output"), 0);
  require(detail.find("/project/demo/ci/00000000000000000002-abcdabcd/cancel") != std::string::npos,
          "the live run page posts to the cancel endpoint");
  require(detail.find("method=\"post\"") != std::string::npos, "cancel is a POST form");
  require(detail.find("ckgit-admin ci cancel demo 00000000000000000002-abcdabcd") != std::string::npos,
          "the live run page shows the CLI cancel command");
  require(detail.find("integ-live-output") != std::string::npos,
          "the live run page shows the running step's output");

  const std::string done_detail = ckgit::renderCiRunDetail(project, done, std::nullopt, 0);
  require(done_detail.find("/cancel") == std::string::npos, "a finished run offers no cancel");
  require(done_detail.find("3:20 min") != std::string::npos, "a finished run shows its total duration");
}

void testCiWeb() {
  testRoutes();
  testRender();
  testRunDetailAndDisplay();
  testReleaseRoutes();
  testReleaseRender();
}
