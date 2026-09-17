// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/cli_help.hpp"

#include "ckgit/validation.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#ifndef CKGIT_BUILD_VERSION
#define CKGIT_BUILD_VERSION "development"
#endif

namespace ckgit {
namespace {

enum class ValueKind { kFlag, kText, kPath, kProject, kBranch, kPort, kHost, kShell };

struct Option {
  std::string name;
  std::string value_name;
  std::string description;
  ValueKind kind{ValueKind::kFlag};
  bool repeatable{false};
  // Still parsed and validated like any other option; only omitted from the
  // synopsis, the detailed help block, and shell completion. For test-only
  // seams that a normal user has no reason to discover.
  bool hidden{false};
};

struct Argument {
  std::string name;
  std::string description;
  ValueKind kind{ValueKind::kText};
  bool required{false};
  bool repeatable{false};
};

struct Command {
  std::vector<std::string> path;
  std::string purpose;
  std::string scope;
  std::string effects;
  std::vector<Option> options;
  std::vector<Argument> arguments;
  std::vector<std::string> examples;
  bool uses_config{true};
};

const Option kConfig{"--config", "PATH",
                     "Client configuration (default: ~/.config/ck-git-hosting/client.ini).",
                     ValueKind::kPath};
const Option kRepo{"--repo", "PATH", "Select one checkout explicitly; use --repo . for this repository.",
                   ValueKind::kPath};
const Option kDryRun{"--dry-run", "", "Show the concrete plan without changing files, refs, or management."};
const Option kReplace{"--replace-checkout", "",
                      "Make the selected folder this device's main checkout, replacing its previous selection."};
const Option kVerbose{"--verbose", "", "Expand the plan to show every affected, unchanged, and excluded ref."};

const std::vector<Command>& commands() {
  static const std::vector<Command> definitions{
      {{"setup"}, "Configure this device with a guided setup.",
       "Create the client configuration at --config PATH or the default location. Interactive setup asks for missing settings; command-line values also support automation.",
       "Preview the settings before saving a private configuration file. Existing files require --overwrite. Setup does not install the server, authorize SSH keys, or publish projects; run doctor to check the connection.",
       {{"--server", "USER@HOST", "Restricted Git SSH target, for example ckgit@rpi4.", ValueKind::kHost},
        {"--client-id", "ID", "Device identity matching its authorized SSH key.", ValueKind::kText},
        {"--display-name", "TEXT", "Friendly device name.", ValueKind::kText},
        {"--scan-root", "PATH", "Local discovery root; repeat for several roots.", ValueKind::kPath, true},
        {"--remote-name", "NAME", "Git remote name (default: ckgit).", ValueKind::kText},
        {"--public-path-mode", "MODE", "Reported checkout paths: basename (default) or full.", ValueKind::kText},
        {"--web-host", "HOST", "Ordinary SSH login for dashboard forwarding.", ValueKind::kHost},
        {"--remote-port", "PORT", "Dashboard port on the server (default: 8420).", ValueKind::kPort},
        {"--yes", "", "Save the displayed configuration without prompting."},
        {"--overwrite", "", "Allow replacing an existing configuration; retains settings not overridden."}, kDryRun}, {},
       {"ckgit setup", "ckgit setup --server ckgit@rpi4 --client-id laptop --web-host rpi4 --yes", "ckgit setup --overwrite --dry-run"}},
      {{"doctor"}, "Diagnose configuration, SSH access, and dashboard connectivity.",
       "Inspect this configuration and its restricted Git target, then test dashboard forwarding through the configured ordinary SSH login.",
       "Read-only: sends a control ping and opens a short-lived loopback tunnel for an HTTP probe. No browser is opened or project data changed. SSH host keys must already be trusted. Failures include the next repair step and return nonzero.",
       {{"--web-host", "HOST", "Override the dashboard SSH login.", ValueKind::kHost},
        {"--remote-port", "PORT", "Override the remote dashboard port.", ValueKind::kPort},
        {"--timeout", "SECONDS", "Timeout per connection check, 1–60 seconds (default: 10).", ValueKind::kText}}, {},
       {"ckgit doctor", "ckgit doctor --web-host admin@server --timeout 15"}},
      {{"version"}, "Show this CLI's version and the versions running on the server.",
       "Print this ckgit build, then query the configured server for the running versions of the host daemon and its CI-runner and Pages services, so all four are visible at once.",
       "Read-only: sends one control query over the restricted SSH transport. With no configured or reachable server it prints only the local build. `ckgit --version` prints just the local build without contacting the server.",
       {}, {},
       {"ckgit version"}},
      {{"projects"}, "Discover projects hosted on the configured server.",
       "List hosted projects with this device's management state and selected checkout path. Missing and legacy unresolved selections remain visible.",
       "Read-only. Unmanaged means no selection or registration on this device; it does not scan all local folders for ordinary Git clones.",
       {{"--json", "", "Print a versioned JSON report with project names, state, checkout, and clone URL."},
        {"--uncloned", "", "Show only projects not managed on this device."},
        {"--filter", "TEXT", "Show project names containing this text (case-sensitive).", ValueKind::kText}}, {},
       {"ckgit projects", "ckgit projects --uncloned", "ckgit projects --json"}},
      {{"fetch"}, "Download hosted branches and tags without changing working files.",
       "Use all managed main checkouts by default, or select --repo PATH or --project NAME. --all explicitly selects the default scope.",
       "Fetch hosted heads into the configured remote-tracking namespace and fetch tags. Does not change local branches, working files, or delete refs. Conflicting tags fail safely. Projects needing attention are skipped while others continue. Dry-run compares remote tips without downloading objects.",
       {kRepo, {"--project", "NAME", "Select one managed project.", ValueKind::kProject}, {"--all", "", "Use all managed projects."},
        {"--branch", "NAME", "Fetch this hosted branch; repeat to select several (default: all).", ValueKind::kBranch, true},
        {"--no-tags", "", "Exclude tags from this fetch."}, kDryRun, kVerbose}, {},
       {"ckgit fetch --dry-run", "ckgit fetch", "ckgit fetch --repo . --branch main"}},
      {{"update"}, "Fast-forward clean checked-out branches from the server.",
       "Use all managed main checkouts by default, or select --repo PATH or --project NAME. --all explicitly selects the default scope.",
       "Fetch each checkout's current branch and tags, then fast-forward only. Never switches branches, rebases, creates a merge commit, or discards local changes. Dirty, detached, missing, and diverged checkouts need attention and are skipped while others continue. Dry-run makes no changes and explains when ancestry requires a fetch.",
       {kRepo, {"--project", "NAME", "Select one managed project.", ValueKind::kProject}, {"--all", "", "Use all managed projects."}, kDryRun, kVerbose}, {},
       {"ckgit update --dry-run", "ckgit update --repo .", "ckgit update --all"}},
      {{"publish"}, "Publish committed local branches and tags.",
       "Inside a Git working tree, use that repository (or the explicit path). Outside Git, inspect only immediate child folders and list unpublished repositories; already published projects are skipped.",
       "A single repository is previewed unless --yes is given. Folder mode asks for confirmation on a terminal; use --yes for automation. Publishing creates the hosted project if needed, adds its configured remote, uploads selected committed refs, and makes the checkout managed on this device. New projects use the current committed branch as their default when it is selected; otherwise the first --branch becomes the default. Existing project identity comes from its configured remote. Uploads never force-update or delete server refs. Uncommitted files and remote-tracking branches (such as origin/main) are excluded. Earlier --branch and --no-tags choices are not a saved policy.",
       {{"--name", "NAME", "Hosted name for a new project (default: folder name); must agree with an existing remote. Repository mode only.", ValueKind::kProject},
        {"--branch", "NAME", "Upload this local branch; repeat for multiple branches (default: all local branches).", ValueKind::kBranch, true},
        {"--no-tags", "", "Exclude local tags (default: include all local tags)."},
        kReplace, {"--yes", "", "Execute the displayed plan without a confirmation prompt."}, kDryRun, kVerbose},
       {{"REPOSITORY-OR-FOLDER", "Repository or parent folder to publish (default: current directory).", ValueKind::kPath}},
       {"ckgit publish", "ckgit publish --dry-run --verbose", "ckgit publish --yes", "ckgit publish /path/to/projects", "ckgit publish --branch main --no-tags --yes"}},
      {{"sync"}, "Upload committed changes from managed checkouts to the server.",
       "By default, use the private managed-project inventory for this configuration and device, with one main checkout per project. --repo selects one checkout; --scan explicitly discovers paired repositories under configured scan roots.",
       "Upload all local branches and tags after a non-forcing Git preflight. This does not fetch, pull, merge, copy uncommitted files, or upload remote-tracking branches. Earlier publish --branch/--no-tags choices do not limit sync. A conflicting project is skipped while other projects continue. --dry-run shows the same ref and management plan without changing either.",
       {kRepo, {"--scan", "", "Discover paired checkouts under configured scan roots; use each project's effective main selection."},
        kReplace, kDryRun, kVerbose}, {},
       {"ckgit sync --dry-run", "ckgit sync", "ckgit sync --repo . --dry-run --verbose", "ckgit sync --repo /path/to/moved-project --replace-checkout"}},
      {{"status"}, "Show managed projects, checkout availability, and local/server ref differences.",
       "By default, report the same managed projects and main checkout selections as sync, including missing folders. Use --repo . for the current repository, or --scan to inspect additional discovered checkouts.",
       "Read-only: compares committed refs with the configured server. A differing tip is not a verified safe update; sync --dry-run performs Git's preflight. Missing folders, unresolved legacy registrations, and connection failures need attention and remain visible.",
       {kRepo, {"--scan", "", "Inspect repositories under configured scan roots instead of the managed inventory."}}, {},
       {"ckgit status", "ckgit status --repo .", "ckgit status --scan"}},
      {{"clone"}, "Download a hosted project into a new local folder.",
       "Clone NAME from the configured server. DESTINATION defaults to NAME and must not already exist. For bulk clone, choose --all or repeated --project NAME, plus --into DIR; omit positional arguments. Already managed projects are skipped. Missing selections or destination collisions need attention and are never overwritten. Bulk clone previews and prompts on a terminal; --yes executes without prompting.",
       "Creates a Git working tree with the configured remote name. If this device has no other main checkout for the project, the new checkout joins ordinary status and sync automatically. If another main checkout exists, keep that selection and explain how to select the new copy. Cloning does not recurse into submodules.",
       {{"--all", "", "Clone every hosted project not yet managed on this device."},
        {"--project", "NAME", "Select a hosted project for bulk clone; repeat for several.", ValueKind::kProject, true},
        {"--into", "DIR", "Existing parent directory for bulk clone; each project gets its own subfolder.", ValueKind::kPath},
        {"--yes", "", "Execute the bulk plan without prompting."}, kDryRun},
       {{"NAME", "Hosted project name, without .git; omit in bulk mode.", ValueKind::kProject, true},
            {"DESTINATION", "New local folder (default: NAME). Its parent must already exist.", ValueKind::kPath}},
       {"ckgit clone my-project", "ckgit clone my-project /path/to/work-copy", "ckgit clone --all --into /path/to/projects --dry-run", "ckgit clone --all --into /path/to/projects --yes", "ckgit clone --project first --project second --into /path/to/projects"}},
      {{"scan"}, "Discover and inspect local Git working trees.",
       "Recursively inspect ROOT arguments, or configured scan_root entries when no roots are given. Configured exclusions apply. Explicit roots can be used without a configuration file.",
       "Read-only local discovery: shows branches, tags, remotes, and working-tree state. It does not register, publish, or upload projects, and it does not query the server. Discovery warnings return exit 3.",
       {{"--json", "", "Print the versioned JSON discovery report instead of text."}},
       {{"ROOT", "Directory to scan recursively; repeat to inspect several roots.", ValueKind::kPath, false, true}},
       {"ckgit scan /path/to/projects", "ckgit scan --json /path/to/projects", "ckgit scan"}},
      {{"web"}, "Open the web dashboard through an SSH tunnel.",
       "Use ADMIN-HOST when supplied, otherwise configured web_host, falling back to the host part of server with your normal SSH login. This login must allow port forwarding; the restricted ckgit Git account does not.",
       "Starts a loopback-only local tunnel, checks the dashboard, prints its URL, and opens a browser. Keep this command running while browsing. Ctrl+C closes the tunnel. Supplying ADMIN-HOST allows use without client configuration.",
       {{"--port", "PORT", "Local port (default: 8420 if free, otherwise an available port).", ValueKind::kPort},
        {"--remote-port", "PORT", "Dashboard port on the server (default: configured web_port, otherwise 8420).", ValueKind::kPort},
        {"--project", "NAME", "Open this hosted project's overview directly.", ValueKind::kProject},
        {"--no-open", "", "Print the dashboard URL without opening a browser."}},
       {{"ADMIN-HOST", "SSH host alias or user@host with forwarding permission.", ValueKind::kHost}},
       {"ckgit web", "ckgit web --no-open", "ckgit web --project my-project", "ckgit web --port 8421 --remote-port 8420 admin@server"}},
      {{"release"}, "Inspect and download a project's durable, tag-triggered releases.",
       "A release is created on the server by a `.ckgit/ci.yml` package step running against a pushed tag; it is kept until the tag is deleted.",
       "Use list to see a project's releases and their assets, or download to fetch one asset.",
       {}, {}, {"ckgit release list my-project", "ckgit release download my-project --asset packages --into /tmp"}},
      {{"release", "list"}, "List a hosted project's durable releases.",
       "Query PROJECT over the restricted SSH control channel used by refs and refresh.",
       "Read-only. Shows each release's tag, commit, creation time, and assets (name, size, sha256), newest first, bounded to the 64 most recent releases.",
       {{"--json", "", "Print a versioned JSON report instead of text."}},
       {{"PROJECT", "Hosted project to list releases for.", ValueKind::kProject, true}},
       {"ckgit release list my-project", "ckgit release list my-project --json"}},
      {{"release", "download"}, "Download one release asset.",
       "List PROJECT's releases over SSH, then choose the newest release or --tag, and its sole asset or --asset.",
       "Opens a short-lived loopback tunnel like web (the same ordinary SSH login; the restricted ckgit Git account cannot forward ports), downloads the asset into --into (default: the current directory) as NAME.tar, verifies its size and sha256 against the SSH listing, then closes the tunnel. Requires http_port configured and the dashboard reachable on the server.",
       {{"--tag", "TAG", "Release to download from (default: the newest).", ValueKind::kText},
        {"--asset", "NAME", "Asset to download (default: the release's only asset).", ValueKind::kText},
        {"--into", "DIR", "Directory to write NAME.tar into (default: the current directory).", ValueKind::kPath},
        {"--dashboard-url", "URL", "Test seam: fetch from this base URL instead of opening an SSH tunnel.",
         ValueKind::kText, false, true}},
       {{"PROJECT", "Hosted project to download a release asset from.", ValueKind::kProject, true}},
       {"ckgit release download my-project --asset packages", "ckgit release download my-project --tag v1.2.0 --asset packages --into /tmp"}},
      {{"checkout"}, "Inspect and select this device's managed checkouts.",
       "One main checkout is used per project and device. Full local paths are stored privately; the server receives only the path information allowed by public_path_mode.",
       "Use list to inspect the inventory, set-canonical to select the main folder, migrate to resolve legacy server-only registrations, or forget to stop managing a checkout while retaining its files.",
       {}, {}, {"ckgit checkout list", "ckgit checkout set-canonical my-project /path/to/project", "ckgit checkout migrate --dry-run"}},
      {{"checkout", "list"}, "List managed projects and their main checkouts on this device.",
       "Use the same inventory as status and sync, optionally filtered to NAME. Missing or unresolved folders remain listed. --scan additionally exposes discovered copies.",
       "Read-only. Displays the effective main checkout selection; listing another copy does not make it participate in ordinary sync.",
       {{"--scan", "", "Include additional paired copies found under configured scan roots."}},
       {{"NAME", "Show only this hosted project.", ValueKind::kProject}},
       {"ckgit checkout list", "ckgit checkout list my-project", "ckgit checkout list --scan"}},
      {{"checkout", "set-canonical"}, "Select a project's main checkout on this device.",
       "Select PATH for hosted project NAME. The folder must already be a Git checkout with a matching configured remote.",
       "Updates the private local selection and server registration used by status, publish, and sync. The command name is retained for compatibility; canonical means main checkout on this device. No commits are uploaded and no other folder is deleted.",
       {}, {{"NAME", "Hosted project to select.", ValueKind::kProject, true},
            {"PATH", "Existing checkout to use as this device's main folder.", ValueKind::kPath, true}},
       {"ckgit checkout set-canonical my-project /path/to/project"}},
      {{"checkout", "migrate"}, "Resolve legacy checkout registrations into the private local inventory.",
       "Inspect this device's legacy server registrations and configured scan roots. Match checkouts by their validated server remote and reported folder identity.",
       "Adopts only uniquely matched checkouts into local management. Ambiguous matches and missing folders stay unresolved, with instructions to select a folder explicitly. This does not upload commits or require public_path_mode=full.",
       {kDryRun}, {}, {"ckgit checkout migrate --dry-run", "ckgit checkout migrate", "ckgit checkout set-canonical my-project /path/to/project"}},
      {{"checkout", "forget"}, "Stop managing a checkout without deleting its files.",
       "Forget hosted project NAME on this device, including missing or unresolved checkout selections.",
       "Preview and confirm removal of the private main selection and this device's server registration. Requires a server connection. Files, remotes, commits, the hosted project, and other devices remain intact. Ordinary managed commands stop using this checkout. Use register to manage it again; an explicit sync --scan can also rediscover it.",
       {{"--yes", "", "Execute without a confirmation prompt."}, kDryRun},
       {{"NAME", "Hosted project to forget on this device.", ValueKind::kProject, true}},
       {"ckgit checkout forget my-project --dry-run", "ckgit checkout forget my-project", "ckgit checkout forget my-project --yes"}},
      {{"register"}, "Make an existing paired checkout managed on this device.",
       "Use the current repository, or --repo PATH. The configured remote must identify a project on the configured server.",
       "Records the checkout privately and reports it with the configured path privacy. Does not create projects or upload commits. Another existing main checkout is protected unless --replace-checkout is supplied.",
       {kRepo, kReplace}, {}, {"ckgit register", "ckgit register --repo /path/to/project --replace-checkout"}},
      {{"create"}, "Create an empty hosted project.",
       "Create NAME on the configured server. For an existing local repository, publish usually completes the whole workflow in one command.",
       "Creates the empty remote project with its chosen default branch. Does not create a local checkout, add a remote, register a folder, or upload commits.",
       {{"--default-branch", "BRANCH", "Initial default branch (default: main).", ValueKind::kBranch}},
       {{"NAME", "New hosted project name, without .git.", ValueKind::kProject, true}},
       {"ckgit create my-project", "ckgit create my-project --default-branch develop"}},
      {{"config"}, "Inspect the client configuration.",
       "Use --config PATH or ~/.config/ck-git-hosting/client.ini.",
       "Use show to inspect effective values. This command does not change configuration.",
       {}, {}, {"ckgit config show", "ckgit config show --config /path/to/client.ini"}},
      {{"config", "show"}, "Show effective client configuration values.",
       "Read --config PATH, or ~/.config/ck-git-hosting/client.ini when --config is omitted.",
       "Read-only and local: displays the device identity, target server, remote name, scan roots, exclusions, and path privacy. Does not contact the server.",
       {}, {}, {"ckgit config show", "ckgit --config /path/to/client.ini config show"}},
      {{"completion"}, "Print shell completion definitions.",
       "Generate completion for bash or zsh from the same command and option definitions used by help and argument parsing.",
       "Writes shell code to stdout without reading configuration or contacting the server. Load it into the named shell; it completes commands, options, and local paths without publishing anything.",
       {}, {{"SHELL", "Shell to generate for: bash or zsh.", ValueKind::kShell, true}},
       {"source <(ckgit completion bash)", "autoload -Uz compinit; compinit", "source <(ckgit completion zsh)"}, false},
  };
  return definitions;
}

std::string join(const std::vector<std::string>& values, std::string_view separator = " ") {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) result += separator;
    result += value;
  }
  return result;
}

