// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ckgit/client_config.hpp"
#include "ckgit/client_state.hpp"
#include "ckgit/git_repository.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/process.hpp"
#include "ckgit/ref_status.hpp"
#include "ckgit/server_identity.hpp"
#include "ckgit/validation.hpp"

namespace {

constexpr int kUsage = 2;
constexpr int kPartial = 3;
constexpr int kBusy = 4;

bool registerRemoteCheckout(const ckgit::RepositoryAudit& audit, const ckgit::ClientConfig& config,
                            std::string_view project, std::string* failure_reason);
std::optional<std::string> pairedProjectForAudit(const ckgit::RepositoryAudit& audit,
                                                 const ckgit::ClientConfig& config, bool* invalid);

std::filesystem::path canonicalSelectionPath(const ckgit::ClientConfig& config) {
  return config.config_directory / "canonical.ini";
}

std::filesystem::path syncLockPath(const ckgit::ClientConfig& config) {
  return config.config_directory / "sync.lock";
}

std::optional<ckgit::SyncLock> acquireSyncLock(const ckgit::ClientConfig& config, std::string_view command) {
  auto lock = ckgit::SyncLock::tryAcquire(syncLockPath(config));
  if (!lock.has_value()) {
    std::cerr << "ckgit: another sync or publish for this configuration is running; " << command
              << " was not started\n";
  }
  return lock;
}

using ProjectCheckouts = std::map<std::string, std::vector<const ckgit::RepositoryAudit*>>;

// Groups audited working trees by their exactly paired project.  Checkouts
// whose configured remote is unsafe are reported and excluded.
ProjectCheckouts groupPairedCheckouts(const std::vector<ckgit::RepositoryAudit>& audits,
                                      const ckgit::ClientConfig& config, bool* attention) {
  ProjectCheckouts groups;
  for (const auto& audit : audits) {
    bool invalid = false;
    const auto project = pairedProjectForAudit(audit, config, &invalid);
    if (project.has_value()) {
      groups[*project].push_back(&audit);
    } else if (invalid) {
      std::cerr << "ckgit: " << audit.path.string() << ": configured remote is unsafe or ambiguous\n";
      *attention = true;
    }
  }
  return groups;
}

// Proposes a canonical checkout only when exactly one candidate is neither a
// linked worktree nor an ephemeral-looking path.  The proposal is advice: it
// is never applied without `ckgit checkout set-canonical`.
const ckgit::RepositoryAudit* proposeCanonicalCheckout(const std::vector<const ckgit::RepositoryAudit*>& checkouts) {
  const ckgit::RepositoryAudit* proposed = nullptr;
  for (const auto* audit : checkouts) {
    if (audit->linked_worktree || ckgit::looksEphemeralCheckoutPath(audit->path)) {
      continue;
    }
    if (proposed != nullptr) {
      return nullptr;
    }
    proposed = audit;
  }
  return proposed;
}

bool samePath(const std::filesystem::path& left, const std::filesystem::path& right) {
  return left.lexically_normal() == right.lexically_normal();
}

// Chooses the one checkout automatic sync may use for a project, or explains
// why the project is skipped.
const ckgit::RepositoryAudit* chooseAutomaticCheckout(
    const std::string& project, const std::vector<const ckgit::RepositoryAudit*>& checkouts,
    const std::map<std::string, std::filesystem::path>& selections, std::string* reason) {
  const auto selected = selections.find(project);
  if (selected != selections.end()) {
    for (const auto* audit : checkouts) {
      if (samePath(audit->path, selected->second)) {
        return audit;
      }
    }
    *reason = "selected canonical checkout " + selected->second.string() + " is unavailable; " +
              std::to_string(checkouts.size()) + " other checkout(s) skipped";
    return nullptr;
  }
  if (checkouts.size() == 1) {
    return checkouts.front();
  }
  *reason = "duplicate checkouts; automatic sync skipped until `ckgit checkout set-canonical " + project +
            " PATH` selects one";
  if (const auto* proposed = proposeCanonicalCheckout(checkouts)) {
    *reason += " (proposed: " + proposed->path.string() + ")";
  }
  return nullptr;
}

std::string checkoutMarkers(const ckgit::RepositoryAudit& audit) {
  std::string markers;
  if (audit.linked_worktree) {
    markers += " [linked worktree]";
  }
  if (ckgit::looksEphemeralCheckoutPath(audit.path)) {
    markers += " [ephemeral name]";
  }
  return markers;
}

void printUsage(std::ostream& output) {
  output << "Usage:\n"
         << "  ckgit scan [--json] [--config PATH] [ROOT ...]\n"
         << "  ckgit config show --config PATH\n\n"
         << "  ckgit status --config PATH [--repo PATH]\n\n"
         << "  ckgit clone --config PATH NAME [DESTINATION]\n\n"
         << "  ckgit create --config PATH NAME [--default-branch BRANCH]\n\n"
         << "  ckgit publish --config PATH [--name NAME] [--yes] [REPOSITORY]\n\n"
         << "  ckgit register --config PATH [--repo PATH]\n\n"
         << "  ckgit sync --config PATH [--repo PATH] [--dry-run]\n\n"
         << "  ckgit checkout list --config PATH [NAME]\n"
         << "  ckgit checkout set-canonical --config PATH NAME PATH\n\n"
         << "scan is read-only: it discovers Git working trees and audits local "
            "refs, remotes, and worktree state.\n"
         << "Exit codes: 0 ok, 1 error, 2 usage, 3 partial or attention needed, "
            "4 another sync or publish holds the lock.\n";
}

void printConfig(const ckgit::ClientConfig& config) {
  std::cout << "client_id=" << config.client_id << "\n"
            << "display_name=" << config.display_name << "\n"
            << "server=" << config.server << "\n"
            << "remote_name=" << config.remote_name << "\n"
            << "public_path_mode=" << (config.expose_full_paths ? "full" : "basename") << "\n";
  for (const auto& root : config.scan_roots) {
    std::cout << "scan_root=" << root.string() << "\n";
  }
  for (const auto& exclusion : config.exclusions) {
    std::cout << "exclude=" << exclusion << "\n";
  }
}

std::string jsonEscape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (const unsigned char character : value) {
    switch (character) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (character < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped += kHex[character >> 4];
          escaped += kHex[character & 0x0f];
        } else {
          escaped += static_cast<char>(character);
        }
    }
  }
  return escaped;
}

