// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "ckgit/client_config.hpp"
#include "ckgit/cli_help.hpp"
#include "ckgit/client_state.hpp"
#include "ckgit/control_rpc.hpp"
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
constexpr unsigned short kDefaultDashboardPort = 8420;
// Control round trips are short SSH sessions; transfers are bounded only by a
// generous wall-clock limit, because a first push of a large history can take
// many minutes over a LAN to a small server.
constexpr std::chrono::seconds kControlTimeout{60};
constexpr std::chrono::hours kTransferTimeout{4};
constexpr std::size_t kTransferOutputLimit = 8 * 1024 * 1024;

std::string describeProcessFailure(const ckgit::ProcessResult& result, std::chrono::milliseconds timeout);
bool samePath(const std::filesystem::path& left, const std::filesystem::path& right);
std::vector<ckgit::RefTip> fetchServerRefs(const ckgit::ClientConfig& config, const std::string& project);
std::vector<ckgit::RefTip> localRefTips(const ckgit::RepositoryAudit& audit);
std::string reportedPathForRegistration(const ckgit::RepositoryAudit& audit, const ckgit::ClientConfig& config);
std::filesystem::path canonicalSelectionPath(const ckgit::ClientConfig& config);
// Git's transport ssh gets the same non-interactive, keepalive settings as
// the control channel unless the user already configured a command; a dead
// connection then fails within minutes instead of holding the lock for hours.
constexpr const char* kTransferSshCommand =
    "ssh -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30 -o ServerAliveCountMax=6";

class TransferSshGuard {
 public:
  explicit TransferSshGuard(const std::filesystem::path& repository) {
    if (std::getenv("GIT_SSH_COMMAND") != nullptr || std::getenv("GIT_SSH") != nullptr) {
      return;
    }
    const auto configured = ckgit::runProcess({"git", "-C", repository.string(), "config", "--get", "core.sshCommand"});
    if (configured.exit_code == 0 && !configured.output.empty()) {
      return;
    }
    if (setenv("GIT_SSH_COMMAND", kTransferSshCommand, 1) == 0) {
      applied_ = true;
    }
  }
  ~TransferSshGuard() {
    if (applied_) {
      unsetenv("GIT_SSH_COMMAND");
    }
  }
  TransferSshGuard(const TransferSshGuard&) = delete;
  TransferSshGuard& operator=(const TransferSshGuard&) = delete;

 private:
  bool applied_{false};
};

// Interactive runs show Git's own transfer progress on the terminal; scheduled
// runs keep everything captured.
ckgit::ProcessOptions transferOptions() {
  ckgit::ProcessOptions options;
  options.timeout = kTransferTimeout;
  options.output_limit = kTransferOutputLimit;
  options.inherit_stderr = isatty(STDERR_FILENO) != 0;
  return options;
}

struct PushScope {
  std::vector<std::string> branches;  // empty: every local branch
  bool include_tags{true};
};

// Positive refspecs only: no force marker, no deletion, no mirror.  Branches
// and tags are all pushed unless the caller limited the scope explicitly.
std::vector<std::string> pushCommand(const std::filesystem::path& repository, const std::string& remote_name,
                                     bool dry_run, const PushScope& scope = PushScope{}) {
  std::vector<std::string> command{"git", "-C", repository.string(), "push", "--porcelain", "--atomic"};
  if (dry_run) {
    command.push_back("--dry-run");
  } else if (isatty(STDERR_FILENO) != 0) {
    command.push_back("--progress");
  }
  command.push_back(remote_name);
  if (scope.branches.empty()) {
    command.push_back("refs/heads/*:refs/heads/*");
  } else {
    for (const auto& branch : scope.branches) {
      command.push_back("refs/heads/" + branch + ":refs/heads/" + branch);
    }
  }
  if (scope.include_tags) {
    command.push_back("refs/tags/*:refs/tags/*");
  }
  return command;
}

// Runs one control operation over SSH and returns its bounded reply.
ckgit::ProcessResult controlRpc(const ckgit::ClientConfig& config, const std::string& request) {
  return ckgit::runProcess(
      {"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "RequestTTY=no",
       "-o", "ClearAllForwardings=yes", config.server, "ckgit-rpc 1 " + request},
      kControlTimeout, ckgit::kMaximumControlResponseBytes + 4096);
}

// The checkouts this host registered on the server, keyed by project.
std::map<std::string, std::filesystem::path> fetchRegisteredCheckouts(const ckgit::ClientConfig& config) {
  const auto result = controlRpc(config, "checkouts");
  if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
    throw std::runtime_error("could not query registered checkouts (" +
                             describeProcessFailure(result, kControlTimeout) + ")");
  }
  const std::string& reply = result.output;
  const auto header_end = reply.find('\n');
  if (reply.rfind("ok ", 0) != 0 || header_end == std::string::npos || reply.back() != '\n') {
    throw std::runtime_error("registered checkout reply has invalid framing");
  }
  std::map<std::string, std::filesystem::path> checkouts;
  std::size_t start = header_end + 1;
  std::size_t expected = 0;
  {
    const std::string_view count(reply.data() + 3, start - 4);
    const auto [end, error] = std::from_chars(count.data(), count.data() + count.size(), expected);
    if (error != std::errc{} || end != count.data() + count.size() || expected > ckgit::kMaximumControlRefs) {
      throw std::runtime_error("registered checkout reply has an invalid count");
    }
  }
  while (start < reply.size()) {
    const std::size_t newline = reply.find('\n', start);
    const std::string_view line(reply.data() + start, newline - start);
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos) {
      throw std::runtime_error("registered checkout reply has an invalid record");
    }
    const std::string project(line.substr(0, space));
    const auto path = ckgit::decodeCheckoutPathToken(line.substr(space + 1));
    if (!ckgit::isValidProjectName(project) || !path.has_value() || path->empty() ||
        !checkouts.emplace(project, *path).second) {
      throw std::runtime_error("registered checkout reply contains unsafe data");
    }
    start = newline + 1;
  }
  if (checkouts.size() != expected) {
    throw std::runtime_error("registered checkout reply count does not match records");
  }
  return checkouts;
}

struct PublishPlan {
  std::vector<std::string> new_refs;       // in scope, absent on the server
  std::vector<std::string> updated_refs;   // in scope, server holds another commit
  std::vector<std::string> equal_refs;     // in scope, already identical
  std::vector<std::string> server_only;    // retained; never deleted
  std::vector<std::string> left_out;       // local refs excluded by the scope flags
};

bool inScope(const std::string& ref, const PushScope& scope) {
  if (ref.rfind("refs/tags/", 0) == 0) {
    return scope.include_tags;
  }
  if (scope.branches.empty()) {
    return true;
  }
  return std::find(scope.branches.begin(), scope.branches.end(), ref.substr(std::string("refs/heads/").size())) !=
         scope.branches.end();
}

// Compares local tips with the server's for an existing project.  A differing
// tip is listed as an update; whether it is a fast-forward is decided by the
// atomic push itself, which the server rejects otherwise.
PublishPlan planPublish(const ckgit::RepositoryAudit& audit, const ckgit::ClientConfig& config,
                        const std::string& project, const PushScope& scope, bool creating = false) {
  PublishPlan plan;
  const auto server_refs = creating ? std::vector<ckgit::RefTip>{} : fetchServerRefs(config, project);
  for (const auto& status : ckgit::compareRefTips(localRefTips(audit), server_refs)) {
    if (status.relation == ckgit::RefRelation::kServerOnly) {
      plan.server_only.push_back(status.name);
      continue;  // Never deleted by publish.
    }
    if (!inScope(status.name, scope)) {
      plan.left_out.push_back(status.name);
    } else if (status.relation == ckgit::RefRelation::kLocalOnly) {
      plan.new_refs.push_back(status.name);
    } else if (status.relation == ckgit::RefRelation::kMismatched) {
      plan.updated_refs.push_back(status.name);
    } else {
      plan.equal_refs.push_back(status.name);
    }
  }
  return plan;
}

std::string describeRefList(const std::vector<std::string>& refs, bool verbose) {
  std::string text;
  const auto limit = verbose ? refs.size() : std::min<std::size_t>(refs.size(), 12);
  for (std::size_t index = 0; index < limit; ++index) {
    text += (index == 0 ? "" : ", ") + refs[index];
  }
  if (refs.size() > limit) {
    text += ", ... (" + std::to_string(refs.size() - limit) + " more; use --verbose to show all)";
  }
  return text;
}

std::string describePlan(const PublishPlan& plan, bool verbose) {
  std::string text;
  const auto append = [&](std::string_view label, const std::vector<std::string>& refs) {
    text += "  " + std::string(label) + ": " + std::to_string(refs.size());
    if (!refs.empty()) {
      text += " (" + describeRefList(refs, verbose) + ")";
    }
    text += '\n';
  };
  append("new on server", plan.new_refs);
  append("to update (differing tips; permission checked by Git preflight)", plan.updated_refs);
  append("already up to date", plan.equal_refs);
  append("left out by --branch/--no-tags", plan.left_out);
  append("server-only refs retained", plan.server_only);
  return text;
}

