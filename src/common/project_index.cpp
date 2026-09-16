// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/project_index.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "ckgit/process.hpp"
#include "ckgit/repository_store.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kSweepInterval = std::chrono::seconds(60);
constexpr auto kProjectDeadline = std::chrono::seconds(60);
constexpr std::size_t kRefOutputLimit = 8 * 1024 * 1024;
constexpr std::size_t kTreeOutputLimit = 4 * 1024 * 1024;
constexpr std::size_t kActivityOutputLimit = (kProjectIndexCommitLimit + 1) * 24;
constexpr std::size_t kCommitListingOutputLimit = (kProjectIndexCommitLimit + 1) * 90;
// Hooks get prompt service without starving startup or sweep progress when
// a busy project receives pushes continuously.
constexpr std::size_t kUrgentRefreshBurst = 4;

std::uint64_t unsignedNumber(std::string_view value) {
  std::uint64_t number{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? number : 0;
}

std::string trimNewlines(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

std::vector<std::string_view> split(std::string_view value, char delimiter) {
  std::vector<std::string_view> fields;
  for (std::size_t start = 0; start <= value.size();) {
    const auto end = value.find(delimiter, start);
    fields.push_back(value.substr(start, end == value.npos ? end : end - start));
    if (end == value.npos) {
      break;
    }
    start = end + 1;
  }
  return fields;
}

bool isObjectId(std::string_view value) {
  return (value.size() == 40 || value.size() == 64) &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

bool isPlainDirectory(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_directory(std::filesystem::symlink_status(path, error)) && !error;
}

bool isRepositoryDirectory(const std::filesystem::path& path) {
  if (!isPlainDirectory(path) || !isPlainDirectory(path / "objects")) {
    return false;
  }
  std::error_code error;
  return std::filesystem::is_regular_file(std::filesystem::symlink_status(path / "HEAD", error)) &&
         !error;
}

std::set<std::string> inventory(const std::filesystem::path& root) {
  std::set<std::string> names;
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  if (error) {
    throw std::runtime_error("cannot inspect hosted project root");
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const auto path = iterator->path();
    const auto filename = path.filename().string();
    if (filename.size() > 4 && filename.ends_with(".git")) {
      const auto name = filename.substr(0, filename.size() - 4);
      if (isValidProjectName(name) && isRepositoryDirectory(path)) {
        names.insert(name);
      }
    }
    iterator.increment(error);
    if (error) {
      // Never interpret a partial inventory as permission to delete records
      // (or their on-disk metadata).
      throw std::runtime_error("cannot complete hosted project inventory");
    }
  }
  return names;
}

std::string readHead(const std::filesystem::path& path) {
  const int descriptor = open((path / "HEAD").c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::runtime_error("cannot read repository HEAD");
  }
  struct stat status {};
  std::array<char, 4097> buffer{};
  const bool regular = fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode);
  const auto count = regular ? read(descriptor, buffer.data(), buffer.size()) : -1;
  close(descriptor);
  if (count < 0 || static_cast<std::size_t>(count) == buffer.size()) {
    throw std::runtime_error("invalid repository HEAD");
  }
  return trimNewlines(std::string(buffer.data(), static_cast<std::size_t>(count)));
}

std::uint64_t fingerprint(std::string_view refs, std::string_view head) {
  std::uint64_t hash = 14695981039346656037ULL;
  const auto append = [&hash](std::string_view bytes) {
    for (const unsigned char byte : bytes) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
  };
  append(refs);
  append(std::string_view("\0", 1));
  append(head);
  return hash;
}

std::uint64_t nowEpoch() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string utcDay(std::uint64_t epoch) {
  if (epoch > static_cast<std::uint64_t>(std::numeric_limits<time_t>::max())) {
    return {};
  }
  const auto time = static_cast<time_t>(epoch);
  std::tm broken_down{};
  if (gmtime_r(&time, &broken_down) == nullptr || broken_down.tm_year < -1900 ||
      broken_down.tm_year > 8099) {
    return {};
  }
  std::array<char, 11> formatted{};
  if (std::strftime(formatted.data(), formatted.size(), "%Y-%m-%d", &broken_down) == 0) {
    return {};
  }
  return formatted.data();
}

class Inspection {
 public:
  explicit Inspection(std::filesystem::path repository)
      : repository_(std::move(repository)), deadline_(Clock::now() + kProjectDeadline) {}

  ProcessResult git(std::initializer_list<std::string> arguments, std::size_t limit) const {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - Clock::now());
    if (remaining.count() <= 0) {
      throw std::runtime_error("project indexing exceeded 60 seconds");
    }
    std::vector<std::string> command{"git", "--no-optional-locks", "--no-pager", "--git-dir",
                                     repository_.string(), "-c", "log.showSignature=false", "-c",
                                     "core.commitGraph=false"};
    command.insert(command.end(), arguments.begin(), arguments.end());
    const auto result = runProcess(command, remaining, limit);
    if (result.timed_out) {
      throw std::runtime_error("project indexing exceeded 60 seconds");
    }
    return result;
  }

  std::string require(std::initializer_list<std::string> arguments, std::size_t limit) const {
    const auto result = git(arguments, limit);
    if (result.exit_code != 0 || result.output_truncated) {
      throw std::runtime_error(result.output_truncated ? "project indexing output limit exceeded"
                                                       : "Git could not inspect repository");
    }
    return result.output;
  }

 private:
  std::filesystem::path repository_;
  Clock::time_point deadline_;
};

void readRefs(ProjectSummary& summary, const std::string& listing) {
  for (const auto line : split(listing, '\n')) {
    if (line.empty()) {
      continue;
    }
    const auto fields = split(line, '\t');
    if (fields.size() != 4 || !isObjectId(fields[1])) {
      throw std::runtime_error("Git returned an invalid ref record");
    }
    ProjectRef ref;
    if (fields[0].starts_with("refs/heads/")) {
      ++summary.branch_count;
      ref.name = fields[0].substr(11);
    } else if (fields[0].starts_with("refs/tags/")) {
      ++summary.tag_count;
      ref.name = fields[0].substr(10);
      ref.is_tag = true;
    } else {
      continue;
    }
    // A commit's peeled ID is used for browsing; an annotated tag's own
    // creation (tagger) date is kept distinct from the commit it points at.
    ref.id = isObjectId(fields[2]) ? fields[2] : fields[1];
    ref.epoch_seconds = unsignedNumber(fields[3]);
    summary.refs.push_back(std::move(ref));
  }
  std::sort(summary.refs.begin(), summary.refs.end(), [](const ProjectRef& left, const ProjectRef& right) {
    if (left.epoch_seconds != right.epoch_seconds) {
      return left.epoch_seconds > right.epoch_seconds;
    }
    return left.name != right.name ? left.name < right.name : left.is_tag < right.is_tag;
  });
  summary.refs_truncated = summary.refs.size() > kProjectIndexRefLimit;
  if (summary.refs_truncated) {
    summary.refs.resize(kProjectIndexRefLimit);
  }
}

void readLastCommit(ProjectSummary& summary, const Inspection& inspection) {
  // Committer timestamps need not increase along a branch. A one-commit
  // log only examines tips and can miss a later-dated ancestor, so choose
  // the maximum timestamp over a bounded walk of every advertised ref.
  const auto listing = inspection.git(
      {"log", "--all", "--format=%ct%x09%H",
       "--max-count=" + std::to_string(kProjectIndexCommitLimit + 1)}, kCommitListingOutputLimit);
  if (listing.output_truncated) {
    throw std::runtime_error("commit index output limit exceeded");
  }
  if (listing.exit_code != 0 || listing.output.empty()) {
    if (summary.branch_count != 0 || summary.tag_count != 0) {
      throw std::runtime_error("Git could not read the last commit");
    }
    return;
  }
  std::string newest_id;
  std::uint64_t newest_epoch{};
  std::size_t count{};
  for (const auto line : split(listing.output, '\n')) {
    if (line.empty()) {
      continue;
    }
    if (count++ == kProjectIndexCommitLimit) {
      summary.history_truncated = true;
      break;
    }
    const auto fields = split(line, '\t');
    if (fields.size() != 2 || !isObjectId(fields[1])) {
      throw std::runtime_error("Git returned an invalid commit record");
    }
    const auto epoch = unsignedNumber(fields[0]);
    if (newest_id.empty() || epoch > newest_epoch) {
      newest_id = fields[1];
      newest_epoch = epoch;
    }
  }
  if (newest_id.empty()) {
    return;
  }
  // Put the bounded subject last so a huge commit message cannot hide its ID
  // or time. Names and subjects are escaped only by the HTML page builder.
  const auto result = inspection.git({"show", "--no-patch", "--format=%H%x00%ct%x00%an%x00%s", newest_id, "--"},
                                     16 * 1024);
  if (result.exit_code != 0 || result.output.empty()) {
    if (summary.branch_count != 0 || summary.tag_count != 0) {
      throw std::runtime_error("Git could not read the last commit");
    }
    return;
  }
  const auto fields = split(result.output, '\0');
  if (fields.size() < 4 || !isObjectId(fields[0])) {
    throw std::runtime_error("Git returned an invalid commit record");
  }
  CommitSummary commit;
  commit.id = fields[0];
  commit.epoch_seconds = unsignedNumber(fields[1]);
  commit.author = fields[2].substr(0, 1024);
  commit.subject = trimNewlines(std::string(fields[3].substr(0, 4096)));
  summary.last_commit_epoch_seconds = commit.epoch_seconds;
  summary.last_commit = std::move(commit);
}

void readSize(ProjectSummary& summary, const Inspection& inspection) {
  const auto output = inspection.require({"count-objects", "-v"}, 64 * 1024);
  for (const auto line : split(output, '\n')) {
    const auto separator = line.find(": ");
    if (separator == line.npos) {
      continue;
    }
    const auto key = line.substr(0, separator);
    const auto value = unsignedNumber(line.substr(separator + 2));
    if (key == "count" || key == "in-pack") {
      summary.object_count += value;
    } else if (key == "size" || key == "size-pack") {
      summary.size_bytes += value * 1024;
    }
  }
}

void readActivity(ProjectSummary& summary, const Inspection& inspection) {
  if (!summary.valid_head) {
    return;
  }
  const auto output = inspection.require(
      {"log", "--format=%ct", "--max-count=" + std::to_string(kProjectIndexCommitLimit + 1),
       summary.head_id, "--"}, kActivityOutputLimit);
  std::size_t count = 0;
  for (const auto line : split(output, '\n')) {
    if (line.empty()) {
      continue;
    }
    if (count++ == kProjectIndexCommitLimit) {
      summary.history_truncated = true;
      break;
    }
    const auto day = utcDay(unsignedNumber(line));
    if (!day.empty()) {
      ++summary.activity[day];
    }
  }
}

void readReadme(ProjectSummary& summary, const Inspection& inspection) {
  if (!summary.valid_head) {
    return;
  }
  const auto output = inspection.require({"ls-tree", "-z", summary.head_id}, kTreeOutputLimit);
  constexpr std::array<std::string_view, 4> candidates{"readme.md", "readme.markdown", "readme", "readme.txt"};
  std::size_t best = candidates.size();
  std::string object_id;
  for (const auto record : split(output, '\0')) {
    const auto tab = record.find('\t');
    if (tab == record.npos) {
      continue;
    }
    const auto info = split(record.substr(0, tab), ' ');
    if (info.size() != 3 || info[1] != "blob" || info[0] == "120000" || !isObjectId(info[2])) {
      continue;
    }
    const auto name = record.substr(tab + 1);
    std::string lower(name);
    for (char& character : lower) {
      if (character >= 'A' && character <= 'Z') {
        character = static_cast<char>(character + ('a' - 'A'));
      }
    }
    const auto found = std::find(candidates.begin(), candidates.end(), lower);
    const auto rank = static_cast<std::size_t>(found - candidates.begin());
    if (rank < best) {
      best = rank;
      summary.readme_path = name;
      object_id = info[2];
    }
  }
  if (!object_id.empty()) {
    const auto content = inspection.git({"cat-file", "blob", object_id}, kProjectIndexReadmeLimit);
    if (content.exit_code != 0) {
      throw std::runtime_error("Git could not read README");
    }
    summary.readme_content = content.output;
    summary.readme_truncated = content.output_truncated;
  }
}

}  // namespace

