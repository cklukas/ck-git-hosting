// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Included after the shared client helpers in main.cpp.

bool confirmManagementPlan(bool yes, const std::string& question) {
  if (yes) return true;
  if (!isatty(STDIN_FILENO)) {
    std::cout << "Preview only. Run in a terminal to confirm, or pass --yes.\n";
    return false;
  }
  std::cout << question << " [y/N] " << std::flush;
  std::string answer;
  std::getline(std::cin, answer);
  return answer == "y" || answer == "Y" || answer == "yes";
}

struct DiscoveryCheckoutState {
  std::string state;
  std::string problem;
};

DiscoveryCheckoutState discoveryCheckoutState(const ckgit::ClientConfig& config,
    const std::string& project, const std::filesystem::path& path) {
  if (!path.is_absolute()) return {"unresolved", "legacy registration needs a local checkout path"};
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
    return {"missing", "checkout folder is missing or unavailable"};
  try {
    const auto audit = ckgit::inspectDiscoveredRepository(path);
    bool invalid = false;
    const auto paired = pairedProjectForAudit(audit, config, &invalid);
    if (invalid || !paired || *paired != project)
      return {"invalid", "checkout no longer has a matching configured Git remote"};
  } catch (const std::exception& failure) {
    return {"invalid", failure.what()};
  }
  return {"managed", {}};
}

int projectsCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  bool json = false, uncloned = false;
  std::string filter;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (arguments[i] == "--config" && i + 1 < arguments.size()) config_path = arguments[++i];
    else if (arguments[i] == "--filter" && i + 1 < arguments.size()) filter = arguments[++i];
    else if (arguments[i] == "--json") json = true;
    else if (arguments[i] == "--uncloned") uncloned = true;
    else throw std::invalid_argument("invalid projects option: " + arguments[i]);
  }
  const auto config = ckgit::loadClientConfig(resolveConfigPath(config_path));
  const auto hosted = fetchPublishedProjects(config);
  const auto local = ckgit::loadCanonicalCheckouts(canonicalSelectionPath(config));
  const auto reported = fetchRegisteredCheckouts(config);
  if (json) std::cout << "{\"schema_version\":1,\"server\":\"" << jsonEscape(config.server) << "\",\"projects\":[";
  else std::cout << "Hosted projects on " << config.server << "\n";
  std::size_t count = 0;
  for (const auto& name : hosted) {
    const auto selection = local.find(name);
    const auto registration = reported.find(name);
    const bool managed = selection != local.end() || registration != reported.end();
    if ((uncloned && managed) || name.find(filter) == std::string::npos) continue;
    const auto path = selection != local.end() ? selection->second
        : registration != reported.end() ? registration->second : std::filesystem::path{};
    const auto checkout = managed ? discoveryCheckoutState(config, name, path)
        : DiscoveryCheckoutState{"unmanaged", {}};
    if (json) {
      if (count) std::cout << ',';
      std::cout << "{\"name\":\"" << jsonEscape(name) << "\",\"state\":\"" << checkout.state
                << "\",\"checkout\":" << (managed ? "\"" + jsonEscape(path.string()) + "\"" : "null")
                << ",\"clone_url\":\"" << jsonEscape(ckgit::hostedRepositoryUrl(config.server, name)) << '"';
      if (!checkout.problem.empty()) std::cout << ",\"problem\":\"" << jsonEscape(checkout.problem) << '"';
      std::cout << '}';
    } else {
      std::cout << "  " << name << "  " << checkout.state;
      if (managed) std::cout << "  " << path.string();
      if (!checkout.problem.empty()) std::cout << " (" << checkout.problem << ")";
      std::cout << '\n';
    }
    ++count;
  }
  if (json) std::cout << "]}\n";
  else std::cout << count << " project(s). Use ckgit clone NAME, or ckgit clone --all --into DIR.\n";
  return 0;
}