const char* transportName(ckgit::RemoteTransport transport) {
  switch (transport) {
    case ckgit::RemoteTransport::kScpLikeSsh: return "ssh-scp";
    case ckgit::RemoteTransport::kSshUrl: return "ssh-url";
    case ckgit::RemoteTransport::kOther: return "other";
  }
  return "other";
}

void printTextAudit(const ckgit::RepositoryAudit& audit) {
  std::cout << audit.path.string() << "\n"
            << "  head: "
            << (audit.detached_head ? "detached-or-unborn" : audit.current_branch) << "\n"
            << "  worktree: " << (audit.changed_entries == 0 ? "clean" : "dirty (")
            << (audit.changed_entries == 0 ? "" : std::to_string(audit.changed_entries) + " entries)")
            << "\n  branches: " << audit.branches.size() << "\n"
            << "  tags: " << audit.tags.size() << "\n";
  for (const auto& remote : audit.remotes) {
    std::cout << "  remote " << remote.remote_name << ": " << remote.display_url
              << " [" << transportName(remote.transport) << "]\n";
  }
}

void printJsonAudit(const ckgit::RepositoryAudit& audit, bool first) {
  if (!first) {
    std::cout << ",";
  }
  std::cout << "\n    {\"path\":\"" << jsonEscape(audit.path.string()) << "\","
            << "\"head\":";
  if (audit.detached_head) {
    std::cout << "null";
  } else {
    std::cout << "\"" << jsonEscape(audit.current_branch) << "\"";
  }
  std::cout << ",\"changed_entries\":" << audit.changed_entries
            << ",\"branch_count\":" << audit.branches.size()
            << ",\"tag_count\":" << audit.tags.size() << ",\"remotes\":[";
  for (std::size_t index = 0; index < audit.remotes.size(); ++index) {
    const auto& remote = audit.remotes[index];
    if (index != 0) {
      std::cout << ",";
    }
    std::cout << "{\"name\":\"" << jsonEscape(remote.remote_name)
              << "\",\"url\":\"" << jsonEscape(remote.display_url)
              << "\",\"transport\":\"" << transportName(remote.transport) << "\"}";
  }
  std::cout << "]}";
}

