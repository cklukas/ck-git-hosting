// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/git_repository.hpp"
#include "ckgit/process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error("bulk discovery: " + message);
  }
}

template <typename Action>
void requireFailure(Action action, const char* message) {
  bool failed = false;
  try {
    action();
  } catch (const std::exception&) {
    failed = true;
  }
  require(failed, message);
}

void git(const std::filesystem::path& directory,
         std::initializer_list<std::string> arguments) {
  std::vector<std::string> command{"git", "-C", directory.string()};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto result = ckgit::runProcess(command, std::chrono::seconds(20), 1024 * 1024);
  require(result.exit_code == 0 && !result.timed_out && !result.output_truncated,
          "fixture Git failed: " + result.output);
}

void write(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path);
  output << content;
  require(output.good(), "could not write fixture: " + path.string());
}

class Fixture {
 public:
  Fixture() {
    const char* configured_tmp = std::getenv("TMPDIR");
    const char* configured_root = std::getenv("CKGIT_TEST_ROOT");
    const std::filesystem::path approved = configured_root && *configured_root
        ? configured_root : "/Volumes/PRO-BLADE/tmp";
    require(configured_tmp && *configured_tmp, "TMPDIR must be explicitly selected");
    const auto temporary_root = std::filesystem::canonical(configured_tmp);
    const auto approved_root = std::filesystem::canonical(approved);
    const auto relative = temporary_root.lexically_relative(approved_root);
    require(!relative.empty() && *relative.begin() != "..", "TMPDIR must be below the approved root");
    auto pattern = (temporary_root / "ckgit-bulk-discovery-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const auto created = mkdtemp(writable.data());
    require(created != nullptr, "could not create isolated fixture");
    root = created;
  }

  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  std::filesystem::path repository(const std::string& name, bool bare = false) const {
    const auto directory = root / name;
    std::filesystem::create_directories(directory);
    if (bare) {
      git(directory, {"init", "--bare", "--initial-branch=main"});
    } else {
      git(directory, {"init", "--initial-branch=main"});
    }
    return directory;
  }

  std::filesystem::path root;
};

bool hasWarning(const ckgit::DiscoveryResult& result, const std::filesystem::path& path) {
  return std::any_of(result.warnings.begin(), result.warnings.end(), [&](const auto& warning) {
    return warning.find(path.string()) != std::string::npos;
  });
}

}  // namespace

