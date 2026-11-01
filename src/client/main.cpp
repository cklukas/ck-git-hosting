// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
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
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "ckgit/socket.hpp"
#include "ckgit/windows.hpp"
#include "ckgit/path_utf8.hpp"
#include "ckgit/directory_identity.hpp"
#ifdef _WIN32
#include <io.h>
#include <shellapi.h>
#else
#include <netdb.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "ckgit/ci_control.hpp"
#include "ckgit/ci_store.hpp"
#include "ckgit/ci_workflow.hpp"
#include "ckgit/client_config.hpp"
#include "ckgit/cli_help.hpp"
#include "ckgit/client_state.hpp"
#include "ckgit/control_rpc.hpp"
#include "ckgit/git_repository.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/http_router.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/process.hpp"
#include "ckgit/ref_status.hpp"
#include "ckgit/server_identity.hpp"
#include "ckgit/text.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/web_renderer.hpp"

namespace {
namespace net = ckgit::net;
#ifdef _WIN32
constexpr int STDIN_FILENO = 0, STDOUT_FILENO = 1, STDERR_FILENO = 2;
int isatty(int descriptor) { return _isatty(descriptor); }
int setenv(const char* name, const char* value, int overwrite) {
  if (!overwrite && std::getenv(name)) return 0;
  return _wputenv_s(ckgit::utf8ToWide(name).c_str(), ckgit::utf8ToWide(value).c_str());
}
int unsetenv(const char* name) { return _wputenv_s(ckgit::utf8ToWide(name).c_str(), L""); }
#endif

// Only this client opts into process tool selection. Config parsing in other
// programs remains pure, and sandbox launches never consult this scope.
std::unique_ptr<ckgit::ProcessToolScope> client_tools;
void selectClientTools(const ckgit::ClientConfig& config) {
  client_tools.reset();
  client_tools = std::make_unique<ckgit::ProcessToolScope>(
      ckgit::pathToUtf8(config.git_program), ckgit::pathToUtf8(config.ssh_program));
}
ckgit::ClientConfig loadConfiguredClient(const std::filesystem::path& path) {
  auto config = ckgit::loadClientConfig(path);
  selectClientTools(config);
  return config;
}

constexpr int kUsage = 2;
constexpr int kPartial = 3;
constexpr int kBusy = 4;
// `ckgit ci watch`: the watched run ended in any state other than success.
constexpr int kRunUnsuccessful = 5;
// Internal to bulk clone: a project managed meanwhile was skipped. Never an
// exit status, so it lies outside the documented codes.
constexpr int kCloneSkipped = -1;
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

// Runs one control operation over SSH and returns its bounded reply. The
// request is checked against the same operation table the server applies, so
// a malformed request fails here instead of as an opaque remote denial.
ckgit::ProcessResult controlRpc(const ckgit::ClientConfig& config, std::string_view operation,
                                const std::vector<std::string>& arguments = {}) {
  if (!ckgit::isValidControlOperation(operation, arguments)) {
    throw std::invalid_argument("invalid control request for " + std::string(operation));
  }
  std::string request = "ckgit-rpc 1 " + std::string(operation);
  for (const auto& argument : arguments) request += " " + argument;
  return ckgit::runProcess(
      {"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "RequestTTY=no",
       "-o", "ClearAllForwardings=yes", config.server, request},
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
#ifdef _WIN32
  const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
  if (!local || !*local) throw std::runtime_error("LOCALAPPDATA is not set; pass --config PATH");
  return std::filesystem::path(local) / L"ck-git-hosting" / L"client.ini";
#else
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path(xdg) / "ck-git-hosting" / "client.ini";
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("HOME is not set; pass --config PATH");
  }
  return std::filesystem::path(home) / ".config" / "ck-git-hosting" / "client.ini";
#endif
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
            << "git_program=" << ckgit::pathToUtf8(config.git_program) << "\n"
            << "ssh_program=" << ckgit::pathToUtf8(config.ssh_program) << "\n"
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
    const auto config = loadConfiguredClient(*config_path);
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
  printConfig(path, config_path.has_value(), loadConfiguredClient(path));
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
  const ckgit::ProcessResult result = controlRpc(config, "refs", {project});
  if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
    throw std::runtime_error("could not query paired server refs (" +
                             describeProcessFailure(result, kControlTimeout) + ")");
  }
  return ckgit::parseRefsControlResponse(result.output);
}

std::vector<ckgit::CiReleaseRecord> fetchReleases(const ckgit::ClientConfig& config,
                                                  const std::string& project) {
  const ckgit::ProcessResult result = controlRpc(config, "releases", {project});
  if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
    throw std::runtime_error("could not query paired server releases (" +
                             describeProcessFailure(result, kControlTimeout) + ")");
  }
  return ckgit::parseReleasesControlResponse(result.output);
}