const Command* findCommand(const std::vector<std::string>& path) {
  const auto& definitions = commands();
  const auto found = std::find_if(definitions.begin(), definitions.end(),
                                [&](const Command& command) { return command.path == path; });
  return found == definitions.end() ? nullptr : &*found;
}

std::vector<const Command*> children(const std::vector<std::string>& path) {
  std::vector<const Command*> result;
  for (const auto& command : commands()) {
    if (command.path.size() == path.size() + 1 &&
        std::equal(path.begin(), path.end(), command.path.begin())) result.push_back(&command);
  }
  return result;
}

const Option* findOption(const Command& command, const std::string& name) {
  if (command.uses_config && name == kConfig.name) return &kConfig;
  const auto found = std::find_if(command.options.begin(), command.options.end(),
                                [&](const Option& option) { return option.name == name; });
  return found == command.options.end() ? nullptr : &*found;
}

std::pair<std::string, std::optional<std::string>> splitOption(const std::string& token) {
  const auto equals = token.find('=');
  if (equals == std::string::npos) return {token, std::nullopt};
  return {token.substr(0, equals), token.substr(equals + 1)};
}

bool looksLikeOption(const std::string& value) {
  return value.size() > 1 && value.front() == '-';
}

std::string valueError(ValueKind kind, const std::string& value) {
  if (value.empty()) return "must not be empty";
  if (kind == ValueKind::kProject) return projectNameError(value);
  if (kind == ValueKind::kBranch && !isValidBranchName(value)) return "must be a valid local Git branch name";
  if (kind == ValueKind::kPort) {
    unsigned int port = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
    if (error != std::errc{} || end != value.data() + value.size() || port == 0 || port > 65535)
      return "must be a port between 1 and 65535";
  }
  if (kind == ValueKind::kShell && value != "bash" && value != "zsh") return "must be bash or zsh";
  if (kind == ValueKind::kHost) {
    const auto at = value.find('@');
    const auto safe = [](std::string_view part, bool host) {
      return !part.empty() && part.size() <= 255 && part.front() != '-' &&
             std::all_of(part.begin(), part.end(), [host](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' || character == '_' ||
                      (host && character == '.');
             });
    };
    if ((at != std::string::npos && !safe(std::string_view(value).substr(0, at), false)) ||
        !safe(std::string_view(value).substr(at == std::string::npos ? 0 : at + 1), true))
      return "must be an SSH host alias or user@host";
  }
  return {};
}