int scan(const std::vector<std::string>& arguments) {
  bool json = false;
  std::optional<std::filesystem::path> config_path;
  std::vector<std::filesystem::path> roots;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto& argument = arguments[index];
    if (argument == "--json") {
      json = true;
    } else if (argument == "--config") {
      if (++index == arguments.size()) {
        std::cerr << "ckgit: --config requires a path\n";
        return kUsage;
      }
      config_path.emplace(arguments[index]);
    } else if (!argument.empty() && argument.front() == '-') {
      std::cerr << "ckgit: unknown scan option: " << argument << "\n";
      return kUsage;
    } else {
      roots.emplace_back(argument);
    }
  }
  std::vector<std::string> exclusions;
  if (config_path.has_value()) {
    const auto config = ckgit::loadClientConfig(*config_path);
    exclusions = config.exclusions;
    if (roots.empty()) {
      roots = config.scan_roots;
    }
  }
  if (roots.empty()) {
    std::cerr << "ckgit: scan requires roots or a configuration with scan_root entries\n";
    return kUsage;
  }

  const ckgit::DiscoveryResult discovery = ckgit::discoverWorkingTrees(roots, exclusions);
  std::vector<ckgit::RepositoryAudit> audited;
  std::vector<std::string> warnings = discovery.warnings;
  for (const auto& path : discovery.repositories) {
    try {
      audited.push_back(ckgit::inspectRepository(path));
    } catch (const std::exception& error) {
      warnings.push_back(path.string() + ": " + error.what());
    }
  }

  if (json) {
    std::cout << "{\"schema_version\":1,\"repositories\":[";
    for (std::size_t index = 0; index < audited.size(); ++index) {
      printJsonAudit(audited[index], index == 0);
    }
    std::cout << "\n  ],\"warnings\":[";
    for (std::size_t index = 0; index < warnings.size(); ++index) {
      if (index != 0) {
        std::cout << ",";
      }
      std::cout << "\"" << jsonEscape(warnings[index]) << "\"";
    }
    std::cout << "]}\n";
  } else {
    for (const auto& audit : audited) {
      printTextAudit(audit);
    }
    for (const auto& warning : warnings) {
      std::cerr << "warning: " << warning << "\n";
    }
    std::cout << "Scanned " << audited.size() << " repository"
              << (audited.size() == 1 ? "" : "ies") << ".\n";
  }
  return warnings.empty() ? 0 : kPartial;
}

int configCommand(const std::vector<std::string>& arguments) {
  if (arguments.size() != 3 || arguments[0] != "show" || arguments[1] != "--config") {
    std::cerr << "ckgit: usage: ckgit config show --config PATH\n";
    return kUsage;
  }
  printConfig(ckgit::loadClientConfig(arguments[2]));
  return 0;
}

std::vector<ckgit::RefTip> localRefTips(const ckgit::RepositoryAudit& audit) {
  std::vector<ckgit::RefTip> refs;
  refs.reserve(audit.branches.size() + audit.tags.size());
  refs.insert(refs.end(), audit.branches.begin(), audit.branches.end());
  refs.insert(refs.end(), audit.tags.begin(), audit.tags.end());
  return refs;
}

std::vector<ckgit::RefTip> fetchServerRefs(const ckgit::ClientConfig& config,
                                           const std::string& project) {
  const ckgit::ProcessResult result = ckgit::runProcess(
      {"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "RequestTTY=no",
       "-o", "ClearAllForwardings=yes", config.server, "ckgit-rpc 1 refs " + project});
  if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
    throw std::runtime_error("could not query paired server refs");
  }
  return ckgit::parseRefsControlResponse(result.output);
}

bool printPairedStatus(const ckgit::RepositoryAudit& audit, const ckgit::ClientConfig& config) {
  std::map<std::string, std::string> projects;
  bool configured_remote_seen = false;
  for (const auto& remote : audit.remotes) {
    if (remote.remote_name != config.remote_name) {
      continue;
    }
    configured_remote_seen = true;
    const auto project = ckgit::projectForConfiguredServerRemote(remote, config);
    if (project.has_value()) {
      projects.emplace(*project, remote.display_url);
    }
  }
  if (projects.empty()) {
    std::cout << audit.path.string() << "\n  pairing: "
              << (configured_remote_seen ? "configured remote does not match paired server" : "not configured")
              << "\n";
    return configured_remote_seen;
  }
  if (projects.size() != 1) {
    std::cout << audit.path.string() << "\n  pairing: ambiguous configured remote URLs\n";
    return true;
  }

  const auto& [project, remote_url] = *projects.begin();
  try {
    const auto server_refs = fetchServerRefs(config, project);
    const auto comparison = ckgit::compareRefTips(localRefTips(audit), server_refs);
    std::size_t equal = 0;
    std::size_t local_only = 0;
    std::size_t server_only = 0;
    std::size_t mismatched = 0;
    for (const auto& ref : comparison) {
      switch (ref.relation) {
        case ckgit::RefRelation::kEqual: ++equal; break;
        case ckgit::RefRelation::kLocalOnly: ++local_only; break;
        case ckgit::RefRelation::kServerOnly: ++server_only; break;
        case ckgit::RefRelation::kMismatched: ++mismatched; break;
      }
    }
    std::cout << audit.path.string() << "\n"
              << "  project: " << project << "\n"
              << "  paired remote: " << remote_url << "\n"
              << "  refs: " << equal << " equal, " << local_only << " local-only, "
              << server_only << " server-only, " << mismatched << " mismatched\n";
    // Without server objects locally, a different object ID is intentionally
    // never guessed to be a fast-forward.  It requires manual review or sync's
    // later explicit preflight.
    return mismatched != 0 || server_only != 0;
  } catch (const std::exception&) {
    std::cout << audit.path.string() << "\n  pairing: server ref query failed\n";
    return true;
  }
}

int statusCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::filesystem::path> repository_path;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if ((arguments[index] == "--config" || arguments[index] == "--repo") &&
        index + 1 < arguments.size()) {
      const std::string value = arguments[++index];
      if (arguments[index - 1] == "--config") {
        config_path.emplace(value);
      } else {
        repository_path.emplace(value);
      }
    } else {
      std::cerr << "ckgit: usage: ckgit status --config PATH [--repo PATH]\n";
      return kUsage;
    }
  }
  if (!config_path.has_value()) {
    std::cerr << "ckgit: status requires --config PATH\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(*config_path);
  std::vector<ckgit::RepositoryAudit> audits;
  bool attention = false;
  if (repository_path.has_value()) {
    try {
      audits.push_back(ckgit::inspectRepository(*repository_path));
    } catch (const std::exception& error) {
      std::cerr << "ckgit: cannot inspect requested repository: " << error.what() << "\n";
      return kPartial;
    }
  } else {
    const auto discovery = ckgit::discoverWorkingTrees(config.scan_roots, config.exclusions);
    for (const auto& warning : discovery.warnings) {
      std::cerr << "warning: " << warning << "\n";
      attention = true;
    }
    for (const auto& path : discovery.repositories) {
      try {
        audits.push_back(ckgit::inspectRepository(path));
      } catch (const std::exception& error) {
        std::cerr << "warning: " << path.string() << ": " << error.what() << "\n";
        attention = true;
      }
    }
  }
  for (const auto& audit : audits) {
    attention = printPairedStatus(audit, config) || attention;
  }
  if (audits.empty()) {
    std::cout << "No repositories found.\n";
  }
  return attention ? kPartial : 0;
}

int cloneCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::vector<std::string> positional;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index] == "--config" && index + 1 < arguments.size()) {
      config_path.emplace(arguments[++index]);
    } else if (!arguments[index].empty() && arguments[index].front() == '-') {
      std::cerr << "ckgit: usage: ckgit clone --config PATH NAME [DESTINATION]\n";
      return kUsage;
    } else {
      positional.push_back(arguments[index]);
    }
  }
  if (!config_path.has_value() || positional.empty() || positional.size() > 2 ||
      !ckgit::isValidProjectName(positional.front())) {
    std::cerr << "ckgit: usage: ckgit clone --config PATH NAME [DESTINATION]\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(*config_path);
  const std::filesystem::path destination = positional.size() == 2 ? positional[1] : positional[0];
  if (destination.empty()) {
    std::cerr << "ckgit: clone destination is required\n";
    return kUsage;
  }
  std::error_code error;
  const auto destination_status = std::filesystem::symlink_status(destination, error);
  if ((destination_status.type() != std::filesystem::file_type::not_found && !error) ||
      (error != std::errc{} && error != std::errc::no_such_file_or_directory)) {
    std::cerr << "ckgit: clone destination already exists or cannot be inspected\n";
    return kUsage;
  }
  const std::filesystem::path parent = destination.has_parent_path() ? destination.parent_path() : ".";
  const auto parent_status = std::filesystem::symlink_status(parent, error);
  if (error || !std::filesystem::is_directory(parent_status) ||
      std::filesystem::is_symlink(parent_status)) {
    std::cerr << "ckgit: clone destination parent must be an existing non-symlink directory\n";
    return kUsage;
  }
  const std::string remote_url = config.server + ":" + positional.front() + ".git";
  const auto result = ckgit::runProcess(
      {"git", "clone", "--origin", config.remote_name, "--no-recurse-submodules", "--", remote_url,
       destination.string()});
  if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
    std::cerr << "ckgit: Git clone failed\n";
    return 1;
  }
  std::cout << "Cloned " << positional.front() << " to " << destination.string() << "\n";
  return 0;
}

int createCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::string> project_name;
  std::string branch{"main"};
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if ((arguments[index] == "--config" || arguments[index] == "--default-branch") &&
        index + 1 < arguments.size()) {
      const std::string value = arguments[++index];
      if (arguments[index - 1] == "--config") {
        config_path.emplace(value);
      } else {
        branch = value;
      }
    } else if (!arguments[index].empty() && arguments[index].front() != '-' && !project_name.has_value()) {
      project_name.emplace(arguments[index]);
    } else {
      std::cerr << "ckgit: usage: ckgit create --config PATH NAME [--default-branch BRANCH]\n";
      return kUsage;
    }
  }
  if (!config_path.has_value() || !project_name.has_value() ||
      !ckgit::isValidProjectName(*project_name) || !ckgit::isValidBranchName(branch)) {
    std::cerr << "ckgit: usage: ckgit create --config PATH NAME [--default-branch BRANCH]\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(*config_path);
  const auto result = ckgit::runProcess(
      {"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "RequestTTY=no",
       "-o", "ClearAllForwardings=yes", config.server,
       "ckgit-rpc 1 create " + *project_name + " " + branch});
  if (result.exit_code != 0 || result.timed_out || result.output_truncated || result.output != "ok created\n") {
    std::cerr << "ckgit: remote project creation failed\n";
    return 1;
  }
  std::cout << "Created remote project " << *project_name << " with default branch " << branch << "\n";
  return 0;
}

int publishCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::string> requested_name;
  std::optional<std::filesystem::path> repository_path;
  bool confirmed = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if ((arguments[index] == "--config" || arguments[index] == "--name") && index + 1 < arguments.size()) {
      const std::string value = arguments[++index];
      if (arguments[index - 1] == "--config") {
        config_path.emplace(value);
      } else {
        requested_name.emplace(value);
      }
    } else if (arguments[index] == "--yes") {
      confirmed = true;
    } else if (!arguments[index].empty() && arguments[index].front() != '-' && !repository_path.has_value()) {
      repository_path.emplace(arguments[index]);
    } else {
      std::cerr << "ckgit: usage: ckgit publish --config PATH [--name NAME] [--yes] [REPOSITORY]\n";
      return kUsage;
    }
  }
  if (!config_path.has_value()) {
    std::cerr << "ckgit: publish requires --config PATH\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(*config_path);
  ckgit::RepositoryAudit audit;
  try {
    audit = ckgit::inspectRepository(repository_path.value_or("."));
  } catch (const std::exception& error) {
    std::cerr << "ckgit: cannot inspect repository to publish: " << error.what() << "\n";
    return 1;
  }
  const std::string project = requested_name.value_or(audit.path.filename().string());
  if (!ckgit::isValidProjectName(project) || audit.detached_head || audit.current_branch.empty()) {
    std::cerr << "ckgit: publish requires a valid project name and a checked-out, committed branch\n";
    return kUsage;
  }
  const std::string remote_url = config.server + ":" + project + ".git";
  bool remote_exists = false;
  for (const auto& remote : audit.remotes) {
    if (remote.remote_name != config.remote_name) {
      continue;
    }
    if (remote.raw_url != remote_url) {
      std::cerr << "ckgit: configured remote name already points somewhere else\n";
      return 1;
    }
    remote_exists = true;
  }
  if (!confirmed) {
    std::cout << "Would " << (remote_exists ? "push to" : "create and push to") << " " << remote_url
              << " from " << audit.path.string() << "\n"
              << "  branches: " << audit.branches.size() << ", tags: " << audit.tags.size() << "\n"
              << "Run again with --yes to continue.\n";
    return 0;
  }
  const auto lock = acquireSyncLock(config, "publish");
  if (!lock.has_value()) {
    return kBusy;
  }
  if (!remote_exists) {
    const auto create_result = ckgit::runProcess(
        {"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "RequestTTY=no",
         "-o", "ClearAllForwardings=yes", config.server,
         "ckgit-rpc 1 create " + project + " " + audit.current_branch});
    if (create_result.exit_code != 0 || create_result.timed_out || create_result.output_truncated ||
        create_result.output != "ok created\n") {
      std::cerr << "ckgit: remote project creation failed; local repository was unchanged\n";
      return 1;
    }
    const auto add_result = ckgit::runProcess(
        {"git", "-C", audit.path.string(), "remote", "add", config.remote_name, remote_url});
    if (add_result.exit_code != 0 || add_result.timed_out || add_result.output_truncated) {
      std::cerr << "ckgit: remote project was created but the local remote could not be added\n";
      return 1;
    }
  }
  const auto push_result = ckgit::runProcess(
      {"git", "-C", audit.path.string(), "push", "--porcelain", "--atomic", config.remote_name,
       "refs/heads/*:refs/heads/*", "refs/tags/*:refs/tags/*"});
  if (push_result.exit_code != 0 || push_result.timed_out || push_result.output_truncated) {
    std::cerr << "ckgit: non-destructive publish push failed\n";
    return 1;
  }
  std::string registration_failure;
  if (!registerRemoteCheckout(audit, config, project, &registration_failure)) {
    std::cerr << "ckgit: published " << project
              << " but could not refresh checkout registration: " << registration_failure << "\n";
    return kPartial;
  }
  std::cout << "Published " << project << " from " << audit.path.string() << "\n";
  return 0;
}

