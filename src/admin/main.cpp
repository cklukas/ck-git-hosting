// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "ckgit/authorized_keys.hpp"
#include "ckgit/ci_store.hpp"
#include "ckgit/pages_store.hpp"
#include "ckgit/install_layout.hpp"
#include "ckgit/repository_store.hpp"
#include "ckgit/control_rpc.hpp"
#include "ckgit/recovery.hpp"
#include "ckgit/server_config.hpp"
#include "ckgit/process.hpp"

namespace {

constexpr int kUsage = 2;
constexpr std::size_t kMaximumKeyFileBytes = 8 * 1024;

void usage(std::ostream& output) {
  output << "Usage:\n"
         << "  ckgit-admin create NAME (--config FILE | --repo-root ROOT) [--default-branch BRANCH] [--hook-directory PATH] [--dry-run]\n"
         << "  ckgit-admin remove-project NAME (--config FILE | --repo-root ROOT --state-root ROOT) [--control-socket PATH] [--dry-run] [--yes]\n"
         << "  ckgit-admin authorized-key --client-id ID --public-key FILE [--shell PATH] [--repo-root ROOT]\n"
         << "                             [--control-socket PATH] [--state-root ROOT]\n"
         << "  ckgit-admin backup --output DIR [ROOT OPTIONS] [--dry-run] [--yes]\n"
         << "  ckgit-admin verify-backup BACKUP\n"
         << "  ckgit-admin restore-backup BACKUP [ROOT OPTIONS] [--dry-run] [--yes]\n"
         << "  ckgit-admin trash list [ROOT OPTIONS]\n"
         << "  ckgit-admin restore-project ENTRY [--name NAME] [ROOT OPTIONS] [--dry-run] [--yes]\n"
         << "  ckgit-admin ci (enable|disable|status) NAME (--config FILE | --state-root ROOT)\n"
         << "\n"
         << "ROOT OPTIONS: --config FILE or --repo-root ROOT --state-root ROOT;\n"
         << "              optionally --hook-directory PATH to select destination post-receive hooks.\n"
         << "Recovery changes and remove-project first show their plan. Confirm on a terminal, or use --yes;\n"
         << "--dry-run validates without writing. Existing destinations are never overwritten.\n"
         << "backup saves reachable Git history, all refs/HEAD, original config for review, and private metadata.\n"
         << "verify-backup checks the SHA-256 inventory, Git objects, and metadata without restoring.\n"
         << "restore-backup requires empty destination metadata; fresh hosting settings replace original Git config.\n"
         << "restore-project recovers Git from a retained trash entry, keeps that copy, and recreates no metadata.\n"
         << "Reflogs, unreachable objects, server OS settings, credentials, and SSH keys need separate backups.\n\n"
         << "authorized-key prints one restricted OpenSSH authorized_keys line for a device key.\n"
         << "It never edits a file; append the line to the installed authorized_keys yourself.\n";
}

int recoveryCommand(int argc, char* argv[]) {
  const std::string verb = argv[1];
  if (verb == "verify-backup") {
    if (argc != 3) { std::cerr << "ckgit-admin: verify-backup requires one backup directory\n"; return kUsage; }
    const auto summary = ckgit::verifyBackup(argv[2]);
    std::cout << "Verified " << argv[2] << ": " << summary.projects.size() << " project(s), "
              << summary.files << " files, " << summary.bytes << " bytes.\n";
    return 0;
  }
  std::optional<std::filesystem::path> config;
  std::optional<std::filesystem::path> hooks;
  std::optional<std::filesystem::path> output;
  std::optional<std::string> name;
  std::vector<std::string> positional;
  ckgit::RecoveryPaths paths;
  bool dry_run = false, yes = false, positional_only = false;
  int start = 2;
  if (verb == "trash") {
    if (argc < 3 || std::string(argv[2]) != "list") { std::cerr << "ckgit-admin: usage: ckgit-admin trash list [ROOT OPTIONS]\n"; return kUsage; }
    start = 3;
  }
  for (int index = start; index < argc; ++index) {
    const std::string argument = argv[index];
    if (!positional_only && argument == "--") positional_only = true;
    else if (positional_only) positional.push_back(argument);
    else if (argument == "--dry-run") dry_run = true;
    else if (argument == "--yes") yes = true;
    else if (argument == "--config" || argument == "--repo-root" || argument == "--state-root" ||
             argument == "--hook-directory" || (verb == "backup" && argument == "--output") ||
             (verb == "restore-project" && argument == "--name")) {
      if (index + 1 == argc) { std::cerr << "ckgit-admin: " << argument << " requires a value\n"; return kUsage; }
      const std::string value = argv[++index];
      if (argument == "--config") config = value;
      else if (argument == "--repo-root") paths.repo_root = value;
      else if (argument == "--state-root") paths.state_root = value;
      else if (argument == "--hook-directory") hooks = value;
      else if (argument == "--output") output = value;
      else name = value;
    } else if (!argument.empty() && argument.front() != '-') positional.push_back(argument);
    else { std::cerr << "ckgit-admin: unknown " << verb << " option: " << argument << "\n"; return kUsage; }
  }
  if (config.has_value() && (!paths.repo_root.empty() || !paths.state_root.empty())) {
    std::cerr << "ckgit-admin: --config cannot be combined with --repo-root or --state-root\n";
    return kUsage;
  }
  std::optional<std::filesystem::path> control_socket;
  if (config.has_value()) {
    const auto server = ckgit::loadServerConfig(*config);
    paths.repo_root = server.repo_root;
    paths.state_root = server.state_root.value_or(std::filesystem::path{});
    paths.hook_directory = server.hook_directory;
    control_socket = server.control_socket;
  }
  if (hooks.has_value()) paths.hook_directory = hooks;
  if (paths.repo_root.empty() || paths.state_root.empty()) {
    std::cerr << "ckgit-admin: recovery requires --config with state_root, or both --repo-root and --state-root\n";
    return kUsage;
  }
  if ((verb == "backup" && (!output.has_value() || !positional.empty())) ||
      ((verb == "restore-backup" || verb == "restore-project") && positional.size() != 1) ||
      (verb == "trash" && (!positional.empty() || dry_run || yes))) {
    std::cerr << "ckgit-admin: invalid " << verb << " arguments; use ckgit-admin --help\n";
    return kUsage;
  }
  if (verb == "trash") {
    const auto entries = ckgit::listTrashedRepositories(paths);
    if (entries.empty()) std::cout << "No retained repository trash.\n";
    else {
      std::cout << "Entry\tProject\n";
      for (const auto& entry : entries) std::cout << entry.entry << "\t" << entry.project << "\n";
    }
    return 0;
  }
  std::vector<std::string> affected;
  if (verb == "backup") {
    const auto summary = ckgit::backupHostedRepositories(paths, *output, true);
    affected = summary.projects;
    std::cout << "Would back up " << affected.size() << " project(s) from " << paths.repo_root.string()
              << " and private metadata from " << paths.state_root.string() << " to " << output->string() << ".\n";
  } else if (verb == "restore-backup") {
    affected = ckgit::restoreBackup(paths, positional.front(), true);
    std::cout << "Would restore " << affected.size() << " project(s) to " << paths.repo_root.string()
              << " and metadata to " << paths.state_root.string() << ".\n";
  } else {
    const auto destination = ckgit::restoreTrashedRepository(paths, positional.front(), name, true);
    affected.push_back(destination.stem().string());
    std::cout << "Would restore Git to " << destination.string() << "; keep the trash copy and recreate no metadata.\n";
  }
  for (const auto& project : affected) std::cout << "  " << project << "\n";
  if (verb != "backup") {
    std::cout << "Fresh hosting safety settings will be installed. Original Git config stays in the backup for review.\n"
              << "Destination hooks: " << (paths.hook_directory.has_value() ? paths.hook_directory->string() : "none configured") << "\n";
  }
  if (dry_run) { std::cout << "Preview only; nothing changed.\n"; return 0; }
  if (!yes) {
    if (!isatty(STDIN_FILENO)) { std::cout << "Preview only; use --yes to execute this plan.\n"; return 0; }
    std::cout << "Execute this plan? Type yes: " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    if (answer != "yes") { std::cout << "Cancelled; nothing changed.\n"; return 0; }
  }
  if (verb == "backup") {
    const auto summary = ckgit::backupHostedRepositories(paths, *output);
    std::cout << "Backup verified and saved to " << output->string() << " (" << summary.files << " files).\n";
  } else if (verb == "restore-backup") {
    affected = ckgit::restoreBackup(paths, positional.front());
    std::cout << "Restored " << affected.size() << " project(s) and their private metadata.\n";
  } else {
    const auto restored = ckgit::restoreTrashedRepository(paths, positional.front(), name);
    std::cout << "Restored Git to " << restored.string() << "; retained trash copy, no metadata recreated.\n";
  }
  if (verb != "backup" && control_socket.has_value()) {
    for (const auto& project : affected) {
      try { ckgit::forwardControlRpc(*control_socket, "admin", "refresh", project, {}, nullptr, std::chrono::seconds(2)); }
      catch (const std::exception&) { /* The index sweep also discovers restored repositories. */ }
    }
  }
  return 0;
}

int create(int argc, char* argv[]) {
  if (argc < 3) {
    usage(std::cerr);
    return kUsage;
  }
  const std::string name = argv[2];
  std::optional<std::filesystem::path> config;
  std::filesystem::path root;
  std::optional<std::filesystem::path> hook_directory;
  std::string branch{"main"};
  bool dry_run = false;
  for (int index = 3; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--repo-root" || argument == "--config" || argument == "--default-branch" ||
        argument == "--hook-directory") {
      if (index + 1 == argc) {
        std::cerr << "ckgit-admin: " << argument << " requires a value\n";
        return kUsage;
      }
      const std::string value = argv[++index];
      if (argument == "--repo-root") {
        root = value;
      } else if (argument == "--config") {
        config = value;
      } else if (argument == "--hook-directory") {
        hook_directory.emplace(value);
      } else {
        branch = value;
      }
    } else if (argument == "--dry-run") {
      dry_run = true;
    } else {
      std::cerr << "ckgit-admin: unknown option: " << argument << "\n";
      return kUsage;
    }
  }
  if (config.has_value() && !root.empty()) {
    std::cerr << "ckgit-admin: --config cannot be combined with --repo-root\n";
    return kUsage;
  }
  if (config.has_value()) {
    const auto server = ckgit::loadServerConfig(*config);
    root = server.repo_root;
    if (!hook_directory.has_value()) hook_directory = server.hook_directory;
  }
  if (root.empty()) {
    std::cerr << "ckgit-admin: create requires --config or --repo-root\n";
    return kUsage;
  }
  const auto repository = ckgit::createBareRepository(root, name, branch, dry_run, hook_directory);
  std::cout << (dry_run ? "Would create " : "Created ") << repository.string() << "\n";
  return 0;
}