CliInvocation failure(const std::vector<std::string>& path, const std::string& message) {
  CliInvocation result;
  result.exit_code = 2;
  result.standard_error = "ckgit: " + message + "\nTry 'ckgit help" +
                          (path.empty() ? std::string{} : " " + join(path)) + "'.\n";
  return result;
}

CliInvocation success(std::string output) {
  CliInvocation result;
  result.exit_code = 0;
  result.standard_output = std::move(output);
  return result;
}

std::string synopsis(const Command& command) {
  if (command.path == std::vector<std::string>{"clone"}) {
    return "ckgit clone [--config PATH] NAME [DESTINATION]\n"
           "       ckgit clone [--config PATH] --all --into DIR [--dry-run] [--yes]\n"
           "       ckgit clone [--config PATH] --project NAME [--project NAME ...] --into DIR [--dry-run] [--yes]";
  }
  std::string result = "ckgit " + join(command.path);
  if (!children(command.path).empty()) result += " COMMAND";
  if (command.uses_config) result += " [--config PATH]";
  for (const auto& option : command.options) {
    if (option.hidden) continue;
    result += " [" + option.name;
    if (!option.value_name.empty()) result += " " + option.value_name;
    if (option.repeatable) result += " ...";
    result += "]";
  }
  for (const auto& argument : command.arguments) {
    result += " ";
    if (!argument.required) result += "[";
    result += argument.name;
    if (argument.repeatable) result += " ...";
    if (!argument.required) result += "]";
  }
  return result;
}

