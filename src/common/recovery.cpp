// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/recovery.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ckgit/hash.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/process.hpp"
#include "ckgit/repository_store.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t kManifestLimit = 64 * 1024 * 1024;
constexpr std::size_t kEntryLimit = 1000000;
constexpr std::chrono::hours kRecoveryTimeout{4};

class DirectoryLock {
 public:
  explicit DirectoryLock(const fs::path& path) {
    fd_ = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd_ < 0) throw std::runtime_error("could not open recovery lifecycle lock: " + path.string());
    while (flock(fd_, LOCK_EX) != 0) {
      if (errno == EINTR) continue;
      close(fd_);
      throw std::runtime_error("could not acquire recovery lifecycle lock");
    }
  }
  ~DirectoryLock() { close(fd_); }
  DirectoryLock(const DirectoryLock&) = delete;
  DirectoryLock& operator=(const DirectoryLock&) = delete;
 private:
  int fd_{-1};
};

class PrivateMask {
 public:
  PrivateMask() : previous_(umask(0077)) {}
  ~PrivateMask() { umask(previous_); }
 private:
  mode_t previous_;
};

bool entryExists(const fs::path& path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) return false;
  if (error) throw std::runtime_error("cannot inspect " + path.string() + ": " + error.message());
  return status.type() != fs::file_type::not_found;
}

fs::path directory(const fs::path& path) {
  if (!fs::is_directory(fs::symlink_status(path))) {
    throw std::runtime_error("expected an existing non-symlink directory: " + path.string());
  }
  return fs::canonical(path);
}

bool below(const fs::path& child, const fs::path& parent) {
  auto current = child.begin();
  for (const auto& part : parent) {
    if (current == child.end() || *current != part) return false;
    ++current;
  }
  return true;
}

RecoveryPaths validatedPaths(const RecoveryPaths& paths) {
  RecoveryPaths result{validatedRepositoryRoot(paths.repo_root), fs::canonical(validatedMetadataRoot(paths.state_root)), paths.hook_directory};
  if (fs::equivalent(result.repo_root, result.state_root) ||
      below(result.repo_root, result.state_root) || below(result.state_root, result.repo_root)) {
    throw std::runtime_error("repository and metadata roots must be separate, non-overlapping directories");
  }
  if (result.hook_directory.has_value()) {
    result.hook_directory = directory(*result.hook_directory);
    const auto hook = *result.hook_directory / "post-receive";
    if (!fs::is_regular_file(fs::symlink_status(hook)) || access(hook.c_str(), X_OK) != 0) {
      throw std::runtime_error("target hook directory must contain an executable non-symlink post-receive hook");
    }
  }
  return result;
}

class Stage {
 public:
  explicit Stage(const fs::path& parent) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned sequence = 0; sequence < 1000; ++sequence) {
      path_ = parent / (".ckgit-recovery-" + std::to_string(getpid()) + "-" + std::to_string(stamp) + "-" + std::to_string(sequence));
      if (mkdir(path_.c_str(), 0700) == 0) return;
      if (errno != EEXIST) break;
    }
    throw std::runtime_error("could not create private recovery staging directory");
  }
  ~Stage() { if (!keep_) { std::error_code error; fs::remove_all(path_, error); } }
  const fs::path& path() const { return path_; }
  void keep() { keep_ = true; }
 private:
  fs::path path_;
  bool keep_{false};
};

std::string readFile(const fs::path& path, std::size_t maximum = kManifestLimit) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat status {};
  if (fd < 0 || fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uintmax_t>(status.st_size) > maximum) {
    if (fd >= 0) close(fd);
    throw std::runtime_error("file is unsafe, missing, or too large: " + path.string());
  }
  std::string result;
  std::array<char,65536> buffer{};
  while (true) {
    const auto count = read(fd, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || (count > 0 && result.size() + static_cast<std::size_t>(count) > maximum)) {
      close(fd);
      throw std::runtime_error("could not read bounded recovery file: " + path.string());
    }
    if (count == 0) break;
    result.append(buffer.data(), static_cast<std::size_t>(count));
  }
  close(fd);
  return result;
}

void writeFile(const fs::path& path, const std::string& content) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("could not create " + path.string());
  std::size_t offset = 0;
  while (offset < content.size()) {
    const auto written = write(fd, content.data() + offset, content.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) { close(fd); throw std::runtime_error("could not write " + path.string()); }
    offset += static_cast<std::size_t>(written);
  }
  const int synced = fsync(fd);
  close(fd);
  if (synced != 0) throw std::runtime_error("could not sync " + path.string());
}