// One checkout's place in `ckgit status`: how its committed refs compare with
// the paired server, or why they could not be compared.
struct PairedStatus {
  enum class State { kInSync, kUnpublished, kDiffers, kUnpaired, kProblem };
  std::filesystem::path path;
  std::string project;  // empty when the checkout is not paired with one project
  std::string remote_url;
  State state{State::kProblem};
  std::size_t equal{0};
  std::size_t local_only{0};
  std::size_t server_only{0};
  std::size_t mismatched{0};
  std::string problem;  // why the refs could not be compared
  bool attention{false};
};

PairedStatus collectPairedStatus(const ckgit::RepositoryAudit& audit, const ckgit::ClientConfig& config) {
  PairedStatus status;
  status.path = audit.path;
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
    status.state = PairedStatus::State::kUnpaired;
    status.problem = configured_remote_seen ? "configured remote does not match paired server" : "not configured";
    status.attention = configured_remote_seen;
    return status;
  }
  if (projects.size() != 1) {
    status.state = PairedStatus::State::kUnpaired;
    status.problem = "ambiguous configured remote URLs";
    status.attention = true;
    return status;
  }

  std::tie(status.project, status.remote_url) = *projects.begin();
  try {
    const auto server_refs = fetchServerRefs(config, status.project);
    for (const auto& ref : ckgit::compareRefTips(localRefTips(audit), server_refs)) {
      switch (ref.relation) {
        case ckgit::RefRelation::kEqual: ++status.equal; break;
        case ckgit::RefRelation::kLocalOnly: ++status.local_only; break;
        case ckgit::RefRelation::kServerOnly: ++status.server_only; break;
        case ckgit::RefRelation::kMismatched: ++status.mismatched; break;
      }
    }
    // Without server objects locally, a different object ID is intentionally
    // never guessed to be a fast-forward.  It requires manual review or sync's
    // later explicit preflight.
    status.attention = status.mismatched != 0 || status.server_only != 0;
    status.state = status.attention         ? PairedStatus::State::kDiffers
                   : status.local_only != 0 ? PairedStatus::State::kUnpublished
                                            : PairedStatus::State::kInSync;
  } catch (const std::exception& error) {
    status.state = PairedStatus::State::kProblem;
    status.problem = std::string("server ref query failed: ") + error.what();
    status.attention = true;
  }
  return status;
}

// The detailed form: one block per checkout (--repo, and --verbose).
void printPairedStatusBlock(const PairedStatus& status) {
  if (status.project.empty() || status.state == PairedStatus::State::kProblem) {
    std::cout << status.path.string() << "\n  pairing: " << status.problem << "\n";
  } else {
    std::cout << status.path.string() << "\n"
              << "  project: " << status.project << "\n"
              << "  paired remote: " << status.remote_url << "\n"
              << "  refs: " << status.equal << " equal, " << status.local_only << " local-only, "
              << status.server_only << " server-only, " << status.mismatched << " mismatched\n";
  }
}

// One row of the compact table: a project (or, for an unpaired checkout,
// its folder name), its state, the non-zero ref counts, and the checkout.
struct StatusTableRow {
  std::string name;
  PairedStatus::State kind{PairedStatus::State::kProblem};
  std::string state;
  std::string refs;
  std::string checkout;
  bool attention{false};
};

StatusTableRow statusTableRow(const PairedStatus& status, const std::string& fallback_name) {
  StatusTableRow row;
  row.name = status.project.empty() ? fallback_name : status.project;
  row.kind = status.state;
  row.checkout = status.path.string();
  row.attention = status.attention;
  switch (status.state) {
    case PairedStatus::State::kInSync: row.state = "✓ in sync"; break;
    case PairedStatus::State::kUnpublished: row.state = "↑ unpublished"; break;
    case PairedStatus::State::kDiffers: row.state = "! differs"; break;
    case PairedStatus::State::kUnpaired: row.state = status.attention ? "! unpaired" : "- unpaired"; break;
    case PairedStatus::State::kProblem: row.state = "! unavailable"; break;
  }
  if (status.state == PairedStatus::State::kUnpaired || status.state == PairedStatus::State::kProblem) {
    row.refs = status.problem;
    return row;
  }
  const auto add = [&row](std::size_t count, const char* label) {
    if (count == 0) return;
    row.refs += (row.refs.empty() ? "" : ", ") + std::to_string(count) + " " + label;
  };
  add(status.equal, "equal");
  add(status.local_only, "local-only");
  add(status.server_only, "server-only");
  add(status.mismatched, "mismatched");
  if (row.refs.empty()) row.refs = "no refs";
  return row;
}

// Display width of UTF-8 text, counting code points (the table's marks are
// single-width).
std::size_t displayWidth(const std::string& text) {
  return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char character) {
    return (static_cast<unsigned char>(character) & 0xc0) != 0x80;
  }));
}

// `text` followed by spaces up to `width`, plus a two-space column gap.
std::string paddedTo(const std::string& text, std::size_t width) {
  const std::size_t shown = displayWidth(text);
  return text + std::string((shown >= width ? 0 : width - shown) + 2, ' ');
}