void optionHelp(std::ostream& output, const Option& option) {
  output << "  " << option.name;
  if (!option.value_name.empty()) output << " " << option.value_name;
  output << "\n      " << option.description << "\n";
}

std::vector<std::string> completionChoices(const std::vector<std::string>& path) {
  std::vector<std::string> choices{"--help", "-h"};
  if (path.empty()) {
    choices.insert(choices.end(), {"--version", "--config", "help"});
  } else if (const auto* command = findCommand(path)) {
    if (command->uses_config) choices.push_back("--config");
    for (const auto& option : command->options)
      if (!option.hidden) choices.push_back(option.name);
    if (path == std::vector<std::string>{"completion"}) choices.insert(choices.end(), {"bash", "zsh"});
  }
  for (const auto* child : children(path)) choices.push_back(child->path.back());
  return choices;
}

std::string shellCompletion(const std::string& shell) {
  // All interpolated strings below come from the fixed command schema, never
  // from configuration, user paths, branch names, or server responses.
  std::set<std::string> value_options{kConfig.name};
  for (const auto& command : commands())
    for (const auto& option : command.options)
      if (option.kind != ValueKind::kFlag && !option.hidden) value_options.insert(option.name);
  const std::vector<std::string> valued(value_options.begin(), value_options.end());
  std::ostringstream output;
  if (shell == "bash") {
    output << "# Generated by ckgit completion bash; no server access.\n"
              "_ckgit_complete() {\n"
              "  local cur=${COMP_WORDS[COMP_CWORD]} context='' word candidate choices index skip=0\n"
              "  COMPREPLY=()\n"
              "  for ((index=1; index<COMP_CWORD; ++index)); do\n"
              "    word=${COMP_WORDS[index]}\n"
              "    if ((skip)); then skip=0; continue; fi\n"
              "    case $word in\n"
           << "      " << join(valued, "|") << ") skip=1; continue ;;\n"
              "      -*) continue ;;\n"
              "    esac\n"
              "    candidate=${context:+$context }$word\n"
              "    case $candidate in\n";
    for (const auto& command : commands()) output << "      '" << join(command.path) << "') context=$candidate ;;\n";
    output << "      help) context='' ;;\n"
              "    esac\n"
              "  done\n"
              "  case $context in\n"
           << "    '') choices='" << join(completionChoices({})) << "' ;;\n";
    for (const auto& command : commands())
      output << "    '" << join(command.path) << "') choices='" << join(completionChoices(command.path)) << "' ;;\n";
    output << "  esac\n"
              "  if [[ $cur == -* || $skip == 0 ]]; then\n"
              "    while IFS= read -r word; do COMPREPLY+=(\"$word\"); done < <(compgen -W \"$choices\" -- \"$cur\")\n"
              "  fi\n"
              "  if [[ $cur != -* ]]; then\n"
              "    while IFS= read -r word; do COMPREPLY+=(\"$word\"); done < <(compgen -f -- \"$cur\")\n"
              "  fi\n"
              "}\n"
              "complete -o filenames -F _ckgit_complete ckgit\n";
  } else {
    output << "#compdef ckgit\n# Generated by ckgit completion zsh; no server access.\n"
              "_ckgit_complete() {\n"
              "  local context='' word candidate index skip=0\n"
              "  local -a choices\n"
              "  for ((index=2; index<CURRENT; ++index)); do\n"
              "    word=$words[index]\n"
              "    if ((skip)); then skip=0; continue; fi\n"
              "    case $word in\n"
           << "      " << join(valued, "|") << ") skip=1; continue ;;\n"
              "      -*) continue ;;\n"
              "    esac\n"
              "    candidate=${context:+$context }$word\n"
              "    case $candidate in\n";
    for (const auto& command : commands()) output << "      '" << join(command.path) << "') context=$candidate ;;\n";
    output << "      help) context='' ;;\n"
              "    esac\n"
              "  done\n"
              "  case $context in\n"
           << "    '') choices=(" << join(completionChoices({})) << ") ;;\n";
    for (const auto& command : commands())
      output << "    '" << join(command.path) << "') choices=(" << join(completionChoices(command.path)) << ") ;;\n";
    output << "  esac\n"
              "  if ((!skip)); then compadd -- $choices; fi\n"
              "  _files\n"
              "}\n"
              "compdef _ckgit_complete ckgit\n";
  }
  return output.str();
}

}  // namespace