std::string registrationPlan(const ckgit::ClientConfig& config, const ckgit::RepositoryAudit& audit,
                             const std::string& project, bool replace,
                             const std::map<std::string, std::filesystem::path>& registered) {
  const auto local = ckgit::loadCanonicalCheckouts(canonicalSelectionPath(config));
  const auto selected = local.find(project);
  const auto remote = registered.find(project);
  std::string result = "  main checkout on this device: ";
  if (selected != local.end()) {
    if (samePath(selected->second, audit.path)) {
      result += "keep " + audit.path.string();
    } else {
      result += (replace ? "replace " : "BLOCKED: already ") + selected->second.string();
      result += replace ? " with " + audit.path.string() : "; use --replace-checkout to select this folder";
    }
  } else if (remote != registered.end() &&
             !(remote->second.is_absolute() ? samePath(remote->second, audit.path)
                 : ckgit::sameReportedCheckout(remote->second.string(), reportedPathForRegistration(audit, config)))) {
    result += (replace ? "replace legacy registration " : "BLOCKED: legacy registration ") + remote->second.string();
    result += replace ? " with " + audit.path.string() : "; use --replace-checkout to select this folder";
  } else {
    result += "manage " + audit.path.string() + " privately for ordinary sync";
  }
  const auto report = reportedPathForRegistration(audit, config);
  result += "\n  server checkout report: " + report;
  result += config.expose_full_paths ? " (full path)" : " (basename only; absolute path stays on this device)";
  if (remote == registered.end()) {
    result += "; create report";
  } else if (ckgit::sameReportedCheckout(remote->second.string(), report)) {
    result += remote->second.string() == report ? "; refresh report" : "; update previous report " + remote->second.string();
  } else {
    result += replace ? "; replace previous report " + remote->second.string()
                      : "; BLOCKED by previous report " + remote->second.string();
  }
  result += '\n';
  return result;
}

void printTransferPlan(const ckgit::ClientConfig& config, const ckgit::RepositoryAudit& audit,
                       const std::string& project, const PublishPlan& plan, bool creating,
                       const std::string& default_branch, bool replace, bool verbose,
                       const std::map<std::string, std::filesystem::path>& registered) {
  std::cout << "Project " << project << " → " << config.server << "\n"
            << "  source folder: " << audit.path.string() << "\n"
            << "  default branch: " << (creating ? default_branch + " (new project)" : "unchanged on server") << "\n"
            << describePlan(plan, verbose)
            << "  uncommitted content excluded: " << (audit.changed_entries_truncated ? "at least " : "")
            << audit.changed_entries << " worktree entries\n"
            << registrationPlan(config, audit, project, replace, registered)
            << "  Uploads committed local branches and tags; remote-tracking branches are excluded.\n";
}

// Legacy servers may not supply registrations. The private main selection
// still applies, and the server checks any registration attempted later.
std::map<std::string, std::filesystem::path> registeredCheckoutsOrWarn(const ckgit::ClientConfig& config) {
  try {
    return fetchRegisteredCheckouts(config);
  } catch (const std::exception& error) {
    std::cerr << "ckgit: warning: could not check registered checkouts (" << error.what()
              << "); the private main checkout selection still applies\n";
    return {};
  }
}

// Refuses to silently move a project's main checkout on this host.  Returns
// false (with a message) when another path is registered and no override.
bool ensureMainCheckout(const ckgit::ClientConfig& config, const std::string& project,
                        const ckgit::RepositoryAudit& audit, bool replace,
                        const std::map<std::string, std::filesystem::path>& registered) {
  if (replace) {
    return true;
  }
  const auto local = ckgit::loadCanonicalCheckouts(canonicalSelectionPath(config));
  const auto selected = local.find(project);
  if (selected != local.end() && !samePath(selected->second, audit.path)) {
    std::cerr << "ckgit: " << project << " already has a main checkout on this device: " << selected->second.string()
              << "; " << audit.path.string() << " was not used.\n"
              << "       Use --replace-checkout to make this folder the main checkout.\n";
    return false;
  }
  const auto existing = registered.find(project);
  if (existing == registered.end()) {
    return true;
  }
  // The client knows its full path even in basename privacy mode, so a stored
  // full path is compared exactly; only two bare names compare by name.
  const bool same = existing->second.is_absolute()
                        ? samePath(existing->second, audit.path)
                        : ckgit::sameReportedCheckout(existing->second.string(), reportedPathForRegistration(audit, config));
  if (same) {
    return true;
  }
  std::cerr << "ckgit: " << project << " is already published from " << existing->second.string()
            << " on this host; " << audit.path.string() << " was not used.\n"
            << "       Sync the main checkout instead, or run again with --replace-checkout to make "
            << audit.path.string() << " the main checkout for " << project << ".\n";
  return false;
}

// One safe line that says why a child failed: the timeout, or the exit code
// with the most specific line Git printed.  A porcelain rejection ("! ref
// [rejected] (reason)") or a "remote:" / "fatal:" line beats the generic
// "failed to push some refs" summary that always comes last.
std::string describeProcessFailure(const ckgit::ProcessResult& result, std::chrono::milliseconds timeout) {
  if (result.timed_out) {
    return "timed out after " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(timeout).count()) +
           " s";
  }
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= result.output.size()) {
    const std::size_t newline = result.output.find('\n', start);
    const std::size_t end = newline == std::string::npos ? result.output.size() : newline;
    std::string line = result.output.substr(start, end - start);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
    if (newline == std::string::npos) {
      break;
    }
    start = newline + 1;
  }
  std::string chosen;
  const auto prefer = [&](auto predicate) {
    if (chosen.empty()) {
      for (const auto& line : lines) {
        if (predicate(line)) {
          chosen = line;
          return;
        }
      }
    }
  };
  prefer([](const std::string& line) { return line.rfind("!", 0) == 0; });
  prefer([](const std::string& line) { return line.rfind("remote: ", 0) == 0 && line.size() > 8; });
  prefer([](const std::string& line) { return line.rfind("fatal: ", 0) == 0; });
  if (chosen.empty()) {
    // Git ends a refused push with a generic summary; the line before it, for
    // example a local pre-push hook's own message, is the useful one.
    for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
      if (line->rfind("error: failed to push some refs", 0) != 0) {
        chosen = *line;
        break;
      }
    }
  }
  if (chosen.empty() && !lines.empty()) {
    chosen = lines.back();
  }
  for (auto& character : chosen) {
    if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f) {
      character = ' ';
    }
  }
  if (chosen.size() > 200) {
    chosen.resize(200);
  }
  return "exit code " + std::to_string(result.exit_code) + (chosen.empty() ? "" : ": " + chosen);
}

// Without --config, every command uses the standard per-user location.
std::filesystem::path defaultClientConfigPath() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path(xdg) / "ck-git-hosting" / "client.ini";
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("HOME is not set; pass --config PATH");
  }
  return std::filesystem::path(home) / ".config" / "ck-git-hosting" / "client.ini";
}

std::filesystem::path resolveConfigPath(const std::optional<std::filesystem::path>& requested) {
  if (requested.has_value()) {
    return *requested;
  }
  const auto fallback = defaultClientConfigPath();
  std::error_code error;
  if (!std::filesystem::is_regular_file(fallback, error)) {
    throw std::runtime_error("no client configuration at " + fallback.string() + "; pass --config PATH");
  }
  return fallback;
}

bool registerRemoteCheckout(const ckgit::RepositoryAudit& audit, const ckgit::ClientConfig& config,
                            std::string_view project, std::string* failure_reason, bool replace = false);
std::optional<std::string> pairedProjectForAudit(const ckgit::RepositoryAudit& audit,
                                                 const ckgit::ClientConfig& config, bool* invalid);
bool samePath(const std::filesystem::path& left, const std::filesystem::path& right);

std::filesystem::path canonicalSelectionPath(const ckgit::ClientConfig& config) {
  return config.config_directory / "canonical.ini";
}

std::filesystem::path syncLockPath(const ckgit::ClientConfig& config) {
  return config.config_directory / "sync.lock";
}

std::optional<ckgit::SyncLock> acquireSyncLock(const ckgit::ClientConfig& config, std::string_view command) {
  auto lock = ckgit::SyncLock::tryAcquire(syncLockPath(config));
  if (!lock.has_value()) {
    std::cerr << "ckgit: another ckgit operation for this configuration is running; " << command
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
    *reason = "selected main checkout " + selected->second.string() + " is unavailable; " +
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

struct ManagedCheckout {
  std::filesystem::path path;
  std::optional<ckgit::RepositoryAudit> audit;
  std::string problem;
  bool legacy{false};
};

struct ManagedInventory {
  std::map<std::string, ManagedCheckout> projects;
  std::map<std::string, std::filesystem::path> reported;
  bool server_available{true};
  bool attention{false};
};

// The private selection is authoritative. Server records supplement older
// installations but can never replace a locally chosen absolute path.
ManagedInventory managedInventory(const ckgit::ClientConfig& config) {
  ManagedInventory result;
  const auto local = ckgit::loadCanonicalCheckouts(canonicalSelectionPath(config));
  for (const auto& [project, path] : local) {
    result.projects.emplace(project, ManagedCheckout{path, std::nullopt, {}, false});
  }
  try {
    result.reported = fetchRegisteredCheckouts(config);
    for (const auto& [project, path] : result.reported) {
      result.projects.try_emplace(project, ManagedCheckout{path, std::nullopt, {}, true});
    }
  } catch (const std::exception& error) {
    std::cerr << "ckgit: warning: " << error.what() << "; showing the private local inventory\n";
    result.server_available = false;
    result.attention = true;
  }
  for (auto& [project, checkout] : result.projects) {
    if (!checkout.path.is_absolute()) {
      checkout.problem = "legacy registration " + checkout.path.string() +
          " needs a local path; run ckgit checkout migrate to find it under configured scan roots, "
          "or checkout set-canonical " + project + " PATH";
    } else {
      try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(checkout.path, error);
        if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
          throw std::runtime_error("folder is missing or unavailable; reconnect its volume or use checkout set-canonical " +
                                   project + " PATH to select its new location");
        }
        auto audit = ckgit::inspectDiscoveredRepository(checkout.path);
        bool invalid = false;
        const auto paired = pairedProjectForAudit(audit, config, &invalid);
        if (!paired.has_value() || *paired != project) {
          throw std::runtime_error("folder is no longer paired with this project; check its remote before selecting a main checkout");
        }
        checkout.audit = std::move(audit);
      } catch (const std::exception& error) {
        checkout.problem = error.what();
      }
    }
    result.attention = result.attention || !checkout.problem.empty();
  }
  return result;
}

