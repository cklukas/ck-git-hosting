// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/git_repository.hpp"
#include "ckgit/path_utf8.hpp"
#include "ckgit/process.hpp"
#include "ckgit/temp_directory.hpp"
#include "ckgit/windows.hpp"
#include <fstream>
#include <iostream>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int run(const std::vector<std::string>& args) {
  require(args.size() == 2 || args.size() == 3, "explicit scratch parent required");
  const auto root = ckgit::createTempDirectory(ckgit::pathFromUtf8(args[1]), "git-inspection-");
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{root};
  const auto repo = root / ckgit::pathFromUtf8("repository \xc3\xbc \xf0\x9f\x98\x80");
  const auto empty = root / "empty-template";
  std::filesystem::create_directory(repo);
  std::filesystem::create_directory(empty);
  const auto git = [&](std::initializer_list<std::string> arguments) {
    std::vector<std::string> command{"git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
        "-c", "commit.gpgsign=false", "-c", "core.hooksPath=" + ckgit::pathToUtf8(empty), "-C", ckgit::pathToUtf8(repo)};
    command.insert(command.end(), arguments);
    const auto result = ckgit::runProcess(command);
    if (result.exit_code || result.timed_out) throw std::runtime_error("Git fixture failed: " + result.output);
  };
  git({"init", "--initial-branch=main", "--template=" + ckgit::pathToUtf8(empty)});
  std::ofstream(repo / "file.txt") << "initial\n";
  git({"add", "file.txt"});
  git({"commit", "-m", "Initial fixture"});
  git({"remote", "add", "ckgit", "ckgit@example.invalid:fixture.git"});
  auto audit = ckgit::inspectRepository(repo);
  require(std::filesystem::equivalent(audit.path, repo), "Unicode repository path roundtrip");
  require(audit.current_branch == "main" && !audit.detached_head && audit.changed_entries == 0, "clean repository audit");
  require(audit.remotes.size() == 1 && !audit.branches.empty(), "remote and ref inspection");
  std::ofstream(repo / "file.txt", std::ios::app) << "changed\n";
  require(ckgit::inspectRepository(repo).changed_entries == 1, "dirty status inspection");
  std::filesystem::create_directory(repo / "nested");
  require(ckgit::hasRepositoryContext(repo / "nested"), "nested repository context");
  require(!ckgit::hasRepositoryContext(empty), "non-repository context");
  const auto shallow = ckgit::discoverImmediateWorkingTrees(root);
  const auto recursive = ckgit::discoverWorkingTrees({root}, {});
  require(shallow.repositories.size() == 1 && recursive.repositories.size() == 1, "working tree discovery");
  const auto linked = root / "linked";
  git({"worktree", "add", "--detach", ckgit::pathToUtf8(linked), "HEAD"});
  const auto linked_audit = ckgit::inspectRepository(linked);
  require(linked_audit.linked_worktree && linked_audit.detached_head, "linked worktree inspection");
  if (args.size() == 3) {
    const auto config = root / ckgit::pathFromUtf8("client config \xc3\xbc.ini");
    const auto cli = [&](std::initializer_list<std::string> arguments, int expected = 0) {
      std::vector<std::string> command{args[2]};
      command.insert(command.end(), arguments);
      const auto result = ckgit::runProcess(command, std::chrono::seconds(30));
      if (result.exit_code != expected || result.timed_out) throw std::runtime_error("CLI fixture failed: " + result.output);
      return result.output;
    };
    cli({"--version"});
    cli({"help", "ci"});
    cli({"setup", "--yes", "--config", ckgit::pathToUtf8(config), "--server", "ckgit@example.invalid",
        "--client-id", "native-test", "--display-name", "Native \xc3\xbc", "--scan-root", ckgit::pathToUtf8(root)});
    const auto shown = cli({"config", "show", "--config", ckgit::pathToUtf8(config)});
    require(shown.find("Native \xc3\xbc") != std::string::npos, "native CLI Unicode configuration");
    const auto scanned = cli({"scan", "--config", ckgit::pathToUtf8(config), "--json"});
    require(scanned.find("repository \\u") != std::string::npos || scanned.find("repository \xc3\xbc") != std::string::npos,
        "native CLI Unicode scan");
    cli({"setup", "--yes", "--config", ckgit::pathToUtf8(config), "--server", "ckgit@different.invalid"}, 2);
    cli({"setup", "--yes", "--overwrite", "--config", ckgit::pathToUtf8(config), "--display-name", "Changed"});
#ifdef _WIN32
    cli({"setup", "--dry-run", "--overwrite", "--config", ckgit::pathToUtf8(config), "--scan-root", "~"});
#endif
  }
  std::cout << "native Git discovery and Unicode inspection tests passed\n";
  return 0;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  try {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
#ifdef _WIN32
      args.push_back(ckgit::wideToUtf8(argv[i]));
#else
      args.emplace_back(argv[i]);
#endif
    }
    return run(args);
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
