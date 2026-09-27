// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_control.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "ckgit/control_rpc.hpp"
#include "ckgit/text.hpp"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("ci control: " + message);
}

class StateFixture {
 public:
  StateFixture() {
    const char* configured_tmp = std::getenv("TMPDIR");
    require(configured_tmp && *configured_tmp, "TMPDIR must be explicitly selected");
    auto pattern = (std::filesystem::path(configured_tmp) / "ckgit-ci-control-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(mkdtemp(writable.data()) != nullptr, "could not create isolated fixture");
    root = writable.data();
    state = root / "state";
    std::filesystem::create_directories(state);
    require(chmod(state.c_str(), 0700) == 0, "could not secure fixture state");
  }
  ~StateFixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  StateFixture(const StateFixture&) = delete;
  StateFixture& operator=(const StateFixture&) = delete;
  std::filesystem::path root;
  std::filesystem::path state;
};

bool rejects(auto parse) {
  try {
    parse();
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

// A finished run with two steps, one of them named with non-ASCII and control
// text, a detail, and two artifacts (one stored, one only noted), written and
// read back through the real store.
ckgit::CiRunRecord storedRun(const std::filesystem::path& state) {
  ckgit::CiRunRecord run;
  run.run_id = "00000000001727000000-abcd1234";
  run.project_name = "demo";
  run.ref = "refs/heads/feature/\xc3\xa4nderung";
  run.commit_id = std::string(40, 'c');
  run.status = ckgit::CiRunStatus::Failure;
  run.started_epoch_seconds = 1727000000;
  run.finished_epoch_seconds = 1727000200;
  run.heartbeat_epoch_seconds = 1727000200;
  run.detail = "step 'tests' exited 2";
  run.steps = {{"build", 0, false, false}, {"tests\twith \x1b[31mcolour", 2, false, true}};
  ckgit::prepareCiRunDirectory(state, "demo", run.run_id);
  ckgit::writeCiRunRecord(state, run);
  const auto artifacts = ckgit::prepareCiArtifactDirectory(state, "demo", run.run_id);
  {
    std::ofstream blob(artifacts / "build.tar", std::ios::binary);
    blob << "payload";
  }
  require(chmod((artifacts / "build.tar").c_str(), 0600) == 0, "could not secure the fixture artifact");
  ckgit::writeCiArtifactRecord(state, "demo", run.run_id,
                               {"build", 7, std::string(64, 'e'), 1727000200, 1727604000, {}});
  ckgit::writeCiArtifactRecord(state, "demo", run.run_id, {"huge", 0, {}, 1727000200, 1727604000, "over the size cap"});
  const auto loaded = ckgit::loadCiRun(state, "demo", run.run_id);
  require(loaded.has_value() && loaded->artifacts.size() == 2, "the fixture run loads with its artifacts");
  return *loaded;
}

void testRunRoundTrip() {
  StateFixture fixture;
  const ckgit::CiRunRecord run = storedRun(fixture.state);
  const std::string wire = ckgit::formatCiRunControlResponse({1727000300, run});
  require(wire.rfind("ok 1727000300 2\nrun " + run.run_id + " failure ", 0) == 0,
          "the run response starts as documented");
  require(wire.find('\x1b') == std::string::npos && wire.find('\t') == std::string::npos,
          "free text travels hex-encoded");
  const auto parsed = ckgit::parseCiRunControlResponse(wire, "demo");
  require(parsed.server_epoch_seconds == 1727000300, "the server clock round trips");
  const auto& back = parsed.run;
  require(back.run_id == run.run_id && back.project_name == "demo" && back.ref == run.ref &&
              back.commit_id == run.commit_id && back.status == run.status &&
              back.started_epoch_seconds == run.started_epoch_seconds &&
              back.finished_epoch_seconds == run.finished_epoch_seconds &&
              back.heartbeat_epoch_seconds == run.heartbeat_epoch_seconds && back.detail == run.detail,
          "run header fields round trip");
  require(back.steps.size() == 2 && back.steps[1].name == run.steps[1].name && back.steps[1].exit_code == 2 &&
              back.steps[1].output_truncated && !back.steps[1].timed_out,
          "steps round trip, including control characters in a name");
  require(back.artifacts.size() == 2 && back.artifacts[0].name == "build" && back.artifacts[0].bytes == 7 &&
              back.artifacts[0].sha256 == std::string(64, 'e') &&
              back.artifacts[0].expires_epoch_seconds == 1727604000 &&
              back.artifacts[1].name == "huge" && back.artifacts[1].sha256.empty() &&
              back.artifacts[1].note == "over the size cap",
          "stored and noted artifacts round trip");

  const auto rejectsRun = [](const std::string& response) {
    return rejects([&] { ckgit::parseCiRunControlResponse(response, "demo"); });
  };
  const std::string commit(40, 'a');
  const std::string run_line = "run 1-a success 1 2 2 0 " + commit + " 72\n";
  const std::string header = "ok 5 0\n" + run_line + "detail -\n";
  require(ckgit::parseCiRunControlResponse(header, "demo").run.steps.empty(), "a minimal run parses");
  require(rejectsRun("ok 5 0\nrun 1-a success 1 2 2 1 " + commit + " 72\ndetail -\n"),
          "a missing step line is rejected");
  require(rejectsRun(header + "step 0 0 0 0 -\n"),
          "an undeclared step line is rejected");
  require(rejectsRun("ok 5 1\n" + run_line + "detail -\n"),
          "a missing artifact line is rejected");
  require(rejectsRun("ok 5 0\nrun 1-a success 1 2 2 1 " + commit + " 72\ndetail -\nstep 1 0 0 0 -\n"),
          "a step with the wrong index is rejected");
  require(rejectsRun("ok 5 0\nrun 1-a bogus 1 2 2 0 " + commit + " 72\ndetail -\n"),
          "an unknown status is rejected");
  require(rejectsRun("ok 5 0\nrun 1-a success 1 2 2 0 " + commit + " 7\ndetail -\n"),
          "odd-length hex text is rejected");
  require(rejectsRun("ok 5 0\nrun 1-a success 01 2 2 0 " + commit + " 72\ndetail -\n"),
          "a number with a leading zero is rejected");
  require(rejectsRun("ok 5 0\nrun ../x success 1 2 2 0 " + commit + " 72\ndetail -\n"),
          "an unsafe run id is rejected");
  require(rejectsRun(header + "artifact x 1 - 1 0 -\n"),
          "an artifact beyond the declared count is rejected");
  require(rejectsRun("ok 5 2\n" + run_line + "detail -\nartifact x 1 - 1 0 -\nartifact x 1 - 1 0 -\n"),
          "a repeated artifact name is rejected");
  require(rejectsRun(header.substr(0, header.size() - 1)),
          "a response without its final newline is rejected");
  require(rejectsRun("error norun no such CI run\n"),
          "an error reply is not a run");
}

void testStatusRoundTrip() {
  StateFixture fixture;
  const ckgit::CiRunRecord finished = storedRun(fixture.state);
  ckgit::CiRunRecord active = finished;
  active.run_id = "00000000001727000900-abcd1234";
  active.status = ckgit::CiRunStatus::Running;
  active.finished_epoch_seconds = 0;
  active.steps.resize(1);
  active.artifacts.clear();
  ckgit::CiStatusReport report;
  report.server_epoch_seconds = 1727001000;
  report.projects.push_back({"demo", true, {ckgit::summarizeCiRun(active), ckgit::summarizeCiRun(finished)}});
  report.projects.push_back({"quiet", false, {}});
  const std::string wire = ckgit::formatCiStatusControlResponse(report);
  require(wire.rfind("ok 1727001000 2\nproject demo enabled 2\nrun " + active.run_id + " running ", 0) == 0,
          "the status response starts as documented");
  const auto parsed = ckgit::parseCiStatusControlResponse(wire);
  require(parsed.server_epoch_seconds == 1727001000 && parsed.projects.size() == 2, "both projects round trip");
  const auto& demo = parsed.projects[0];
  require(demo.project_name == "demo" && demo.ci_enabled && demo.runs.size() == 2 &&
              demo.runs[0].run.run_id == active.run_id && demo.runs[0].completed_steps == 1 &&
              demo.runs[0].run.status == ckgit::CiRunStatus::Running && demo.runs[1].completed_steps == 2 &&
              demo.runs[1].run.ref == finished.ref && demo.runs[1].run.steps.empty() &&
              demo.runs[1].run.detail.empty(),
          "summaries carry header fields and step counts only");
  require(parsed.projects[1].project_name == "quiet" && !parsed.projects[1].ci_enabled &&
              parsed.projects[1].runs.empty(),
          "a project without runs round trips");

  const auto rejectsStatus = [](const std::string& response) {
    return rejects([&] { ckgit::parseCiStatusControlResponse(response); });
  };
  const std::string commit(40, 'a');
  const std::string run_a = "run 2-a success 1 2 2 0 " + commit + " 72\n";
  const std::string run_b = "run 1-a success 1 2 2 0 " + commit + " 72\n";
  const std::string ordered = "ok 9 1\nproject demo disabled 2\n" + run_a + run_b;
  require(ckgit::parseCiStatusControlResponse(ordered).projects[0].runs.size() == 2,
          "runs listed newest first parse");
  require(rejectsStatus("ok 9 1\nproject demo disabled 2\n" + run_b + run_a),
          "runs out of order are rejected");
  require(rejectsStatus("ok 9 1\nproject demo disabled 2\n" + run_a + run_a),
          "a repeated run is rejected");
  require(rejectsStatus("ok 9 2\nproject demo disabled 0\nproject demo disabled 0\n"),
          "a repeated project is rejected");
  require(rejectsStatus("ok 9 1\nproject demo maybe 0\n"),
          "an unknown opt-in state is rejected");
  require(rejectsStatus("ok 9 1\nproject ../demo enabled 0\n"),
          "an unsafe project name is rejected");
  require(rejectsStatus("ok 9 1\nproject demo enabled 65\n"),
          "a run count above the listing bound is rejected");
  require(rejectsStatus("ok 9 1\nproject demo enabled 1\n"),
          "a missing run line is rejected");
  require(rejectsStatus("ok 9 0\nproject demo enabled 0\n"),
          "a project beyond the declared count is rejected");
  require(rejectsStatus("ok 9 1\nproject demo enabled 1\n" + run_a.substr(0, run_a.size() - 4) + "\n"),
          "a run line with a missing field is rejected");
  require(rejectsStatus("ok 9 1\r\nproject demo enabled 0\n"),
          "a carriage return is rejected");
  require(rejectsStatus("ok 9 1\nproject demo enabled 0\n\n"),
          "an empty line is rejected");

  ckgit::CiStatusReport oversized;
  for (int index = 0; index < 1200; ++index) {
    oversized.projects.push_back({"project-" + std::to_string(index), true,
                                  {ckgit::summarizeCiRun(active), ckgit::summarizeCiRun(finished)}});
  }
  bool too_large = false;
  try {
    static_cast<void>(ckgit::formatCiStatusControlResponse(oversized));
  } catch (const std::length_error&) {
    too_large = true;
  }
  require(too_large, "a status response beyond the control limit is refused rather than truncated");
}

void testLogChunkRoundTrip() {
  const std::string bytes = std::string("line one\r\n\x1b[1mbold\x1b[0m\n") + std::string(3, '\0') + "tail";
  const std::string wire = ckgit::formatCiLogControlResponse({4096, bytes, false});
  require(wire == "ok more 4096 " + std::to_string(bytes.size()) + "\n" + ckgit::hexEncode(bytes) + "\n",
          "a log chunk is its header plus one hex line");
  const auto parsed = ckgit::parseCiLogControlResponse(wire, 4096);
  require(parsed.bytes == bytes && parsed.offset == 4096 && !parsed.complete, "raw log bytes round trip exactly");
  const auto end = ckgit::parseCiLogControlResponse(ckgit::formatCiLogControlResponse({77, {}, true}), 77);
  require(end.bytes.empty() && end.complete, "an empty final chunk is only its header");
  require(ckgit::formatCiLogControlResponse({0, std::string(ckgit::kMaximumControlLogChunkBytes, 'x'), true}).size() <=
              ckgit::kMaximumControlResponseBytes,
          "the largest chunk fits the control response bound");
  require(rejects([&] { ckgit::parseCiLogControlResponse(wire, 0); }), "a chunk for another offset is rejected");
  require(rejects([&] { ckgit::parseCiLogControlResponse("ok done 0 0\n", 0); }), "an unknown state is rejected");
  require(rejects([&] { ckgit::parseCiLogControlResponse("ok more 0 2\n61\n", 0); }), "a short chunk is rejected");
  require(rejects([&] { ckgit::parseCiLogControlResponse("ok more 0 1\n6162\n", 0); }), "a long chunk is rejected");
  require(rejects([&] { ckgit::parseCiLogControlResponse("ok more 0 0\n61\n", 0); }),
          "payload after an empty chunk is rejected");
  require(rejects([&] { ckgit::parseCiLogControlResponse("ok more 0 1\n6G\n", 0); }), "non-hex payload is rejected");
  require(rejects([&] {
            const std::string oversized = std::to_string(ckgit::kMaximumControlLogChunkBytes + 1);
            ckgit::parseCiLogControlResponse("ok more 0 " + oversized + "\n", 0);
          }),
          "a chunk above the bound is rejected");
}

}  // namespace

void testCiControl() {
  testRunRoundTrip();
  testStatusRoundTrip();
  testLogChunkRoundTrip();
}