std::string hexPath(std::string_view path) {
  constexpr char digits[]="0123456789abcdef";
  std::string result;
  for (const unsigned char byte : path) { result += digits[byte >> 4]; result += digits[byte & 15]; }
  return result;
}

std::vector<fs::path> treeEntries(const fs::path& root) {
  directory(root);
  std::vector<fs::path> result;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    const auto status = entry.symlink_status();
    if ((!fs::is_regular_file(status) && !fs::is_directory(status)) || entry.path().string().size() > 8192) {
      throw std::runtime_error("recovery rejects symlinks and special files: " + entry.path().string());
    }
    result.push_back(entry.path().lexically_relative(root));
    if (result.size() > kEntryLimit) throw std::runtime_error("recovery file count exceeds its safety limit");
  }
  std::sort(result.begin(), result.end());
  return result;
}

void requirePrivateTree(const fs::path& root) {
  auto entries = treeEntries(root);
  entries.emplace_back();
  for (const auto& relative : entries) {
    struct stat status {};
    const auto path = relative.empty() ? root : root / relative;
    if (lstat(path.c_str(), &status) != 0 || status.st_uid != geteuid() || (status.st_mode & 0077) != 0) {
      throw std::runtime_error("private recovery data must be owned by this user with no group or other access: " + path.string());
    }
  }
}

void copyTree(const fs::path& source, const fs::path& destination) {
  const auto entries = treeEntries(source);
  fs::create_directory(destination);
  chmod(destination.c_str(), 0700);
  for (const auto& relative : entries) {
    const auto from = source / relative, to = destination / relative;
    if (fs::is_directory(fs::symlink_status(from))) {
      fs::create_directory(to);
      chmod(to.c_str(), 0700);
    } else {
      fs::copy_file(from, to, fs::copy_options::none);
      chmod(to.c_str(), 0600);
    }
  }
}

void syncDirectory(const fs::path& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("could not open recovery directory for sync");
  const auto result = fsync(fd);
  close(fd);
  if (result != 0) throw std::runtime_error("could not sync recovery directory");
}

void syncTree(const fs::path& root) {
  auto entries = treeEntries(root);
  for (auto iterator = entries.rbegin(); iterator != entries.rend(); ++iterator) {
    const auto path = root / *iterator;
    if (fs::is_directory(path)) { syncDirectory(path); continue; }
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("could not open recovery file for sync");
    const auto result = fsync(fd);
    close(fd);
    if (result != 0) throw std::runtime_error("could not sync recovery file");
  }
  syncDirectory(root);
}

std::string git(const fs::path& repository, std::initializer_list<std::string> arguments) {
  std::vector<std::string> command{"git", "--no-replace-objects", "--no-optional-locks", "-C", repository.string(),
      "-c", "core.hooksPath=/dev/null", "-c", "maintenance.auto=false", "-c", "gc.auto=0"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto result = runProcess(command, kRecoveryTimeout, 16 * 1024 * 1024);
  if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
    throw std::runtime_error("Git recovery validation failed for " + repository.string() + ": " + result.output.substr(0, 4096));
  }
  auto output = result.output;
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
  return output;
}

std::string safeConfig(std::string_view format) {
  if (format != "sha1" && format != "sha256") throw std::runtime_error("unsupported Git object format");
  return std::string("[core]\n\trepositoryformatversion = ") + (format == "sha256" ? "1" : "0") +
      "\n\tfilemode = true\n\tbare = true\n" + (format == "sha256" ? "[extensions]\n\tobjectFormat = sha256\n" : "") +
      "[receive]\n\tdenyDeletes = true\n\tdenyNonFastForwards = true\n\tadvertisePushOptions = false\n";
}

void configure(const fs::path& repository, const std::optional<fs::path>& hooks = std::nullopt) {
  const auto format = git(repository, {"rev-parse", "--show-object-format"});
  fs::remove(repository / "config");
  writeFile(repository / "config", safeConfig(format));
  if (hooks.has_value()) git(repository, {"config", "core.hooksPath", hooks->string()});
}

std::string fingerprint(const fs::path& repository) {
  if (git(repository, {"rev-parse", "--is-bare-repository"}) != "true") {
    throw std::runtime_error("hosted repository must be bare: " + repository.string());
  }
  return readFile(repository / "HEAD", 4096) + "\n" + sha256HexOfFile(repository / "config") + "\n" +
         git(repository, {"for-each-ref", "--format=%(refname) %(objectname) %(symref)"});
}