// The compact form: one line per project, attention rows emphasized on a
// terminal, then a one-line summary.
void printStatusTable(const std::vector<StatusTableRow>& rows) {
  if (rows.empty()) return;
  std::size_t name_width = 7;  // "PROJECT"
  std::size_t state_width = 5;
  std::size_t refs_width = 4;
  for (const auto& row : rows) {
    name_width = std::max(name_width, displayWidth(row.name));
    state_width = std::max(state_width, displayWidth(row.state));
    refs_width = std::max(refs_width, displayWidth(row.refs));
  }
  const bool color = isatty(STDOUT_FILENO) != 0 && std::getenv("NO_COLOR") == nullptr;
  std::cout << paddedTo("PROJECT", name_width) << paddedTo("STATE", state_width) << paddedTo("REFS", refs_width)
            << "CHECKOUT\n";
  std::size_t in_sync = 0;
  std::size_t unpublished = 0;
  std::size_t attention = 0;
  for (const auto& row : rows) {
    const std::string line = paddedTo(row.name, name_width) + paddedTo(row.state, state_width) +
                             paddedTo(row.refs, refs_width) + row.checkout;
    std::cout << (row.attention && color ? "\033[1;33m" + line + "\033[0m" : line) << "\n";
    if (row.attention) {
      ++attention;
    } else if (row.kind == PairedStatus::State::kInSync) {
      ++in_sync;
    } else if (row.kind == PairedStatus::State::kUnpublished) {
      ++unpublished;
    }
  }
  std::cout << rows.size() << " checkout(s): " << in_sync << " in sync";
  if (unpublished != 0) std::cout << ", " << unpublished << " unpublished";
  std::cout << ", " << attention << " need attention";
  if (attention != 0) std::cout << " (!); ckgit status --repo PATH shows one in full";
  std::cout << "\n";
}

int statusCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::filesystem::path> repository_path;
  bool scan = false;
  bool verbose = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if ((arguments[index] == "--config" || arguments[index] == "--repo") && index + 1 < arguments.size()) {
      const auto option = arguments[index++];
      (option == "--config" ? config_path : repository_path) = arguments[index];
    } else if (arguments[index] == "--scan") {
      scan = true;
    } else if (arguments[index] == "--verbose") {
      verbose = true;
    } else {
      std::cerr << "ckgit: invalid status option; see ckgit status --help\n";
      return kUsage;
    }
  }
  if (scan && repository_path.has_value()) {
    std::cerr << "ckgit: --scan and --repo are mutually exclusive\n";
    return kUsage;
  }
  const auto config = loadConfiguredClient(resolveConfigPath(config_path));
  if (!scan && !repository_path.has_value()) {
    const auto inventory = managedInventory(config);
    printManagedScope(inventory, config);
    bool attention = inventory.attention;
    std::vector<StatusTableRow> rows;
    for (const auto& [project, checkout] : inventory.projects) {
      if (!checkout.audit.has_value()) {
        if (verbose) {
          printUnavailableCheckout(project, checkout);
        } else {
          rows.push_back({project, PairedStatus::State::kProblem, "! missing", checkout.problem,
                          checkout.path.string(), true});
        }
        continue;
      }
      const PairedStatus status = collectPairedStatus(*checkout.audit, config);
      attention = status.attention || attention;
      if (verbose) {
        std::cout << "Main checkout on this device" << (checkout.legacy ? " (legacy registration)" : "") << ":\n";
        printPairedStatusBlock(status);
      } else {
        StatusTableRow row = statusTableRow(status, project);
        if (checkout.legacy) row.checkout += " (legacy registration)";
        rows.push_back(std::move(row));
      }
    }
    printStatusTable(rows);
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
  // A single requested repository is always shown in full.
  const bool detailed = verbose || repository_path.has_value();
  std::vector<StatusTableRow> rows;
  for (const auto& path : paths) {
    try {
      const auto audit = repository_path.has_value() ? ckgit::inspectRepository(path)
                                                   : ckgit::inspectDiscoveredRepository(path);
      const PairedStatus status = collectPairedStatus(audit, config);
      attention = status.attention || attention;
      if (detailed) {
        printPairedStatusBlock(status);
      } else {
        rows.push_back(statusTableRow(status, audit.path.filename().string()));
      }
    } catch (const std::exception& error) {
      std::cerr << "ckgit: " << path.string() << ": " << error.what() << "\n";
      attention = true;
    }
  }
  printStatusTable(rows);
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
  const auto config = loadConfiguredClient(resolveConfigPath(config_path));
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
    return kCloneSkipped;
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
  const auto config = loadConfiguredClient(resolveConfigPath(config_path));
  const auto result = controlRpc(config, "create", {*project_name, branch});
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
    const auto create_result = controlRpc(config, "create", {project, default_branch});
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
  const auto config = loadConfiguredClient(resolveConfigPath(config_path));
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
    const auto result = controlRpc(config, replace ? "replace-checkout" : "register",
                                   {std::string(project), encoded_path});
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
  const auto config = loadConfiguredClient(resolveConfigPath(config_path));
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
  const auto config = loadConfiguredClient(resolveConfigPath(config_path));
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
  const auto config = loadConfiguredClient(resolveConfigPath(config_path));
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

class DoctorSignals {
 public:
  DoctorSignals() {
    tunnel_stop_requested = 0;
#ifdef _WIN32
    interrupt_ = std::signal(SIGINT, requestTunnelStop);
    terminate_ = std::signal(SIGTERM, requestTunnelStop);
#else
    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_handler = requestTunnelStop;
    sigaction(SIGINT, &action, &interrupt_); sigaction(SIGTERM, &action, &terminate_);
    sigaction(SIGHUP, &action, &hangup_);
    action.sa_handler = SIG_IGN; sigaction(SIGPIPE, &action, &pipe_);
#endif
  }
  ~DoctorSignals() {
#ifdef _WIN32
    std::signal(SIGINT, interrupt_); std::signal(SIGTERM, terminate_);
#else
    sigaction(SIGINT, &interrupt_, nullptr); sigaction(SIGTERM, &terminate_, nullptr);
    sigaction(SIGHUP, &hangup_, nullptr); sigaction(SIGPIPE, &pipe_, nullptr);
#endif
  }
 private:
#ifdef _WIN32
  using Handler = void (*)(int);
  Handler interrupt_{}, terminate_{};
#else
  struct sigaction interrupt_{}, terminate_{}, hangup_{}, pipe_{};
#endif
};

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

net::Socket loopbackSocket() {
  const auto descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor == net::invalid) {
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
  const auto descriptor = loopbackSocket();
  net::reuse(descriptor);
  const auto address = loopbackAddress(port);
  const bool free_port = bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
  net::close(descriptor);
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
  const auto descriptor = loopbackSocket();
  net::reuse(descriptor);
  auto address = loopbackAddress(0);
  net::Length size = sizeof(address);
  if (bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
    net::close(descriptor);
    throw std::runtime_error("could not allocate a free local port");
  }
  net::close(descriptor);
  return ntohs(address.sin_port);
}

net::Socket connectLoopback(unsigned short port, int timeout_milliseconds) {
  const auto descriptor = loopbackSocket();
  if (!net::nonblocking(descriptor, true)) { net::close(descriptor); return net::invalid; }
  const auto address = loopbackAddress(port);
  if (connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    if (!net::connecting()) {
      net::close(descriptor);
      return net::invalid;
    }
    net::Poll ready{descriptor, POLLOUT, 0};
    if (net::poll(&ready, 1, timeout_milliseconds) <= 0 || !net::connected(descriptor)) {
      net::close(descriptor);
      return net::invalid;
    }
  }
  if (!net::nonblocking(descriptor, false)) { net::close(descriptor); return net::invalid; }
  return descriptor;
}

// Sends one bounded HEAD request through the tunnel and returns the status
// line, so "tunnel up, dashboard disabled" is reported instead of a blank page.
std::optional<std::string> probeDashboard(unsigned short port) {
  const auto descriptor = connectLoopback(port, 3000);
  if (descriptor == net::invalid) {
    return std::nullopt;
  }
  net::timeout(descriptor, 3);
  const std::string request = "HEAD / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  if (net::write(descriptor, request.data(), request.size()) != static_cast<std::ptrdiff_t>(request.size())) {
    net::close(descriptor);
    return std::nullopt;
  }
  std::string reply;
  char buffer[256];
  while (reply.find("\r\n") == std::string::npos && reply.size() < 1024) {
    const auto received = net::read(descriptor, buffer, sizeof(buffer));
    if (received <= 0) {
      break;
    }
    reply.append(buffer, static_cast<std::size_t>(received));
  }
  net::close(descriptor);
  const std::size_t end = reply.find("\r\n");
  if (end == std::string::npos) {
    return std::nullopt;
  }
  return reply.substr(0, end);
}

bool childHasExited(ckgit::ManagedProcess& child, int* status) {
  const auto result = child.poll();
  if (!result) return false;
  *status = *result;
  return true;
}
int childExitCode(int status) { return status; }

// The opener may stay in the foreground for as long as the browser runs on
// some Linux desktops, so it is never waited for or killed; it is reaped
// alongside the tunnel loop.
int openBrowser(const std::string& url) {
#ifdef _WIN32
  if (!url.starts_with("http://") && !url.starts_with("https://")) throw std::invalid_argument("unsupported browser URL");
  const auto result = ShellExecuteW(nullptr, L"open", ckgit::utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<std::intptr_t>(result) <= 32) std::cerr << "ckgit: no browser opener available; visit the URL manually\n";
  return -1;
#else
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
#endif
}

