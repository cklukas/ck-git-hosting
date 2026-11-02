// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include <unistd.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ckgit/ci_dependency_commit.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/process.hpp"

namespace {
void require(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
struct Fixture {
  std::filesystem::path root;
  Fixture() {
    const char* tmp = std::getenv("TMPDIR");
    require(tmp && *tmp, "dependency commit test needs TMPDIR");
    auto pattern = (std::filesystem::path(tmp) / "dependency-commit-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    require(mkdtemp(buffer.data()), "dependency commit fixture");
    root = buffer.data();
  }
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  std::string git(std::vector<std::string> args) const {
    std::vector<std::string> cmd{"git",
                                 "-C",
                                 root.string(),
                                 "-c",
                                 "user.name=Test",
                                 "-c",
                                 "user.email=test@example.invalid",
                                 "-c",
                                 "commit.gpgsign=false",
                                 "-c",
                                 "core.hooksPath=/dev/null"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    auto r = ckgit::runProcess(cmd, std::chrono::seconds(15), 65536);
    if (r.exit_code != 0 || r.timed_out || r.output_truncated)
      throw std::runtime_error("dependency test git failed: " + r.output);
    while (!r.output.empty() && (r.output.back() == '\n' || r.output.back() == '\r')) r.output.pop_back();
    return r.output;
  }
};
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
void testCiDependencyCommit() {
  Fixture f;
  f.git({"init", "--quiet"});
  const std::string input = "manifest contents\n";
  std::ofstream(f.root / "vcpkg.json", std::ios::binary) << input;
  const std::string a(64, 'a');
  const auto lock =
      "version: 1\nplatform: windows\narchitecture: x64\nabi: x64-windows\ninputs:\n"
      "  - path: vcpkg.json\n    sha256: " +
      ckgit::sha256Hex(input) +
      "\npackages:\n"
      "  - name: example\n    version: 1\n    registry: vcpkg\n    sha256: " +
      a + "\n    recipe_sha256: " + a + "\n    direct: true\n    needs: []\n";
  std::ofstream(f.root / "lock.yml", std::ios::binary) << lock;
  f.git({"add", "vcpkg.json", "lock.yml"});
  f.git({"commit", "--quiet", "-m", "initial"});
  const auto original = f.git({"rev-parse", "HEAD"});
  auto parsed = ckgit::readCiDependencyCommitLock(f.root / ".git", original, "lock.yml");
  require(parsed.bytes == lock && parsed.lock.sha256 == ckgit::sha256Hex(lock), "exact commit lock");
  const char* test_root = std::getenv("CKGIT_TEST_ROOT");
  require(test_root && *test_root, "dependency CLI test needs CKGIT_TEST_ROOT");
  const auto admin = std::filesystem::path(test_root) / "bin/ckgit-admin";
  std::filesystem::create_directory(f.root / "repos");
  std::filesystem::create_directory(f.root / "state");
  require(chmod((f.root / "state").c_str(), 0700) == 0, "private dependency state");
  f.git({"clone", "--bare", "--quiet", f.root.string(), (f.root / "repos/demo.git").string()});
  const auto config = f.root / "server.ini";
  std::ofstream(config) << "schema_version=1\nrepo_root=" << (f.root / "repos").string()
                        << "\nstate_root=" << (f.root / "state").string()
                        << "\ncontrol_socket=" << (f.root / "absent.sock").string() << "\n";
  const auto cli = [&](const std::vector<std::string>& args) {
    std::vector<std::string> command{admin.string(), "ci", "dependencies"};
    command.insert(command.end(), args.begin(), args.end());
    command.insert(command.end(), {"--config", config.string()});
    return ckgit::runProcess(command, std::chrono::seconds(15), 65536);
  };
  const std::vector<std::string> check{"check", "demo", original, "lock.yml"};
  auto checked = cli(check);
  require(checked.exit_code != 0 && checked.output.find("not-requested") != std::string::npos,
          "exact review check refuses an unrequested commit");
  require(cli({"request", "demo", original, "lock.yml"}).exit_code == 0, "CLI records exact request");
  checked = cli(check);
  require(checked.exit_code != 0 && checked.output.find("pending") != std::string::npos,
          "exact review check refuses pending packages");
  require(cli({"approve", "demo", parsed.lock.sha256, "example"}).exit_code == 0, "CLI approves exact package");
  bool transaction_called = false;
  const auto transaction = [&](const ckgit::CiDependencyCommitLock& verified) {
    transaction_called = verified.bytes == lock;
  };
  const auto reviewed = [&](std::string_view commit, std::string_view digest, std::string_view platform) {
    ckgit::withReviewedCiDependencyCommit(f.root / "state", "demo", f.root / "repos/demo.git", commit,
                                          "lock.yml", digest, platform, "x64", transaction);
  };
  reviewed(original, parsed.lock.sha256, "windows");
  require(transaction_called, "exact reviewed commit enters transaction");
  transaction_called = false;
  require(rejects([&] { reviewed(original, std::string(64, 'c'), "windows"); }) && !transaction_called,
          "a different lock digest cannot enter transaction");
  require(rejects([&] { reviewed(original, parsed.lock.sha256, "macos"); }) && !transaction_called,
          "a different target cannot enter transaction");
  checked = cli(check);
  require(checked.exit_code == 0 && checked.output.find("approved") != std::string::npos &&
              checked.output.find("no provisioning or CI enforcement") != std::string::npos,
          "exact review check reports approved snapshot without authorizing execution");
  require(cli({"revoke", "demo", parsed.lock.sha256, "example"}).exit_code == 0, "CLI revokes exact package");
  require(rejects([&] { reviewed(original, parsed.lock.sha256, "windows"); }) && !transaction_called,
          "revoked commit cannot enter transaction");
  checked = cli(check);
  require(checked.exit_code != 0 && checked.output.find("pending") != std::string::npos,
          "exact review check observes revocation");
  require(cli({"approve", "demo", parsed.lock.sha256, "example"}).exit_code == 0,
          "CLI reapproves the first commit's lock");
  f.git({"commit", "--allow-empty", "--quiet", "-m", "another commit"});
  const auto another = f.git({"rev-parse", "HEAD"});
  f.git({"push", "--quiet", (f.root / "repos/demo.git").string(), "HEAD:refs/heads/test"});
  checked = cli({"check", "demo", another, "lock.yml"});
  require(checked.exit_code != 0 && checked.output.find("not-requested") != std::string::npos,
          "an identical lock at another unrequested commit is not implicitly approved");
  require(rejects([&] { reviewed(another, parsed.lock.sha256, "windows"); }) && !transaction_called,
          "unrequested identical lock cannot enter transaction");
  const auto tree = f.git({"rev-parse", "HEAD^{tree}"});
  require(rejects([&] { (void)ckgit::readCiDependencyCommitLock(f.root / ".git", tree, "lock.yml"); }),
          "tree is not a commit");
  std::ofstream(f.root / "vcpkg.json", std::ios::binary | std::ios::trunc) << "changed\n";
  f.git({"add", "vcpkg.json"});
  f.git({"commit", "--quiet", "-m", "changed"});
  const auto changed = f.git({"rev-parse", "HEAD"});
  require(rejects([&] { (void)ckgit::readCiDependencyCommitLock(f.root / ".git", changed, "lock.yml"); }),
          "changed input rejected");
  f.git({"replace", original, changed});
  parsed = ckgit::readCiDependencyCommitLock(f.root / ".git", original, "lock.yml");
  require(parsed.bytes == lock, "git replacement refs do not alter reviewed commit");
  require(symlink("lock.yml", (f.root / "link.yml").c_str()) == 0, "symlink fixture");
  f.git({"add", "link.yml"});
  f.git({"commit", "--quiet", "-m", "symlink"});
  const auto linked = f.git({"rev-parse", "HEAD"});
  require(rejects([&] { (void)ckgit::readCiDependencyCommitLock(f.root / ".git", linked, "link.yml"); }),
          "symlink lock rejected");
}