class ProjectIndex::Impl {
 public:
  Impl(const std::filesystem::path& repo_root,
       const std::optional<std::filesystem::path>& state_root)
      : root_(validatedRepositoryRoot(repo_root)) {
    if (state_root) {
      state_root_ = validatedMetadataRoot(*state_root);
    }
    for (const auto& name : inventory(root_)) {
      ProjectSummary placeholder;
      placeholder.name = name;
      placeholder.indexing = true;
      records_.emplace(name, std::move(placeholder));
      pending_.insert(name);
    }
  }

  ~Impl() { stop(); }

  void start() {
    std::lock_guard lock(mutex_);
    if (worker_.joinable()) {
      return;
    }
    stopping_ = false;
    worker_ = std::thread([this] { run(); });
  }

  void stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void refresh(std::string_view name) {
    if (!isValidProjectName(name)) {
      throw std::invalid_argument("invalid project name for index refresh");
    }
    const bool exists = isRepositoryDirectory(root_ / (std::string(name) + ".git"));
    {
      std::lock_guard lock(mutex_);
      if (exists && !records_.contains(std::string(name))) {
        ProjectSummary placeholder;
        placeholder.name = name;
        placeholder.indexing = true;
        records_.emplace(std::string(name), std::move(placeholder));
      }
      pending_.erase(std::string(name));
      if (urgent_.insert(std::string(name)).second) {
        urgent_order_.emplace_back(name);
      }
    }
    wake_.notify_one();
  }