// `page`, when given, is the dashboard path to open (ckgit open) instead of
// the overview or --project.
int webCommand(const std::vector<std::string>& arguments, const std::string& page = {}) {
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
    const auto config = loadConfiguredClient(resolveConfigPath(config_path));
    if (!admin_host.has_value()) {
      admin_host = config.web_host.empty() ? config.server.substr(config.server.find('@') + 1) : config.web_host;
    }
    if (!remote_port_explicit) remote_port = config.web_port;
  }
  const unsigned short port = pickLocalPort(local_port);
  const std::string url = "http://127.0.0.1:" + std::to_string(port) +
      (!page.empty() ? page : project.has_value() ? "/project/" + *project : "/");
  const std::string forward = "127.0.0.1:" + std::to_string(port) + ":127.0.0.1:" + std::to_string(remote_port);

  const DoctorSignals signals;
  std::cout << "Opening tunnel to " << *admin_host << " ...\n" << std::flush;
  ckgit::ManagedProcess child(
      {"ssh", "-N", "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes", "-o", "RequestTTY=no",
       "-o", "ServerAliveInterval=30", "-L", forward, *admin_host});
  int status = 0;
  const auto stopChild = [&]() {
    if (!childHasExited(child, &status)) {
      child.stop();
    }
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  bool listening = false;
  while (!listening && !tunnel_stop_requested) {
    if (childHasExited(child, &status)) {
      std::cerr << "ckgit: ssh exited before the tunnel was ready\n";
      return childExitCode(status) == 0 ? 1 : childExitCode(status);
    }
    const auto probe = connectLoopback(port, 250);
    if (probe != net::invalid) {
      net::close(probe);
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
#ifndef _WIN32
      if (waitpid(opener, &opener_status, WNOHANG) == opener) {
        opener = -1;
      }
#else
      (void)opener_status;
#endif
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

// Extracted from web's ssh -N -L start/wait/stop: opens a loopback tunnel to
// the paired server's dashboard through the same ordinary SSH login web uses
// (the restricted ckgit Git account allows no forwarding), waits for it to
// come up, calls fn(local_port), then always closes the tunnel, including
// when fn throws or Ctrl+C interrupts the wait. Unlike web, this is a single
// bounded operation: no browser is opened and nothing loops after fn returns.
template <typename Fn>
void withDashboardTunnel(const ckgit::ClientConfig& config, Fn&& fn) {
  const std::string admin_host =
      config.web_host.empty() ? config.server.substr(config.server.find('@') + 1) : config.web_host;
  const unsigned short remote_port = config.web_port;
  const unsigned short port = pickLocalPort(std::nullopt);

  const DoctorSignals signals;
  ckgit::ManagedProcess child(
      {"ssh", "-N", "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes", "-o", "RequestTTY=no",
       "-o", "ServerAliveInterval=30", "-L",
       "127.0.0.1:" + std::to_string(port) + ":127.0.0.1:" + std::to_string(remote_port), admin_host});
  int status = 0;
  const auto stopChild = [&]() {
    if (!childHasExited(child, &status)) {
      child.stop();
    }
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  bool listening = false;
  while (!listening && !tunnel_stop_requested) {
    if (childHasExited(child, &status)) {
      throw std::runtime_error("ssh exited before the tunnel to " + admin_host + " was ready");
    }
    const auto probe = connectLoopback(port, 250);
    if (probe != net::invalid) {
      net::close(probe);
      listening = true;
      break;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      stopChild();
      throw std::runtime_error("the tunnel to " + admin_host + " did not become ready within 20 seconds");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (tunnel_stop_requested) {
    stopChild();
    throw std::runtime_error("interrupted");
  }
  const auto status_line = probeDashboard(port);
  if (!status_line.has_value() || status_line->rfind("HTTP/1.1 200", 0) != 0) {
    stopChild();
    throw std::runtime_error(
        "the tunnel is up but the dashboard did not answer on the server's port " + std::to_string(remote_port) +
        "; set http_port in /etc/ck-git-hosting/server.ini and restart the service");
  }
  try {
    fn(port);
  } catch (...) {
    stopChild();
    throw;
  }
  stopChild();
}

struct DashboardTarget {
  std::string host;
  unsigned short port;
};

// Parses the hidden --dashboard-url test seam: an http origin only, no path.
DashboardTarget parseDashboardUrl(const std::string& url) {
  constexpr std::string_view scheme{"http://"};
  if (url.rfind(scheme, 0) != 0) {
    throw std::runtime_error("--dashboard-url must start with http://");
  }
  const std::string_view rest(url.data() + scheme.size(), url.size() - scheme.size());
  if (rest.find('/') != std::string_view::npos) {
    throw std::runtime_error("--dashboard-url must be an origin only, for example http://127.0.0.1:8420");
  }
  const auto colon = rest.rfind(':');
  if (colon == std::string_view::npos) {
    throw std::runtime_error("--dashboard-url must include a port, for example http://127.0.0.1:8420");
  }
  const std::string host(rest.substr(0, colon));
  const std::string_view port_text = rest.substr(colon + 1);
  unsigned int port = 0;
  const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
  if (error != std::errc{} || end != port_text.data() + port_text.size() || port == 0 || port > 65535 ||
      host.empty()) {
    throw std::runtime_error("--dashboard-url has an invalid host or port");
  }
  return DashboardTarget{host, static_cast<unsigned short>(port)};
}

// Fetches `target` from host:port over one plain HTTP/1.1 request and returns
// the body, which must be exactly `expected_bytes` long -- the caller already
// knows the size from the release listing, so this doubles as a
// transfer-integrity check without needing to parse Content-Length.
std::string fetchHttpBody(const std::string& host, unsigned short port, const std::string& target,
                          std::uint64_t expected_bytes) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* resolved = nullptr;
  const int resolve_error = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &resolved);
  if (resolve_error != 0 || resolved == nullptr) {
    throw std::runtime_error("could not resolve " + host + ": " +
                             (resolve_error != 0 ? gai_strerror(resolve_error) : "no address"));
  }
  net::Socket descriptor = net::invalid;
  for (const addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
    descriptor = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (descriptor == net::invalid) continue;
    if (connect(descriptor, candidate->ai_addr, static_cast<net::Length>(candidate->ai_addrlen)) == 0) break;
    net::close(descriptor);
    descriptor = net::invalid;
  }
  freeaddrinfo(resolved);
  if (descriptor == net::invalid) {
    throw std::runtime_error("could not connect to " + host + ":" + std::to_string(port));
  }
  net::timeout(descriptor, 30);
  const std::string request = "GET " + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
  std::size_t sent = 0;
  while (sent < request.size()) {
    const auto written = net::write(descriptor, request.data() + sent, request.size() - sent);
    if (written <= 0) {
      net::close(descriptor);
      throw std::runtime_error("could not send the request to " + host);
    }
    sent += static_cast<std::size_t>(written);
  }

  std::string buffer;
  char chunk[8192];
  std::size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    const auto received = net::read(descriptor, chunk, sizeof(chunk));
    if (received <= 0) {
      net::close(descriptor);
      throw std::runtime_error("connection closed before the response headers arrived");
    }
    buffer.append(chunk, static_cast<std::size_t>(received));
    if (buffer.size() > 16384) {
      net::close(descriptor);
      throw std::runtime_error("response headers exceed the expected size");
    }
    header_end = buffer.find("\r\n\r\n");
  }
  const std::string_view status_line(buffer.data(), buffer.find("\r\n"));
  if (status_line.rfind("HTTP/1.1 200", 0) != 0) {
    net::close(descriptor);
    throw std::runtime_error("server returned " + std::string(status_line));
  }

  std::string body = buffer.substr(header_end + 4);
  while (body.size() < expected_bytes) {
    const auto received = net::read(descriptor, chunk, sizeof(chunk));
    if (received < 0) {
      net::close(descriptor);
      throw std::runtime_error("could not read the response body");
    }
    if (received == 0) break;
    if (body.size() + static_cast<std::size_t>(received) > expected_bytes) {
      net::close(descriptor);
      throw std::runtime_error("asset is larger than the size listed for it");
    }
    body.append(chunk, static_cast<std::size_t>(received));
  }
  net::close(descriptor);
  if (body.size() != expected_bytes) {
    throw std::runtime_error("asset size mismatch: expected " + std::to_string(expected_bytes) +
                             " bytes, got " + std::to_string(body.size()));
  }
  return body;
}

// Fetches one dashboard download of exactly `bytes` bytes: from the hidden
// --dashboard-url test seam when given, otherwise through a short-lived tunnel
// over the administrator's ordinary SSH login, as `ckgit web` opens it.
std::string fetchDashboardDownload(const ckgit::ClientConfig& config, const std::optional<std::string>& dashboard_url,
                                   const std::string& target, std::uint64_t bytes) {
  if (dashboard_url.has_value()) {
    const auto url_target = parseDashboardUrl(*dashboard_url);
    return fetchHttpBody(url_target.host, url_target.port, target, bytes);
  }
  std::string body;
  withDashboardTunnel(config, [&](unsigned short port) { body = fetchHttpBody("127.0.0.1", port, target, bytes); });
  return body;
}

// Checks a downloaded bundle against the sha256 its listing recorded, then
// installs it as into_dir/file_name through a staged rename, so a failed or
// tampered transfer never replaces anything. An empty expected digest (a
// bundle recorded before checksums) installs with a warning. Returns the
// installed path, or std::nullopt after reporting why nothing was written.
std::optional<std::filesystem::path> installVerifiedDownload(const std::filesystem::path& into_dir,
                                                             const std::string& file_name, const std::string& body,
                                                             const std::string& expected_sha256,
                                                             const std::string& label) {
  const std::string digest = ckgit::sha256Hex(body);
  if (!expected_sha256.empty() && digest != expected_sha256) {
    std::cerr << "ckgit: downloaded " << label << " sha256 mismatch (expected " << expected_sha256 << ", got "
              << digest << "); refusing to write it\n";
    return std::nullopt;
  }
  if (expected_sha256.empty()) {
    std::cerr << "ckgit: warning: " << label << " has no recorded sha256; downloaded content was not verified\n";
  }
  const std::filesystem::path destination = into_dir / file_name;
  const std::filesystem::path staging = into_dir / ("." + file_name + ".partial");
  {
    std::ofstream out(staging, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::cerr << "ckgit: could not create " << staging.string() << "\n";
      return std::nullopt;
    }
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!out) {
      std::error_code ignore;
      std::filesystem::remove(staging, ignore);
      std::cerr << "ckgit: could not write " << staging.string() << "\n";
      return std::nullopt;
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(staging, destination, rename_error);
  if (rename_error) {
    std::error_code ignore;
    std::filesystem::remove(staging, ignore);
    std::cerr << "ckgit: could not install " << destination.string() << ": " << rename_error.message() << "\n";
    return std::nullopt;
  }
  return destination;
}

// `ckgit release list PROJECT`: the releases control response as text or,
// with --json, a versioned report like `projects --json`.
int releaseListCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  bool json = false;
  std::string project;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto& argument = arguments[index];
    if (argument == "--config" && index + 1 < arguments.size()) {
      config_path = arguments[++index];
    } else if (argument == "--json") {
      json = true;
    } else {
      project = argument;
    }
  }
  const auto config = loadConfiguredClient(resolveConfigPath(config_path));
  std::vector<ckgit::CiReleaseRecord> releases;
  try {
    releases = fetchReleases(config, project);
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << error.what() << "\n";
    return 1;
  }
  if (json) {
    std::cout << "{\"schema_version\":1,\"releases\":[";
    for (std::size_t index = 0; index < releases.size(); ++index) {
      if (index != 0) std::cout << ",";
      const auto& release = releases[index];
      std::cout << "\n    {\"tag\":\"" << jsonEscape(release.tag) << "\",\"commit\":\""
                << jsonEscape(release.commit_id) << "\",\"created_epoch\":" << release.created_epoch_seconds
                << ",\"assets\":[";
      for (std::size_t asset_index = 0; asset_index < release.assets.size(); ++asset_index) {
        if (asset_index != 0) std::cout << ",";
        const auto& asset = release.assets[asset_index];
        std::cout << "{\"name\":\"" << jsonEscape(asset.name) << "\",\"bytes\":" << asset.bytes
                  << ",\"sha256\":\"" << jsonEscape(asset.sha256) << "\"}";
      }
      std::cout << "]}";
    }
    std::cout << "\n  ]}\n";
    return 0;
  }
  if (releases.empty()) {
    std::cout << "No releases for " << project << ".\n";
    return 0;
  }
  for (const auto& release : releases) {
    std::cout << release.tag << "  " << release.commit_id.substr(0, 12) << "  "
              << ckgit::formatUtcTimestamp(release.created_epoch_seconds) << "\n";
    for (const auto& asset : release.assets) {
      std::cout << "  " << asset.name << "  " << asset.bytes
                << " bytes  sha256:" << (asset.sha256.empty() ? "unknown" : asset.sha256) << "\n";
    }
  }
  return 0;
}

// `ckgit release download PROJECT`: lists releases over SSH like list, then
// fetches one asset through the dashboard tunnel and verifies it before
// installing it, so a failed or tampered transfer never replaces anything.
int releaseDownloadCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::string project;
  std::optional<std::string> tag_filter;
  std::optional<std::string> asset_filter;
  std::filesystem::path into_dir = ".";
  std::optional<std::string> dashboard_url;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto& argument = arguments[index];
    if ((argument == "--config" || argument == "--tag" || argument == "--asset" || argument == "--into" ||
         argument == "--dashboard-url") &&
        index + 1 < arguments.size()) {
      const std::string value = arguments[++index];
      if (argument == "--config") config_path = value;
      else if (argument == "--tag") tag_filter = value;
      else if (argument == "--asset") asset_filter = value;
      else if (argument == "--into") into_dir = value;
      else dashboard_url = value;
    } else {
      project = argument;
    }
  }
  const auto config = loadConfiguredClient(resolveConfigPath(config_path));
  std::vector<ckgit::CiReleaseRecord> releases;
  try {
    releases = fetchReleases(config, project);
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << error.what() << "\n";
    return 1;
  }
  if (releases.empty()) {
    std::cerr << "ckgit: " << project << " has no releases\n";
    return 1;
  }
  const ckgit::CiReleaseRecord* release = nullptr;
  if (tag_filter.has_value()) {
    const auto found = std::find_if(releases.begin(), releases.end(), [&](const ckgit::CiReleaseRecord& candidate) {
      return candidate.tag == *tag_filter;
    });
    if (found == releases.end()) {
      std::cerr << "ckgit: " << project << " has no release tagged " << *tag_filter << "\n";
      return 1;
    }
    release = &*found;
  } else {
    release = &releases.front();
  }
  const ckgit::CiArtifactRecord* asset = nullptr;
  if (asset_filter.has_value()) {
    const auto found =
        std::find_if(release->assets.begin(), release->assets.end(), [&](const ckgit::CiArtifactRecord& candidate) {
          return candidate.name == *asset_filter;
        });
    if (found == release->assets.end()) {
      std::cerr << "ckgit: release " << release->tag << " has no asset named " << *asset_filter << "\n";
      return 1;
    }
    asset = &*found;
  } else if (release->assets.size() == 1) {
    asset = &release->assets.front();
  } else {
    std::cerr << "ckgit: release " << release->tag << " has " << release->assets.size()
              << " assets; choose one with --asset:";
    for (const auto& candidate : release->assets) std::cerr << " " << candidate.name;
    std::cerr << "\n";
    return 1;
  }
  std::error_code directory_error;
  if (!std::filesystem::is_directory(into_dir, directory_error)) {
    std::cerr << "ckgit: --into " << into_dir.string() << " is not a directory\n";
    return 1;
  }
  const std::string target = "/project/" + project + "/releases/" + release->tag + "/" + asset->name;
  std::string body;
  try {
    body = fetchDashboardDownload(config, dashboard_url, target, asset->bytes);
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << error.what() << "\n";
    return 1;
  }
  const auto destination = installVerifiedDownload(into_dir, asset->name + ".tar", body, asset->sha256,
                                                   "release " + release->tag + " asset " + asset->name);
  if (!destination.has_value()) return 1;
  std::cout << "Downloaded " << release->tag << "/" << asset->name << " to " << destination->string() << " ("
            << body.size() << " bytes, sha256 " << (asset->sha256.empty() ? "unverified" : "verified") << ").\n";
  return 0;
}

