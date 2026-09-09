// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/project_index.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "ckgit/metadata_store.hpp"
#include "ckgit/process.hpp"
#include "ckgit/repository_store.hpp"

namespace {

void requireIndex(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error("ProjectIndex: " + message);
  }
}

class IndexFixture {
 public:
  IndexFixture() {
    const char* configured_tmp = std::getenv("TMPDIR");
    const char* configured_root = std::getenv("CKGIT_TEST_ROOT");
    const std::filesystem::path allowed = configured_root && *configured_root
        ? configured_root : "/Volumes/PRO-BLADE/tmp";
    requireIndex(configured_tmp && *configured_tmp, "TMPDIR must be explicitly selected");
    const auto temporary_root = std::filesystem::canonical(configured_tmp);
    const auto allowed_root = std::filesystem::canonical(allowed);
    const auto relative = temporary_root.lexically_relative(allowed_root);
    requireIndex(!relative.empty() && *relative.begin() != "..", "TMPDIR must be beneath the approved root");
    auto pattern = (temporary_root / "ckgit-index-tests-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const auto created = mkdtemp(writable.data());
    requireIndex(created != nullptr, "could not create isolated fixture");
    root = created;
    repos = root / "repos";
    state = root / "state";
    work = root / "work";
    std::filesystem::create_directories(repos);
    std::filesystem::create_directories(state);
    requireIndex(chmod(state.c_str(), 0700) == 0, "could not secure fixture state");
    std::filesystem::create_directories(work);
    git(work, {"init", "--initial-branch=main"});
  }

  ~IndexFixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  static std::string git(const std::filesystem::path& directory,
                         std::initializer_list<std::string> arguments) {
    std::vector<std::string> command{"git", "-C", directory.string()};
    command.insert(command.end(), arguments.begin(), arguments.end());
    return run(command);
  }

  static std::string run(const std::vector<std::string>& command) {
    const auto result = ckgit::runProcess(command, std::chrono::seconds(10), 1024 * 1024);
    requireIndex(result.exit_code == 0 && !result.timed_out && !result.output_truncated,
                 "fixture command failed: " + result.output);
    auto output = result.output;
    while (!output.empty() && output.back() == '\n') {
      output.pop_back();
    }
    return output;
  }

  void write(const std::string& name, const std::string& content) const {
    std::ofstream file(work / name, std::ios::binary);
    file << content;
    requireIndex(file.good(), "could not write fixture content");
  }

  void commit(const std::string& date, const std::string& subject) const {
    git(work, {"add", "--all"});
    run({"env", "GIT_AUTHOR_DATE=" + date, "GIT_COMMITTER_DATE=" + date,
         "git", "-C", work.string(), "-c", "user.name=Index <author>", "-c",
         "user.email=index@example.invalid", "commit", "-m", subject});
  }

  void publish(const std::string& project = "sample") const {
    git(work, {"push", (repos / (project + ".git")).string(), "--all"});
    git(work, {"push", (repos / (project + ".git")).string(), "--tags"});
  }

  std::filesystem::path root;
  std::filesystem::path repos;
  std::filesystem::path state;
  std::filesystem::path work;
};

ckgit::ProjectSummary requireProject(const ckgit::ProjectIndex& index, const std::string& name) {
  const auto record = index.find(name);
  requireIndex(record.has_value(), "missing project " + name);
  return *record;
}

std::string shellQuote(const std::string& value) {
  std::string result = "'";
  for (const char character : value) {
    result += character == '\'' ? "'\\''" : std::string(1, character);
  }
  return result + "'";
}

class PathOverride {
 public:
  explicit PathOverride(const std::filesystem::path& directory) {
    const char* previous = std::getenv("PATH");
    had_path_ = previous != nullptr;
    previous_ = previous ? previous : "";
    requireIndex(setenv("PATH", (directory.string() + ":" + previous_).c_str(), 1) == 0,
                 "could not configure fixture Git wrapper");
  }
  ~PathOverride() {
    if (had_path_) setenv("PATH", previous_.c_str(), 1);
    else unsetenv("PATH");
  }
 private:
  bool had_path_{};
  std::string previous_;
};

void testRefreshPriority() {
  IndexFixture fixture;
  for (const auto* name : {"priority-a", "priority-b", "priority-z"}) {
    ckgit::createBareRepository(fixture.repos, name, "main");
  }
  const auto real_git = IndexFixture::run({"which", "git"});
  const auto bin = fixture.root / "bin";
  const auto log = fixture.root / "calls.log";
  const auto pause = fixture.root / "pause";
  const auto entered = fixture.root / "entered";
  const auto release = fixture.root / "release";
  std::filesystem::create_directories(bin);
  // Observe call order and provide a deterministic point to enqueue a hook
  // while a regular sweep project is in progress. The real Git still supplies
  // every repository result; this does not mock the index's data parsing.
  std::ofstream(bin / "git") << "#!/bin/sh\nrepository=\nprevious=\nrefs=no\n"
      "for argument in \"$@\"; do\n"
      "  if [ \"$previous\" = --git-dir ]; then repository=$argument; fi\n"
      "  if [ \"$argument\" = for-each-ref ]; then refs=yes; fi\n"
      "  previous=$argument\ndone\n"
      "if [ \"$refs\" = yes ]; then\n"
      "  printf '%s\\n' \"$repository\" >> " << shellQuote(log.string()) << "\n"
      "  if [ \"$repository\" = " << shellQuote((fixture.repos / "priority-a.git").string()) <<
      " ] && [ -f " << shellQuote(pause.string()) << " ]; then\n"
      "    : > " << shellQuote(entered.string()) << "\n"
      "    while [ ! -f " << shellQuote(release.string()) << " ]; do /bin/sleep 0.01; done\n"
      "  fi\nfi\nexec " << shellQuote(real_git) << " \"$@\"\n";
  requireIndex(chmod((bin / "git").c_str(), 0700) == 0, "could not enable fixture Git wrapper");
  const PathOverride path(bin);
  ckgit::ProjectIndex index(fixture.repos);
  index.refresh("priority-z");
  index.start();
  const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool finished = false;
  do {
    const auto rows = index.snapshot();
    finished = rows.size() == 3;
    for (const auto& row : rows) finished = finished && !row.indexing;
    if (finished) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < startup_deadline);
  index.stop();
  requireIndex(finished, "priority fixture startup completed");
  std::ifstream first_log(log);
  std::string first;
  std::getline(first_log, first);
  first_log.close();
  requireIndex(first == (fixture.repos / "priority-z.git").string(),
               "an explicit hook refresh takes precedence over alphabetical startup inventory");

  std::ofstream(log, std::ios::trunc).close();
  std::ofstream(pause).close();
  std::exception_ptr sweep_error;
  std::thread sweeper([&] {
    try { index.sweep(); } catch (...) { sweep_error = std::current_exception(); }
  });
  const auto sweep_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!std::filesystem::exists(entered) && std::chrono::steady_clock::now() < sweep_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const bool paused = std::filesystem::exists(entered);
  index.refresh("priority-z");
  // Always release and join before assertions, including failure paths.
  std::ofstream(release).close();
  sweeper.join();
  if (sweep_error) std::rethrow_exception(sweep_error);
  requireIndex(paused, "sweep reached its deterministic enqueue point");
  std::ifstream sweep_log(log);
  std::string second;
  std::getline(sweep_log, first);
  std::getline(sweep_log, second);
  requireIndex(first == (fixture.repos / "priority-a.git").string() &&
                   second == (fixture.repos / "priority-z.git").string(),
               "a hook queued during a sweep runs before the next regular project");
}

}  // namespace