int cloneEntryCommand(const std::vector<std::string>& arguments) {
  bool bulk = false;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (arguments[i] == "--") break;
    if (arguments[i] == "--config" && i + 1 < arguments.size()) { ++i; continue; }
    if (arguments[i] == "--all" || arguments[i] == "--project") { bulk = true; break; }
  }
  if (!bulk) return cloneCommand(arguments);
  std::optional<std::filesystem::path> config_path, into;
  std::set<std::string> selected;
  bool all = false, yes = false, dry_run = false;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const auto& arg = arguments[i];
    if ((arg == "--config" || arg == "--into" || arg == "--project") && i + 1 < arguments.size()) {
      const auto value = arguments[++i];
      if (arg == "--config") config_path = value;
      else if (arg == "--into") into = value;
      else selected.insert(value);
    } else if (arg == "--all") all = true;
    else if (arg == "--yes") yes = true;
    else if (arg == "--dry-run") dry_run = true;
    else throw std::invalid_argument("invalid bulk clone argument: " + arg);
  }
  if (!into || (all && !selected.empty())) {
    std::cerr << "ckgit: choose --all or repeated --project NAME, and supply --into DIR\n";
    return kUsage;
  }
  std::error_code error;
  const auto parent_status = std::filesystem::symlink_status(*into, error);
  if (error || !std::filesystem::is_directory(parent_status) || std::filesystem::is_symlink(parent_status)) {
    std::cerr << "ckgit: --into must be an existing non-symlink directory\n";
    return kUsage;
  }
  const auto parent = std::filesystem::canonical(*into);
  const auto resolved_config = resolveConfigPath(config_path);
  const auto config = ckgit::loadClientConfig(resolved_config);
  const auto hosted = fetchPublishedProjects(config);
  const auto local = ckgit::loadCanonicalCheckouts(canonicalSelectionPath(config));
  const auto reported = fetchRegisteredCheckouts(config);
  if (all) selected = hosted;
  std::vector<std::string> pending;
  bool attention = false;
  std::size_t skipped = 0;
  std::cout << "Clone plan from " << config.server << " into " << parent.string() << "\n";
  for (const auto& name : selected) {
    if (!hosted.contains(name)) {
      std::cout << "  Skip " << name << ": project is not hosted.\n";
      attention = true; ++skipped; continue;
    }
    if (local.contains(name) || reported.contains(name)) {
      const auto path = local.contains(name) ? local.at(name) : reported.at(name);
      const auto checkout = discoveryCheckoutState(config, name, path);
      std::cout << "  Skip " << name << ": already managed at " << path.string() << ".";
      if (!checkout.problem.empty()) {
        std::cout << " " << checkout.problem << ". Repair its remote or use checkout set-canonical, migrate, or forget.";
        attention = true;
      }
      std::cout << '\n'; ++skipped; continue;
    }
    const auto status = std::filesystem::symlink_status(parent / name, error);
    if ((!error && status.type() != std::filesystem::file_type::not_found) ||
        (error && error != std::errc::no_such_file_or_directory)) {
      std::cout << "  Skip " << name << ": destination exists or is unavailable; use register for an existing paired checkout.\n";
      attention = true; ++skipped; continue;
    }
    std::cout << "  Clone " << name << " -> " << (parent / name).string() << '\n';
    pending.push_back(name);
  }
  if (pending.empty()) {
    std::cout << "No projects to clone; " << skipped << " skipped.\n";
    return attention ? kPartial : 0;
  }
  if (dry_run || !confirmManagementPlan(yes, "Clone these " + std::to_string(pending.size()) + " projects?"))
    return attention ? kPartial : 0;
  std::size_t cloned = 0, failed = 0;
  for (const auto& name : pending) {
    try {
      // Each clone locks and rechecks management independently. A concurrent
      // selection cannot silently be replaced between preview and execution.
      const int result = cloneCommand({"--config", resolved_config.string(), "--", name, (parent / name).string()}, true);
      if (result == 0) ++cloned;
      else if (result == 5) ++skipped;
      else { ++failed; attention = true; }
    } catch (const std::exception& failure) {
      std::cerr << "ckgit: " << name << ": " << failure.what() << '\n';
      ++failed; attention = true;
    }
  }
  std::cout << "Clone summary: " << cloned << " cloned, " << skipped << " skipped, " << failed << " failed.\n";
  return attention ? kPartial : 0;
}

int forgetCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::string project;
  bool yes = false, dry_run = false, positional_only = false;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (!positional_only && arguments[i] == "--") positional_only = true;
    else if (!positional_only && arguments[i] == "--config" && i + 1 < arguments.size()) config_path = arguments[++i];
    else if (!positional_only && arguments[i] == "--yes") yes = true;
    else if (!positional_only && arguments[i] == "--dry-run") dry_run = true;
    else if (project.empty()) project = arguments[i];
    else return kUsage;
  }
  if (!ckgit::isValidProjectName(project)) return kUsage;
  const auto config = ckgit::loadClientConfig(resolveConfigPath(config_path));
  std::optional<ckgit::SyncLock> lock;
  if (!dry_run) {
    lock = acquireSyncLock(config, "checkout forget");
    if (!lock) return kBusy;
  }
  const auto local = ckgit::loadCanonicalCheckouts(canonicalSelectionPath(config));
  const auto reported = fetchRegisteredCheckouts(config);
  if (!local.contains(project) && !reported.contains(project)) {
    std::cout << project << " is already unmanaged on this device.\n";
    return 0;
  }
  std::cout << "Forget " << project << " on device " << config.client_id << ".\n";
  if (local.contains(project)) std::cout << "  Remove main checkout selection: " << local.at(project).string() << '\n';
  std::cout << "  Remove this device's registration on " << config.server
            << ".\n  Files, Git remotes, commits, and the hosted project are retained.\n";
  if (dry_run || !confirmManagementPlan(yes, "Stop managing this checkout?")) return 0;
  // Remove the remote report first: a failed connection must not leave a
  // legacy registration that could silently rediscover the forgotten checkout.
  const auto result = controlRpc(config, "forget-checkout " + project);
  if (result.exit_code != 0 || result.timed_out || result.output_truncated || result.output != "ok forgotten\n") {
    std::cerr << "ckgit: could not forget server registration; local selection retained. Retry when connected.\n";
    return 1;
  }
  ckgit::forgetCanonicalCheckout(canonicalSelectionPath(config), project);
  std::cout << "Forgot " << project << ". Ordinary status, sync, fetch, and update will no longer manage it.\n"
            << "Use register to manage it again; an explicit sync --scan can also rediscover paired copies.\n";
  return 0;
}