  void refreshMetadata(std::string_view name) {
    if (!isValidProjectName(name)) {
      throw std::invalid_argument("invalid project name for metadata refresh");
    }
    ProjectSummary updated;
    updated.name = name;
    std::string error;
    try {
      metadata(updated);
    } catch (const std::exception& failure) {
      // This cache refresh follows a successful create/register operation.
      // A derived read error must never turn that success into a failed RPC.
      error = failure.what();
    }
    const bool exists = isRepositoryDirectory(root_ / (std::string(name) + ".git"));
    std::lock_guard lock(mutex_);
    if (exists && !records_.contains(std::string(name))) {
      ProjectSummary placeholder;
      placeholder.name = name;
      placeholder.indexing = true;
      records_.emplace(std::string(name), std::move(placeholder));
    }
    const auto found = records_.find(std::string(name));
    if (found != records_.end()) {
      if (error.empty()) {
        found->second.checkouts = std::move(updated.checkouts);
        found->second.events = std::move(updated.events);
        found->second.ci_runs = std::move(updated.ci_runs);
      } else {
        found->second.index_error = std::move(error);
      }
      ++metadata_revisions_[std::string(name)];
    }
  }

  void erase(std::string_view name) {
    std::lock_guard lock(mutex_);
    records_.erase(std::string(name));
    pending_.erase(std::string(name));
    urgent_.erase(std::string(name));
    std::erase(urgent_order_, std::string(name));
    metadata_revisions_.erase(std::string(name));
  }