std::optional<std::string> pairedProjectForAudit(const ckgit::RepositoryAudit& audit,
                                                 const ckgit::ClientConfig& config, bool* invalid) {
  *invalid = false;
  std::optional<std::string> project_name;
  for (const auto& remote : audit.remotes) {
    if (remote.remote_name != config.remote_name) {
      continue;
    }
    const auto project = ckgit::projectForConfiguredServerRemote(remote, config);
    if (!project.has_value() || (project_name.has_value() && *project_name != *project)) {
      *invalid = true;
      return std::nullopt;
    }
    project_name = project;
  }
  return project_name;
}

std::string reportedPathForRegistration(const ckgit::RepositoryAudit& audit,
                                        const ckgit::ClientConfig& config) {
  return config.expose_full_paths ? audit.path.string() : audit.path.filename().string();
}

bool registerRemoteCheckout(const ckgit::RepositoryAudit& audit, const ckgit::ClientConfig& config,
                            std::string_view project, std::string* failure_reason) {
  try {
    const std::string encoded_path = ckgit::encodeCheckoutPath(reportedPathForRegistration(audit, config));
    const auto result = ckgit::runProcess(
        {"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "RequestTTY=no",
         "-o", "ClearAllForwardings=yes", config.server,
         "ckgit-rpc 1 register " + std::string(project) + " " + encoded_path});
    if (result.exit_code == 0 && !result.timed_out && !result.output_truncated &&
        result.output == "ok registered\n") {
      return true;
    }
    *failure_reason = "remote checkout registration was rejected";
  } catch (const std::exception& error) {
    *failure_reason = error.what();
  }
  return false;
}

int registerCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::filesystem::path> repository_path;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if ((arguments[index] == "--config" || arguments[index] == "--repo") &&
        index + 1 < arguments.size()) {
      const std::string value = arguments[++index];
      if (arguments[index - 1] == "--config") {
        config_path.emplace(value);
      } else {
        repository_path.emplace(value);
      }
    } else {
      std::cerr << "ckgit: usage: ckgit register --config PATH [--repo PATH]\n";
      return kUsage;
    }
  }
  if (!config_path.has_value()) {
    std::cerr << "ckgit: register requires --config PATH\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(*config_path);
  ckgit::RepositoryAudit audit;
  try {
    audit = ckgit::inspectRepository(repository_path.value_or("."));
  } catch (const std::exception& error) {
    std::cerr << "ckgit: cannot inspect repository to register: " << error.what() << "\n";
    return 1;
  }
  bool invalid_remote = false;
  const auto project = pairedProjectForAudit(audit, config, &invalid_remote);
  if (!project.has_value()) {
    std::cerr << "ckgit: " << audit.path.string() << ": "
              << (invalid_remote ? "configured remote has an unrecognized or ambiguous URL"
                                 : "registration requires an exactly matched configured remote") << "\n";
    return 1;
  }
  std::string failure_reason;
  if (!registerRemoteCheckout(audit, config, *project, &failure_reason)) {
    std::cerr << "ckgit: checkout registration failed: " << failure_reason << "\n";
    return 1;
  }
  std::cout << "Registered " << *project << " from " << reportedPathForRegistration(audit, config) << "\n";
  return 0;
}

int syncOne(const ckgit::RepositoryAudit& audit, const ckgit::ClientConfig& config, bool dry_run) {
  bool invalid_remote = false;
  const auto project = pairedProjectForAudit(audit, config, &invalid_remote);
  if (!project.has_value()) {
    std::cerr << "ckgit: " << audit.path.string() << ": "
              << (invalid_remote ? "configured remote has an unrecognized or ambiguous URL"
                                 : "sync requires an exactly matched configured remote") << "\n";
    return 1;
  }
  const std::vector<std::string> common_push = {
      "git", "-C", audit.path.string(), "push", "--porcelain", "--atomic", config.remote_name,
      "refs/heads/*:refs/heads/*", "refs/tags/*:refs/tags/*"};
  std::vector<std::string> preflight = common_push;
  preflight.insert(preflight.begin() + 5, "--dry-run");
  const auto preflight_result = ckgit::runProcess(preflight);
  if (preflight_result.exit_code != 0 || preflight_result.timed_out || preflight_result.output_truncated) {
    std::cerr << "ckgit: " << audit.path.string()
              << ": sync preflight rejected a non-destructive atomic update\n";
    return kPartial;
  }
  if (dry_run) {
    std::cout << "Sync preflight succeeded for " << audit.path.string() << "; no refs changed.\n";
    return 0;
  }
  std::string registration_failure;
  if (!registerRemoteCheckout(audit, config, *project, &registration_failure)) {
    std::cerr << "ckgit: " << audit.path.string()
              << ": checkout registration failed; no refs were pushed: " << registration_failure << "\n";
    return kPartial;
  }
  const auto push_result = ckgit::runProcess(common_push);
  if (push_result.exit_code != 0 || push_result.timed_out || push_result.output_truncated) {
    std::cerr << "ckgit: " << audit.path.string()
              << ": sync push failed after preflight; no fallback force push was attempted\n";
    return kPartial;
  }
  std::cout << "Synced " << audit.path.string();
  if (audit.changed_entries != 0) {
    std::cout << " (" << audit.changed_entries << " uncommitted worktree entries were not transferred)";
  }
  std::cout << "\n";
  return 0;
}

int syncCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::filesystem::path> repository_path;
  bool dry_run = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if ((arguments[index] == "--config" || arguments[index] == "--repo") && index + 1 < arguments.size()) {
      const std::string value = arguments[++index];
      if (arguments[index - 1] == "--config") {
        config_path.emplace(value);
      } else {
        repository_path.emplace(value);
      }
    } else if (arguments[index] == "--dry-run") {
      dry_run = true;
    } else {
      std::cerr << "ckgit: usage: ckgit sync --config PATH --repo PATH [--dry-run]\n";
      return kUsage;
    }
  }
  if (!config_path.has_value()) {
    std::cerr << "ckgit: sync requires --config PATH\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(*config_path);
  const auto lock = acquireSyncLock(config, "sync");
  if (!lock.has_value()) {
    return kBusy;
  }
  std::vector<ckgit::RepositoryAudit> audits;
  bool partial = false;
  if (repository_path.has_value()) {
    // An explicit checkout may be synced even when it is not the canonical one.
    try {
      audits.push_back(ckgit::inspectRepository(*repository_path));
    } catch (const std::exception& error) {
      std::cerr << "ckgit: cannot inspect repository to sync: " << error.what() << "\n";
      return 1;
    }
    return syncOne(audits.front(), config, dry_run) != 0 ? kPartial : 0;
  } else {
    const auto discovery = ckgit::discoverWorkingTrees(config.scan_roots, config.exclusions);
    for (const auto& warning : discovery.warnings) {
      std::cerr << "warning: " << warning << "\n";
      partial = true;
    }
    for (const auto& path : discovery.repositories) {
      try {
        audits.push_back(ckgit::inspectRepository(path));
      } catch (const std::exception& error) {
        std::cerr << "warning: " << path.string() << ": " << error.what() << "\n";
        partial = true;
      }
    }
  }
  const auto selections = ckgit::loadCanonicalCheckouts(canonicalSelectionPath(config));
  const auto groups = groupPairedCheckouts(audits, config, &partial);
  for (const auto& [project, checkouts] : groups) {
    std::string reason;
    const auto* chosen = chooseAutomaticCheckout(project, checkouts, selections, &reason);
    if (chosen == nullptr) {
      std::cerr << "ckgit: " << project << ": " << reason << "\n";
      partial = true;
      continue;
    }
    partial = syncOne(*chosen, config, dry_run) != 0 || partial;
  }
  return partial ? kPartial : 0;
}