#include "discovery.inc"
#include "setup_doctor.inc"
#include "incoming.inc"
#include "ci.inc"
#include "default_branch.inc"
#include "open.inc"

// `ckgit version`: the local build plus the versions running on the server, so
// the operator sees all four suite versions at once and can spot a service left
// on an older build. Degrades to the local build alone when no server is
// configured or reachable; `ckgit --version` prints only the local build.
int versionCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> requested;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto& option = arguments[index];
    if (option == "--config") {
      if (index + 1 == arguments.size()) { std::cerr << "ckgit: --config requires a value\n"; return kUsage; }
      requested = arguments[++index];
    } else {
      std::cerr << "ckgit: unknown version option: " << option << "\nTry 'ckgit help version'.\n";
      return kUsage;
    }
  }
  std::cout << "ckgit " << ckgit::buildVersion() << "  (local CLI)\n";
  std::optional<ckgit::ClientConfig> config;
  try {
    config = loadConfiguredClient(resolveConfigPath(requested));
  } catch (const std::exception&) {
    std::cout << "server: not configured; run ckgit setup to query the host and service versions\n";
    return 0;
  }
  const auto result = controlRpc(*config, "versions");
  static constexpr std::string_view kHeader = "ok versions\n";
  if (result.exit_code != 0 || result.timed_out || result.output_truncated ||
      result.output.rfind(kHeader.data(), 0, kHeader.size()) != 0) {
    std::cout << "server " << config->server << ": could not read versions ("
              << describeProcessFailure(result, kControlTimeout) << ")\n";
    return 0;
  }
  std::cout << "server " << config->server << ":\n";
  const std::string& body = result.output;
  for (std::size_t pos = kHeader.size(); pos < body.size();) {
    const std::size_t eol = body.find('\n', pos);
    const std::string line = body.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    pos = eol == std::string::npos ? body.size() : eol + 1;
    if (line.empty()) continue;
    std::vector<std::string> fields;  // name, version, status (started is ignored)
    for (std::size_t start = 0; fields.size() < 3 && start <= line.size();) {
      const std::size_t space = line.find(' ', start);
      if (space == std::string::npos) { fields.push_back(line.substr(start)); break; }
      fields.push_back(line.substr(start, space - start));
      start = space + 1;
    }
    const std::string name = fields.size() > 0 ? fields[0] : "";
    const std::string version = fields.size() > 1 ? fields[1] : "";
    const std::string status = fields.size() > 2 ? fields[2] : "";
    if (name.empty()) continue;
    std::string label = name;
    if (label.size() < 18) label.append(18 - label.size(), ' ');
    std::cout << "  " << label << (version.empty() ? "unknown" : version)
              << (status.empty() || status == "running" ? "" : "  (" + status + ")") << "\n";
  }
  return 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
  try {
    [[maybe_unused]] const ckgit::net::Runtime network;
    ckgit::installChildTerminationForwarding();
    std::vector<std::string> raw_arguments;
    for (int index = 1; index < argc; ++index) {
#ifdef _WIN32
      raw_arguments.push_back(ckgit::wideToUtf8(argv[index]));
#else
      raw_arguments.emplace_back(argv[index]);
#endif
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
    if (command == "open") {
      return openCommand(arguments);
    }
    if (command == "release") {
      if (!arguments.empty() && arguments.front() == "list") {
        return releaseListCommand(std::vector<std::string>(arguments.begin() + 1, arguments.end()));
      }
      if (!arguments.empty() && arguments.front() == "download") {
        return releaseDownloadCommand(std::vector<std::string>(arguments.begin() + 1, arguments.end()));
      }
      printUsage(std::cerr);
      return kUsage;
    }
    if (command == "ci") {
      return ciCommand(arguments);
    }
    if (command == "default-branch") {
      return defaultBranchCommand(arguments);
    }
    if (command == "version") {
      return versionCommand(arguments);
    }
    printUsage(std::cerr);
    return kUsage;
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << error.what() << "\n";
    return 1;
  }
}