int removeProject(int argc, char* argv[]) {
  if (argc < 3) {
    usage(std::cerr);
    return kUsage;
  }
  const std::string name = argv[2];
  std::optional<std::filesystem::path> config;
  std::filesystem::path repo_root;
  std::filesystem::path state_root;
  std::filesystem::path pages_root;
  std::optional<std::filesystem::path> control_socket;
  bool dry_run = false, yes = false;
  for (int index = 3; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--dry-run") {
      dry_run = true;
    } else if (argument == "--yes") {
      yes = true;
    } else if ((argument == "--config" || argument == "--repo-root" || argument == "--state-root" ||
                argument == "--control-socket") && index + 1 < argc) {
      const std::string value = argv[++index];
      if (argument == "--config") config = value;
      else if (argument == "--repo-root") repo_root = value;
      else if (argument == "--state-root") state_root = value;
      else control_socket = value;
    } else {
      std::cerr << "ckgit-admin: invalid remove-project option: " << argument << "\n";
      return kUsage;
    }
  }
  if (config.has_value() && (!repo_root.empty() || !state_root.empty())) {
    std::cerr << "ckgit-admin: --config cannot be combined with --repo-root or --state-root\n";
    return kUsage;
  }
  if (config.has_value()) {
    const auto server = ckgit::loadServerConfig(*config);
    repo_root = server.repo_root;
    state_root = server.state_root.value_or(std::filesystem::path{});
    if (!control_socket.has_value() && !server.control_socket.empty()) control_socket = server.control_socket;
    pages_root = server.pages_root.value_or(std::filesystem::path{});
  }
  if (repo_root.empty() || state_root.empty()) {
    std::cerr << "ckgit-admin: remove-project requires --config with state_root, or both --repo-root and --state-root\n";
    return kUsage;
  }
  if (dry_run) {
    const auto destination = ckgit::removeProject(repo_root, state_root, name, true);
    std::cout << "Would move repository to " << destination.string() << " and remove its metadata\n";
    return 0;
  }
  // The exact trash destination is computed at execution time (its suffix is
  // derived from that moment), so the plan describes the operation rather
  // than promising a path that an interactive confirmation delay could miss.
  std::cout << "Remove hosted project " << name << ": moves its Git repository into "
            << (repo_root / ".trash").string()
            << " and clears its checkout registrations, derived metadata, and event-archive entries.\n"
               "The repository stays recoverable with ckgit-admin trash list / restore-project.\n";
  if (!yes) {
    if (!isatty(STDIN_FILENO)) { std::cout << "Preview only; use --yes to execute this plan.\n"; return 0; }
    std::cout << "Execute this plan? Type yes: " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    if (answer != "yes") { std::cout << "Cancelled; nothing changed.\n"; return 0; }
  }
  const auto destination = ckgit::removeProject(repo_root, state_root, name, false);
  try {
    ckgit::removeProjectCi(state_root, name);
  } catch (const std::exception&) {
    // CI state is best-effort cleanup; the repository move already succeeded.
  }
  if (!pages_root.empty()) {
    try {
      ckgit::removeProjectPages(pages_root, name);
    } catch (const std::exception&) {
    }
  }
  if (control_socket.has_value()) {
    try {
      ckgit::forwardControlRpc(*control_socket, "admin", "refresh", name, {}, nullptr,
                               std::chrono::seconds(2));
    } catch (const std::exception&) {
      // The periodic index sweep also discovers the removed repository.
    }
  }
  std::cout << "Moved repository to " << destination.string() << " and removed its metadata\n";
  return 0;
}

