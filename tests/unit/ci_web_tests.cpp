// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_store.hpp"
#include "ckgit/http_router.hpp"
#include "ckgit/project_summary.hpp"
#include "ckgit/web_renderer.hpp"

#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("ci web: " + message);
}

void testRoutes() {
  const auto list = ckgit::parseHttpRoute("/project/demo/ci");
  require(list.kind == ckgit::RouteKind::kCiRuns && list.project == "demo", "the CI list route parses");

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
  project.ci_runs.push_back(run);
  const std::string html = ckgit::renderCiRuns(project);
  require(html.find("failure") != std::string::npos, "shows the run status");
  require(html.find("main") != std::string::npos, "shows the branch");
  require(html.find("/project/demo/ci/00000000000000000001-abcdabcd/1.log") != std::string::npos,
          "links each step to its log");
  require(html.find("step &#39;tests&#39; failed") != std::string::npos,
          "shows the failure detail, HTML-escaped");

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

}  // namespace

void testCiWeb() {
  testRoutes();
  testRender();
}
