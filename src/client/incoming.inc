// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

// Included inside main.cpp's anonymous namespace, after the shared client
// helpers. Incoming transfers deliberately bypass configured fetch refspecs:
// those may write local branches or prune tags, neither of which is authorized.
constexpr std::chrono::seconds kIncomingInspectionTimeout{120};

struct IncomingOptions {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::filesystem::path> repository;
  std::optional<std::string> project;
  std::vector<std::string> branches;
  bool all{false};
  bool tags{true};
  bool dry_run{false};
  bool verbose{false};
};

// Git status may otherwise rewrite the index even during a preview. Virtual
// replacement history must not turn divergent stored commits into an apparent
// fast-forward. These settings also cover the shared repository audit.
class IncomingGitEnvironment {
 public:
  IncomingGitEnvironment() {
    try {
      set("GIT_OPTIONAL_LOCKS", "0");
      set("GIT_NO_REPLACE_OBJECTS", "1");
      set("GIT_GRAFT_FILE", "/dev/null");
      // Git warns even about an empty explicitly selected graft file. Shared
      // audits capture stderr together with porcelain output, so suppress this
      // notice for every Git child, without discarding existing config pairs.
      unsigned count = 0;
      const char* configured = std::getenv("GIT_CONFIG_COUNT");
      if (configured != nullptr && *configured != '\0') {
        const std::string value(configured);
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
        if (error != std::errc{} || end != value.data() + value.size() || count > 10000) {
          throw std::runtime_error("invalid GIT_CONFIG_COUNT; inspect the Git environment before retrying");
        }
      }
      set("GIT_CONFIG_KEY_" + std::to_string(count), "advice.graftFileDeprecated");
      set("GIT_CONFIG_VALUE_" + std::to_string(count), "false");
      set("GIT_CONFIG_COUNT", std::to_string(count + 1));
    } catch (...) {
      restore();
      throw;
    }
  }
  ~IncomingGitEnvironment() { restore(); }
 private:
  void set(const std::string& name, const std::string& value) {
    const char* previous = std::getenv(name.c_str());
    environment_.emplace_back(name, previous == nullptr ? std::nullopt : std::optional<std::string>(previous));
    if (setenv(name.c_str(), value.c_str(), 1) != 0) throw std::runtime_error("could not configure safe Git inspection");
  }
  void restore() const {
    for (auto entry = environment_.rbegin(); entry != environment_.rend(); ++entry) {
      if (entry->second.has_value()) setenv(entry->first.c_str(), entry->second->c_str(), 1);
      else unsetenv(entry->first.c_str());
    }
  }
  std::vector<std::pair<std::string, std::optional<std::string>>> environment_;
};

ckgit::ProcessResult incomingGit(const std::filesystem::path& path,
                                 std::initializer_list<std::string> arguments) {
  std::vector<std::string> command{"git", "--no-optional-locks", "-C", path.string()};
  command.insert(command.end(), arguments.begin(), arguments.end());
  return ckgit::runProcess(command, kIncomingInspectionTimeout, kTransferOutputLimit);
}

std::string incomingOutput(const ckgit::ProcessResult& result, std::string_view operation) {
  if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
    throw std::runtime_error(std::string(operation) + " (" +
                             describeProcessFailure(result, kIncomingInspectionTimeout) + ")");
  }
  auto output = result.output;
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
    output.pop_back();
  }
  return output;
}