std::string readPublicKeyFile(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
    throw std::runtime_error("public key must be a regular non-symlink file: " + path.string());
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > kMaximumKeyFileBytes) {
    throw std::runtime_error("public key file is too large or unavailable: " + path.string());
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open public key file: " + path.string());
  }
  std::ostringstream content;
  content << file.rdbuf();
  return content.str();
}

int ciCommand(int argc, char* argv[]) {
  if (argc < 3) { usage(std::cerr); return kUsage; }
  const std::string action = argv[2];
  if (action != "enable" && action != "disable" && action != "status") {
    std::cerr << "ckgit-admin: ci action must be enable, disable, or status\n";
    return kUsage;
  }
  std::string name;
  std::optional<std::filesystem::path> config;
  std::filesystem::path state_root;
  for (int index = 3; index < argc; ++index) {
    const std::string argument = argv[index];
    if ((argument == "--config" || argument == "--state-root") && index + 1 < argc) {
      const std::string value = argv[++index];
      if (argument == "--config") config = value;
      else state_root = value;
    } else if (!argument.empty() && argument.front() != '-' && name.empty()) {
      name = argument;
    } else {
      std::cerr << "ckgit-admin: invalid ci option: " << argument << "\n";
      return kUsage;
    }
  }
  if (name.empty()) { std::cerr << "ckgit-admin: ci " << action << " requires a project name\n"; return kUsage; }
  if (config.has_value() && !state_root.empty()) {
    std::cerr << "ckgit-admin: --config cannot be combined with --state-root\n";
    return kUsage;
  }
  if (config.has_value()) state_root = ckgit::loadServerConfig(*config).state_root.value_or(std::filesystem::path{});
  if (state_root.empty()) {
    std::cerr << "ckgit-admin: ci requires --config with state_root, or --state-root\n";
    return kUsage;
  }
  if (action == "status") {
    std::cout << name << ": CI " << (ckgit::isProjectCiEnabled(state_root, name) ? "enabled" : "disabled") << "\n";
    return 0;
  }
  ckgit::setProjectCiEnabled(state_root, name, action == "enable");
  std::cout << "CI " << (action == "enable" ? "enabled" : "disabled") << " for project " << name << "\n";
  return 0;
}