  std::vector<ProjectSummary> snapshot(bool table_only = false) const {
    std::vector<ProjectSummary> records;
    {
      std::lock_guard lock(mutex_);
      records.reserve(records_.size());
      for (const auto& [name, record] : records_) {
        static_cast<void>(name);
        if (!table_only) {
          records.push_back(record);
          continue;
        }
        ProjectSummary row;
        row.name = record.name;
        row.default_branch = record.default_branch;
        row.valid_head = record.valid_head;
        row.branch_count = record.branch_count;
        row.tag_count = record.tag_count;
        row.last_commit_epoch_seconds = record.last_commit_epoch_seconds;
        row.checkouts = record.checkouts;
        row.last_commit = record.last_commit;
        row.head_id = record.head_id;
        row.size_bytes = record.size_bytes;
        row.object_count = record.object_count;
        row.fingerprint = record.fingerprint;
        row.generated_epoch_seconds = record.generated_epoch_seconds;
        row.indexing = record.indexing;
        row.index_error = record.index_error;
        records.push_back(std::move(row));
      }
    }
    std::sort(records.begin(), records.end(), [](const ProjectSummary& left, const ProjectSummary& right) {
      return left.last_commit_epoch_seconds != right.last_commit_epoch_seconds
                 ? left.last_commit_epoch_seconds > right.last_commit_epoch_seconds
                 : left.name < right.name;
    });
    return records;
  }

  std::optional<ProjectSummary> find(std::string_view name) const {
    std::lock_guard lock(mutex_);
    const auto found = records_.find(std::string(name));
    return found == records_.end() ? std::nullopt : std::optional<ProjectSummary>(found->second);
  }

  std::optional<std::string> readCiLog(std::string_view project, std::string_view run_id,
                                       std::size_t step) const {
    if (!state_root_ || !isValidProjectName(project)) return std::nullopt;
    return readCiRunLog(*state_root_, project, run_id, step, kProjectIndexCiLogLimit);
  }