void printManagedScope(const ManagedInventory& inventory, const ckgit::ClientConfig& config) {
  std::cout << "Scope: " << inventory.projects.size() << " managed project(s) on " << config.display_name
            << " (" << config.client_id << ") → " << config.server << "\n";
  if (inventory.projects.empty()) {
    std::cout << "No managed projects. Publish or clone a project, or use checkout migrate for older registrations.\n";
  }
}

void printUnavailableCheckout(const std::string& project, const ManagedCheckout& checkout) {
  std::cout << project << "\n  main checkout on this device: " << checkout.path.string()
            << "\n  unavailable: " << checkout.problem << "\n";
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
  output << ckgit::clientHelp();
}

void printConfig(const std::filesystem::path& path, bool explicit_path, const ckgit::ClientConfig& config) {
  std::cout << "config=" << path.string() << (explicit_path ? " (--config)" : " (default)") << "\n"
            << "client_id=" << config.client_id << "\n"
            << "display_name=" << config.display_name << "\n"
            << "server=" << config.server << "\n"
            << "web_host=" << config.web_host << "\n"
            << "web_port=" << config.web_port << "\n"
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
            << (audit.changed_entries == 0 ? "" : (audit.changed_entries_truncated ? "at least " : "") +
                                                    std::to_string(audit.changed_entries) + " entries)")
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
            << ",\"changed_entries_truncated\":" << (audit.changed_entries_truncated ? "true" : "false")
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
  if (!config_path.has_value()) {
    std::error_code error;
    const auto fallback = defaultClientConfigPath();
    if (std::filesystem::is_regular_file(fallback, error)) {
      config_path = fallback;
    }
  }
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
      audited.push_back(ckgit::inspectDiscoveredRepository(path));
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
    std::cout << "Scanned " << audited.size() << (audited.size() == 1 ? " repository" : " repositories") << ".\n";
  }
  return warnings.empty() ? 0 : kPartial;
}

int configCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  if (arguments.empty() || arguments.front() != "show") {
    std::cerr << "ckgit: expected config show; see ckgit config --help\n";
    return kUsage;
  }
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    if (arguments[index] == "--config" && index + 1 < arguments.size()) {
      config_path = arguments[++index];
    } else {
      std::cerr << "ckgit: invalid config show option; see ckgit config show --help\n";
      return kUsage;
    }
  }
  const auto path = resolveConfigPath(config_path);
  printConfig(path, config_path.has_value(), ckgit::loadClientConfig(path));
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
       "-o", "ClearAllForwardings=yes", config.server, "ckgit-rpc 1 refs " + project},
      kControlTimeout, ckgit::kMaximumControlResponseBytes + 4096);
  if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
    throw std::runtime_error("could not query paired server refs (" +
                             describeProcessFailure(result, kControlTimeout) + ")");
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
  } catch (const std::exception& error) {
    std::cout << audit.path.string() << "\n  pairing: server ref query failed: " << error.what() << "\n";
    return true;
  }
}

int statusCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::filesystem::path> repository_path;
  bool scan = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if ((arguments[index] == "--config" || arguments[index] == "--repo") && index + 1 < arguments.size()) {
      const auto option = arguments[index++];
      (option == "--config" ? config_path : repository_path) = arguments[index];
    } else if (arguments[index] == "--scan") {
      scan = true;
    } else {
      std::cerr << "ckgit: invalid status option; see ckgit status --help\n";
      return kUsage;
    }
  }
  if (scan && repository_path.has_value()) {
    std::cerr << "ckgit: --scan and --repo are mutually exclusive\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(resolveConfigPath(config_path));
  if (!scan && !repository_path.has_value()) {
    const auto inventory = managedInventory(config);
    printManagedScope(inventory, config);
    bool attention = inventory.attention;
    for (const auto& [project, checkout] : inventory.projects) {
      if (!checkout.audit.has_value()) {
        printUnavailableCheckout(project, checkout);
      } else {
        std::cout << "Main checkout on this device" << (checkout.legacy ? " (legacy registration)" : "") << ":\n";
        attention = printPairedStatus(*checkout.audit, config) || attention;
      }
    }
    return attention ? kPartial : 0;
  }
  std::cout << "Scope: " << (scan ? "discovered repositories under configured scan roots" : "requested repository")
            << " on " << config.display_name << " → " << config.server << "\n";
  bool attention = false;
  std::vector<std::filesystem::path> paths;
  if (repository_path.has_value()) {
    paths.push_back(*repository_path);
  } else {
    const auto discovery = ckgit::discoverWorkingTrees(config.scan_roots, config.exclusions);
    paths = discovery.repositories;
    for (const auto& warning : discovery.warnings) {
      std::cerr << "warning: " << warning << "\n";
      attention = true;
    }
  }
  for (const auto& path : paths) {
    try {
      const auto audit = repository_path.has_value() ? ckgit::inspectRepository(path)
                                                   : ckgit::inspectDiscoveredRepository(path);
      attention = printPairedStatus(audit, config) || attention;
    } catch (const std::exception& error) {
      std::cerr << "ckgit: " << path.string() << ": " << error.what() << "\n";
      attention = true;
    }
  }
  if (paths.empty()) {
    std::cout << "No repositories found in this scope.\n";
  }
  return attention ? kPartial : 0;
}