void testProjectIndex() {
  IndexFixture fixture;
  ckgit::createBareRepository(fixture.repos, "empty", "main");
  ckgit::createBareRepository(fixture.repos, "sample", "main");
  ckgit::createBareRepository(fixture.repos, "invalid", "missing");
  fixture.write("README.txt", "plain fallback\n");
  fixture.commit("2024-02-28T23:59:59+0000", "First commit");
  fixture.publish();
  std::filesystem::create_directory_symlink(fixture.repos / "sample.git", fixture.repos / "alias.git");

  ckgit::ProjectIndex index(fixture.repos, fixture.state);
  requireIndex(index.snapshot().size() == 3, "startup inventories bare repositories without following symlinks");
  requireIndex(requireProject(index, "sample").indexing, "startup exposes a placeholder before running Git");
  index.sweep();
  const auto empty = requireProject(index, "empty");
  requireIndex(!empty.indexing && !empty.valid_head && empty.default_branch == "main" &&
                   !empty.last_commit && empty.branch_count == 0 && empty.index_error.empty(),
               "empty repository remains available without errors");
  auto sample = requireProject(index, "sample");
  requireIndex(sample.index_error.empty(), "sample builds successfully: " + sample.index_error);
  requireIndex(sample.readme_path == "README.txt" && sample.readme_content == "plain fallback\n",
               "plain README fallback is indexed");
  requireIndex(sample.activity.at("2024-02-28") == 1, "initial UTC activity is counted");

  fixture.write("README", "extensionless fallback\n");
  fixture.commit("2024-02-29T00:30:00+0100", "UTC is still February 28");
  fixture.publish();
  index.sweep();
  sample = requireProject(index, "sample");
  requireIndex(sample.readme_path == "README", "extensionless README outranks README.txt");
  requireIndex(sample.activity.at("2024-02-28") == 2 && sample.activity.size() == 1,
               "activity uses UTC rather than the commit timezone");
  requireIndex(sample.last_commit && sample.last_commit->subject == "First commit" &&
                   sample.last_commit->id != sample.head_id,
               "a later-dated ancestor wins when committer timestamps go backwards");

  fixture.write("README.markdown", "# Markdown fallback\n");
  fixture.commit("2024-02-29T01:00:00+0000", "Markdown fallback");
  fixture.publish();
  index.sweep();
  requireIndex(requireProject(index, "sample").readme_path == "README.markdown",
               "README.markdown outranks extensionless README");

  fixture.write("ReadMe.MD", "# Most specific README\n\n<script>escaped by renderer</script>\n");
  fixture.commit("2024-02-29T02:00:00+0000", "Subject <script> & \"quotes\"");
  IndexFixture::git(fixture.work, {"branch", "feature/slash"});
  IndexFixture::run({"env", "GIT_COMMITTER_DATE=2024-03-15T10:00:00+0000",
                     "git", "-C", fixture.work.string(), "-c", "user.name=Index", "-c",
                     "user.email=index@example.invalid",
                     "tag", "-a", "main", "-m", "Annotated tag shares the branch name"});
  fixture.publish();
  fixture.publish("invalid");
  index.sweep();
  sample = requireProject(index, "sample");
  requireIndex(sample.index_error.empty(), "populated index has no error: " + sample.index_error);
  requireIndex(sample.valid_head && sample.head_id.size() == 40 && sample.branch_count == 2 &&
                   sample.tag_count == 1, "branches, annotated tags and resolved HEAD are indexed");
  requireIndex(sample.last_commit && sample.last_commit->id == sample.head_id &&
                   sample.last_commit->subject == "Subject <script> & \"quotes\"" &&
                   sample.last_commit->author == "Index author", "last commit retains text for escaping by page renderers");
  requireIndex(sample.readme_path == "ReadMe.MD", "README selection is case insensitive and follows priority");
  requireIndex(sample.activity.at("2024-02-29") == 2, "leap day activity is counted");
  requireIndex(sample.object_count > 0 && sample.size_bytes > 0, "object count and storage size are available");
  const auto tag_creation_epoch = static_cast<std::uint64_t>(std::stoull(
      IndexFixture::git(fixture.work, {"for-each-ref", "--format=%(creatordate:unix)", "refs/tags/main"})));
  bool same_branch = false;
  bool same_tag = false;
  for (const auto& ref : sample.refs) {
    if (ref.name == "main") {
      if (ref.is_tag) {
        requireIndex(ref.id == sample.head_id, "an annotated tag's ID is peeled to the referenced commit");
        requireIndex(ref.epoch_seconds == tag_creation_epoch && ref.epoch_seconds != sample.last_commit_epoch_seconds,
                     "an annotated tag's displayed date is its own creation date, not the peeled commit's date");
      } else {
        requireIndex(ref.id == sample.head_id && ref.epoch_seconds == sample.last_commit_epoch_seconds,
                     "a branch's date and ID are its own commit's");
      }
      (ref.is_tag ? same_tag : same_branch) = true;
    }
  }
  requireIndex(same_branch && same_tag, "equal branch/tag names stay distinguishable");
  const auto invalid = requireProject(index, "invalid");
  requireIndex(!invalid.valid_head && invalid.default_branch == "missing" && invalid.last_commit &&
                   invalid.activity.empty() && invalid.index_error.empty(),
               "invalid symbolic HEAD still exposes refs and last commit");
  std::ofstream(fixture.repos / "invalid.git" / "HEAD") << sample.head_id << '\n';
  index.sweep();
  const auto detached = requireProject(index, "invalid");
  requireIndex(detached.valid_head && detached.default_branch.empty() && detached.head_id == sample.head_id &&
                   detached.activity == sample.activity && detached.fingerprint != invalid.fingerprint,
               "detached HEAD remains browsable and has a complete calendar");

  const auto fingerprint = sample.fingerprint;
  const auto generated = sample.generated_epoch_seconds;
  index.sweep();
  sample = requireProject(index, "sample");
  requireIndex(sample.fingerprint == fingerprint && sample.generated_epoch_seconds == generated,
               "no-op sweep preserves fingerprint and generation");
  ckgit::registerCheckout(fixture.state, "sample", "index-host",
                          ckgit::encodeCheckoutPath("/work/sample"));
  ckgit::appendStateEvent(fixture.state, "git-push", "sample", "index-host");
  index.refreshMetadata("sample");
  sample = requireProject(index, "sample");
  requireIndex(sample.checkouts.size() == 1 && !sample.events.empty() && sample.fingerprint == fingerprint,
               "metadata refresh publishes immediately without changing the Git fingerprint");
  const auto log_path = fixture.state / "events" / "events.log";
  std::ifstream log_stream(log_path, std::ios::binary);
  const std::string original_log{std::istreambuf_iterator<char>(log_stream), std::istreambuf_iterator<char>()};
  log_stream.close();
  std::ofstream(log_path, std::ios::app) << "malformed event\n";
  index.refreshMetadata("sample");
  const auto failed_metadata = requireProject(index, "sample");
  requireIndex(!failed_metadata.index_error.empty() && failed_metadata.checkouts.size() == 1 &&
                   failed_metadata.events.size() == sample.events.size(),
               "a derived metadata read failure preserves the snapshot without failing its caller");
  std::ofstream(log_path, std::ios::binary | std::ios::trunc) << original_log;

  // More refs than the display cap must retain the true total count.
  const auto many = fixture.repos / "sample.git" / "refs" / "heads" / "many";
  std::filesystem::create_directories(many);
  for (std::size_t number = 0; number < ckgit::kProjectIndexRefLimit; ++number) {
    std::ofstream(many / std::to_string(number)) << sample.head_id << '\n';
  }
  index.sweep();
  sample = requireProject(index, "sample");
  requireIndex(sample.refs.size() == ckgit::kProjectIndexRefLimit && sample.refs_truncated &&
                   sample.branch_count == ckgit::kProjectIndexRefLimit + 2 && sample.tag_count == 1,
               "ref display cap preserves real branch and tag totals");

  std::string binary_readme(9000, 'x');
  binary_readme.push_back('\0');
  binary_readme += "<script>never markup</script>";
  binary_readme.push_back(static_cast<char>(0xff));
  fixture.write("ReadMe.MD", binary_readme);
  fixture.commit("2024-03-01T00:00:00+0000", "Binary README");
  fixture.publish();
  index.sweep();
  sample = requireProject(index, "sample");
  requireIndex(sample.index_error.empty() && sample.readme_content == binary_readme,
               "binary README bytes remain bounded and length-aware for the renderer to classify");

  fixture.write("ReadMe.MD", std::string(ckgit::kProjectIndexReadmeLimit + 1, 'x'));
  fixture.commit("2024-03-01T01:00:00+0000", "Large README");
  fixture.publish();
  index.start();
  const auto queued_at = std::chrono::steady_clock::now();
  index.refresh("sample");
  requireIndex(std::chrono::steady_clock::now() - queued_at < std::chrono::seconds(1),
               "refresh queues without waiting for indexing");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    sample = requireProject(index, "sample");
    if (sample.last_commit && sample.last_commit->subject == "Large README") {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  index.stop();
  requireIndex(sample.last_commit && sample.last_commit->subject == "Large README",
               "queued refresh publishes a pushed commit");
  requireIndex(sample.readme_truncated && sample.readme_content.size() == ckgit::kProjectIndexReadmeLimit,
               "README content is capped and the truncation is explicit");
  requireIndex(index.snapshot().front().name == "sample", "projects sort by newest commit first");
  const auto table_rows = index.tableSnapshot();
  requireIndex(table_rows.front().name == "sample" && table_rows.front().last_commit &&
                   table_rows.front().checkouts.size() == 1 && table_rows.front().readme_content.empty() &&
                   table_rows.front().activity.empty() && table_rows.front().refs.empty() &&
                   table_rows.front().events.empty(),
               "table snapshots retain row fields without copying source and calendar collections");
  requireIndex(index.snapshot().front().readme_content.size() == ckgit::kProjectIndexReadmeLimit,
               "full snapshots retain their original data contract");

  ckgit::createBareRepository(fixture.repos, "new-project", "main");
  index.refresh("new-project");
  requireIndex(requireProject(index, "new-project").indexing,
               "refresh immediately adds new repository placeholders");
  bool rejected = false;
  try {
    index.refresh("../outside");
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  requireIndex(rejected, "refresh validates project names");

  std::filesystem::remove_all(fixture.repos / "sample.git");
  index.sweep();
  requireIndex(!index.find("sample"), "sweep drops hand-deleted repositories");
  requireIndex(ckgit::loadCheckoutMetadata(fixture.state, "sample").empty() &&
                   ckgit::loadProjectEvents(fixture.state, "sample").empty(),
               "orphan sweep removes registrations and events of a deleted project");
  testRefreshPriority();
}