void mirror(const fs::path& source, const fs::path& destination) {
  const auto before = fingerprint(source);
  git(source, {"fsck", "--full", "--strict", "--no-reflogs"});
  git(source, {"clone", "--mirror", "--no-local", "--no-hardlinks", "--no-recurse-submodules", "--", source.string(), destination.string()});
  // Git's transport transfers symbolic refs as their resolved tips. Preserve
  // their names and link targets explicitly in the independent mirror too.
  std::istringstream symbolic(git(source, {"for-each-ref", "--format=%(refname)%09%(symref)"}));
  for (std::string line; std::getline(symbolic, line);) {
    const auto separator = line.find('\t');
    if (separator == std::string::npos) throw std::runtime_error("could not inspect symbolic hosted refs");
    if (separator + 1 < line.size()) {
      git(destination, {"symbolic-ref", line.substr(0, separator), line.substr(separator + 1)});
    }
  }
  const auto head = readFile(source / "HEAD", 4096);
  if (!head.starts_with("ref: ")) {
    // A detached HEAD can be reachable only from HEAD itself, outside refs/*.
    // Fetch its advertised object explicitly before restoring that HEAD file.
    git(destination, {"fetch", "--no-tags", "--no-prune", "--no-prune-tags", "--no-write-fetch-head",
        "--no-recurse-submodules", "--refmap=", "--", source.string(), "HEAD"});
  }
  fs::remove(destination / "HEAD");
  writeFile(destination / "HEAD", head);
  if (git(source, {"for-each-ref", "--format=%(refname) %(objectname) %(symref)"}) !=
      git(destination, {"for-each-ref", "--format=%(refname) %(objectname) %(symref)"}) || before != fingerprint(source)) {
    throw std::runtime_error("repository changed or some hosted refs were hidden during backup; retry with Git writes stopped");
  }
  configure(destination);
  git(destination, {"fsck", "--full", "--strict", "--no-reflogs"});
  for (const auto& relative : treeEntries(destination)) {
    const auto path = destination / relative;
    chmod(path.c_str(), fs::is_directory(path) ? 0700 : 0600);
  }
  chmod(destination.c_str(), 0700);
}