int cloneCommand(const std::vector<std::string>& arguments, bool only_unmanaged = false) {
  std::optional<std::filesystem::path> config_path;
  std::vector<std::string> positional;
  bool positional_only = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (!positional_only && arguments[index] == "--") {
      positional_only = true;
    } else if (!positional_only && arguments[index] == "--config" && index + 1 < arguments.size()) {
      config_path.emplace(arguments[++index]);
    } else if (!positional_only && !arguments[index].empty() && arguments[index].front() == '-') {
      std::cerr << "ckgit: usage: ckgit clone --config PATH NAME [DESTINATION]\n";
      return kUsage;
    } else {
      positional.push_back(arguments[index]);
    }
  }
  if (positional.empty() || positional.size() > 2 || !ckgit::isValidProjectName(positional.front())) {
    std::cerr << "ckgit: usage: ckgit clone --config PATH NAME [DESTINATION]\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(resolveConfigPath(config_path));
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
  const auto lock = acquireSyncLock(config, "clone");
  if (!lock.has_value()) {
    return kBusy;
  }
  // Read management before cloning. A second copy must never silently replace
  // this device's main checkout, including an unresolved legacy registration.
  const auto local = ckgit::loadCanonicalCheckouts(canonicalSelectionPath(config));
  const auto registered = fetchRegisteredCheckouts(config);
  const auto existing_local = local.find(positional.front());
  const auto existing_remote = registered.find(positional.front());
  if (only_unmanaged && (existing_local != local.end() || existing_remote != registered.end())) {
    std::cout << "Skipped " << positional.front() << ": already managed on this device.\n";
    return 5;  // Internal bulk-clone skip, never exposed as an exit status.
  }
  const std::string remote_url = ckgit::hostedRepositoryUrl(config.server, positional.front());
  std::vector<std::string> clone_command{"git", "clone", "--origin", config.remote_name, "--no-recurse-submodules"};
  if (isatty(STDERR_FILENO) != 0) {
    clone_command.push_back("--progress");
  }
  clone_command.insert(clone_command.end(), {"--", remote_url, destination.string()});
  const TransferSshGuard ssh_guard(parent);
  const auto result = ckgit::runProcess(clone_command, transferOptions());
  if (result.exit_code != 0 || result.timed_out) {
    std::cerr << "ckgit: Git clone of " << positional.front() << " failed ("
              << describeProcessFailure(result, kTransferTimeout) << ")\n";
    return 1;
  }
  const auto audit = ckgit::inspectDiscoveredRepository(destination);
  std::cout << "Cloned " << positional.front() << " to " << audit.path.string() << "\n";
  const bool has_other_main = existing_local != local.end()
      ? !samePath(existing_local->second, audit.path)
      : existing_remote != registered.end() &&
          (!existing_remote->second.is_absolute() || !samePath(existing_remote->second, audit.path));
  const auto quote = [](std::string_view value) {
    std::string quoted = "'";
    for (const char character : value) {
      quoted += character == '\'' ? "'\\''" : std::string(1, character);
    }
    return quoted + "'";
  };
  if (has_other_main) {
    const auto& existing = existing_local != local.end() ? existing_local->second : existing_remote->second;
    std::cout << "This is a secondary copy; the existing main checkout selection remains " << existing.string() << ".\n";
    if (!existing.is_absolute()) {
      std::cout << "Its local path is unresolved; use checkout migrate to restore that legacy selection.\n";
    }
    std::cout << "To make this the main checkout on this device, run:\n  ckgit checkout set-canonical --config "
              << quote(resolveConfigPath(config_path).string()) << " " << positional.front() << " "
              << quote(audit.path.string()) << "\n";
    return 0;
  }
  std::string failure_reason;
  if (!registerRemoteCheckout(audit, config, positional.front(), &failure_reason)) {
    std::cerr << "ckgit: clone succeeded, but checkout management failed: " << failure_reason
              << ". To select this as the main checkout when the server is available, run:\n"
              << "  ckgit checkout set-canonical --config " << quote(resolveConfigPath(config_path).string())
              << " " << positional.front() << " " << quote(audit.path.string()) << "\n";
    return kPartial;
  }
  std::cout << "Managed as the main checkout on this device; ordinary ckgit sync will upload its committed changes.\n";
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
  if (!project_name.has_value() || !ckgit::isValidProjectName(*project_name) ||
      !ckgit::isValidBranchName(branch)) {
    std::cerr << "ckgit: usage: ckgit create --config PATH NAME [--default-branch BRANCH]\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(resolveConfigPath(config_path));
  const auto result = ckgit::runProcess(
      {"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "RequestTTY=no",
       "-o", "ClearAllForwardings=yes", config.server,
       "ckgit-rpc 1 create " + *project_name + " " + branch},
      kControlTimeout);
  if (result.exit_code != 0 || result.timed_out || result.output_truncated || result.output != "ok created\n") {
    std::cerr << "ckgit: remote project creation failed (" << describeProcessFailure(result, kControlTimeout)
              << ")\n";
    return 1;
  }
  std::cout << "Created remote project " << *project_name << " with default branch " << branch << "\n";
  return 0;
}

bool hasCommittedCurrentBranch(const ckgit::RepositoryAudit& audit) {
  return !audit.detached_head && !audit.current_branch.empty() &&
         std::any_of(audit.branches.begin(), audit.branches.end(), [&](const ckgit::RefTip& branch) {
           return branch.name == "refs/heads/" + audit.current_branch;
         });
}

int publishRepository(const ckgit::ClientConfig& config, const ckgit::RepositoryAudit& audit,
                      const std::string& project, const PushScope& scope, bool confirmed,
                      bool replace_checkout, bool lock_held = false, bool verbose = false) {
  if (!ckgit::isValidProjectName(project) || !hasCommittedCurrentBranch(audit)) {
    std::cerr << "ckgit: publish requires a valid project name and a checked-out, committed branch\n";
    return kUsage;
  }
  for (const auto& branch : scope.branches) {
    const bool exists = std::any_of(audit.branches.begin(), audit.branches.end(), [&](const ckgit::RefTip& tip) {
      return tip.name == "refs/heads/" + branch;
    });
    if (!exists) {
      std::cerr << "ckgit: --branch " << branch << " does not exist in " << audit.path.string() << "\n";
      return kUsage;
    }
  }
  const std::size_t pushed_branches = scope.branches.empty() ? audit.branches.size() : scope.branches.size();
  const std::size_t pushed_tags = scope.include_tags ? audit.tags.size() : 0;
  // A new project's default branch must be one that is actually pushed.
  const bool current_in_scope = scope.branches.empty() ||
      std::find(scope.branches.begin(), scope.branches.end(), audit.current_branch) != scope.branches.end();
  const std::string default_branch = current_in_scope ? audit.current_branch : scope.branches.front();
  const std::string remote_url = ckgit::hostedRepositoryUrl(config.server, project);
  bool invalid_remote = false;
  const auto paired = pairedProjectForAudit(audit, config, &invalid_remote);
  if (invalid_remote || (paired.has_value() && *paired != project)) {
    std::cerr << "ckgit: configured remote does not identify project " << project << " on " << config.server << "\n";
    return 1;
  }
  const bool remote_exists = paired.has_value();
  const auto registered = registeredCheckoutsOrWarn(config);
  const auto plan = planPublish(audit, config, project, scope, !remote_exists);
  std::cout << "Would " << (remote_exists ? "push to" : "create and push to") << " " << remote_url
            << " from " << audit.path.string() << "\n";
  printTransferPlan(config, audit, project, plan, !remote_exists, default_branch, replace_checkout, verbose, registered);
  if (!ensureMainCheckout(config, project, audit, replace_checkout, registered)) {
    return 1;
  }
  if (remote_exists) {
    const TransferSshGuard ssh_guard(audit.path);
    const auto preflight = ckgit::runProcess(pushCommand(audit.path, config.remote_name, true, scope),
                                           kTransferTimeout, kTransferOutputLimit);
    if (preflight.exit_code != 0 || preflight.timed_out || preflight.output_truncated) {
      std::cerr << "ckgit: Git preflight rejected this plan; no refs changed ("
                << describeProcessFailure(preflight, kTransferTimeout) << ")\n";
      return kPartial;
    }
    std::cout << "  Git preflight: ref updates permitted without force; server hooks run on upload.\n";
  } else {
    std::cout << "  Git preflight: deferred until project creation; the server may reject creation or upload.\n";
  }
  if (!confirmed) {
    std::cout << "Run again with --yes to continue.\n";
    return 0;
  }
  std::optional<ckgit::SyncLock> lock;
  if (!lock_held) {
    lock = acquireSyncLock(config, "publish");
    if (!lock.has_value()) {
      return kBusy;
    }
  }
  // One main checkout per project and host: refuse a second folder unless
  // the user explicitly makes it the main one.
  if (!ensureMainCheckout(config, project, audit, replace_checkout, registeredCheckoutsOrWarn(config))) {
    return 1;
  }
  if (!remote_exists) {
    const auto create_result = ckgit::runProcess(
        {"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "RequestTTY=no",
         "-o", "ClearAllForwardings=yes", config.server,
         "ckgit-rpc 1 create " + project + " " + default_branch},
        kControlTimeout);
    if (create_result.exit_code != 0 || create_result.timed_out || create_result.output_truncated ||
        create_result.output != "ok created\n") {
      std::cerr << "ckgit: " << audit.path.string() << ": remote project creation failed ("
                << describeProcessFailure(create_result, kControlTimeout) << "); local repository was unchanged\n";
      return 1;
    }
    const auto add_result = ckgit::runProcess(
        {"git", "-C", audit.path.string(), "remote", "add", config.remote_name, remote_url});
    if (add_result.exit_code != 0 || add_result.timed_out || add_result.output_truncated) {
      std::cerr << "ckgit: remote project was created but the local remote could not be added\n";
      return 1;
    }
  }
  const bool nothing_to_push = remote_exists && plan.new_refs.empty() && plan.updated_refs.empty();
  if (nothing_to_push) {
    std::cout << "Everything in scope is already published; refreshing the registration only.\n";
  } else {
    std::cout << "Pushing " << pushed_branches << " branch" << (pushed_branches == 1 ? "" : "es")
              << " and " << pushed_tags << " tag" << (pushed_tags == 1 ? "" : "s") << " to "
              << remote_url << " ...\n" << std::flush;
    const TransferSshGuard ssh_guard(audit.path);
    const auto push_result = ckgit::runProcess(pushCommand(audit.path, config.remote_name, false, scope),
                                               transferOptions());
    if (push_result.exit_code != 0 || push_result.timed_out) {
      std::cerr << "ckgit: " << audit.path.string() << ": non-destructive publish push failed ("
                << describeProcessFailure(push_result, kTransferTimeout)
                << "); the project and the " << config.remote_name << " remote exist.\n"
                << "Retry with ckgit publish --yes from inside this repository; "
                   "folder publishing skips paired projects.\n";
      return 1;
    }
  }
  std::string registration_failure;
  if (!registerRemoteCheckout(audit, config, project, &registration_failure, replace_checkout)) {
    std::cerr << "ckgit: published " << project
              << " but could not refresh checkout registration: " << registration_failure << "\n";
    return kPartial;
  }
  std::cout << "Published " << project << " from " << audit.path.string() << "\n";
  return 0;
}

std::set<std::string> fetchPublishedProjects(const ckgit::ClientConfig& config) {
  const auto result = controlRpc(config, "list-projects");
  if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
    throw std::runtime_error("could not list published projects (" +
                            describeProcessFailure(result, kControlTimeout) + ")");
  }
  const auto& reply = result.output;
  const auto header_end = reply.find('\n');
  if (reply.rfind("ok ", 0) != 0 || header_end == std::string::npos || reply.back() != '\n') {
    throw std::runtime_error("published project reply has invalid framing");
  }
  std::size_t expected = 0;
  const auto [end, error] = std::from_chars(reply.data() + 3, reply.data() + header_end, expected);
  if (error != std::errc{} || end != reply.data() + header_end || expected > ckgit::kMaximumControlRefs) {
    throw std::runtime_error("published project reply has an invalid count");
  }
  std::set<std::string> projects;
  for (std::size_t start = header_end + 1; start < reply.size();) {
    const auto newline = reply.find('\n', start);
    const auto name = reply.substr(start, newline - start);
    if (!ckgit::isValidProjectName(name) || !projects.insert(name).second) {
      throw std::runtime_error("published project reply contains invalid or duplicate names");
    }
    start = newline + 1;
  }
  if (projects.size() != expected) {
    throw std::runtime_error("published project reply count does not match records");
  }
  return projects;
}