  std::optional<std::string> readCiArtifact(std::string_view project, std::string_view run_id,
                                            std::string_view artifact_name) const {
    if (!state_root_ || !isValidProjectName(project)) return std::nullopt;
    return ckgit::readCiArtifact(*state_root_, project, run_id, artifact_name, kProjectIndexCiArtifactLimit);
  }

  void sweep(bool background = false) {
    std::lock_guard work_lock(work_mutex_);
    const auto names = inventory(root_);
    {
      std::lock_guard lock(mutex_);
      for (auto iterator = records_.begin(); iterator != records_.end();) {
        if (!names.contains(iterator->first)) {
          metadata_revisions_.erase(iterator->first);
          iterator = records_.erase(iterator);
        } else {
          ++iterator;
        }
      }
      for (const auto& name : names) {
        if (!records_.contains(name)) {
          ProjectSummary placeholder;
          placeholder.name = name;
          placeholder.indexing = true;
          records_.emplace(name, std::move(placeholder));
        }
      }
    }
    if (state_root_) {
      sweepOrphanProjectMetadata(root_, *state_root_);
    }
    for (const auto& name : names) {
      if (background && stopping()) {
        return;
      }
      // A sweep can span many repositories. Serve bounded hook work between
      // projects instead of making a fresh push wait for the full inventory.
      drainUrgent(background);
      if (background && stopping()) {
        return;
      }
      {
        std::lock_guard lock(mutex_);
        pending_.erase(name);
      }
      rebuild(name, false);
    }
  }

 private:
  // Caller holds mutex_. FIFO order ensures a repeatedly refreshed name
  // cannot jump ahead of another project's already-queued hook.
  std::string takeUrgent() {
    if (urgent_order_.empty()) {
      return {};
    }
    auto name = std::move(urgent_order_.front());
    urgent_order_.pop_front();
    urgent_.erase(name);
    return name;
  }

  // Caller holds work_mutex_, but never mutex_ while rebuilding.
  void drainUrgent(bool honor_stop) {
    for (std::size_t count = 0; count < kUrgentRefreshBurst; ++count) {
      std::string name;
      {
        std::lock_guard lock(mutex_);
        if (honor_stop && stopping_) {
          return;
        }
        name = takeUrgent();
      }
      if (name.empty()) {
        return;
      }
      rebuild(name, true);
    }
  }

  bool stopping() const {
    std::lock_guard lock(mutex_);
    return stopping_;
  }

  void metadata(ProjectSummary& summary) const {
    if (state_root_) {
      summary.checkouts = loadCheckoutMetadata(*state_root_, summary.name);
      summary.events = loadProjectEvents(*state_root_, summary.name);
      summary.ci_runs = loadCiRuns(*state_root_, summary.name);
    }
  }

  // Caller holds work_mutex_. Git work never holds the snapshot mutex.
  void rebuild(const std::string& name, bool force) {
    const auto repository = root_ / (name + ".git");
    if (!isRepositoryDirectory(repository)) {
      erase(name);
      return;
    }
    ProjectSummary summary;
    summary.name = name;
    std::uint64_t metadata_revision{};
    {
      std::lock_guard lock(mutex_);
      metadata_revision = metadata_revisions_[name];
    }
    try {
      const Inspection inspection(repository);
      const auto refs = inspection.require(
          {"for-each-ref", "--format=%(refname)%09%(objectname)%09%(*objectname)%09%(creatordate:unix)",
           "refs/heads", "refs/tags"}, kRefOutputLimit);
      const auto head = readHead(repository);
      summary.fingerprint = fingerprint(refs, head);
      const auto old = find(name);
      if (!force && old && !old->indexing && old->index_error.empty() &&
          old->fingerprint == summary.fingerprint) {
        // Metadata can change without Git refs (e.g. checkout registration).
        // A no-op sweep preserves the generation time and all Git-derived data.
        summary = *old;
        metadata(summary);
      } else {
        constexpr std::string_view prefix = "ref: refs/heads/";
        if (head.starts_with(prefix) && isValidBranchName(std::string_view(head).substr(prefix.size()))) {
          summary.default_branch = head.substr(prefix.size());
        }
        readRefs(summary, refs);
        const auto resolved = inspection.git({"rev-parse", "--verify", "--end-of-options", "HEAD^{commit}"}, 4096);
        if (resolved.exit_code == 0 && !resolved.output_truncated) {
          summary.head_id = trimNewlines(resolved.output);
          summary.valid_head = isObjectId(summary.head_id);
        }
        readLastCommit(summary, inspection);
        readSize(summary, inspection);
        readActivity(summary, inspection);
        readReadme(summary, inspection);
        metadata(summary);
        summary.generated_epoch_seconds = nowEpoch();
      }
    } catch (const std::exception& error) {
      // Keep a previous usable snapshot during transient errors. The error
      // forces another attempt at the next sweep, even if refs did not move.
      if (const auto old = find(name); old && !old->indexing) {
        summary = *old;
      }
      summary.index_error = error.what();
      summary.indexing = false;
    }
    // Do not resurrect an entry removed while Git was running.
    if (!isRepositoryDirectory(repository)) {
      erase(name);
      return;
    }
    std::lock_guard lock(mutex_);
    // An immediate registration update wins over a rebuild that started
    // earlier and read metadata before that registration was committed.
    if (metadata_revisions_[name] != metadata_revision) {
      if (const auto found = records_.find(name); found != records_.end()) {
        summary.checkouts = found->second.checkouts;
        summary.events = found->second.events;
      }
    }
    records_[name] = std::move(summary);
  }

