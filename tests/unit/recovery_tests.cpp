// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/metadata_store.hpp"
#include "ckgit/process.hpp"
#include "ckgit/recovery.hpp"
#include "ckgit/repository_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("recovery: " + message);
}

template <typename Operation>
void rejected(Operation operation, const std::string& message) {
  bool failed = false;
  try { operation(); } catch (const std::exception&) { failed = true; }
  require(failed, message);
}

void privateDirectory(const fs::path& path) {
  fs::create_directories(path);
  require(chmod(path.c_str(), 0700) == 0, "set private fixture directory permissions");
}

void writeFile(const fs::path& path, const std::string& content, mode_t mode = 0600) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
  file.close();
  require(bool(file), "write fixture file " + path.string());
  require(chmod(path.c_str(), mode) == 0, "set fixture file permissions");
}

std::string readFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  require(bool(file), "open fixture file " + path.string());
  std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  require(!file.bad(), "read fixture file " + path.string());
  return content;
}

using FileTree = std::map<std::string, std::string>;

FileTree contents(const fs::path& root) {
  FileTree result;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    const auto name = entry.path().lexically_relative(root).generic_string();
    const auto status = entry.symlink_status();
    if (fs::is_symlink(status)) result.emplace(name, "symlink:" + fs::read_symlink(entry.path()).string());
    else if (fs::is_directory(status)) result.emplace(name + "/", "");
    else {
      require(fs::is_regular_file(status), "fixture tree contains regular files and directories");
      result.emplace(name, readFile(entry.path()));
    }
  }
  return result;
}

void requirePrivateTree(const fs::path& root) {
  const auto check = [](const fs::path& path) {
    struct stat status {};
    require(lstat(path.c_str(), &status) == 0 && status.st_uid == geteuid(),
            "backup entry belongs to the current user");
    require((S_ISDIR(status.st_mode) && (status.st_mode & 0777) == 0700) ||
                (S_ISREG(status.st_mode) && (status.st_mode & 0777) == 0600),
            "backup entries are private real directories or regular files: " + path.string());
  };
  check(root);
  for (const auto& entry : fs::recursive_directory_iterator(root)) check(entry.path());
}