bool samePublishSource(const ckgit::RepositoryAudit& approved, const ckgit::RepositoryAudit& current) {
  if (approved.path != current.path || approved.current_branch != current.current_branch ||
      approved.detached_head != current.detached_head) {
    return false;
  }
  const auto before = localRefTips(approved);
  const auto after = localRefTips(current);
  return before.size() == after.size() && std::equal(before.begin(), before.end(), after.begin(),
      [](const ckgit::RefTip& a, const ckgit::RefTip& b) {
        return a.name == b.name && a.object_id == b.object_id;
      });
}

int publishFolder(const ckgit::ClientConfig& config, const std::filesystem::path& folder,
                  const PushScope& scope, bool confirmed, bool dry_run, bool verbose) {
  const auto discovery = ckgit::discoverImmediateWorkingTrees(folder);
  bool attention = !discovery.warnings.empty();
  for (const auto& warning : discovery.warnings) {
    std::cerr << "ckgit: " << warning << "\n";
  }
  std::vector<ckgit::RepositoryAudit> candidates;
  std::size_t skipped = 0;
  for (const auto& path : discovery.repositories) {
    try {
      auto audit = ckgit::inspectDiscoveredRepository(path);
      bool invalid = false;
      const auto paired = pairedProjectForAudit(audit, config, &invalid);
      if (paired.has_value() || invalid) {
        std::cout << "Skipping " << path.string() << ": "
                  << (invalid ? "configured remote points elsewhere or is ambiguous"
                              : "already published as " + *paired + "; use ckgit sync") << "\n";
        ++skipped;
        attention = attention || invalid;
        continue;
      }
      if (!ckgit::isValidProjectName(path.filename().string())) {
        throw std::runtime_error("folder name is not a valid project name; publish it individually with --name");
      }
      if (!hasCommittedCurrentBranch(audit)) {
        throw std::runtime_error("publish requires a checked-out, committed branch");
      }
      for (const auto& branch : scope.branches) {
        if (std::none_of(audit.branches.begin(), audit.branches.end(), [&](const ckgit::RefTip& tip) {
              return tip.name == "refs/heads/" + branch;
            })) {
          throw std::runtime_error("--branch " + branch + " does not exist");
        }
      }
      candidates.push_back(std::move(audit));
    } catch (const std::exception& error) {
      std::cerr << "ckgit: skipping " << path.string() << ": " << error.what() << "\n";
      ++skipped;
      attention = true;
    }
  }
  // A directory without the pairing remote can still have a name already in
  // use on the server. Query before prompting; never adopt that project.
  if (!candidates.empty()) {
    const auto published = fetchPublishedProjects(config);
    std::erase_if(candidates, [&](const ckgit::RepositoryAudit& audit) {
      if (!published.contains(audit.path.filename().string())) {
        return false;
      }
      std::cout << "Skipping " << audit.path.string() << ": project name already exists on the server\n";
      ++skipped;
      return true;
    });
  }
  if (candidates.empty()) {
    std::cout << "No unpublished projects to publish in the immediate subdirectories of "
              << std::filesystem::canonical(folder).string() << ".\n";
    return attention ? kPartial : 0;
  }
  const auto registered = registeredCheckoutsOrWarn(config);
  std::cout << "Unpublished projects to publish to " << config.server << ":\n";
  for (const auto& audit : candidates) {
    const auto project = audit.path.filename().string();
    const bool current_in_scope = scope.branches.empty() ||
        std::find(scope.branches.begin(), scope.branches.end(), audit.current_branch) != scope.branches.end();
    const auto default_branch = current_in_scope ? audit.current_branch : scope.branches.front();
    const auto plan = planPublish(audit, config, project, scope, true);
    printTransferPlan(config, audit, project, plan, true, default_branch, false, verbose, registered);
  }
  std::cout << candidates.size() << " project(s) to publish, " << skipped << " skipped.\n";
  if (dry_run || (!confirmed && isatty(STDIN_FILENO) == 0)) {
    std::cout << "Preview only. Run again in a terminal to confirm, or use --yes to publish.\n";
    return attention ? kPartial : 0;
  }
  if (!confirmed) {
    std::cout << "Publish these " << candidates.size() << " project(s)? [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer) || (answer != "y" && answer != "Y" && answer != "yes" && answer != "YES")) {
      std::cout << "Cancelled; no projects were published.\n";
      return attention ? kPartial : 0;
    }
  }
  const auto lock = acquireSyncLock(config, "publish");
  if (!lock.has_value()) {
    return kBusy;
  }
  std::size_t succeeded = 0;
  for (const auto& candidate : candidates) {
    try {
      // Recheck after confirmation: a changed checkout or newly paired remote
      // needs another preview. The server also refuses competing creates.
      if (std::filesystem::is_symlink(std::filesystem::symlink_status(candidate.path))) {
        throw std::runtime_error("checkout became a symlink; run publish again to review");
      }
      const auto current = ckgit::inspectDiscoveredRepository(candidate.path);
      if (!samePublishSource(candidate, current) ||
          std::any_of(current.remotes.begin(), current.remotes.end(), [&](const ckgit::RemoteUrl& remote) {
            return remote.remote_name == config.remote_name;
          })) {
        throw std::runtime_error("repository changed after the preview; run publish again to review");
      }
      if (publishRepository(config, current, current.path.filename().string(), scope, true, false, true, verbose) == 0) {
        ++succeeded;
      } else {
        attention = true;
      }
    } catch (const std::exception& error) {
      std::cerr << "ckgit: " << candidate.path.string() << ": " << error.what() << "\n";
      attention = true;
    }
  }
  std::cout << "Published " << succeeded << " of " << candidates.size() << " project(s); "
            << skipped << " skipped.\n";
  return attention ? kPartial : 0;
}

int publishCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::string> requested_name;
  std::optional<std::filesystem::path> repository_path;
  PushScope scope;
  bool confirmed = false;
  bool dry_run = false;
  bool verbose = false;
  bool replace_checkout = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if ((arguments[index] == "--config" || arguments[index] == "--name" || arguments[index] == "--branch") &&
        index + 1 < arguments.size()) {
      const std::string value = arguments[++index];
      if (arguments[index - 1] == "--config") {
        config_path.emplace(value);
      } else if (arguments[index - 1] == "--branch") {
        if (!ckgit::isValidBranchName(value)) {
          std::cerr << "ckgit: --branch needs a valid branch name\n";
          return kUsage;
        }
        if (std::find(scope.branches.begin(), scope.branches.end(), value) == scope.branches.end()) {
          scope.branches.push_back(value);
        }
      } else {
        requested_name.emplace(value);
      }
    } else if (arguments[index] == "--yes") {
      confirmed = true;
    } else if (arguments[index] == "--dry-run") {
      dry_run = true;
    } else if (arguments[index] == "--verbose") {
      verbose = true;
    } else if (arguments[index] == "--no-tags") {
      scope.include_tags = false;
    } else if (arguments[index] == "--replace-checkout") {
      replace_checkout = true;
    } else if (!arguments[index].empty() && arguments[index].front() != '-' && !repository_path.has_value()) {
      repository_path.emplace(arguments[index]);
    } else {
      std::cerr << "ckgit: usage: ckgit publish --config PATH [--name NAME] [--branch NAME ...] [--no-tags] "
                   "[--replace-checkout] [--yes] [--dry-run] [REPOSITORY-OR-FOLDER]\n";
      return kUsage;
    }
  }
  const auto config = ckgit::loadClientConfig(resolveConfigPath(config_path));
  const auto path = repository_path.value_or(".");
  try {
    // Preserve errors for broken repositories and Git environment overrides;
    // folder discovery is only for paths outside a repository context.
    if (std::getenv("GIT_DIR") == nullptr && std::getenv("GIT_WORK_TREE") == nullptr &&
        !ckgit::hasRepositoryContext(path)) {
      if (requested_name.has_value() || replace_checkout) {
        std::cerr << "ckgit: --name and --replace-checkout require a single repository\n";
        return kUsage;
      }
      return publishFolder(config, path, scope, confirmed, dry_run, verbose);
    }
    const auto audit = ckgit::inspectRepository(path);
    bool invalid = false;
    const auto paired = pairedProjectForAudit(audit, config, &invalid);
    if (invalid) {
      std::cerr << "ckgit: configured remote points elsewhere or is ambiguous; check " << config.remote_name << "\n";
      return 1;
    }
    if (paired.has_value() && requested_name.has_value() && *paired != *requested_name) {
      std::cerr << "ckgit: --name " << *requested_name << " conflicts with existing project " << *paired
                << " identified by remote " << config.remote_name << "; omit --name to publish that project.\n";
      return kUsage;
    }
    const auto project = paired.value_or(requested_name.value_or(audit.path.filename().string()));
    return publishRepository(config, audit, project, scope, confirmed && !dry_run, replace_checkout, false, verbose);
  } catch (const std::exception& error) {
    std::cerr << "ckgit: cannot publish: " << error.what() << "\n";
    return 1;
  }
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
                            std::string_view project, std::string* failure_reason, bool replace) {
  try {
    if (!ensureMainCheckout(config, std::string(project), audit, replace, {})) {
      *failure_reason = "another private main checkout is selected; use --replace-checkout to select this folder";
      return false;
    }
    const std::string encoded_path = ckgit::encodeCheckoutPath(reportedPathForRegistration(audit, config));
    const auto result = controlRpc(config, std::string(replace ? "replace-checkout " : "register ") +
                                               std::string(project) + " " + encoded_path);
    if (result.exit_code == 0 && !result.timed_out && !result.output_truncated &&
        result.output == "ok registered\n") {
      try {
        ckgit::saveCanonicalCheckout(canonicalSelectionPath(config), project, audit.path);
      } catch (const std::exception& error) {
        *failure_reason = "server registration succeeded, but the private local inventory could not be saved: " +
                          std::string(error.what()) + "; retry the same command to finish management";
        return false;
      }
      return true;
    }
    if (result.output.rfind("error conflict", 0) == 0) {
      *failure_reason = "this host already registered a different checkout for " + std::string(project) +
                        "; use --replace-checkout to make this one the main checkout";
      return false;
    }
    *failure_reason = "remote checkout registration was rejected (" +
                      describeProcessFailure(result, kControlTimeout) + ")";
  } catch (const std::exception& error) {
    *failure_reason = error.what();
  }
  return false;
}

int registerCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::filesystem::path> repository_path;
  bool replace_checkout = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if ((arguments[index] == "--config" || arguments[index] == "--repo") &&
        index + 1 < arguments.size()) {
      const std::string value = arguments[++index];
      if (arguments[index - 1] == "--config") {
        config_path.emplace(value);
      } else {
        repository_path.emplace(value);
      }
    } else if (arguments[index] == "--replace-checkout") {
      replace_checkout = true;
    } else {
      std::cerr << "ckgit: usage: ckgit register --config PATH [--repo PATH] [--replace-checkout]\n";
      return kUsage;
    }
  }
  const auto config = ckgit::loadClientConfig(resolveConfigPath(config_path));
  const auto lock = acquireSyncLock(config, "register");
  if (!lock.has_value()) {
    return kBusy;
  }
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
  if (!registerRemoteCheckout(audit, config, *project, &failure_reason, replace_checkout)) {
    std::cerr << "ckgit: checkout registration failed: " << failure_reason << "\n";
    return 1;
  }
  std::cout << "Registered " << *project << " from " << reportedPathForRegistration(audit, config) << "\n";
  return 0;
}

int syncOne(const ckgit::RepositoryAudit& audit, const ckgit::ClientConfig& config, bool dry_run,
            bool replace_checkout = false, bool verbose = false,
            const std::map<std::string, std::filesystem::path>* known_registered = nullptr) {
  bool invalid_remote = false;
  const auto project = pairedProjectForAudit(audit, config, &invalid_remote);
  if (!project.has_value()) {
    std::cerr << "ckgit: " << audit.path.string() << ": "
              << (invalid_remote ? "configured remote has an unrecognized or ambiguous URL"
                                 : "sync requires an exactly matched configured remote") << "\n";
    return 1;
  }
  const auto registered = known_registered != nullptr ? *known_registered : registeredCheckoutsOrWarn(config);
  try {
    const auto plan = planPublish(audit, config, *project, PushScope{});
    printTransferPlan(config, audit, *project, plan, false, {}, replace_checkout, verbose, registered);
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << *project << ": could not prepare upload plan: " << error.what() << "\n";
    return kPartial;
  }
  if (!ensureMainCheckout(config, *project, audit, replace_checkout, registered)) {
    return 1;
  }
  const TransferSshGuard ssh_guard(audit.path);
  const auto preflight_result = ckgit::runProcess(pushCommand(audit.path, config.remote_name, true),
                                                  kTransferTimeout, kTransferOutputLimit);
  if (preflight_result.exit_code != 0 || preflight_result.timed_out || preflight_result.output_truncated) {
    std::cerr << "ckgit: " << audit.path.string()
              << ": sync preflight rejected a non-destructive atomic update ("
              << describeProcessFailure(preflight_result, kTransferTimeout) << ")\n";
    return kPartial;
  }
  std::cout << "  Git preflight: ref updates permitted without force; server hooks run on upload.\n";
  if (dry_run) {
    std::cout << "Preview only for " << audit.path.string() << "; no refs or checkout management changed.\n";
    return 0;
  }
  std::string registration_failure;
  if (!registerRemoteCheckout(audit, config, *project, &registration_failure, replace_checkout)) {
    std::cerr << "ckgit: " << audit.path.string()
              << ": checkout registration failed; no refs were pushed: " << registration_failure << "\n";
    return kPartial;
  }
  const auto push_result = ckgit::runProcess(pushCommand(audit.path, config.remote_name, false), transferOptions());
  if (push_result.exit_code != 0 || push_result.timed_out) {
    std::cerr << "ckgit: " << audit.path.string() << ": sync push failed after preflight ("
              << describeProcessFailure(push_result, kTransferTimeout)
              << "); no fallback force push was attempted\n";
    return kPartial;
  }
  std::cout << "Synced " << audit.path.string() << " (uploaded committed changes; no download)\n";
  return 0;
}

// Ordinary sync uploads exactly the effective managed set also shown by
// status and checkout list. Privacy only changes the report sent to the server.
int syncRegistered(const ckgit::ClientConfig& config, bool dry_run, bool verbose) {
  const auto inventory = managedInventory(config);
  printManagedScope(inventory, config);
  bool partial = inventory.attention;
  std::size_t succeeded = 0;
  std::size_t skipped = 0;
  for (const auto& [project, checkout] : inventory.projects) {
    if (!checkout.audit.has_value()) {
      printUnavailableCheckout(project, checkout);
      ++skipped;
      continue;
    }
    if (syncOne(*checkout.audit, config, dry_run, false, verbose, &inventory.reported) != 0) {
      partial = true;
    } else {
      ++succeeded;
    }
  }
  std::cout << (dry_run ? "Would upload " : "Uploaded ") << succeeded << " of " << inventory.projects.size()
            << " managed project(s); " << skipped << " unavailable.\n";
  return partial ? kPartial : 0;
}

int syncCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::filesystem::path> repository_path;
  bool dry_run = false;
  bool verbose = false;
  bool scan = false;
  bool replace_checkout = false;
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
    } else if (arguments[index] == "--verbose") {
      verbose = true;
    } else if (arguments[index] == "--scan") {
      scan = true;
    } else if (arguments[index] == "--replace-checkout") {
      replace_checkout = true;
    } else {
      std::cerr << "ckgit: usage: ckgit sync --config PATH [--repo PATH] [--scan] [--replace-checkout] [--dry-run]\n";
      return kUsage;
    }
  }
  if (scan && repository_path.has_value()) {
    std::cerr << "ckgit: --scan and --repo are mutually exclusive\n";
    return kUsage;
  }
  if (replace_checkout && !repository_path.has_value()) {
    std::cerr << "ckgit: --replace-checkout needs --repo PATH to name the new main checkout\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(resolveConfigPath(config_path));
  std::optional<ckgit::SyncLock> lock;
  if (!dry_run) {
    lock = acquireSyncLock(config, "sync");
    if (!lock.has_value()) {
      return kBusy;
    }
  }
  std::vector<ckgit::RepositoryAudit> audits;
  bool partial = false;
  if (repository_path.has_value()) {
    // Explicit selection names the source but does not replace a different
    // main checkout unless --replace-checkout was supplied.
    try {
      audits.push_back(ckgit::inspectRepository(*repository_path));
    } catch (const std::exception& error) {
      std::cerr << "ckgit: cannot inspect repository to sync: " << error.what() << "\n";
      return 1;
    }
    std::cout << "Scope: requested repository on " << config.display_name << " → " << config.server << "\n";
    return syncOne(audits.front(), config, dry_run, replace_checkout, verbose);

  }
  if (!scan) {
    return syncRegistered(config, dry_run, verbose);
  } else {
    std::cout << "Scope: discovered paired repositories under configured scan roots on " << config.display_name
              << " → " << config.server << "\n";
    const auto discovery = ckgit::discoverWorkingTrees(config.scan_roots, config.exclusions);
    for (const auto& warning : discovery.warnings) {
      std::cerr << "warning: " << warning << "\n";
      partial = true;
    }
    for (const auto& path : discovery.repositories) {
      try {
        audits.push_back(ckgit::inspectDiscoveredRepository(path));
      } catch (const std::exception& error) {
        std::cerr << "warning: " << path.string() << ": " << error.what() << "\n";
        partial = true;
      }
    }
  }
  const auto inventory = managedInventory(config);
  auto selections = ckgit::loadCanonicalCheckouts(canonicalSelectionPath(config));
  for (const auto& [project, checkout] : inventory.projects) {
    // A legacy absolute registration is the same effective main selected by
    // ordinary sync. Basename records remain unresolved until explicit discovery.
    if (checkout.path.is_absolute()) {
      selections.try_emplace(project, checkout.path);
    }
  }
  partial = partial || !inventory.server_available;
  const auto groups = groupPairedCheckouts(audits, config, &partial);
  for (const auto& [project, checkouts] : groups) {
    std::string reason;
    const auto* chosen = chooseAutomaticCheckout(project, checkouts, selections, &reason);
    if (chosen == nullptr) {
      std::cerr << "ckgit: " << project << ": " << reason << "\n";
      partial = true;
      continue;
    }
    partial = syncOne(*chosen, config, dry_run, false, verbose, &inventory.reported) != 0 || partial;
  }
  return partial ? kPartial : 0;
}