std::vector<std::string> projects(const fs::path& root) {
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(root)) {
    const auto filename = entry.path().filename().string();
    if (!filename.ends_with(".git")) continue;
    const auto name = filename.substr(0, filename.size() - 4);
    if (!isValidProjectName(name) || !fs::is_directory(entry.symlink_status())) {
      throw std::runtime_error("unsafe hosted repository entry: " + filename);
    }
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

void validateState(const fs::path& state, const std::vector<std::string>& names) {
  requirePrivateTree(state);
  for (const auto& entry : fs::directory_iterator(state)) {
    const auto name = entry.path().filename().string();
    if ((name != "checkouts" && name != "projects" && name != "events" && name != "ci") ||
        !fs::is_directory(entry.symlink_status())) {
      throw std::runtime_error("unsupported metadata entry in recovery: " + name);
    }
  }
  for (const auto& project : listProjectMetadataNames(state)) {
    if (std::find(names.begin(), names.end(), project) == names.end()) {
      throw std::runtime_error("metadata has no matching hosted project: " + project);
    }
    static_cast<void>(loadCheckoutMetadata(state, project));
  }
  const auto checkouts = state / "checkouts";
  if (entryExists(checkouts)) for (const auto& project : fs::directory_iterator(checkouts)) {
    if (!fs::is_directory(project.symlink_status()) || !isValidProjectName(project.path().filename().string())) {
      throw std::runtime_error("unsupported checkout metadata directory");
    }
    for (const auto& record : fs::directory_iterator(project.path())) {
      const auto filename = record.path().filename().string();
      if (!fs::is_regular_file(record.symlink_status()) || !filename.ends_with(".ini") ||
          !isValidClientId(filename.substr(0, filename.size() - 4))) {
        throw std::runtime_error("unsupported checkout metadata record");
      }
    }
  }
  const auto events = state / "events";
  if (entryExists(events)) for (const auto& entry : fs::directory_iterator(events)) {
    const auto name = entry.path().filename().string();
    if (!fs::is_regular_file(entry.symlink_status()) ||
        (name != "events.log" && !(name.starts_with("events.") && name.ends_with(".log")))) {
      throw std::runtime_error("unsupported server event metadata record");
    }
  }
  const auto ci = state / "ci";
  if (entryExists(ci)) for (const auto& entry : fs::directory_iterator(ci)) {
    const auto name = entry.path().filename().string();
    if (!fs::is_directory(entry.symlink_status()) ||
        (name != "spool" && name != "working" && name != "runs" && name != "projects")) {
      throw std::runtime_error("unsupported CI metadata entry: " + name);
    }
  }
}

std::string manifest(const fs::path& root, BackupSummary* summary) {
  std::string result = "CKGIT-BACKUP 1\nHASH SHA256\n";
  for (const auto& relative : treeEntries(root)) {
    if (relative == "manifest.sha256") continue;
    const auto path = root / relative;
    if (fs::is_directory(path)) result += "D " + hexPath(relative.generic_string()) + "\n";
    else {
      const auto bytes = fs::file_size(path);
      result += "F " + hexPath(relative.generic_string()) + " " + std::to_string(bytes) + " " + sha256HexOfFile(path) + "\n";
      ++summary->files;
      summary->bytes += bytes;
    }
    if (result.size() > kManifestLimit) throw std::runtime_error("backup manifest exceeds its safety limit");
  }
  return result;
}

void absentDestination(const fs::path& path) {
  if (entryExists(path)) throw std::runtime_error("recovery destination already exists: " + path.string());
}

void publishDirectory(const fs::path& source, const fs::path& destination) {
  // Reserve the absent destination before rename; never overwrite an existing
  // project, including an empty directory or symbolic link.
  if (mkdir(destination.c_str(), 0700) != 0) throw std::runtime_error("recovery destination already exists or cannot be reserved: " + destination.string());
  if (rename(source.c_str(), destination.c_str()) != 0) {
    rmdir(destination.c_str());
    throw std::runtime_error("could not publish verified recovery data: " + destination.string());
  }
}

void install(const RecoveryPaths& paths, Stage& stage, const std::vector<std::string>& names,
             const std::optional<fs::path>& metadata) {
  std::vector<std::pair<fs::path,fs::path>> installed;
  try {
    for (const auto& name : names) {
      const auto from = stage.path() / "repositories" / (name + ".git"), to = paths.repo_root / (name + ".git");
      publishDirectory(from, to);
      installed.emplace_back(from, to);
      if (chmod(to.c_str(), 0750) != 0) throw std::runtime_error("could not set restored repository mode");
    }
    std::vector<fs::path> metadata_entries;
    if (metadata.has_value()) for (const auto& entry : fs::directory_iterator(*metadata)) metadata_entries.push_back(entry.path());
    for (const auto& entry : metadata_entries) {
      const auto to = paths.state_root / entry.filename();
      publishDirectory(entry, to);
      installed.emplace_back(entry, to);
    }
    syncDirectory(paths.repo_root);
    syncDirectory(paths.state_root);
  } catch (const std::exception& error) {
    bool rolled_back = true;
    for (auto iterator = installed.rbegin(); iterator != installed.rend(); ++iterator) {
      std::error_code rollback_error;
      fs::rename(iterator->second, iterator->first, rollback_error);
      rolled_back = rolled_back && !rollback_error;
    }
    // Keep the verified copies on failure: a Git writer might have reached a
    // briefly installed repository before rollback. Never discard its objects.
    stage.keep();
    throw std::runtime_error(std::string(error.what()) + (rolled_back ? "; installation rolled back; recovery data retained at " : "; rollback needs manual attention; recovery data retained at ") + stage.path().string());
  }
}

std::optional<std::string> trashProject(std::string_view entry) {
  const auto split = entry.rfind(".git.");
  if (split == std::string_view::npos || !isValidProjectName(entry.substr(0, split))) return std::nullopt;
  const auto suffix = entry.substr(split + 5);
  if (suffix.empty() || suffix.front() == '.' || suffix.back() == '.' ||
      suffix.find("..") != std::string_view::npos ||
      !std::all_of(suffix.begin(), suffix.end(), [](unsigned char byte) { return (byte >= '0' && byte <= '9') || byte == '.'; })) return std::nullopt;
  return std::string(entry.substr(0, split));
}
}  // namespace