// Retain directory handles across the network transfer. Comparing their device
// and inode catches a moved/replaced checkout, including a replaced .git folder
// at the same spelling. Re-auditing also checks the selected project and HEAD.
class IncomingIdentity {
 public:
  explicit IncomingIdentity(const ckgit::RepositoryAudit& audit) : path_(audit.path) {
    git_path_ = incomingOutput(incomingGit(path_, {"rev-parse", "--absolute-git-dir"}),
                               "could not locate checkout metadata");
    common_path_ = incomingOutput(incomingGit(path_, {"rev-parse", "--git-common-dir"}),
                                  "could not locate shared checkout metadata");
    if (common_path_.is_relative()) common_path_ = path_ / common_path_;
    common_path_ = std::filesystem::canonical(common_path_);
    root_fd_ = open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    git_fd_ = open(git_path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    common_fd_ = open(common_path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd_ < 0 || git_fd_ < 0 || common_fd_ < 0) {
      if (root_fd_ >= 0) close(root_fd_);
      if (git_fd_ >= 0) close(git_fd_);
      if (common_fd_ >= 0) close(common_fd_);
      throw std::runtime_error("checkout or its Git metadata is unavailable; inspect its location before retrying");
    }
  }
  ~IncomingIdentity() {
    close(root_fd_);
    close(git_fd_);
    close(common_fd_);
  }
  IncomingIdentity(const IncomingIdentity&) = delete;
  IncomingIdentity& operator=(const IncomingIdentity&) = delete;

  void verify() const {
    const auto unchanged = [](int fd, const std::filesystem::path& path) {
      struct stat held {}, current {};
      return fstat(fd, &held) == 0 && lstat(path.c_str(), &current) == 0 && S_ISDIR(current.st_mode) &&
             held.st_dev == current.st_dev && held.st_ino == current.st_ino;
    };
    auto common = std::filesystem::path(incomingOutput(incomingGit(path_, {"rev-parse", "--git-common-dir"}),
                                                       "could not recheck shared checkout metadata"));
    if (common.is_relative()) common = path_ / common;
    if (!unchanged(root_fd_, path_) || !unchanged(git_fd_, git_path_) || !unchanged(common_fd_, common_path_) ||
        std::filesystem::canonical(common) != common_path_ ||
        incomingOutput(incomingGit(path_, {"rev-parse", "--absolute-git-dir"}),
                       "could not recheck checkout metadata") != git_path_.string()) {
      throw std::runtime_error("checkout identity changed during the operation; inspect its location and retry");
    }
  }

 private:
  std::filesystem::path path_;
  std::filesystem::path git_path_;
  std::filesystem::path common_path_;
  int root_fd_{-1};
  int git_fd_{-1};
  int common_fd_{-1};
};

void requireIncomingPairing(const ckgit::RepositoryAudit& audit, const ckgit::ClientConfig& config,
                            const std::string& project) {
  bool invalid = false;
  const auto paired = pairedProjectForAudit(audit, config, &invalid);
  if (invalid || !paired.has_value() || *paired != project) {
    throw std::runtime_error("checkout is not paired with " + project + " on " + config.server +
                             "; inspect remote " + config.remote_name + " before retrying");
  }
}

std::string incomingHead(const ckgit::RepositoryAudit& audit) {
  return incomingOutput(incomingGit(audit.path, {"rev-parse", "--verify", "HEAD^{commit}"}),
                        "current branch has no commit; use ckgit clone for a new checkout");
}

void requireUpdateReady(const ckgit::RepositoryAudit& audit) {
  if (audit.detached_head || audit.current_branch.empty()) {
    throw std::runtime_error("detached HEAD; switch to the local branch you want to update, then retry");
  }
  if (audit.changed_entries != 0 || audit.changed_entries_truncated) {
    throw std::runtime_error("working tree is not clean; commit or stash tracked, staged, and untracked changes "
                             "yourself before updating (ckgit does not stash files)");
  }
  for (const auto* marker : {"MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "rebase-merge", "rebase-apply", "sequencer"}) {
    const auto path = incomingOutput(incomingGit(audit.path, {"rev-parse", "--git-path", marker}),
                                     "could not inspect an in-progress Git operation");
    std::filesystem::path resolved(path);
    if (resolved.is_relative()) resolved = audit.path / resolved;
    std::error_code error;
    const bool exists = std::filesystem::exists(resolved, error);
    if (error || exists) {
      throw std::runtime_error("a Git operation is in progress or cannot be inspected; finish or abort it yourself before updating");
    }
  }
  const auto common_path = incomingOutput(incomingGit(audit.path, {"rev-parse", "--git-common-dir"}),
                                          "could not inspect legacy grafted history");
  std::filesystem::path grafts = std::filesystem::path(common_path) / "info/grafts";
  if (grafts.is_relative()) grafts = audit.path / grafts;
  std::error_code graft_error;
  const bool has_grafts = std::filesystem::exists(grafts, graft_error);
  if (graft_error || (has_grafts && std::filesystem::file_size(grafts, graft_error) != 0) || graft_error) {
    throw std::runtime_error("legacy Git grafts alter this checkout's history; inspect and remove them yourself before updating");
  }
}

bool incomingAncestor(const std::filesystem::path& path, const std::string& ancestor, const std::string& descendant) {
  const auto result = incomingGit(path, {"merge-base", "--is-ancestor", ancestor, descendant});
  if (!result.timed_out && !result.output_truncated && (result.exit_code == 0 || result.exit_code == 1)) {
    return result.exit_code == 0;
  }
  throw std::runtime_error("could not verify commit ancestry; inspect local history before updating (" +
                           describeProcessFailure(result, kIncomingInspectionTimeout) + ")");
}

struct IncomingRef {
  std::string source;
  std::string destination;
  std::string object_id;
};

std::vector<IncomingRef> incomingRefs(const std::vector<ckgit::RefTip>& hosted,
                                     const ckgit::ClientConfig& config, const IncomingOptions& options) {
  std::vector<IncomingRef> selected;
  std::set<std::string> found_branches;
  for (const auto& ref : hosted) {
    if (ref.name.rfind("refs/heads/", 0) == 0) {
      const auto branch = ref.name.substr(11);
      if (options.branches.empty() ||
          std::find(options.branches.begin(), options.branches.end(), branch) != options.branches.end()) {
        found_branches.insert(branch);
        selected.push_back({ref.name, "refs/remotes/" + config.remote_name + "/" + branch, ref.object_id});
      }
    } else if (options.tags && ref.name.rfind("refs/tags/", 0) == 0) {
      selected.push_back({ref.name, ref.name, ref.object_id});
    }
  }
  for (const auto& branch : options.branches) {
    if (found_branches.count(branch) == 0) {
      throw std::runtime_error("hosted branch " + branch + " is missing; publish it from its source checkout, "
                               "or select a local branch that exists on the server");
    }
  }
  return selected;
}

void printIncomingPlan(const std::vector<IncomingRef>& refs, bool verbose, bool dry_run) {
  std::cout << "  " << (dry_run ? "Would fetch " : "Fetch ") << refs.size() << " hosted reference(s):\n";
  const std::size_t shown = verbose ? refs.size() : std::min<std::size_t>(refs.size(), 12);
  for (std::size_t index = 0; index < shown; ++index) {
    const auto& ref = refs[index];
    std::cout << "    " << ref.source << " @ " << ref.object_id.substr(0, 12) << " → " << ref.destination << "\n";
  }
  if (shown < refs.size()) {
    std::cout << "    " << refs.size() - shown << " more; --verbose to show all\n";
  }
  std::cout << "  Local branches and working files are unchanged by fetch; existing tags are never replaced; no refs are pruned.\n";
}

void requireDirectIncomingRefs(const std::filesystem::path& path, const std::vector<IncomingRef>& refs) {
  // A general ref listing omits dangling symbolic refs. Ask Git directly so
  // a symbolic destination pointing at a nonexistent local branch is caught.
  for (const auto& ref : refs) {
    const auto result = incomingGit(path, {"symbolic-ref", "--quiet", "--no-recurse", ref.destination});
    if (result.timed_out || result.output_truncated || (result.exit_code != 0 && result.exit_code != 1)) {
      throw std::runtime_error("could not inspect fetch destination " + ref.destination);
    }
    if (result.exit_code == 0) {
      throw std::runtime_error("fetch destination " + ref.destination +
                               " is a symbolic reference; inspect and repair it yourself before fetching. Local branches were not changed");
    }
  }
}

int incomingOne(const ckgit::RepositoryAudit& original, const std::string& project,
                const ckgit::ClientConfig& config, IncomingOptions options, bool update) {
  std::cout << "Project " << project << " ← " << config.server << "\n  checkout: " << original.path.string() << "\n";
  try {
    const IncomingIdentity identity(original);
    identity.verify();
    auto audit = ckgit::inspectDiscoveredRepository(original.path);
    requireIncomingPairing(audit, config, project);
    std::string head;
    if (update) {
      requireUpdateReady(audit);
      head = incomingHead(audit);
      options.branches = {audit.current_branch};
    }
    const auto refs = incomingRefs(fetchServerRefs(config, project), config, options);
    printIncomingPlan(refs, options.verbose, options.dry_run);
    // An atomic Git fetch also rejects a tag that changes between this plan
    // and transfer. Preflight makes an already known conflict actionable.
    for (const auto& ref : refs) {
      if (ref.source.rfind("refs/tags/", 0) != 0) continue;
      for (const auto& tag : audit.tags) {
        if (tag.name == ref.source && tag.object_id != ref.object_id) {
          throw std::runtime_error("tag " + ref.source.substr(10) +
                                   " differs locally and on the server; inspect both versions yourself. "
                                   "No tag will be replaced" + (update ? std::string{} : "; use --no-tags to fetch branches only"));
        }
      }
    }
    const auto branch_ref = std::find_if(refs.begin(), refs.end(), [](const IncomingRef& ref) {
      return ref.source.rfind("refs/heads/", 0) == 0;
    });
    if (update) {
      std::cout << "  Current branch: " << audit.current_branch << " @ " << head.substr(0, 12)
                << "\n  " << (options.dry_run ? "Would update" : "Update") << " only refs/heads/"
                << audit.current_branch << " from " << branch_ref->source << " @ " << branch_ref->object_id.substr(0, 12)
                << " by fast-forward; all other local branches stay unchanged.\n";
    }
    if (options.dry_run) {
      requireDirectIncomingRefs(audit.path, refs);
      if (update && head != branch_ref->object_id) {
        const auto present = incomingGit(audit.path, {"cat-file", "-e", branch_ref->object_id + "^{commit}"});
        if (present.timed_out || present.output_truncated) {
          throw std::runtime_error("could not inspect the target commit for this preview");
        }
        if (present.exit_code != 0) {
          std::cout << "  Ancestry will be checked after fetch; the hosted commit is not available locally yet.\n";
        } else if (incomingAncestor(audit.path, head, branch_ref->object_id)) {
          std::cout << "  Fast-forward verified for the advertised commit.\n";
        } else if (incomingAncestor(audit.path, branch_ref->object_id, head)) {
          std::cout << "  Local branch is ahead; its commits will be kept. Use ckgit sync to upload them.\n";
        } else {
          throw std::runtime_error("local and hosted branches have diverged; inspect and reconcile them yourself before retrying. No merge or rebase was performed");
        }
      }
      std::cout << "  Preview only; nothing fetched or updated.\n";
      return 0;
    }
    identity.verify();
    auto fresh = ckgit::inspectDiscoveredRepository(audit.path);
    requireIncomingPairing(fresh, config, project);
    if (update) {
      requireUpdateReady(fresh);
      if (fresh.current_branch != audit.current_branch || incomingHead(fresh) != head) {
        throw std::runtime_error("current branch changed while preparing the update; review it and retry");
      }
    }
    if (!refs.empty()) {
      // Explicit URL and destination refspecs avoid remote.<name>.fetch and
      // remote-specific pruning settings. The empty refmap also suppresses
      // opportunistic updates from any configured fetch mappings.
      std::vector<std::string> command{"git", "-C", audit.path.string(), "-c", "fetch.prune=false",
          "-c", "fetch.pruneTags=false", "fetch", "--atomic", "--no-tags", "--no-prune", "--no-prune-tags",
          "--no-recurse-submodules", "--no-auto-maintenance", "--refmap="};
      if (isatty(STDERR_FILENO) != 0) command.push_back("--progress");
      command.push_back("--");
      command.push_back(ckgit::hostedRepositoryUrl(config.server, project));
      for (const auto& ref : refs) command.push_back(ref.source + ":" + ref.destination);
      requireDirectIncomingRefs(audit.path, refs);
      std::cout << "  Downloading from server ...\n" << std::flush;
      const TransferSshGuard ssh_guard(audit.path);
      const auto result = ckgit::runProcess(command, transferOptions());
      if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
        throw std::runtime_error("fetch failed; working files and local branches were not updated (" +
                                 describeProcessFailure(result, kTransferTimeout) + ")");
      }
    }
    identity.verify();
    fresh = ckgit::inspectDiscoveredRepository(audit.path);
    requireIncomingPairing(fresh, config, project);
    if (!update) {
      std::cout << "  Fetched " << project << "; local branches and working files unchanged.\n";
      return 0;
    }
    requireUpdateReady(fresh);
    if (fresh.current_branch != audit.current_branch || incomingHead(fresh) != head) {
      throw std::runtime_error("current branch changed during fetch; fetched references are available, but no local branch was updated");
    }
    // Read the fetched ref rather than using the earlier advertisement: the
    // server may have advanced during the transfer. Merge takes the verified
    // object ID so later changes to the remote-tracking ref cannot change it.
    const auto target = incomingOutput(incomingGit(audit.path,
        {"rev-parse", "--verify", branch_ref->destination + "^{commit}"}), "could not read the fetched branch");
    if (head == target) {
      std::cout << "  Already up to date: " << audit.current_branch << ".\n";
      return 0;
    }
    if (!incomingAncestor(audit.path, head, target)) {
      if (incomingAncestor(audit.path, target, head)) {
        std::cout << "  Local branch is ahead; its commits were kept. Use ckgit sync to upload them.\n";
        return 0;
      }
      throw std::runtime_error("local and hosted branches have diverged; inspect and reconcile them yourself before retrying. No merge or rebase was performed");
    }
    // Repeat after ancestry inspection, immediately before Git's own locked
    // fast-forward. Git still protects files that change after this check.
    identity.verify();
    fresh = ckgit::inspectDiscoveredRepository(audit.path);
    requireIncomingPairing(fresh, config, project);
    requireUpdateReady(fresh);
    if (fresh.current_branch != audit.current_branch || incomingHead(fresh) != head) {
      throw std::runtime_error("current branch changed before fast-forward; review it and retry");
    }
    const auto result = incomingGit(audit.path, {"-c", "core.hooksPath=/dev/null", "-c", "merge.autoStash=false",
        "-c", "submodule.recurse=false", "-c", "branch." + audit.current_branch + ".mergeOptions=",
        "merge", "--ff-only", "--no-edit", "--no-autostash", "--no-overwrite-ignore", target});
    if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
      throw std::runtime_error("Git could not finish the fast-forward; inspect git status before retrying (" +
                               describeProcessFailure(result, kIncomingInspectionTimeout) + ")");
    }
    std::cout << "  Updated " << audit.current_branch << " by fast-forward: " << head.substr(0, 12)
              << " → " << target.substr(0, 12) << ".\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << project << ": " << error.what() << "\n";
    return kPartial;
  }
}