int checkoutList(const ckgit::ClientConfig& config, const std::optional<std::string>& only_project, bool scan) {
  const auto inventory = managedInventory(config);
  printManagedScope(inventory, config);
  bool attention = !inventory.server_available;
  std::size_t listed = 0;
  for (const auto& [project, checkout] : inventory.projects) {
    if (only_project.has_value() && *only_project != project) {
      continue;
    }
    ++listed;
    if (!checkout.audit.has_value()) {
      printUnavailableCheckout(project, checkout);
      attention = true;
      continue;
    }
    std::cout << project << "\n  main       " << checkout.path.string() << checkoutMarkers(*checkout.audit)
              << (checkout.legacy ? " [legacy registration; private path saved on next sync]" : " [private local inventory]")
              << "\n";
    const auto report = inventory.reported.find(project);
    if (report != inventory.reported.end()) {
      std::cout << "  reported to server: " << report->second.string() << "\n";
    }
  }
  if (scan) {
    std::cout << "Additional discovery under configured scan roots (does not change the main checkout):\n";
    const auto discovery = ckgit::discoverWorkingTrees(config.scan_roots, config.exclusions);
    for (const auto& warning : discovery.warnings) {
      std::cerr << "warning: " << warning << "\n";
      attention = true;
    }
    for (const auto& path : discovery.repositories) {
      try {
        const auto audit = ckgit::inspectDiscoveredRepository(path);
        bool invalid = false;
        const auto project = pairedProjectForAudit(audit, config, &invalid);
        if (!project.has_value() || (only_project.has_value() && *only_project != *project)) {
          attention = attention || invalid;
          continue;
        }
        const auto main = inventory.projects.find(*project);
        const bool selected = main != inventory.projects.end() && main->second.path.is_absolute() &&
                              samePath(main->second.path, audit.path);
        if (selected) {
          continue;
        }
        std::cout << "  " << *project << "  " << audit.path.string() << checkoutMarkers(audit)
                  << " [additional copy; not selected for ordinary sync]\n";
      } catch (const std::exception& error) {
        std::cerr << "warning: " << path.string() << ": " << error.what() << "\n";
        attention = true;
      }
    }
  }
  if (listed == 0 && only_project.has_value()) {
    std::cout << "No managed checkout found for " << *only_project << ". Use --scan to look for additional copies.\n";
  }
  return attention ? kPartial : 0;
}

int checkoutMigrate(const ckgit::ClientConfig& config, bool dry_run) {
  const auto inventory = managedInventory(config);
  printManagedScope(inventory, config);
  if (!inventory.server_available) {
    std::cerr << "ckgit: migration needs the server's legacy registrations; reconnect and retry.\n";
    return kPartial;
  }
  std::vector<ckgit::RepositoryAudit> discovered;
  const bool needs_discovery = std::any_of(inventory.projects.begin(), inventory.projects.end(), [](const auto& entry) {
    return entry.second.legacy && !entry.second.path.is_absolute();
  });
  bool partial = false;
  if (needs_discovery) {
    const auto discovery = ckgit::discoverWorkingTrees(config.scan_roots, config.exclusions);
    for (const auto& warning : discovery.warnings) {
      std::cerr << "warning: " << warning << "\n";
      partial = true;
    }
    for (const auto& path : discovery.repositories) {
      try {
        discovered.push_back(ckgit::inspectDiscoveredRepository(path));
      } catch (const std::exception& error) {
        std::cerr << "warning: " << path.string() << ": " << error.what() << "\n";
        partial = true;
      }
    }
  }
  for (const auto& [project, checkout] : inventory.projects) {
    if (!checkout.legacy) {
      std::cout << "Already managed " << project << " from " << checkout.path.string() << "\n";
      partial = partial || !checkout.audit.has_value();
      continue;
    }
    std::vector<const ckgit::RepositoryAudit*> candidates;
    if (checkout.audit.has_value()) {
      candidates.push_back(&*checkout.audit);
    } else if (!checkout.path.is_absolute()) {
      for (const auto& audit : discovered) {
        bool invalid = false;
        const auto paired = pairedProjectForAudit(audit, config, &invalid);
        if (paired.has_value() && *paired == project && audit.path.filename() == checkout.path) {
          candidates.push_back(&audit);
        }
      }
    }
    if (candidates.size() != 1) {
      std::cout << project << ": " << (candidates.empty() ? "no matching local checkout" : "ambiguous matching checkouts")
                << "; no private selection changed.\n";
      for (const auto* audit : candidates) {
        std::cout << "  " << audit->path.string() << "\n";
      }
      std::cout << "Select it explicitly with ckgit checkout set-canonical " << project
                << " PATH, or configure scan_root and retry checkout migrate.\n";
      partial = true;
      continue;
    }
    const auto& path = candidates.front()->path;
    if (!dry_run) {
      ckgit::saveCanonicalCheckout(canonicalSelectionPath(config), project, path);
    }
    std::cout << (dry_run ? "Would manage " : "Managed ") << project << " from " << path.string()
              << " (absolute path stays on this device)\n";
  }
  return partial ? kPartial : 0;
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
  std::string failure_reason;
  if (!registerRemoteCheckout(audit, config, project, &failure_reason, true)) {
    std::cerr << "ckgit: could not select this main checkout: "
              << failure_reason << "\n";
    return kPartial;
  }
  std::cout << "Selected " << audit.path.string() << " as the main checkout on this device for " << project
            << "; ordinary sync and sync --scan use this same selection\n";
  return 0;
}

int checkoutCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::vector<std::string> positional;
  bool scan = false;
  bool dry_run = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index] == "--config" && index + 1 < arguments.size()) {
      config_path.emplace(arguments[++index]);
    } else if (arguments[index] == "--scan") {
      scan = true;
    } else if (arguments[index] == "--dry-run") {
      dry_run = true;
    } else if (!arguments[index].empty() && arguments[index].front() == '-') {
      std::cerr << "ckgit: invalid checkout option; see ckgit checkout --help\n";
      return kUsage;
    } else {
      positional.push_back(arguments[index]);
    }
  }
  const bool list = positional.size() >= 1 && positional.size() <= 2 && positional[0] == "list" &&
                    (positional.size() == 1 || ckgit::isValidProjectName(positional[1])) && !dry_run;
  const bool set_canonical = positional.size() == 3 && positional[0] == "set-canonical" &&
                             ckgit::isValidProjectName(positional[1]) && !positional[2].empty() && !scan && !dry_run;
  const bool migrate = positional.size() == 1 && positional[0] == "migrate" && !scan;
  if (!list && !set_canonical && !migrate) {
    std::cerr << "ckgit: invalid checkout arguments; see ckgit checkout --help\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(resolveConfigPath(config_path));
  if (list) {
    return checkoutList(config, positional.size() == 2 ? std::optional<std::string>(positional[1]) : std::nullopt, scan);
  }
  std::optional<ckgit::SyncLock> lock;
  if (!dry_run) {
    lock = acquireSyncLock(config, "checkout");
    if (!lock.has_value()) {
      return kBusy;
    }
  }
  if (migrate) {
    return checkoutMigrate(config, dry_run);
  }
  return checkoutSetCanonical(config, positional[1], positional[2]);
}

volatile std::sig_atomic_t tunnel_stop_requested = 0;

void requestTunnelStop(int) {
  tunnel_stop_requested = 1;
}

bool isValidSshTarget(std::string_view target) {
  const std::size_t at = target.find('@');
  const std::string_view user = at == std::string_view::npos ? std::string_view{} : target.substr(0, at);
  const std::string_view host = at == std::string_view::npos ? target : target.substr(at + 1);
  const auto safe = [](std::string_view value, bool allow_dot) {
    return !value.empty() && value.size() <= 255 && value.front() != '-' &&
           std::all_of(value.begin(), value.end(), [allow_dot](unsigned char character) {
             return std::isalnum(character) != 0 || character == '-' || character == '_' ||
                    (allow_dot && character == '.');
           });
  };
  return (at == std::string_view::npos || safe(user, false)) && safe(host, true) &&
         target.find('@', at == std::string_view::npos ? 0 : at + 1) == std::string_view::npos;
}

int loopbackSocket() {
  const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) {
    throw std::runtime_error("could not create a loopback socket");
  }
  return descriptor;
}

sockaddr_in loopbackAddress(unsigned short port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  return address;
}

// A free port is one this process can bind right now; the tunnel binds it a
// moment later, so a rare race only produces ssh's own clear failure.
// SO_REUSEADDR matches ssh's own listener: connections left in TIME_WAIT by a
// previous tunnel do not make the port count as busy, a live listener does.
bool localPortIsFree(unsigned short port) {
  const int descriptor = loopbackSocket();
  const int enabled = 1;
  setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  const auto address = loopbackAddress(port);
  const bool free_port = bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
  close(descriptor);
  return free_port;
}