BackupSummary backupHostedRepositories(const RecoveryPaths& requested, const fs::path& output, bool dry_run) {
  const auto paths = validatedPaths(requested);
  const auto parent = directory(output.has_parent_path() ? output.parent_path() : fs::path("."));
  if (output.filename().empty() || output.filename() == "." || output.filename() == "..") throw std::runtime_error("backup output must name a new directory");
  const auto destination = parent / output.filename();
  if (below(destination, paths.repo_root) || below(destination, paths.state_root)) throw std::runtime_error("backup output must be outside repository and metadata roots");
  absentDestination(destination);
  const DirectoryLock lifecycle(paths.repo_root);
  const auto names = projects(paths.repo_root);
  if (dry_run) {
    // Public readers take their own state locks; do not hold a second state
    // descriptor's exclusive lock while invoking them during read-only preview.
    validateState(paths.state_root, names);
    for (const auto& name : names) {
      fingerprint(paths.repo_root / (name + ".git"));
      git(paths.repo_root / (name + ".git"), {"fsck", "--full", "--strict", "--no-reflogs"});
    }
    return {names, 0, 0};
  }
  const DirectoryLock state_lock(paths.state_root);
  const PrivateMask mask;
  Stage stage(parent);
  fs::create_directory(stage.path() / "repositories");
  fs::create_directory(stage.path() / "configs");
  std::map<std::string,std::string> before;
  for (const auto& name : names) before.emplace(name, fingerprint(paths.repo_root / (name + ".git")));
  requirePrivateTree(paths.state_root);
  copyTree(paths.state_root, stage.path() / "state");
  validateState(stage.path() / "state", names);
  for (const auto& name : names) {
    const auto source = paths.repo_root / (name + ".git");
    writeFile(stage.path() / "configs" / (name + ".config"), readFile(source / "config"));
    mirror(source, stage.path() / "repositories" / (name + ".git"));
  }
  for (const auto& [name, snapshot] : before) {
    if (fingerprint(paths.repo_root / (name + ".git")) != snapshot) throw std::runtime_error("hosted refs, HEAD, or config changed during backup; retry with Git writes stopped");
  }
  BackupSummary result{names, 0, 0};
  writeFile(stage.path() / "manifest.sha256", manifest(stage.path(), &result));
  static_cast<void>(verifyBackup(stage.path()));
  syncTree(stage.path());
  publishDirectory(stage.path(), destination);
  stage.keep();
  syncDirectory(parent);
  return result;
}

BackupSummary verifyBackup(const fs::path& requested) {
  const auto root = directory(requested);
  requirePrivateTree(root);
  for (const auto& entry : fs::directory_iterator(root)) {
    const auto name = entry.path().filename().string();
    if (name != "repositories" && name != "configs" && name != "state" && name != "manifest.sha256") throw std::runtime_error("unknown backup entry: " + name);
  }
  directory(root / "repositories");
  directory(root / "configs");
  validatedMetadataRoot(root / "state");
  BackupSummary result;
  if (readFile(root / "manifest.sha256") != manifest(root, &result)) throw std::runtime_error("backup manifest checksum or file inventory does not match");
  result.projects = projects(root / "repositories");
  if (std::distance(fs::directory_iterator(root / "repositories"), fs::directory_iterator{}) != static_cast<std::ptrdiff_t>(result.projects.size())) throw std::runtime_error("unknown entry in backed-up repositories");
  std::set<std::string> configs;
  for (const auto& entry : fs::directory_iterator(root / "configs")) configs.insert(entry.path().filename().string());
  for (const auto& name : result.projects) {
    if (configs.erase(name + ".config") != 1) throw std::runtime_error("backup lacks original configuration for " + name);
    const auto repository = root / "repositories" / (name + ".git");
    const auto config = readFile(repository / "config");
    // Inspect the exact safe format before invoking Git, so an untrusted
    // backup cannot inject includes, commands, hooks, or external object stores.
    if (config != safeConfig("sha1") && config != safeConfig("sha256")) throw std::runtime_error("backup mirror has unexpected executable Git settings");
    for (const auto* unsafe : {"commondir", "gitdir", "shallow", "objects/info/alternates", "objects/info/http-alternates", "info/grafts"}) {
      if (entryExists(repository / unsafe)) throw std::runtime_error("backup mirror depends on external, shallow, or grafted history");
    }
    fingerprint(repository);
    git(repository, {"fsck", "--full", "--strict", "--no-reflogs"});
  }
  if (!configs.empty()) throw std::runtime_error("backup contains unmatched original configurations");
  validateState(root / "state", result.projects);
  return result;
}