int incomingCommand(const std::vector<std::string>& arguments, bool update) {
  const IncomingGitEnvironment git_environment;
  IncomingOptions options;
  const std::string verb = update ? "update" : "fetch";
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if ((argument == "--config" || argument == "--repo" || argument == "--project" ||
         (!update && argument == "--branch")) && index + 1 < arguments.size()) {
      const auto value = arguments[++index];
      if (argument == "--config") options.config_path = value;
      else if (argument == "--repo") options.repository = value;
      else if (argument == "--project" && ckgit::isValidProjectName(value)) options.project = value;
      else if (argument == "--branch" && ckgit::isValidBranchName(value)) options.branches.push_back(value);
      else {
        std::cerr << "ckgit: invalid " << argument << "; try ckgit help " << verb << "\n";
        return kUsage;
      }
    } else if (argument == "--dry-run") options.dry_run = true;
    else if (argument == "--verbose") options.verbose = true;
    else if (argument == "--all") options.all = true;
    else if (!update && argument == "--no-tags") options.tags = false;
    else {
      std::cerr << "ckgit: invalid or incomplete " << argument << "; try ckgit help " << verb << "\n";
      return kUsage;
    }
  }
  if (static_cast<int>(options.repository.has_value()) + static_cast<int>(options.project.has_value()) +
          static_cast<int>(options.all) > 1) {
    std::cerr << "ckgit: --repo, --project, and --all are mutually exclusive\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(resolveConfigPath(options.config_path));
  std::optional<ckgit::SyncLock> lock;
  if (!options.dry_run) {
    lock = acquireSyncLock(config, verb);
    if (!lock.has_value()) return kBusy;
  }
  if (options.repository.has_value()) {
    try {
      const auto audit = ckgit::inspectRepository(*options.repository);
      bool invalid = false;
      const auto project = pairedProjectForAudit(audit, config, &invalid);
      if (invalid || !project.has_value()) {
        throw std::runtime_error("repository is not paired with this configured server; inspect its " + config.remote_name + " remote");
      }
      std::cout << "Scope: requested repository on " << config.display_name << " ← " << config.server << "\n";
      return incomingOne(audit, *project, config, options, update);
    } catch (const std::exception& error) {
      std::cerr << "ckgit: cannot " << verb << " requested repository: " << error.what() << "\n";
      return kPartial;
    }
  }
  const auto inventory = managedInventory(config);
  printManagedScope(inventory, config);
  if (options.project.has_value() && inventory.projects.count(*options.project) == 0) {
    std::cerr << "ckgit: " << *options.project << " is not managed on this device; clone it or select an existing checkout with --repo PATH\n";
    return kPartial;
  }
  bool partial = !inventory.server_available;
  for (const auto& [project, checkout] : inventory.projects) {
    if (options.project.has_value() && project != *options.project) continue;
    if (!checkout.audit.has_value()) {
      printUnavailableCheckout(project, checkout);
      partial = true;
      continue;
    }
    partial = incomingOne(*checkout.audit, project, config, options, update) != 0 || partial;
  }
  return partial ? kPartial : 0;
}

int fetchCommand(const std::vector<std::string>& arguments) { return incomingCommand(arguments, false); }
int updateCommand(const std::vector<std::string>& arguments) { return incomingCommand(arguments, true); }