unsigned short pickLocalPort(const std::optional<unsigned short>& requested) {
  if (requested.has_value()) {
    if (!localPortIsFree(*requested)) {
      throw std::runtime_error("local port " + std::to_string(*requested) + " is already in use");
    }
    return *requested;
  }
  if (localPortIsFree(kDefaultDashboardPort)) {
    return kDefaultDashboardPort;
  }
  const int descriptor = loopbackSocket();
  const int enabled = 1;
  setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  auto address = loopbackAddress(0);
  socklen_t size = sizeof(address);
  if (bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
    close(descriptor);
    throw std::runtime_error("could not allocate a free local port");
  }
  close(descriptor);
  return ntohs(address.sin_port);
}

int connectLoopback(unsigned short port, int timeout_milliseconds) {
  const int descriptor = loopbackSocket();
  const int flags = fcntl(descriptor, F_GETFL, 0);
  fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
  const auto address = loopbackAddress(port);
  if (connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    if (errno != EINPROGRESS) {
      close(descriptor);
      return -1;
    }
    pollfd ready{descriptor, POLLOUT, 0};
    int status = 0;
    socklen_t status_size = sizeof(status);
    if (poll(&ready, 1, timeout_milliseconds) <= 0 ||
        getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &status, &status_size) != 0 || status != 0) {
      close(descriptor);
      return -1;
    }
  }
  fcntl(descriptor, F_SETFL, flags);
  return descriptor;
}

// Sends one bounded HEAD request through the tunnel and returns the status
// line, so "tunnel up, dashboard disabled" is reported instead of a blank page.
std::optional<std::string> probeDashboard(unsigned short port) {
  const int descriptor = connectLoopback(port, 3000);
  if (descriptor < 0) {
    return std::nullopt;
  }
  timeval timeout{};
  timeout.tv_sec = 3;
  setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  const std::string request = "HEAD / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  if (send(descriptor, request.data(), request.size(), 0) != static_cast<ssize_t>(request.size())) {
    close(descriptor);
    return std::nullopt;
  }
  std::string reply;
  char buffer[256];
  while (reply.find("\r\n") == std::string::npos && reply.size() < 1024) {
    const ssize_t received = recv(descriptor, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      break;
    }
    reply.append(buffer, static_cast<std::size_t>(received));
  }
  close(descriptor);
  const std::size_t end = reply.find("\r\n");
  if (end == std::string::npos) {
    return std::nullopt;
  }
  return reply.substr(0, end);
}

bool childHasExited(int child, int* status) {
  const pid_t reaped = waitpid(child, status, WNOHANG);
  return reaped == child;
}

int childExitCode(int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}

// The opener may stay in the foreground for as long as the browser runs on
// some Linux desktops, so it is never waited for or killed; it is reaped
// alongside the tunnel loop.
int openBrowser(const std::string& url) {
#ifdef __APPLE__
  const std::vector<std::string> command{"open", url};
#else
  const std::vector<std::string> command{"xdg-open", url};
#endif
  try {
    return ckgit::spawnAttachedProcess(command);
  } catch (const std::exception&) {
    std::cerr << "ckgit: no browser opener available; visit the URL manually\n";
    return -1;
  }
}

int webCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<unsigned short> local_port;
  unsigned short remote_port = kDefaultDashboardPort;
  bool remote_port_explicit = false;
  bool open_browser = true;
  std::optional<std::string> admin_host;
  std::optional<std::string> project;
  const auto parsePort = [](const std::string& value, unsigned short* port) {
    unsigned int parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 || parsed > 65535) {
      return false;
    }
    *port = static_cast<unsigned short>(parsed);
    return true;
  };
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string& argument = arguments[index];
    if ((argument == "--config" || argument == "--port" || argument == "--remote-port" ||
         argument == "--project") && index + 1 < arguments.size()) {
      const std::string value = arguments[++index];
      unsigned short port = 0;
      if (argument == "--config") {
        config_path.emplace(value);
      } else if (argument == "--project") {
        project = value;
      } else if (!parsePort(value, &port)) {
        std::cerr << "ckgit: " << argument << " needs a port between 1 and 65535\n";
        return kUsage;
      } else if (argument == "--port") {
        local_port = port;
      } else {
        remote_port = port;
        remote_port_explicit = true;
      }
    } else if (argument == "--no-open") {
      open_browser = false;
    } else if (!argument.empty() && argument.front() != '-' && !admin_host.has_value() &&
               isValidSshTarget(argument)) {
      admin_host = argument;
    } else {
      std::cerr << "ckgit: usage: ckgit web [--config PATH] [--port PORT] [--remote-port PORT] [--project NAME] [--no-open] [ADMIN-HOST]\n";
      return kUsage;
    }
  }
  if (!admin_host.has_value() || config_path.has_value()) {
    // The tunnel uses the administrator's own SSH login on the server host;
    // the restricted ckgit account deliberately allows no forwarding.
    const auto config = ckgit::loadClientConfig(resolveConfigPath(config_path));
    if (!admin_host.has_value()) {
      admin_host = config.web_host.empty() ? config.server.substr(config.server.find('@') + 1) : config.web_host;
    }
    if (!remote_port_explicit) remote_port = config.web_port;
  }
  const unsigned short port = pickLocalPort(local_port);
  const std::string url = "http://127.0.0.1:" + std::to_string(port) +
      (project.has_value() ? "/project/" + *project : "/");
  const std::string forward = "127.0.0.1:" + std::to_string(port) + ":127.0.0.1:" + std::to_string(remote_port);

  struct sigaction action {};
  action.sa_handler = requestTunnelStop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
    throw std::runtime_error("could not install signal handlers");
  }
  std::cout << "Opening tunnel to " << *admin_host << " ...\n" << std::flush;
  const int child = ckgit::spawnAttachedProcess(
      {"ssh", "-N", "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes", "-o", "RequestTTY=no",
       "-o", "ServerAliveInterval=30", "-L", forward, *admin_host});
  int status = 0;
  const auto stopChild = [&]() {
    if (!childHasExited(child, &status)) {
      kill(child, SIGTERM);
      waitpid(child, &status, 0);
    }
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  bool listening = false;
  while (!listening && !tunnel_stop_requested) {
    if (childHasExited(child, &status)) {
      std::cerr << "ckgit: ssh exited before the tunnel was ready\n";
      return childExitCode(status) == 0 ? 1 : childExitCode(status);
    }
    const int probe = connectLoopback(port, 250);
    if (probe >= 0) {
      close(probe);
      listening = true;
      break;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      stopChild();
      std::cerr << "ckgit: the tunnel did not become ready within 20 seconds\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (tunnel_stop_requested) {
    stopChild();
    std::cout << "Tunnel closed.\n";
    return 0;
  }
  const auto status_line = probeDashboard(port);
  if (!status_line.has_value() || status_line->rfind("HTTP/1.1 200", 0) != 0) {
    stopChild();
    std::cerr << "ckgit: the tunnel is up but the dashboard did not answer on the server's port "
              << remote_port << "; set http_port in /etc/ck-git-hosting/server.ini and restart the service, "
              << "or pass --remote-port\n";
    return 1;
  }
  std::cout << "Dashboard: " << url << "\nPress Ctrl+C to close the tunnel.\n" << std::flush;
  int opener = open_browser ? openBrowser(url) : -1;
  while (!tunnel_stop_requested) {
    if (opener > 0) {
      int opener_status = 0;
      if (waitpid(opener, &opener_status, WNOHANG) == opener) {
        opener = -1;
      }
    }
    if (childHasExited(child, &status)) {
      std::cerr << "ckgit: the tunnel was closed by ssh\n";
      return childExitCode(status) == 0 ? 0 : childExitCode(status);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  stopChild();
  std::cout << "Tunnel closed.\n";
  return 0;
}

#include "discovery.inc"
#include "setup_doctor.inc"
#include "incoming.inc"

}  // namespace

int main(int argc, char* argv[]) {
  try {
    ckgit::installChildTerminationForwarding();
    std::vector<std::string> raw_arguments;
    for (int index = 1; index < argc; ++index) {
      raw_arguments.emplace_back(argv[index]);
    }
    const auto invocation = ckgit::prepareClientInvocation(raw_arguments);
    if (invocation.exit_code.has_value()) {
      std::cout << invocation.standard_output;
      std::cerr << invocation.standard_error;
      return *invocation.exit_code;
    }
    const auto& command = invocation.command;
    const auto& arguments = invocation.arguments;
    if (command == "setup") return setupCommand(arguments);
    if (command == "doctor") return doctorCommand(arguments);
    if (command == "projects") return projectsCommand(arguments);
    if (command == "fetch") return fetchCommand(arguments);
    if (command == "update") return updateCommand(arguments);
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
      return cloneEntryCommand(arguments);
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
      if (!arguments.empty() && arguments.front() == "forget") {
        return forgetCommand(std::vector<std::string>(arguments.begin() + 1, arguments.end()));
      }
      return checkoutCommand(arguments);
    }
    if (command == "web") {
      return webCommand(arguments);
    }
    printUsage(std::cerr);
    return kUsage;
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << error.what() << "\n";
    return 1;
  }
}