std::vector<std::string> restoreBackup(const RecoveryPaths& requested, const fs::path& backup, bool dry_run) {
  const auto summary = verifyBackup(backup);
  const auto paths = validatedPaths(requested);
  const auto source = directory(backup);
  if (below(paths.repo_root, source) || below(paths.state_root, source)) {
    throw std::runtime_error("restore destinations must be outside the retained backup directory");
  }
  const DirectoryLock lifecycle(paths.repo_root);
  const DirectoryLock state_lock(paths.state_root);
  if (!fs::is_empty(paths.state_root)) throw std::runtime_error("full restore requires an empty target metadata directory; existing metadata is never overwritten");
  for (const auto& name : summary.projects) absentDestination(paths.repo_root / (name + ".git"));
  if (dry_run) return summary.projects;
  const PrivateMask mask;
  Stage stage(paths.repo_root);
  copyTree(directory(backup) / "repositories", stage.path() / "repositories");
  copyTree(directory(backup) / "state", stage.path() / "state");
  copyTree(directory(backup) / "configs", stage.path() / "configs");
  writeFile(stage.path() / "manifest.sha256", readFile(directory(backup) / "manifest.sha256"));
  if (verifyBackup(stage.path()).projects != summary.projects) throw std::runtime_error("backup changed while preparing restore; retry verification");
  for (const auto& name : summary.projects) {
    const auto repository = stage.path() / "repositories" / (name + ".git");
    configure(repository, paths.hook_directory);
    git(repository, {"fsck", "--full", "--strict", "--no-reflogs"});
  }
  syncTree(stage.path());
  // Metadata may reside on a different filesystem from repositories. Its
  // publication must use a same-filesystem staging directory as well.
  Stage state_stage(paths.state_root);
  copyTree(stage.path() / "state", state_stage.path() / "state");
  syncTree(state_stage.path());
  try { install(paths, stage, summary.projects, state_stage.path() / "state"); }
  catch (...) { state_stage.keep(); throw; }
  return summary.projects;
}

std::vector<TrashedRepository> listTrashedRepositories(const RecoveryPaths& requested) {
  const auto paths = validatedPaths(requested);
  const DirectoryLock lifecycle(paths.repo_root);
  const auto trash = paths.repo_root / ".trash";
  if (!entryExists(trash)) return {};
  directory(trash);
  std::vector<TrashedRepository> result;
  for (const auto& entry : fs::directory_iterator(trash)) {
    const auto name = entry.path().filename().string();
    const auto project = trashProject(name);
    if (!project.has_value() || !fs::is_directory(entry.symlink_status())) throw std::runtime_error("unsafe or unrecognized trash entry: " + name);
    result.push_back({name, *project});
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.entry < right.entry; });
  return result;
}

fs::path restoreTrashedRepository(const RecoveryPaths& requested, std::string_view entry,
                                  const std::optional<std::string>& name, bool dry_run) {
  const auto original = trashProject(entry);
  if (!original.has_value() || (name.has_value() && !isValidProjectName(*name))) throw std::runtime_error("invalid trash entry or restored project name");
  const auto paths = validatedPaths(requested);
  const DirectoryLock lifecycle(paths.repo_root);
  const auto project = name.value_or(*original);
  const auto destination = paths.repo_root / (project + ".git");
  absentDestination(destination);
  const auto source = directory(paths.repo_root / ".trash") / std::string(entry);
  directory(source);
  // This call takes the state reader lock itself, before the exclusive lock.
  const auto existing_metadata = listProjectMetadataNames(paths.state_root);
  if (std::find(existing_metadata.begin(), existing_metadata.end(), project) != existing_metadata.end()) throw std::runtime_error("restored name already has metadata; resolve that collision before restoring Git");
  const DirectoryLock state_lock(paths.state_root);
  fingerprint(source);
  git(source, {"fsck", "--full", "--strict", "--no-reflogs"});
  if (dry_run) return destination;
  const PrivateMask mask;
  Stage stage(paths.repo_root);
  fs::create_directory(stage.path() / "repositories");
  mirror(source, stage.path() / "repositories" / (project + ".git"));
  configure(stage.path() / "repositories" / (project + ".git"), paths.hook_directory);
  syncTree(stage.path());
  install(paths, stage, {project}, std::nullopt);
  return destination;
}

}  // namespace ckgit