  void run() {
    auto next_sweep = Clock::now() + kSweepInterval;
    std::size_t urgent_burst = 0;
    for (;;) {
      std::string name;
      {
        std::unique_lock lock(mutex_);
        wake_.wait_until(lock, next_sweep, [this] {
          return stopping_ || !urgent_.empty() || !pending_.empty();
        });
        if (stopping_) {
          return;
        }
        if (Clock::now() < next_sweep) {
          if (!urgent_.empty() && (urgent_burst < kUrgentRefreshBurst || pending_.empty())) {
            name = takeUrgent();
            ++urgent_burst;
          } else if (!pending_.empty()) {
            const auto first = pending_.begin();
            name = *first;
            pending_.erase(first);
            urgent_burst = 0;
          }
        }
      }
      try {
        if (!name.empty()) {
          std::lock_guard work_lock(work_mutex_);
          rebuild(name, true);
        } else {
          sweep(true);
          urgent_burst = 0;
          next_sweep = Clock::now() + kSweepInterval;
        }
      } catch (const std::exception&) {
        // A temporarily unreadable root must not terminate the daemon or
        // remove valid records. Retry on the next scheduled sweep.
        next_sweep = Clock::now() + kSweepInterval;
      }
    }
  }

  std::filesystem::path root_;
  std::optional<std::filesystem::path> state_root_;
  mutable std::mutex mutex_;
  std::mutex work_mutex_;
  std::condition_variable wake_;
  std::map<std::string, ProjectSummary> records_;
  std::map<std::string, std::uint64_t> metadata_revisions_;
  std::set<std::string> pending_;
  std::set<std::string> urgent_;
  std::deque<std::string> urgent_order_;
  bool stopping_{false};
  std::thread worker_;
};

ProjectIndex::ProjectIndex(const std::filesystem::path& repo_root,
                         const std::optional<std::filesystem::path>& state_root)
    : impl_(std::make_unique<Impl>(repo_root, state_root)) {}
ProjectIndex::~ProjectIndex() = default;
void ProjectIndex::start() { impl_->start(); }
void ProjectIndex::stop() { impl_->stop(); }
void ProjectIndex::refresh(std::string_view name) { impl_->refresh(name); }
void ProjectIndex::refreshMetadata(std::string_view name) { impl_->refreshMetadata(name); }
void ProjectIndex::sweep() { impl_->sweep(); }
void ProjectIndex::erase(std::string_view name) { impl_->erase(name); }
std::vector<ProjectSummary> ProjectIndex::snapshot() const { return impl_->snapshot(); }
std::vector<ProjectSummary> ProjectIndex::tableSnapshot() const { return impl_->snapshot(true); }
std::optional<ProjectSummary> ProjectIndex::find(std::string_view name) const { return impl_->find(name); }
std::optional<std::string> ProjectIndex::readCiLog(std::string_view project, std::string_view run_id,
                                                   std::size_t step_index) const {
  return impl_->readCiLog(project, run_id, step_index);
}
std::optional<std::string> ProjectIndex::readCiArtifact(std::string_view project, std::string_view run_id,
                                                        std::string_view artifact_name) const {
  return impl_->readCiArtifact(project, run_id, artifact_name);
}

}  // namespace ckgit
