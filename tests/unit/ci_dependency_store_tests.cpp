// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ckgit/ci_dependency_store.hpp"

namespace {
void require(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
struct Fixture {
  std::filesystem::path root;
  Fixture() {
    const char* tmp = std::getenv("TMPDIR");
    require(tmp && *tmp, "dependency store test needs TMPDIR");
    std::string pattern = (std::filesystem::path(tmp) / "dependency-store-XXXXXX").string();
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    require(mkdtemp(bytes.data()), "dependency store fixture");
    root = bytes.data();
  }
  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
};
std::string lockText() {
  std::string hash(64, 'a');
  return "version: 1\nplatform: windows\narchitecture: x64\nabi: x64-windows\ninputs:\n"
         "  - path: vcpkg.json\n    sha256: " +
         hash +
         "\npackages:\n"
         "  - name: foo\n    version: 1.0\n    registry: vcpkg\n    sha256: " +
         hash + "\n    recipe_sha256: " + hash + "\n    direct: true\n    needs: []\n";
}
template <class F>
bool rejects(F fn) {
  try {
    fn();
    return false;
  } catch (const std::exception&) {
    return true;
  }
}
}  // namespace
void testCiDependencyStore() {
  Fixture f;
  require(ckgit::listCiDependencyRequests(f.root, "project").empty(), "empty dependency store");
  const auto text = lockText();
  const auto id = ckgit::registerCiDependencyRequest(f.root, "project", text);
  require(!ckgit::getCiDependencyRequest(f.root, "project", std::string(64, 'c')),
          "an unknown exact request is absent");
  require(rejects([&] { (void)ckgit::getCiDependencyRequest(f.root, "project", "bad"); }),
          "an invalid exact request digest is rejected");
  require(id == ckgit::registerCiDependencyRequest(f.root, "project", text), "idempotent request");
  const std::string commit(40, 'b');
  require(id == ckgit::registerCiDependencyRequest(f.root, "project", text, commit), "commit request");
  require(id == ckgit::registerCiDependencyRequest(f.root, "project", text, commit), "idempotent commit");
  require(rejects([&] { ckgit::registerCiDependencyRequest(f.root, "project", text, "../bad"); }),
          "invalid commit rejected");
  require(rejects([&] { ckgit::decideCiDependency(f.root, "project", id, "other", "approve"); }),
          "unknown dependency");
  require(rejects([&] { ckgit::decideCiDependency(f.root, "project", id, "foo", "publish"); }),
          "invalid action");
  require(rejects([&] { ckgit::registerCiDependencyRequest(f.root, "../escape", text); }), "unsafe project");
  ckgit::decideCiDependency(f.root, "project", id, "foo", "approve");
  bool admitted = false;
  const auto transaction = [&](const ckgit::CiDependencyRequest&) { admitted = true; };
  require(rejects([&] { ckgit::withApprovedCiDependencyReview(f.root, "project", id, std::string(40, 'c'),
                                                              "windows", "x64", transaction); }) && !admitted,
          "unrequested commits cannot enter the review transaction");
  require(rejects([&] { ckgit::withApprovedCiDependencyReview(f.root, "project", id, commit,
                                                              "macos", "arm64", transaction); }) && !admitted,
          "a different target cannot enter the review transaction");
  int start_pipe[2], done_pipe[2];
  require(pipe(start_pipe) == 0 && pipe(done_pipe) == 0, "review transaction pipes");
  const pid_t revoker = fork();
  require(revoker >= 0, "fork concurrent revocation");
  if (!revoker) {
    close(start_pipe[1]); close(done_pipe[0]);
    char byte = 0;
    if (read(start_pipe[0], &byte, 1) != 1) _exit(2);
    try {
      if (write(done_pipe[1], "r", 1) != 1) _exit(3);
      ckgit::decideCiDependency(f.root, "project", id, "foo", "revoke");
      if (write(done_pipe[1], "d", 1) != 1) _exit(3);
      _exit(0);
    } catch (...) { _exit(4); }
  }
  close(start_pipe[0]); close(done_pipe[1]);
  ckgit::withApprovedCiDependencyReview(f.root, "project", id, commit, "windows", "x64",
      [&](const ckgit::CiDependencyRequest&) {
        require(write(start_pipe[1], "s", 1) == 1, "start concurrent revocation");
        char ready = 0;
        require(read(done_pipe[0], &ready, 1) == 1 && ready == 'r', "revocation process is ready");
        pollfd done{done_pipe[0], POLLIN, 0};
        require(poll(&done, 1, 200) == 0, "revocation waits for the review transaction");
        admitted = true;
      });
  close(start_pipe[1]);
  char done = 0;
  require(read(done_pipe[0], &done, 1) == 1 && done == 'd', "revocation completes after transaction");
  close(done_pipe[0]);
  int revoke_status = 0;
  require(waitpid(revoker, &revoke_status, 0) == revoker && WIFEXITED(revoke_status) &&
              WEXITSTATUS(revoke_status) == 0 && admitted, "review transaction and revocation complete");
  admitted = false;
  require(rejects([&] { ckgit::withApprovedCiDependencyReview(f.root, "project", id, commit,
                                                              "windows", "x64", transaction); }) && !admitted,
          "revocation prevents later review transactions");
  ckgit::decideCiDependency(f.root, "project", id, "foo", "approve");
  const auto exact = ckgit::getCiDependencyRequest(f.root, "project", id);
  require(exact && exact->commits.size() == 1 && exact->commits[0] == commit &&
              ckgit::ciDependencyApprovalStatus(*exact) == "approved", "exact review lookup returns commit and approval");
  require(ckgit::ciDependencyApprovalStatus(ckgit::listCiDependencyRequests(f.root, "project")[0]) == "approved",
          "exact package approval is reported");
  ckgit::decideCiDependency(f.root, "project", id, "foo", "revoke");
  require(ckgit::ciDependencyApprovalStatus(*ckgit::getCiDependencyRequest(f.root, "project", id)) == "pending",
          "exact lookup observes revocation");
  require(ckgit::ciDependencyApprovalStatus(ckgit::listCiDependencyRequests(f.root, "project")[0]) == "pending",
          "revocation removes approval");
  std::vector<pid_t> children;
  for (int i = 0; i < 4; ++i) {
    const pid_t child = fork();
    require(child >= 0, "fork approval writer");
    if (!child) {
      try {
        ckgit::decideCiDependency(f.root, "project", id, "foo", "approve");
        _exit(0);
      } catch (...) {
        _exit(1);
      }
    }
    children.push_back(child);
  }
  for (pid_t child : children) {
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "concurrent approval writer");
  }
  const auto requests = ckgit::listCiDependencyRequests(f.root, "project");
  require(requests.size() == 1 && requests[0].lock.sha256 == id && requests[0].commits.size() == 1 &&
              requests[0].commits[0] == commit && requests[0].history.size() == 8 &&
              requests[0].history[0].action == "approve" && requests[0].history[1].action == "revoke" &&
              requests[0].history[1].sequence == 2,
          "durable ordered decisions");
  auto second_text = text;
  const auto dependency_position = second_text.find("needs: []");
  require(dependency_position != std::string::npos, "dependency test shape");
  second_text.replace(dependency_position, std::string("needs: []").size(), "needs: [bar]");
  const std::string hash(64, 'a');
  second_text += "  - name: bar\n    version: 2\n    registry: vcpkg\n    sha256: " + hash +
                 "\n    recipe_sha256: " + hash + "\n    direct: false\n    needs: []\n";
  const auto second_id = ckgit::registerCiDependencyRequest(f.root, "second", second_text);
  ckgit::registerCiDependencyRequest(f.root, "project", second_text);
  bool truncated = false;
  require(ckgit::listCiDependencyRequests(f.root, "project", 1, &truncated).size() == 1 && truncated,
          "bounded request list reports omitted entries");
  require(ckgit::listCiDependencyRequests(f.root, "project", 2, &truncated).size() == 2 && !truncated,
          "complete request list clears truncation");
  require(ckgit::listCiDependencyRequests(f.root, "project", 0, &truncated).empty() && truncated,
          "zero-sized request list reports omitted entries");
  ckgit::decideCiDependency(f.root, "second", second_id, "foo", "approve");
  require(ckgit::ciDependencyApprovalStatus(ckgit::listCiDependencyRequests(f.root, "second")[0]) == "pending",
          "partial approval remains pending");
  ckgit::decideCiDependency(f.root, "second", second_id, "bar", "approve");
  require(ckgit::ciDependencyApprovalStatus(ckgit::listCiDependencyRequests(f.root, "second")[0]) == "approved",
          "complete approval is reported");
  ckgit::decideCiDependency(f.root, "second", second_id, "bar", "reject");
  require(ckgit::ciDependencyApprovalStatus(ckgit::listCiDependencyRequests(f.root, "second")[0]) == "rejected",
          "rejected transitive dependency blocks review");
  auto file = f.root / "ci" / "dependencies" / "project" / id / "lock.yml";
  std::ofstream(file, std::ios::app) << "# tamper\n";
  require(rejects([&] { (void)ckgit::getCiDependencyRequest(f.root, "project", id); }),
          "exact lookup rejects a changed lock");
  require(rejects([&] { (void)ckgit::listCiDependencyRequests(f.root, "project"); }),
          "changed lock rejected");
  const auto link = f.root / "ci" / "dependencies" / "linked";
  require(symlink((f.root / "ci" / "dependencies" / "project").c_str(), link.c_str()) == 0,
          "fixture symlink");
  require(rejects([&] { (void)ckgit::listCiDependencyRequests(f.root, "linked"); }),
          "symlink project rejected");
}