std::string buildVersion() { return CKGIT_BUILD_VERSION; }

std::string versionLine(const std::string& program) { return program + " " + buildVersion() + "\n"; }

std::string clientVersion() { return versionLine("ckgit"); }

std::string clientHelp(const std::vector<std::string>& command_path) {
  std::ostringstream output;
  if (command_path.empty()) {
    output << "ckgit — publish Git projects and browse their history.\n\n"
              "Usage: ckgit [--config PATH] COMMAND [OPTIONS] [ARGUMENTS]\n"
              "       ckgit help [COMMAND [SUBCOMMAND]]\n\n"
              "Commands:\n";
    for (const auto* command : children({})) {
      output << "  " << command->path.front() << "\n      " << command->purpose << "\n";
    }
    output << "\nStart here:\n"
              "  ckgit setup                 Configure a new device.\n"
              "  ckgit doctor                Check SSH and dashboard access.\n"
              "  ckgit projects              Discover hosted projects to clone.\n"
              "  ckgit publish --dry-run     Preview this repository, or unpublished child repositories.\n"
              "  ckgit publish --yes         Publish the displayed scope.\n"
              "  ckgit status                Inspect managed projects on this device.\n"
              "  ckgit sync --dry-run        Preview uploading committed branches and tags.\n"
              "  ckgit web                   Open the dashboard.\n\n"
              "Defaults and scope:\n"
              "  Configuration: ~/.config/ck-git-hosting/client.ini; override with --config PATH.\n"
              "  The configuration selects the target server, device identity, and remote name.\n"
              "  status, sync, and checkout list share a private managed-checkout inventory.\n"
              "  Use --repo . for the current repository; scan discovers additional folders.\n"
              "  sync uploads committed local branches and tags; it does not download or merge.\n\n"
              "  fetch downloads refs; update fast-forwards clean current branches.\n\n"
              "Help and conventions:\n"
              "  ckgit help publish, ckgit publish --help, and ckgit publish -h are equivalent.\n"
              "  Help and --version work offline without configuration.\n"
              "  --config may appear before or after the command. --option=value is accepted.\n"
              "  Use -- before positional paths that start with a hyphen.\n"
              "  --version prints the client build identifier.\n";
  } else {
    const auto* command = findCommand(command_path);
    if (!command) return {};
    output << "ckgit " << join(command->path) << " — " << command->purpose << "\n\n"
           << "Usage: " << synopsis(*command) << "\n\n"
           << "Scope and defaults:\n  " << command->scope << "\n\n"
           << "Effects:\n  " << command->effects << "\n";
    const auto subcommands = children(command_path);
    if (!subcommands.empty()) {
      output << "\nCommands:\n";
      for (const auto* child : subcommands) output << "  " << child->path.back() << "\n      " << child->purpose << "\n";
    }
    if (!command->arguments.empty()) {
      output << "\nArguments:\n";
      for (const auto& argument : command->arguments) output << "  " << argument.name << "\n      " << argument.description << "\n";
    }
    output << "\nOptions:\n";
    for (const auto& option : command->options)
      if (!option.hidden) optionHelp(output, option);
    if (command->uses_config) {
      optionHelp(output, kConfig);
      output << "      Its server, device identity, and remote name determine the target.\n";
    }
    output << "  -h, --help\n      Show this help without reading configuration or contacting the server.\n";
    output << "\nExamples:\n";
    for (const auto& example : command->examples) output << "  " << example << "\n";
  }
  output << "\nExit codes:\n"
            "  0  Completed, already current, previewed, or cancelled before publishing.\n"
            "  1  Operation failed; read the reported cause before retrying.\n"
            "  2  Invalid arguments; use the command's help.\n"
            "  3  Partial result or attention needed; inspect skipped/failed projects.\n"
            "  4  Another operation holds this configuration's lock; retry after it finishes.\n";
  return output.str();
}