void testBulkPublishDiscovery() {
  Fixture fixture;
  const auto alpha = fixture.repository("alpha");
  const auto zeta = fixture.repository("zeta");
  const auto hidden = fixture.repository(".hidden");
  const auto nested = fixture.repository("container/nested");
  const auto bare = fixture.repository("bare.git", true);
  git(alpha, {"-c", "user.name=Bulk discovery", "-c", "user.email=bulk@example.invalid",
              "commit", "--allow-empty", "-m", "initial"});
  const auto worktree = fixture.root / "worktree";
  git(alpha, {"worktree", "add", "-b", "other", worktree.string()});
  const auto separate = fixture.root / "separate";
  const auto separate_metadata = fixture.root / "separate-metadata";
  std::filesystem::create_directory(separate);
  git(separate, {"init", "--initial-branch=main", "--separate-git-dir=" + separate_metadata.string()});
  std::filesystem::create_directory_symlink(alpha, fixture.root / "linked-directory");
  std::filesystem::create_directory(fixture.root / "ordinary");
  write(fixture.root / "ordinary-file", "not a repository\n");
  std::filesystem::create_directories(fixture.root / "empty-metadata" / ".git");
  std::filesystem::create_directories(fixture.root / "bad-gitfile");
  write(fixture.root / "bad-gitfile" / ".git", "gitdir: /nonexistent-ckgit-bulk-discovery\n");
  std::filesystem::create_directory(fixture.root / "linked-metadata");
  std::filesystem::create_directory_symlink(alpha / ".git", fixture.root / "linked-metadata" / ".git");

  const auto discovered = ckgit::discoverImmediateWorkingTrees(fixture.root);
  const std::vector<std::filesystem::path> expected{hidden, alpha, separate, worktree, zeta};
  require(discovered.repositories == expected,
          "scan did not return exactly the sorted direct child working trees");
  require(discovered.warnings.size() == 3, "malformed and symlinked metadata warnings were lost");
  for (const auto* name : {"empty-metadata", "bad-gitfile", "linked-metadata"}) {
    require(hasWarning(discovered, fixture.root / name), "missing malformed candidate warning");
  }
  require(ckgit::hasRepositoryContext(alpha), "checkout root was not recognized");
  std::filesystem::create_directories(alpha / "src" / "deep");
  require(ckgit::hasRepositoryContext(alpha / "src" / "deep"), "checkout subdirectory was not recognized");
  require(ckgit::hasRepositoryContext(alpha / ".git" / "objects"), "Git metadata directory was not recognized");
  require(ckgit::hasRepositoryContext(worktree), "linked worktree was not recognized");
  require(ckgit::hasRepositoryContext(separate), "separate Git directory was not recognized");
  require(ckgit::hasRepositoryContext(bare), "bare repository was not recognized");
  require(ckgit::hasRepositoryContext(bare / "refs"), "bare repository subdirectory was not recognized");
  require(ckgit::hasRepositoryContext(nested), "nested checkout was not recognized");
  require(!ckgit::hasRepositoryContext(fixture.root), "ordinary parent folder appeared to be a checkout");
  require(!ckgit::hasRepositoryContext(fixture.root / "ordinary"), "ordinary folder appeared to be a checkout");
  require(ckgit::hasRepositoryContext(fixture.root / "empty-metadata"), "broken metadata lost repository context");
  require(ckgit::hasRepositoryContext(fixture.root / "bad-gitfile"), "invalid gitfile lost repository context");
  std::filesystem::create_directory(fixture.root / "dangling-metadata");
  std::filesystem::create_symlink(fixture.root / "absent", fixture.root / "dangling-metadata" / ".git");
  require(ckgit::hasRepositoryContext(fixture.root / "dangling-metadata"), "dangling metadata lost repository context");
  requireFailure([&] { ckgit::hasRepositoryContext(fixture.root / "missing"); }, "missing folder was accepted");
  requireFailure([&] { ckgit::hasRepositoryContext(fixture.root / "ordinary-file"); }, "file was accepted as folder");
  requireFailure([&] { ckgit::discoverImmediateWorkingTrees(fixture.root / "missing"); }, "missing scan root was accepted");
  requireFailure([&] { ckgit::discoverImmediateWorkingTrees(fixture.root / "ordinary-file"); }, "file scan root was accepted");
  require(ckgit::discoverImmediateWorkingTrees(fixture.root / "ordinary").repositories.empty(),
          "empty folder produced a candidate");

  // Privileged test runners can read mode-000 directories; exercise these
  // diagnostics only when the permission change has an observable effect.
  const auto unreadable = fixture.root / "unreadable";
  std::filesystem::create_directory(unreadable);
  require(chmod(unreadable.c_str(), 0000) == 0, "could not restrict fixture directory");
  const bool permissions_apply = access(unreadable.c_str(), R_OK | X_OK) != 0;
  try {
    if (permissions_apply) {
      require(hasWarning(ckgit::discoverImmediateWorkingTrees(fixture.root), unreadable),
              "unreadable direct child had no warning");
      requireFailure([&] { ckgit::discoverImmediateWorkingTrees(unreadable); }, "unreadable scan root was accepted");
      requireFailure([&] { ckgit::hasRepositoryContext(unreadable); }, "unreadable folder context was accepted");
    }
  } catch (...) {
    chmod(unreadable.c_str(), 0700);
    throw;
  }
  require(chmod(unreadable.c_str(), 0700) == 0, "could not restore fixture permissions");
}