int authorizedKey(int argc, char* argv[]) {
  std::string client_id;
  std::optional<std::filesystem::path> public_key;
  ckgit::ForcedCommandLayout layout{std::filesystem::path(ckgit::kInstalledShellPath),
                                    std::filesystem::path(ckgit::kInstalledRepositoryRoot),
                                    std::filesystem::path(ckgit::kInstalledControlSocket),
                                    std::filesystem::path(ckgit::kInstalledStateRoot)};
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument != "--client-id" && argument != "--public-key" && argument != "--shell" &&
        argument != "--repo-root" && argument != "--control-socket" && argument != "--state-root") {
      std::cerr << "ckgit-admin: unknown option: " << argument << "\n";
      return kUsage;
    }
    if (index + 1 == argc) {
      std::cerr << "ckgit-admin: " << argument << " requires a value\n";
      return kUsage;
    }
    const std::string value = argv[++index];
    if (argument == "--client-id") {
      client_id = value;
    } else if (argument == "--public-key") {
      public_key.emplace(value);
    } else if (argument == "--shell") {
      layout.shell = value;
    } else if (argument == "--repo-root") {
      layout.repo_root = value;
    } else if (argument == "--control-socket") {
      layout.control_socket = value;
    } else {
      layout.state_root = value;
    }
  }
  if (client_id.empty() || !public_key.has_value()) {
    std::cerr << "ckgit-admin: authorized-key requires --client-id and --public-key\n";
    return kUsage;
  }
  std::cout << ckgit::renderAuthorizedKeyLine(client_id, readPublicKeyFile(*public_key), layout);
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    ckgit::installChildTerminationForwarding();
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--help" || argument == "-h" || (index == 1 && argument == "help")) {
        usage(std::cout);
        return 0;
      }
    }
    if (argc >= 2) {
      const std::string verb = argv[1];
      if (verb == "backup" || verb == "verify-backup" || verb == "restore-backup" || verb == "trash" || verb == "restore-project") {
        return recoveryCommand(argc, argv);
      }
    }
    if (argc >= 2 && std::string(argv[1]) == "create") {
      return create(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "authorized-key") {
      return authorizedKey(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "remove-project") {
      return removeProject(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "ci") {
      return ciCommand(argc, argv);
    }
    usage(std::cerr);
    return kUsage;
  } catch (const std::exception& error) {
    std::cerr << "ckgit-admin: " << error.what() << "\n";
    return 1;
  }
}