CliInvocation prepareClientInvocation(const std::vector<std::string>& input) {
  if (input.empty()) return failure({}, "a command is required");
  std::vector<std::string> global_arguments;
  std::size_t offset = 0;
  while (offset < input.size()) {
    const auto [name, attached] = splitOption(input[offset]);
    if (name != "--config") break;
    std::string value;
    if (attached) value = *attached;
    else if (++offset < input.size() && !looksLikeOption(input[offset])) value = input[offset];
    else return failure({}, "--config requires PATH (use --config=PATH for a value beginning with '-')");
    if (value.empty()) return failure({}, "--config requires a non-empty PATH");
    global_arguments.insert(global_arguments.end(), {"--config", value});
    ++offset;
  }
  if (offset == input.size()) return failure({}, "a command is required after --config");
  const auto& first = input[offset];
  if (first == "--version") {
    if (offset + 1 != input.size()) return failure({}, "--version does not accept additional arguments");
    return success(clientVersion());
  }
  if (first == "help" || first == "--help" || first == "-h") {
    std::vector<std::string> path(input.begin() + static_cast<std::ptrdiff_t>(offset + 1), input.end());
    if (path.empty() || findCommand(path)) return success(clientHelp(path));
    return failure({}, "unknown help topic '" + join(path) + "'");
  }
  std::vector<std::string> path{first};
  const Command* command = findCommand(path);
  if (!command) return failure({}, (looksLikeOption(first) ? "unknown option '" : "unknown command '") + first + "'");
  std::vector<std::string> arguments = global_arguments;
  arguments.insert(arguments.end(), input.begin() + static_cast<std::ptrdiff_t>(offset + 1), input.end());

  // Locate a nested command while allowing its options (including --config)
  // before the subcommand. Values cannot accidentally become command names.
  const auto subcommands = children(path);
  if (!subcommands.empty()) {
    std::optional<std::size_t> subcommand_index;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const auto [name, attached] = splitOption(arguments[index]);
      const Option* option = findOption(*command, name);
      if (!option) {
        for (const auto* child : subcommands) {
          if ((option = findOption(*child, name))) break;
        }
      }
      if (option && option->kind != ValueKind::kFlag && !attached) { ++index; continue; }
      if (looksLikeOption(arguments[index])) {
        if (!option && name != "--help" && name != "-h" && name != "--")
          return failure(path, "unknown option '" + name + "' for " + join(path));
        continue;
      }
      subcommand_index = index;
      break;
    }
    if (subcommand_index) {
      path.push_back(arguments[*subcommand_index]);
      const auto* selected = findCommand(path);
      if (!selected) return failure(command->path, "unknown " + first + " command '" + path.back() + "'");
      command = selected;
      arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(*subcommand_index));
    }
  }

  CliInvocation result;
  result.command = first;
  if (path.size() > 1) result.arguments.assign(path.begin() + 1, path.end());
  std::vector<std::string> positionals;
  std::set<std::string> present_options;
  std::set<std::pair<std::string, std::string>> repeated_values;
  bool positional_only = false;
  bool help = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto& token = arguments[index];
    if (!positional_only && token == "--") { positional_only = true; continue; }
    if (!positional_only && (token == "--help" || token == "-h")) { help = true; continue; }
    if (!positional_only && looksLikeOption(token)) {
      const auto [name, attached] = splitOption(token);
      const auto* option = findOption(*command, name);
      if (!option) return failure(path, "unknown option '" + name + "' for " + join(path));
      const bool first_occurrence = present_options.insert(name).second;
      if (option->kind == ValueKind::kFlag) {
        if (attached) return failure(path, name + " does not take a value");
        if (first_occurrence) result.arguments.push_back(name);
        continue;
      }
      std::string value;
      if (attached) value = *attached;
      else if (index + 1 < arguments.size() && !looksLikeOption(arguments[index + 1])) value = arguments[++index];
      else return failure(path, name + " requires " + option->value_name + " (use " + name + "=VALUE for a value beginning with '-')");
      const auto error = valueError(option->kind, value);
      if (!error.empty()) return failure(path, name + " " + error + ": '" + value + "'");
      if (!option->repeatable || repeated_values.emplace(name, value).second)
        result.arguments.insert(result.arguments.end(), {name, value});
      continue;
    }
    positionals.push_back(token);
  }
  if (help) return success(clientHelp(path));
  if (!children(path).empty()) return failure(path, "a " + first + " subcommand is required");
  if (present_options.contains("--repo") && present_options.contains("--scan"))
    return failure(path, "--repo and --scan select different scopes; choose one");
  if (first == "sync" && present_options.contains("--replace-checkout") && !present_options.contains("--repo"))
    return failure(path, "--replace-checkout requires --repo PATH");
  const bool bulk_clone = first == "clone" &&
      (present_options.contains("--all") || present_options.contains("--project"));
  if (bulk_clone && (present_options.contains("--all") && present_options.contains("--project")))
    return failure(path, "choose --all or --project NAME");
  if (bulk_clone && (!positionals.empty() || !present_options.contains("--into")))
    return failure(path, "bulk clone requires --into DIR and no positional arguments");
  if (first == "clone" && !bulk_clone &&
      (present_options.contains("--into") || present_options.contains("--yes") || present_options.contains("--dry-run")))
    return failure(path, "--into, --yes, and --dry-run apply to bulk clone; select --all or --project NAME");
  if ((first == "fetch" || first == "update") &&
      (static_cast<int>(present_options.contains("--repo")) + static_cast<int>(present_options.contains("--project")) +
       static_cast<int>(present_options.contains("--all")) > 1))
    return failure(path, "choose one scope: --repo PATH, --project NAME, or --all");
  // Project names may begin with '-'. Keep the clone boundary after normalizing
  // options, including for a project literally named '--config'.
  if ((first == "clone" || path == std::vector<std::string>{"checkout", "forget"}) &&
      !positionals.empty() && !positionals.front().empty() && positionals.front().front() == '-')
    result.arguments.push_back("--");
  std::size_t positional_index = 0;
  for (const auto& argument : command->arguments) {
    if (positional_index == positionals.size()) {
      if (argument.required && !bulk_clone) return failure(path, "missing required argument " + argument.name);
      continue;
    }
    do {
      auto value = positionals[positional_index++];
      const auto error = valueError(argument.kind, value);
      if (!error.empty()) return failure(path, argument.name + " " + error + ": '" + value + "'");
      // Handlers accept normalized options and positionals. Prefixing a relative
      // path preserves its meaning while preventing old handlers or Git from
      // interpreting a path after '--' as another option.
      if (argument.kind == ValueKind::kPath && value.front() == '-') value.insert(0, "./");
      result.arguments.push_back(std::move(value));
    } while (argument.repeatable && positional_index < positionals.size());
  }
  if (positional_index < positionals.size())
    return failure(path, "unexpected argument '" + positionals[positional_index] + "'");
  if (first == "completion") return success(shellCompletion(positionals.front()));
  return result;
}

}  // namespace ckgit