int checkoutList(const ckgit::ClientConfig& config, const std::optional<std::string>& only_project) {
  bool attention = false;
  const auto discovery = ckgit::discoverWorkingTrees(config.scan_roots, config.exclusions);
  for (const auto& warning : discovery.warnings) {
    std::cerr << "warning: " << warning << "\n";
    attention = true;
  }
  std::vector<ckgit::RepositoryAudit> audits;
  for (const auto& path : discovery.repositories) {
    try {
      audits.push_back(ckgit::inspectRepository(path));
    } catch (const std::exception& error) {
      std::cerr << "warning: " << path.string() << ": " << error.what() << "\n";
      attention = true;
    }
  }
  const auto selections = ckgit::loadCanonicalCheckouts(canonicalSelectionPath(config));
  const auto groups = groupPairedCheckouts(audits, config, &attention);
  std::set<std::string> projects;
  for (const auto& [project, checkouts] : groups) {
    projects.insert(project);
  }
  for (const auto& [project, checkout] : selections) {
    projects.insert(project);
  }
  std::size_t listed = 0;
  for (const auto& project : projects) {
    if (only_project.has_value() && *only_project != project) {
      continue;
    }
    ++listed;
    std::cout << project << "\n";
    const auto group = groups.find(project);
    const std::vector<const ckgit::RepositoryAudit*> checkouts =
        group == groups.end() ? std::vector<const ckgit::RepositoryAudit*>{} : group->second;
    const auto selected = selections.find(project);
    const auto* proposed = selected == selections.end() && checkouts.size() > 1
                               ? proposeCanonicalCheckout(checkouts) : nullptr;
    bool selected_present = false;
    for (const auto* audit : checkouts) {
      const bool is_selected = selected != selections.end() && samePath(audit->path, selected->second);
      selected_present = selected_present || is_selected;
      const char* state = is_selected ? "canonical"
                        : selected != selections.end() ? "other"
                        : checkouts.size() == 1 ? "single"
                        : audit == proposed ? "proposed"
                                            : "other";
      std::cout << "  " << state << std::string(11 - std::string_view(state).size(), ' ')
                << audit->path.string() << checkoutMarkers(*audit) << "\n";
    }
    if (selected != selections.end() && !selected_present) {
      std::cout << "  missing    " << selected->second.string()
                << " (selected canonical checkout is unavailable; automatic sync skips this project)\n";
      attention = true;
    } else if (selected == selections.end() && checkouts.size() > 1) {
      std::cout << "  note: no canonical checkout is selected; automatic sync skips this project\n";
      attention = true;
    }
  }
  if (listed == 0) {
    std::cout << (only_project.has_value() ? "No paired checkout found for " + *only_project + "."
                                           : std::string("No paired checkouts found.")) << "\n";
  }
  return attention ? kPartial : 0;
}

int checkoutSetCanonical(const ckgit::ClientConfig& config, const std::string& project,
                         const std::filesystem::path& requested_path) {
  ckgit::RepositoryAudit audit;
  try {
    audit = ckgit::inspectRepository(requested_path);
  } catch (const std::exception& error) {
    std::cerr << "ckgit: cannot inspect checkout: " << error.what() << "\n";
    return 1;
  }
  bool invalid = false;
  const auto paired = pairedProjectForAudit(audit, config, &invalid);
  if (!paired.has_value() || *paired != project) {
    std::cerr << "ckgit: " << audit.path.string() << " is not a checkout paired with " << project << "\n";
    return 1;
  }
  ckgit::saveCanonicalCheckout(canonicalSelectionPath(config), project, audit.path);
  std::cout << "Selected " << audit.path.string() << " as the canonical checkout for " << project << "\n";
  return 0;
}

int checkoutCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::vector<std::string> positional;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index] == "--config" && index + 1 < arguments.size()) {
      config_path.emplace(arguments[++index]);
    } else if (!arguments[index].empty() && arguments[index].front() == '-') {
      positional.clear();
      break;
    } else {
      positional.push_back(arguments[index]);
    }
  }
  const bool list = positional.size() >= 1 && positional.size() <= 2 && positional[0] == "list" &&
                    (positional.size() == 1 || ckgit::isValidProjectName(positional[1]));
  const bool set_canonical = positional.size() == 3 && positional[0] == "set-canonical" &&
                             ckgit::isValidProjectName(positional[1]) && !positional[2].empty();
  if (!config_path.has_value() || (!list && !set_canonical)) {
    std::cerr << "ckgit: usage: ckgit checkout list --config PATH [NAME]\n"
              << "              ckgit checkout set-canonical --config PATH NAME PATH\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(*config_path);
  if (list) {
    return checkoutList(config, positional.size() == 2 ? std::optional<std::string>(positional[1]) : std::nullopt);
  }
  return checkoutSetCanonical(config, positional[1], positional[2]);
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
      printUsage(std::cout);
      return 0;
    }
    if (argc < 2) {
      printUsage(std::cerr);
      return kUsage;
    }
    std::vector<std::string> arguments;
    for (int index = 2; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
    const std::string command = argv[1];
    if (command == "scan") {
      return scan(arguments);
    }
    if (command == "config") {
      return configCommand(arguments);
    }
    if (command == "status") {
      return statusCommand(arguments);
    }
    if (command == "clone") {
      return cloneCommand(arguments);
    }
    if (command == "create") {
      return createCommand(arguments);
    }
    if (command == "publish") {
      return publishCommand(arguments);
    }
    if (command == "register") {
      return registerCommand(arguments);
    }
    if (command == "sync") {
      return syncCommand(arguments);
    }
    if (command == "checkout") {
      return checkoutCommand(arguments);
    }
    printUsage(std::cerr);
    return kUsage;
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << error.what() << "\n";
    return 1;
  }
}