ckgit::ProcessResult gitResult(const fs::path& directory, const std::vector<std::string>& arguments) {
  std::vector<std::string> command{"git", "--no-optional-locks", "-C", directory.string(),
                                    "-c", "commit.gpgSign=false"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  return ckgit::runProcess(command, std::chrono::seconds(30), 4 * 1024 * 1024);
}

std::string git(const fs::path& directory, const std::vector<std::string>& arguments) {
  auto result = gitResult(directory, arguments);
  require(result.exit_code == 0 && !result.timed_out && !result.output_truncated,
          "fixture Git operation succeeded: " + result.output);
  while (!result.output.empty() && result.output.back() == '\n') result.output.pop_back();
  return result.output;
}

std::string refs(const fs::path& repository) {
  return git(repository, {"for-each-ref", "--sort=refname", "--format=%(refname)%09%(objectname)"});
}

std::string head(const fs::path& repository) {
  return git(repository, {"symbolic-ref", "HEAD"});
}

struct Fixture {
  fs::path root;
  std::vector<std::pair<std::string, std::optional<std::string>>> environment;

  Fixture() {
    const char* temporary = std::getenv("TMPDIR");
    const char* configured_root = std::getenv("CKGIT_TEST_ROOT");
    require(temporary != nullptr && *temporary != '\0', "TMPDIR must be explicitly selected");
    const auto allowed = fs::canonical(configured_root != nullptr && *configured_root
        ? configured_root : "/Volumes/PRO-BLADE/tmp");
    const auto directory = fs::canonical(temporary);
    const auto relative = directory.lexically_relative(allowed);
    require(!relative.empty() && *relative.begin() != "..", "TMPDIR must be under the approved root");
    const auto pattern = (directory / "ckgit-recovery-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const auto created = mkdtemp(writable.data());
    require(created != nullptr, "create recovery fixture");
    root = created;
    privateDirectory(root / "empty-template");
    set("TMPDIR", root.string());
    set("GIT_CONFIG_NOSYSTEM", "1");
    set("GIT_CONFIG_GLOBAL", "/dev/null");
    set("GIT_TEMPLATE_DIR", (root / "empty-template").string());
    set("GIT_AUTHOR_NAME", "Recovery Fixture");
    set("GIT_AUTHOR_EMAIL", "recovery@example.test");
    set("GIT_COMMITTER_NAME", "Recovery Fixture");
    set("GIT_COMMITTER_EMAIL", "recovery@example.test");
    set("GIT_AUTHOR_DATE", "2024-02-29T12:00:00Z");
    set("GIT_COMMITTER_DATE", "2024-02-29T12:00:00Z");
    set("CKGIT_RECOVERY_UNTRUSTED_HOOK_MARKER", (root / "untrusted-hook-ran").string());
    set("CKGIT_RECOVERY_TARGET_HOOK_MARKER", (root / "target-hook-ran").string());
  }

  void set(const std::string& name, const std::string& value) {
    const char* prior = std::getenv(name.c_str());
    environment.emplace_back(name, prior == nullptr ? std::nullopt : std::optional<std::string>(prior));
    require(setenv(name.c_str(), value.c_str(), 1) == 0, "set isolated fixture environment");
  }

  ~Fixture() {
    for (auto entry = environment.rbegin(); entry != environment.rend(); ++entry) {
      if (entry->second) setenv(entry->first.c_str(), entry->second->c_str(), 1);
      else unsetenv(entry->first.c_str());
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
};

ckgit::RecoveryPaths destination(const fs::path& root,
                                const std::optional<fs::path>& hooks = std::nullopt) {
  privateDirectory(root);
  privateDirectory(root / "repositories");
  privateDirectory(root / "state");
  return {root / "repositories", root / "state", hooks};
}

void requireSafeRepository(const fs::path& repository, const fs::path& hooks) {
  require(git(repository, {"rev-parse", "--is-bare-repository"}) == "true", "restored repository is bare");
  require(git(repository, {"config", "--get", "receive.denyDeletes"}) == "true" &&
              git(repository, {"config", "--get", "receive.denyNonFastForwards"}) == "true" &&
              git(repository, {"config", "--get", "receive.advertisePushOptions"}) == "false",
          "restore reinstates receive safety settings");
  require(git(repository, {"config", "--get", "core.hooksPath"}) == fs::canonical(hooks).string(),
          "restore uses the destination's configured hook directory");
  require(git(repository, {"remote"}).empty(), "restored repository retains no backup/source mirror remote");
  const auto private_setting = gitResult(repository, {"config", "--get", "ckgit.fixturePrivate"});
  require(private_setting.exit_code == 1 && !private_setting.timed_out,
          "original repository configuration is retained for review rather than activated");
  git(repository, {"fsck", "--full", "--no-reflogs"});
}

}  // namespace

int testRecovery() {
  Fixture fixture;
  const auto source = destination(fixture.root / "source");
  const auto untrusted_hooks = fixture.root / "untrusted-hooks";
  const auto target_hooks = fixture.root / "target-hooks";
  privateDirectory(untrusted_hooks);
  privateDirectory(target_hooks);
  for (const auto* name : {"reference-transaction", "post-receive", "post-checkout", "post-merge"}) {
    writeFile(untrusted_hooks / name,
        "#!/bin/sh\nprintf '%s\\n' invoked >> \"$CKGIT_RECOVERY_UNTRUSTED_HOOK_MARKER\"\n", 0700);
  }
  writeFile(target_hooks / "post-receive",
      "#!/bin/sh\nprintf '%s\\n' configured >> \"$CKGIT_RECOVERY_TARGET_HOOK_MARKER\"\n", 0700);

  const auto alpha = ckgit::createBareRepository(source.repo_root, "alpha", "stable");
  const auto empty = ckgit::createBareRepository(source.repo_root, "empty", "future");
  const auto seed = fixture.root / "seed";
  git(fixture.root, {"init", "-q", "--initial-branch=main", seed.string()});
  writeFile(seed / "README.md", "# Recovery fixture\nFirst committed version.\n");
  git(seed, {"add", "README.md"});
  git(seed, {"commit", "-q", "-m", "Initial recovery fixture"});
  const auto first = git(seed, {"rev-parse", "HEAD"});
  git(seed, {"branch", "feature/support"});
  git(seed, {"tag", "v1.0"});
  git(seed, {"tag", "-a", "release/v1", "-m", "Annotated release"});
  git(seed, {"update-ref", "refs/archive/saved", first});
  writeFile(seed / "README.md", "# Recovery fixture\nSecond committed version.\n");
  git(seed, {"commit", "-q", "-a", "-m", "Second recovery fixture"});
  git(seed, {"branch", "stable"});
  git(seed, {"push", "--mirror", alpha.string()});
  git(alpha, {"config", "receive.denyDeletes", "false"});
  git(alpha, {"config", "receive.denyNonFastForwards", "false"});
  git(alpha, {"config", "ckgit.fixturePrivate", "source-only configuration"});
  git(alpha, {"config", "core.hooksPath", untrusted_hooks.string()});

  ckgit::registerCheckout(source.state_root, "alpha", "mac",
                          ckgit::encodeCheckoutPath("/private/device/projects/alpha"));
  ckgit::registerCheckout(source.state_root, "alpha", "pi", ckgit::encodeCheckoutPath("alpha-copy"));
  ckgit::registerCheckout(source.state_root, "empty", "mac", ckgit::encodeCheckoutPath("empty"));
  ckgit::appendStateEvent(source.state_root, "project-created", "alpha", "mac");
  ckgit::appendStateEvent(source.state_root, "git-push", "alpha", "pi");
  for (const auto* month : {"2020-01", "2020-02", "2020-03"}) {
    writeFile(source.state_root / "events" / (std::string("events.") + month + ".log"),
              "1582977600\tgit-push\talpha\tmac\n");
  }
  privateDirectory(source.state_root / "projects");
  privateDirectory(source.state_root / "projects" / "alpha");
  writeFile(source.state_root / "projects" / "alpha" / "summary", "derived metadata fixture\n");
  writeFile(source.state_root / "projects" / "alpha" / "sha256-empty", "");
  writeFile(source.state_root / "projects" / "alpha" / "sha256-abc", "abc");
  // An opt-in flag, an (empty) spool and working area, and one recorded run —
  // the CI state the runner keeps under the state root — must survive a backup.
  privateDirectory(source.state_root / "ci");
  privateDirectory(source.state_root / "ci" / "spool");
  privateDirectory(source.state_root / "ci" / "working");
  privateDirectory(source.state_root / "ci" / "projects");
  writeFile(source.state_root / "ci" / "projects" / "alpha.ini", "schema_version=1\nci_enabled=true\n");
  privateDirectory(source.state_root / "ci" / "runs");
  privateDirectory(source.state_root / "ci" / "runs" / "alpha");
  const auto ci_run = source.state_root / "ci" / "runs" / "alpha" / "00000000000000000001-abcdabcd";
  privateDirectory(ci_run);
  writeFile(ci_run / "run.ini", "schema_version=1\n");
  privateDirectory(ci_run / "steps");
  writeFile(ci_run / "steps" / "0.log", "ci log fixture\n");

  const std::vector<std::string> projects{"alpha", "empty"};
  const auto original_refs = refs(alpha);
  const auto original_head = head(alpha);
  const auto original_config = readFile(alpha / "config");
  const auto original_state = contents(source.state_root);
  require(original_refs.find("refs/archive/saved") != std::string::npos &&
              original_refs.find("refs/tags/release/v1") != std::string::npos,
          "fixture contains custom refs and annotated tags");
  require(ckgit::listTrashedRepositories(source).empty(), "a fresh repository root has no trash entries");

  const auto preview = fixture.root / "preview-backup";
  require(ckgit::backupHostedRepositories(source, preview, true).projects == projects && !fs::exists(preview),
          "backup preview lists every project without creating output");
  require(contents(source.state_root) == original_state && readFile(alpha / "config") == original_config,
          "backup preview preserves source metadata and repository configuration");

  const auto backup = fixture.root / "backup";
  const auto summary = ckgit::backupHostedRepositories(source, backup);
  require(summary.projects == projects && summary.files > 0 && summary.bytes > 0,
          "backup reports all projects and nonzero copied data");
  requirePrivateTree(backup);
  require(ckgit::verifyBackup(backup).projects == projects, "completed backup independently verifies");
  require(refs(backup / "repositories/alpha.git") == original_refs &&
              head(backup / "repositories/alpha.git") == original_head,
          "mirror preserves every advertised ref and the non-main default branch");
  require(refs(backup / "repositories/empty.git").empty() &&
              head(backup / "repositories/empty.git") == head(empty),
          "backup preserves an empty repository and its unborn default branch");
  require(readFile(backup / "configs/alpha.config") == original_config,
          "original configuration is preserved byte-for-byte as a separate review artifact");
  require(contents(backup / "state") == original_state,
          "backup preserves private checkouts, every event archive, and derived metadata");
  require(fs::exists(backup / "state" / "ci" / "runs" / "alpha" /
                     "00000000000000000001-abcdabcd" / "run.ini") &&
              fs::exists(backup / "state" / "ci" / "projects" / "alpha.ini"),
          "backup captures CI run records and per-project opt-in under the state root");
  require(contents(source.state_root) == original_state && refs(alpha) == original_refs &&
              readFile(alpha / "config") == original_config,
          "backup does not rewrite the source repository or metadata");
  require(!fs::exists(fixture.root / "untrusted-hook-ran"), "backup does not execute source hooks");

  const auto manifest = backup / "manifest.sha256";
  const auto original_manifest = readFile(manifest);
  require(!original_manifest.empty(), "backup has a nonempty integrity manifest");
  require(original_manifest.find(" 0 e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n") != std::string::npos &&
              original_manifest.find(" 3 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n") != std::string::npos,
          "manifest hashing matches standard SHA-256 vectors for empty data and abc");
  auto tampered_manifest = original_manifest;
  tampered_manifest.front() = tampered_manifest.front() == '0' ? '1' : '0';
  writeFile(manifest, tampered_manifest);
  rejected([&] { ckgit::verifyBackup(backup); }, "manifest tampering is rejected");
  writeFile(manifest, original_manifest);
  writeFile(backup / "configs/alpha.config", original_config + "# altered after backup\n");
  rejected([&] { ckgit::verifyBackup(backup); }, "changed payload is rejected by its manifest digest");
  writeFile(backup / "configs/alpha.config", original_config);
  writeFile(backup / "unexpected-file", "not present in the manifest\n");
  rejected([&] { ckgit::verifyBackup(backup); }, "unmanifested payload is rejected");
  fs::remove(backup / "unexpected-file");

  const auto outside = fixture.root / "outside-sentinel";
  writeFile(outside, "must remain untouched\n");
  fs::remove(backup / "configs/alpha.config");
  fs::create_symlink(outside, backup / "configs/alpha.config");
  rejected([&] { ckgit::verifyBackup(backup); }, "backup verification rejects payload symlinks");
  require(readFile(outside) == "must remain untouched\n", "verification never changes a symlink target");
  fs::remove(backup / "configs/alpha.config");
  writeFile(backup / "configs/alpha.config", original_config);
  require(ckgit::verifyBackup(backup).projects == projects, "restored fixture bytes verify again");

  const auto existing_output = fixture.root / "existing-output";
  privateDirectory(existing_output);
  writeFile(existing_output / "sentinel", "keep existing destination\n");
  const auto existing_contents = contents(existing_output);
  rejected([&] { ckgit::backupHostedRepositories(source, existing_output); },
           "backup refuses an existing destination");
  require(contents(existing_output) == existing_contents, "backup collision preserves destination contents");
  const auto output_link = fixture.root / "output-link";
  fs::create_directory_symlink(existing_output, output_link);
  rejected([&] { ckgit::backupHostedRepositories(source, output_link); }, "backup refuses a symlink destination");
  require(contents(existing_output) == existing_contents, "symlink destination remains unchanged");

  fs::create_directory_symlink(alpha, source.repo_root / "linked.git");
  rejected([&] { ckgit::backupHostedRepositories(source, fixture.root / "unsafe-repository-backup"); },
           "backup rejects a source repository symlink");
  fs::remove(source.repo_root / "linked.git");
  fs::create_symlink(outside, source.state_root / "checkouts/alpha/intruder.ini");
  rejected([&] { ckgit::backupHostedRepositories(source, fixture.root / "unsafe-state-backup"); },
           "backup rejects a source metadata symlink");
  fs::remove(source.state_root / "checkouts/alpha/intruder.ini");
  const auto record = source.state_root / "checkouts/alpha/mac.ini";
  const auto original_record = readFile(record);
  writeFile(record, "schema_version=9\n");
  rejected([&] { ckgit::backupHostedRepositories(source, fixture.root / "invalid-state-backup"); },
           "copied state must pass its strict metadata schema before backup succeeds");
  writeFile(record, original_record);
  require(contents(source.state_root) == original_state, "rejected backups do not rewrite source metadata");

  const auto overlap_target = destination(fixture.root / "overlap-target", target_hooks);
  const auto backup_before_overlap = contents(backup);
  for (const auto& unsafe_target : std::vector<ckgit::RecoveryPaths>{
           {backup, overlap_target.state_root, target_hooks},
           {backup / "configs", overlap_target.state_root, target_hooks},
           {overlap_target.repo_root, backup / "state", target_hooks}}) {
    rejected([&] { ckgit::restoreBackup(unsafe_target, backup); },
             "restore refuses target roots equal to or inside the backup");
    require(contents(backup) == backup_before_overlap && contents(overlap_target.repo_root).empty() &&
                contents(overlap_target.state_root).empty(),
            "overlapping restore never changes backup data or external target directories");
  }
  // Empty backup metadata cannot rely on the ordinary nonempty-state or
  // project-collision guards to keep a restore from modifying its own input.
  const auto empty_source = destination(fixture.root / "empty-source");
  const auto empty_backup = fixture.root / "empty-backup";
  require(ckgit::backupHostedRepositories(empty_source, empty_backup).projects.empty(),
          "a fresh server can produce a backup without projects or metadata");
  require(contents(empty_backup / "state").empty(), "empty-server backup has an empty state directory");
  const auto empty_backup_before_overlap = contents(empty_backup);
  for (const auto& unsafe_target : std::vector<ckgit::RecoveryPaths>{
           {empty_backup, overlap_target.state_root, target_hooks},
           {empty_backup / "repositories", overlap_target.state_root, target_hooks},
           {overlap_target.repo_root, empty_backup / "state", target_hooks}}) {
    rejected([&] { ckgit::restoreBackup(unsafe_target, empty_backup); },
             "empty backup still rejects overlapping restore target roots");
    require(contents(empty_backup) == empty_backup_before_overlap &&
                contents(overlap_target.repo_root).empty() && contents(overlap_target.state_root).empty(),
            "empty-state overlap rejection leaves the entire backup and both external roots untouched");
  }

  const auto restored = destination(fixture.root / "restored", target_hooks);
  require(ckgit::restoreBackup(restored, backup, true) == projects &&
              contents(restored.repo_root).empty() && contents(restored.state_root).empty(),
          "restore preview identifies projects without creating repositories or metadata");
  require(ckgit::restoreBackup(restored, backup) == projects, "restore publishes all verified projects");
  require(refs(restored.repo_root / "alpha.git") == original_refs &&
              head(restored.repo_root / "alpha.git") == original_head &&
              refs(restored.repo_root / "empty.git").empty() &&
              head(restored.repo_root / "empty.git") == head(empty),
          "restore preserves populated and empty repository refs and HEAD");
  require(contents(restored.state_root) == original_state,
          "restore keeps original metadata timestamps, private paths, and old archives");
  requireSafeRepository(restored.repo_root / "alpha.git", target_hooks);
  requireSafeRepository(restored.repo_root / "empty.git", target_hooks);
  require(!fs::exists(fixture.root / "untrusted-hook-ran"), "verification and restore do not execute backed-up hooks");

  const auto collision_state = contents(restored.state_root);
  rejected([&] { ckgit::restoreBackup(restored, backup); }, "restore refuses existing repositories");
  require(contents(restored.state_root) == collision_state && refs(restored.repo_root / "alpha.git") == original_refs,
          "repository collision leaves all existing repositories and metadata untouched");
  const auto metadata_collision = destination(fixture.root / "metadata-collision", target_hooks);
  ckgit::registerCheckout(metadata_collision.state_root, "alpha", "other-device",
                          ckgit::encodeCheckoutPath("existing-local-report"));
  const auto prior_collision_metadata = contents(metadata_collision.state_root);
  rejected([&] { ckgit::restoreBackup(metadata_collision, backup); }, "restore refuses existing project metadata");
  require(contents(metadata_collision.repo_root).empty() &&
              contents(metadata_collision.state_root) == prior_collision_metadata,
          "metadata collision never partially publishes repository data or replaces reports");

  writeFile(seed / "README.md", "# Recovery fixture\nA commit after restoration.\n");
  git(seed, {"commit", "-q", "-a", "-m", "Validate destination receive hook"});
  git(seed, {"push", (restored.repo_root / "alpha.git").string(), "main:main"});
  require(fs::exists(fixture.root / "target-hook-ran") && !fs::exists(fixture.root / "untrusted-hook-ran"),
          "subsequent pushes invoke only the destination's configured hook");
  require(refs(backup / "repositories/alpha.git") == original_refs &&
              ckgit::verifyBackup(backup).projects == projects,
          "restored repository writes do not alter its independent backup");

  const auto removed = ckgit::createBareRepository(source.repo_root, "removed", "main");
  git(seed, {"push", removed.string(), "main:main"});
  const auto removed_refs = refs(removed);
  git(removed, {"config", "core.hooksPath", untrusted_hooks.string()});
  ckgit::registerCheckout(source.state_root, "removed", "mac", ckgit::encodeCheckoutPath("old-checkout"));
  ckgit::appendStateEvent(source.state_root, "git-push", "removed", "mac");
  const auto trash = ckgit::removeProject(source.repo_root, source.state_root, "removed");
  const auto entry = trash.filename().string();
  const auto trash_entries = ckgit::listTrashedRepositories(source);
  require(std::any_of(trash_entries.begin(), trash_entries.end(), [&](const ckgit::TrashedRepository& item) {
    return item.entry == entry && item.project == "removed";
  }), "trash listing recovers the exact entry and original project name");
  const ckgit::RecoveryPaths trash_target{source.repo_root, source.state_root, target_hooks};
  const auto state_after_removal = contents(source.state_root);
  require(ckgit::restoreTrashedRepository(trash_target, entry, std::nullopt, true) == removed &&
              !fs::exists(removed) && fs::exists(trash) && contents(source.state_root) == state_after_removal,
          "trash restore preview preserves the retained source and current state");
  require(ckgit::restoreTrashedRepository(trash_target, entry) == removed && fs::exists(trash),
          "trash restore publishes a copy while retaining the original trash entry");
  require(refs(removed) == removed_refs && contents(source.state_root) == state_after_removal &&
              ckgit::loadCheckoutMetadata(source.state_root, "removed").empty(),
          "trash recovery restores Git without recreating discarded checkout reports or events");
  requireSafeRepository(removed, target_hooks);
  rejected([&] { ckgit::restoreTrashedRepository(trash_target, entry); },
           "trash recovery refuses an occupied project name");
  require(refs(removed) == removed_refs && contents(source.state_root) == state_after_removal,
          "trash collision preserves the restored repository and unrelated metadata");
  const auto renamed = ckgit::restoreTrashedRepository(trash_target, entry, std::string("recovered-copy"));
  require(renamed == source.repo_root / "recovered-copy.git" && refs(renamed) == removed_refs && fs::exists(trash),
          "explicit rename restores the same history without consuming its trash source");
  requireSafeRepository(renamed, target_hooks);
  rejected([&] { ckgit::restoreTrashedRepository(trash_target, "../removed.git"); },
           "trash selection rejects path traversal");
  rejected([&] { ckgit::restoreTrashedRepository(trash_target, entry, std::string("../escape")); },
           "renamed trash restore rejects an unsafe project name");
  const auto embedded_name = "library.git.tools";
  const auto embedded_repository = ckgit::createBareRepository(source.repo_root, embedded_name, "main");
  git(seed, {"push", embedded_repository.string(), "main:main"});
  const auto embedded_refs = refs(embedded_repository);
  const auto embedded_head = head(embedded_repository);
  const auto embedded_trash = ckgit::removeProject(source.repo_root, source.state_root, embedded_name);
  const auto embedded_entry = embedded_trash.filename().string();
  const auto embedded_listing = ckgit::listTrashedRepositories(source);
  require(std::any_of(embedded_listing.begin(), embedded_listing.end(), [&](const ckgit::TrashedRepository& item) {
    return item.entry == embedded_entry && item.project == embedded_name;
  }), "trash listing preserves a valid project name containing an embedded .git. delimiter");
  require(ckgit::restoreTrashedRepository(trash_target, embedded_entry) == embedded_repository &&
              refs(embedded_repository) == embedded_refs && head(embedded_repository) == embedded_head &&
              fs::exists(embedded_trash),
          "embedded .git. project name survives removal and retained-copy restoration exactly");
  requireSafeRepository(embedded_repository, target_hooks);
  fs::create_directory_symlink(removed, source.repo_root / ".trash/unsafe.git.123");
  rejected([&] { ckgit::restoreTrashedRepository(trash_target, "unsafe.git.123"); },
           "trash restore rejects a symlink entry");
  require(refs(removed) == removed_refs && contents(source.state_root) == state_after_removal &&
              !fs::exists(fixture.root / "untrusted-hook-ran"),
          "rejected trash operations preserve every source and never run archived hooks");
  return 0;
}
