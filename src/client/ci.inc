// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

// Included inside main.cpp's anonymous namespace, after the shared client
// helpers. `ckgit ci`: read-only CI inspection and run cancellation over the
// restricted SSH control channel (docs/protocol/01-ssh-and-control-v1.md), and
// offline workflow linting. Enabling CI, reruns, triggers, and secrets stay
// with ckgit-admin on the server.

constexpr std::size_t kCiListDefaultLimit = 20;
constexpr std::chrono::seconds kCiWatchDefaultInterval{5};
constexpr std::chrono::seconds kCiLogFollowInterval{2};
// A watch survives this many consecutive failed polls (a brief network or
// server hiccup) before it gives up.
constexpr int kCiWatchPollAttempts = 3;

// The options and positionals of one `ckgit ci` subcommand, already validated
// and normalized by the shared command schema.
struct CiOptions {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::string> project;
  std::optional<std::string> rev;
  std::optional<std::string> dashboard_url;
  std::filesystem::path into{"."};
  std::size_t limit{kCiListDefaultLimit};
  std::chrono::seconds interval{kCiWatchDefaultInterval};
  bool all{false};
  bool json{false};
  bool follow{false};
  bool yes{false};
  bool dry_run{false};
  std::vector<std::string> positional;
};

// A number the command schema has already checked, including its range.
std::uint64_t schemaNumber(const std::string& text) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) throw std::invalid_argument("invalid number " + text);
  return value;
}

CiOptions parseCiOptions(const std::vector<std::string>& arguments) {
  CiOptions options;
  bool positional_only = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string& argument = arguments[index];
    const bool has_value = index + 1 < arguments.size();
    if (positional_only) {
      options.positional.push_back(argument);
    } else if (argument == "--") {
      positional_only = true;
    } else if (argument == "--config" && has_value) {
      options.config_path = arguments[++index];
    } else if (argument == "--project" && has_value) {
      options.project = arguments[++index];
    } else if (argument == "--rev" && has_value) {
      options.rev = arguments[++index];
    } else if (argument == "--dashboard-url" && has_value) {
      options.dashboard_url = arguments[++index];
    } else if (argument == "--into" && has_value) {
      options.into = arguments[++index];
    } else if (argument == "--limit" && has_value) {
      options.limit = static_cast<std::size_t>(schemaNumber(arguments[++index]));
    } else if (argument == "--interval" && has_value) {
      options.interval = std::chrono::seconds(schemaNumber(arguments[++index]));
    } else if (argument == "--all") {
      options.all = true;
    } else if (argument == "--json") {
      options.json = true;
    } else if (argument == "--follow") {
      options.follow = true;
    } else if (argument == "--yes") {
      options.yes = true;
    } else if (argument == "--dry-run") {
      options.dry_run = true;
    } else {
      options.positional.push_back(argument);
    }
  }
  return options;
}

// A failure the server reported as `error <code> <message>`.
class CiReplyError : public std::runtime_error {
 public:
  CiReplyError(std::string code, const std::string& message) : std::runtime_error(message), code_(std::move(code)) {}
  const std::string& code() const { return code_; }

 private:
  std::string code_;
};

// Runs one CI control operation and returns its `ok` reply. A daemon error is
// raised as CiReplyError; a refusal by an older server's SSH dispatcher, which
// does not know the operation, says so instead of an opaque denial.
std::string ciControlReply(const ckgit::ClientConfig& config, std::string_view operation,
                           const std::vector<std::string>& arguments) {
  const auto result = controlRpc(config, operation, arguments);
  if (result.exit_code == 0 && !result.timed_out && !result.output_truncated) return result.output;
  if (!result.timed_out && !result.output_truncated && result.output.rfind("error ", 0) == 0) {
    const std::string line = result.output.substr(0, result.output.find('\n'));
    const std::size_t space = line.find(' ', 6);
    throw CiReplyError(line.substr(6, space == std::string::npos ? std::string::npos : space - 6),
                       space == std::string::npos ? line : line.substr(space + 1));
  }
  if (result.output.find("denied: command is not an allowed") != std::string::npos) {
    throw std::runtime_error("the server at " + config.server + " does not offer " + std::string(operation) +
                             "; upgrade ck-git-hosting on the server to this client's version");
  }
  throw std::runtime_error("could not query CI on " + config.server + " (" +
                           describeProcessFailure(result, kControlTimeout) + ")");
}

// Server-supplied text (refs, details, step names, notes) reaches a terminal:
// control characters, C1 controls, and the non-ASCII bytes of text that is not
// valid UTF-8 are shown as '?', so no escape sequence is passed through.
std::string terminalText(std::string_view text) {
  const bool utf8 = ckgit::isValidUtf8(text);
  std::string shown;
  shown.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const auto byte = static_cast<unsigned char>(text[index]);
    const bool c1_control = utf8 && byte == 0xc2 && index + 1 < text.size() &&
                            static_cast<unsigned char>(text[index + 1]) <= 0x9f;
    if (c1_control) {
      shown += '?';
      ++index;
    } else if (byte < 0x20 || byte == 0x7f || (!utf8 && byte >= 0x80)) {
      shown += '?';
    } else {
      shown += static_cast<char>(byte);
    }
  }
  return shown;
}

// The same text as a JSON string body: escaped, and valid UTF-8.
std::string jsonText(std::string_view text) {
  if (ckgit::isValidUtf8(text)) return jsonEscape(std::string(text));
  std::string ascii(text);
  for (auto& character : ascii) {
    if (static_cast<unsigned char>(character) >= 0x80) character = '?';
  }
  return jsonEscape(ascii);
}

std::string ciRefLabel(const std::string& ref) {
  if (ref.rfind("refs/heads/", 0) == 0) return ref.substr(11);
  if (ref.rfind("refs/tags/", 0) == 0) return ref.substr(10);
  return ref;
}

std::string ciStepLabel(const ckgit::CiStepResult& step, std::size_t index) {
  return step.name.empty() ? "step " + std::to_string(index) : terminalText(step.name);
}

std::string ciStepOutcome(const ckgit::CiStepResult& step) {
  return (step.timed_out ? std::string("timed out") : "exit " + std::to_string(step.exit_code)) +
         (step.output_truncated ? ", log truncated" : "");
}

std::string_view ciStepIcon(const ckgit::CiStepResult& step) {
  if (step.timed_out) return ckgit::ciRunStatusIcon(ckgit::CiRunStatus::Timeout);
  return ckgit::ciRunStatusIcon(step.exit_code == 0 ? ckgit::CiRunStatus::Success : ckgit::CiRunStatus::Failure);
}

std::string padded(std::string text, std::size_t width) {
  if (text.size() < width) text.append(width - text.size(), ' ');
  return text;
}

// Selects the hosted project: --project, or the project the current
// repository's configured remote pairs with. std::nullopt after explaining
// what to pass instead.
std::optional<std::string> resolveCiProject(const ckgit::ClientConfig& config, const CiOptions& options) {
  if (options.project.has_value()) return options.project;
  std::optional<ckgit::RepositoryAudit> audit;
  try {
    audit = ckgit::inspectRepository(".");
  } catch (const std::exception&) {
    std::cerr << "ckgit: not inside a Git repository; pass --project NAME\n";
    return std::nullopt;
  }
  bool invalid = false;
  const auto project = pairedProjectForAudit(*audit, config, &invalid);
  if (!project.has_value()) {
    std::cerr << "ckgit: " << audit->path.string() << " is not paired with a project on " << config.server
              << (invalid ? " (its " + config.remote_name + " remote is unsafe or ambiguous)" : "")
              << "; pass --project NAME\n";
  }
  return project;
}

// Explains a CI reply error in terms of the request and returns exit 1.
int reportCiReplyError(const CiReplyError& error, const std::string& project, const std::string& run) {
  if (error.code() == "noproject") {
    std::cerr << "ckgit: " << project << " is not a hosted project\n";
  } else if (error.code() == "norun") {
    std::cerr << "ckgit: " << project << " has no CI run " << run << "\n";
  } else {
    std::cerr << "ckgit: the server refused the request: " << terminalText(error.what()) << "\n";
  }
  return 1;
}

ckgit::CiStatusReport fetchCiStatus(const ckgit::ClientConfig& config, const std::string& project) {
  auto report = ckgit::parseCiStatusControlResponse(ciControlReply(config, "ci-status", {project}));
  if (report.projects.size() != 1 || report.projects.front().project_name != project) {
    throw std::runtime_error("CI status response names a different project");
  }
  return report;
}

ckgit::CiRunReport fetchCiRun(const ckgit::ClientConfig& config, const std::string& project,
                              const std::string& run_id) {
  auto report = ckgit::parseCiRunControlResponse(ciControlReply(config, "ci-run", {project, run_id}), project);
  if (report.run.run_id != run_id) throw std::runtime_error("CI run response describes a different run");
  return report;
}

// --- text and JSON renderings ------------------------------------------------

std::string ciRunJson(const ckgit::CiRunRecord& run, std::size_t completed_steps, std::uint64_t now) {
  const auto display = ckgit::ciRunDisplayAt(run, now);
  return "{\"run_id\":\"" + run.run_id + "\",\"status\":\"" + std::string(ckgit::ciRunStatusName(run.status)) +
         "\",\"display_status\":\"" + std::string(display.name) + "\",\"active\":" +
         (display.active ? "true" : "false") + ",\"ref\":\"" + jsonText(run.ref) + "\",\"commit\":\"" +
         run.commit_id + "\",\"started_epoch\":" + std::to_string(run.started_epoch_seconds) +
         ",\"finished_epoch\":" + std::to_string(run.finished_epoch_seconds) +
         ",\"heartbeat_epoch\":" + std::to_string(run.heartbeat_epoch_seconds) +
         ",\"duration_seconds\":" + std::to_string(display.seconds) +
         ",\"completed_steps\":" + std::to_string(completed_steps) + "}";
}

// One run as a status or list line: icon and display status, ref, commit,
// start, timing, and id, judged at the server's time `now`.
std::string ciRunLine(const ckgit::CiRunRecord& run, std::size_t completed_steps, std::uint64_t now) {
  const auto display = ckgit::ciRunDisplayAt(run, now);
  const std::string timing = ckgit::ciRunTiming(display);
  const std::string started =
      run.started_epoch_seconds != 0 ? ckgit::relativeTimeAt(run.started_epoch_seconds, now) : "-";
  std::string line = std::string(display.icon) + " " + padded(std::string(display.name), 12) +
                     padded(terminalText(ciRefLabel(run.ref)), 18) + padded(run.commit_id.substr(0, 12), 14) +
                     padded(started, 17) + padded(timing.empty() ? "-" : timing, 12) + run.run_id;
  if (display.name == "running") line += "  (step " + std::to_string(completed_steps) + " running)";
  return line;
}

// The runs `ci status` reports: the newest, and the newest still live (pending,
// or running with a fresh heartbeat), which may be the same run.
std::pair<const ckgit::CiRunSummary*, const ckgit::CiRunSummary*> ciLatestAndActive(
    const ckgit::CiProjectStatus& project, std::uint64_t now) {
  const ckgit::CiRunSummary* latest = project.runs.empty() ? nullptr : &project.runs.front();
  const auto active = std::find_if(project.runs.begin(), project.runs.end(), [&](const ckgit::CiRunSummary& summary) {
    return ckgit::ciRunDisplayAt(summary.run, now).active;
  });
  return {latest, active == project.runs.end() ? nullptr : &*active};
}

int ciStatusCommand(const ckgit::ClientConfig& config, const CiOptions& options) {
  ckgit::CiStatusReport report;
  std::string project;
  if (!options.all) {
    const auto selected = resolveCiProject(config, options);
    if (!selected.has_value()) return kUsage;
    project = *selected;
  }
  try {
    report = options.all ? ckgit::parseCiStatusControlResponse(ciControlReply(config, "ci-overview", {}))
                         : fetchCiStatus(config, project);
  } catch (const CiReplyError& error) {
    return reportCiReplyError(error, project, {});
  }
  const std::uint64_t now = report.server_epoch_seconds;
  if (options.json) {
    std::cout << "{\"schema_version\":1,\"server_epoch\":" << now << ",\"projects\":[";
    for (std::size_t index = 0; index < report.projects.size(); ++index) {
      const auto& status = report.projects[index];
      const auto [latest, active] = ciLatestAndActive(status, now);
      std::cout << (index == 0 ? "" : ",") << "\n    {\"name\":\"" << status.project_name
                << "\",\"ci_enabled\":" << (status.ci_enabled ? "true" : "false") << ",\"latest\":"
                << (latest ? ciRunJson(latest->run, latest->completed_steps, now) : "null") << ",\"active\":"
                << (active ? ciRunJson(active->run, active->completed_steps, now) : "null") << "}";
    }
    std::cout << "\n  ]}\n";
    return 0;
  }
  for (const auto& status : report.projects) {
    std::cout << status.project_name << ": CI " << (status.ci_enabled ? "enabled" : "disabled")
              << (status.ci_enabled || options.all ? "" : " (an administrator enables it with ckgit-admin on the server)")
              << "\n";
    const auto [latest, active] = ciLatestAndActive(status, now);
    if (latest == nullptr) {
      std::cout << "  no runs recorded\n";
      continue;
    }
    std::cout << "  latest  " << ciRunLine(latest->run, latest->completed_steps, now) << "\n";
    if (active != nullptr && active != latest) {
      std::cout << "  active  " << ciRunLine(active->run, active->completed_steps, now) << "\n";
    }
  }
  if (report.projects.empty()) std::cout << "No hosted projects on " << config.server << ".\n";
  return 0;
}

int ciListCommand(const ckgit::ClientConfig& config, const CiOptions& options) {
  const auto project = resolveCiProject(config, options);
  if (!project.has_value()) return kUsage;
  ckgit::CiStatusReport report;
  try {
    report = fetchCiStatus(config, *project);
  } catch (const CiReplyError& error) {
    return reportCiReplyError(error, *project, {});
  }
  const std::uint64_t now = report.server_epoch_seconds;
  auto runs = report.projects.front().runs;
  if (runs.size() > options.limit) runs.resize(options.limit);
  if (options.json) {
    std::cout << "{\"schema_version\":1,\"project\":\"" << *project << "\",\"ci_enabled\":"
              << (report.projects.front().ci_enabled ? "true" : "false") << ",\"server_epoch\":" << now
              << ",\"runs\":[";
    for (std::size_t index = 0; index < runs.size(); ++index) {
      std::cout << (index == 0 ? "" : ",") << "\n    " << ciRunJson(runs[index].run, runs[index].completed_steps, now);
    }
    std::cout << "\n  ]}\n";
    return 0;
  }
  if (runs.empty()) {
    std::cout << "No CI runs recorded for " << *project << ".\n";
    return 0;
  }
  std::cout << "   " << padded("STATUS", 12) << padded("REF", 18) << padded("COMMIT", 14) << padded("STARTED", 17)
            << padded("DURATION", 12) << "RUN\n";
  for (const auto& summary : runs) std::cout << ciRunLine(summary.run, summary.completed_steps, now) << "\n";
  return 0;
}

void printCiArtifacts(const std::vector<ckgit::CiArtifactRecord>& artifacts) {
  for (const auto& artifact : artifacts) {
    std::cout << "  " << padded(artifact.name, 20);
    if (!artifact.note.empty()) {
      std::cout << "not stored: " << terminalText(artifact.note) << "\n";
      continue;
    }
    const std::string expiry = artifact.expires_epoch_seconds == 0
                                   ? "kept"
                                   : "expires " + ckgit::formatUtcTimestamp(artifact.expires_epoch_seconds);
    std::cout << padded(ckgit::formatBytes(artifact.bytes), 12) << "sha256 "
              << (artifact.sha256.empty() ? "unknown" : artifact.sha256) << "  " << expiry << "\n";
  }
}

std::string ciArtifactsJson(const std::vector<ckgit::CiArtifactRecord>& artifacts) {
  std::string json = "[";
  for (std::size_t index = 0; index < artifacts.size(); ++index) {
    const auto& artifact = artifacts[index];
    json += std::string(index == 0 ? "" : ",") + "{\"name\":\"" + artifact.name + "\",\"bytes\":" +
            std::to_string(artifact.bytes) + ",\"sha256\":" +
            (artifact.sha256.empty() ? std::string("null") : "\"" + artifact.sha256 + "\"") +
            ",\"created_epoch\":" + std::to_string(artifact.created_epoch_seconds) + ",\"expires_epoch\":" +
            (artifact.expires_epoch_seconds == 0 ? std::string("null")
                                                 : std::to_string(artifact.expires_epoch_seconds)) +
            ",\"note\":\"" + jsonText(artifact.note) + "\"}";
  }
  return json + "]";
}

int ciShowCommand(const ckgit::ClientConfig& config, const CiOptions& options) {
  const auto project = resolveCiProject(config, options);
  if (!project.has_value()) return kUsage;
  const std::string& run_id = options.positional.at(0);
  ckgit::CiRunReport report;
  try {
    report = fetchCiRun(config, *project, run_id);
  } catch (const CiReplyError& error) {
    return reportCiReplyError(error, *project, run_id);
  }
  const auto& run = report.run;
  const std::uint64_t now = report.server_epoch_seconds;
  const auto display = ckgit::ciRunDisplayAt(run, now);
  const bool running_step = display.name == "running";
  if (options.json) {
    std::cout << "{\"schema_version\":1,\"project\":\"" << *project << "\",\"server_epoch\":" << now
              << ",\"run\":" << ciRunJson(run, run.steps.size(), now) << ",\"detail\":\"" << jsonText(run.detail)
              << "\",\"steps\":[";
    for (std::size_t index = 0; index < run.steps.size(); ++index) {
      const auto& step = run.steps[index];
      std::cout << (index == 0 ? "" : ",") << "\n    {\"index\":" << index << ",\"name\":\"" << jsonText(step.name)
                << "\",\"exit_code\":" << step.exit_code << ",\"timed_out\":" << (step.timed_out ? "true" : "false")
                << ",\"output_truncated\":" << (step.output_truncated ? "true" : "false") << ",\"running\":false}";
    }
    if (running_step) {
      std::cout << (run.steps.empty() ? "" : ",") << "\n    {\"index\":" << run.steps.size()
                << ",\"name\":null,\"exit_code\":null,\"timed_out\":false,\"output_truncated\":false,\"running\":true}";
    }
    std::cout << "\n  ],\"artifacts\":" << ciArtifactsJson(run.artifacts) << "}\n";
    return 0;
  }
  const std::string timing = ckgit::ciRunTiming(display);
  std::cout << "Run " << run.run_id << " of " << *project << "\n"
            << "  status:    " << display.icon << " " << display.name << "\n"
            << "  ref:       " << terminalText(run.ref) << "\n"
            << "  commit:    " << run.commit_id << "\n";
  if (run.started_epoch_seconds != 0) {
    std::cout << "  started:   " << ckgit::formatUtcTimestamp(run.started_epoch_seconds) << " ("
              << ckgit::relativeTimeAt(run.started_epoch_seconds, now) << ")\n";
  }
  if (!display.active && run.finished_epoch_seconds != 0) {
    std::cout << "  finished:  " << ckgit::formatUtcTimestamp(run.finished_epoch_seconds) << "\n";
  }
  if (!timing.empty()) std::cout << "  " << (display.active ? "elapsed:   " : "duration:  ") << timing << "\n";
  if (!run.detail.empty()) std::cout << "  detail:    " << terminalText(run.detail) << "\n";
  if (display.name == "interrupted") {
    std::cout << "  note:      the runner stopped reporting progress; the next runner sweep settles the status\n";
  }
  std::cout << "Steps:\n";
  for (std::size_t index = 0; index < run.steps.size(); ++index) {
    const auto& step = run.steps[index];
    std::cout << "  " << padded(std::to_string(index), 4) << ciStepIcon(step) << " "
              << padded(ciStepLabel(step, index), 24) << ciStepOutcome(step) << "\n";
  }
  if (running_step) {
    std::cout << "  " << padded(std::to_string(run.steps.size()), 4)
              << ckgit::ciRunStatusIcon(ckgit::CiRunStatus::Running) << " running\n";
  } else if (run.steps.empty()) {
    std::cout << "  none" << (display.name == "pending" ? " yet; the run is queued" : "") << "\n";
  }
  if (!run.artifacts.empty()) {
    std::cout << "Artifacts:\n";
    printCiArtifacts(run.artifacts);
  }
  return 0;
}

int ciArtifactsCommand(const ckgit::ClientConfig& config, const CiOptions& options) {
  const auto project = resolveCiProject(config, options);
  if (!project.has_value()) return kUsage;
  const std::string& run_id = options.positional.at(0);
  ckgit::CiRunReport report;
  try {
    report = fetchCiRun(config, *project, run_id);
  } catch (const CiReplyError& error) {
    return reportCiReplyError(error, *project, run_id);
  }
  if (options.json) {
    std::cout << "{\"schema_version\":1,\"project\":\"" << *project << "\",\"run_id\":\"" << run_id
              << "\",\"artifacts\":" << ciArtifactsJson(report.run.artifacts) << "}\n";
    return 0;
  }
  if (report.run.artifacts.empty()) {
    std::cout << "Run " << run_id << " of " << *project << " has no artifacts.\n";
    return 0;
  }
  printCiArtifacts(report.run.artifacts);
  return 0;
}

int ciDownloadCommand(const ckgit::ClientConfig& config, const CiOptions& options) {
  const auto project = resolveCiProject(config, options);
  if (!project.has_value()) return kUsage;
  const std::string& run_id = options.positional.at(0);
  const std::string& name = options.positional.at(1);
  std::error_code directory_error;
  if (!std::filesystem::is_directory(options.into, directory_error)) {
    std::cerr << "ckgit: --into " << options.into.string() << " is not a directory\n";
    return 1;
  }
  ckgit::CiRunReport report;
  try {
    report = fetchCiRun(config, *project, run_id);
  } catch (const CiReplyError& error) {
    return reportCiReplyError(error, *project, run_id);
  }
  const auto& artifacts = report.run.artifacts;
  const auto artifact = std::find_if(artifacts.begin(), artifacts.end(),
                                     [&](const ckgit::CiArtifactRecord& candidate) { return candidate.name == name; });
  if (artifact == artifacts.end()) {
    std::cerr << "ckgit: run " << run_id << " has no artifact named " << name;
    if (!artifacts.empty()) {
      std::cerr << "; it has:";
      for (const auto& candidate : artifacts) std::cerr << " " << candidate.name;
    }
    std::cerr << "\n";
    return 1;
  }
  if (!artifact->note.empty()) {
    std::cerr << "ckgit: artifact " << name << " was not stored: " << terminalText(artifact->note) << "\n";
    return 1;
  }
  std::string body;
  try {
    body = fetchDashboardDownload(config, options.dashboard_url,
                                  "/project/" + *project + "/ci/" + run_id + "/artifacts/" + name, artifact->bytes);
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << error.what() << "\n";
    return 1;
  }
  const auto destination = installVerifiedDownload(options.into, name + ".tar", body, artifact->sha256,
                                                   "run " + run_id + " artifact " + name);
  if (!destination.has_value()) return 1;
  std::cout << "Downloaded " << name << " of run " << run_id << " to " << destination->string() << " ("
            << body.size() << " bytes, sha256 " << (artifact->sha256.empty() ? "unverified" : "verified") << ").\n";
  return 0;
}

// Fetches one chunk of a step's log. Errors are CiReplyError as for any query.
ckgit::CiLogChunk fetchCiLogChunk(const ckgit::ClientConfig& config, const std::string& project,
                                  const std::string& run_id, std::size_t step, std::uint64_t offset) {
  return ckgit::parseCiLogControlResponse(
      ciControlReply(config, "ci-log", {project, run_id, std::to_string(step), std::to_string(offset)}), offset);
}

int ciLogCommand(const ckgit::ClientConfig& config, const CiOptions& options) {
  const auto project = resolveCiProject(config, options);
  if (!project.has_value()) return kUsage;
  const std::string& run_id = options.positional.at(0);
  const bool explicit_step = options.positional.size() > 1;
  try {
    auto report = fetchCiRun(config, *project, run_id);
    // The default is the running step, which follows the completed ones, or a
    // finished run's last step.
    const bool recorded_active = ckgit::ciRunStatusIsActive(report.run.status);
    if (!explicit_step && !recorded_active && report.run.steps.empty()) {
      std::cerr << "ckgit: run " << run_id << " ran no steps (" << ckgit::ciRunStatusName(report.run.status)
                << (report.run.detail.empty() ? "" : ": " + terminalText(report.run.detail)) << ")\n";
      return 1;
    }
    std::size_t step = report.run.steps.size() - (recorded_active ? 0 : 1);
    if (explicit_step) step = static_cast<std::size_t>(schemaNumber(options.positional[1]));
    std::uint64_t offset = 0;
    bool ended = false;
    while (true) {
      const auto chunk = fetchCiLogChunk(config, *project, run_id, step, offset);
      std::cout.write(chunk.bytes.data(), static_cast<std::streamsize>(chunk.bytes.size()));
      std::cout.flush();
      offset += chunk.bytes.size();
      ended = chunk.complete;
      if (chunk.complete) {
        if (!options.follow || explicit_step) break;
        // Advance to the next step while the run has one: completed already,
        // or about to run because the run is still live.
        report = fetchCiRun(config, *project, run_id);
        const bool live = ckgit::ciRunDisplayAt(report.run, report.server_epoch_seconds).active;
        if (step + 1 >= report.run.steps.size() && !(live && step + 1 == report.run.steps.size())) break;
        ++step;
        offset = 0;
        std::cerr << "--- step " << step
                  << (step < report.run.steps.size() ? " (" + ciStepLabel(report.run.steps[step], step) + ")" : "")
                  << " ---\n";
        continue;
      }
      if (chunk.bytes.size() == ckgit::kMaximumControlLogChunkBytes) continue;  // more is already written
      if (!options.follow) break;
      std::this_thread::sleep_for(kCiLogFollowInterval);
    }
    if (offset == 0 && !ended) {
      std::cerr << "ckgit: step " << step << " of run " << run_id << " has not written any output yet\n";
    }
    if (options.follow) {
      report = fetchCiRun(config, *project, run_id);
      const auto display = ckgit::ciRunDisplayAt(report.run, report.server_epoch_seconds);
      if (display.name == "interrupted") {
        std::cerr << "ckgit: run " << run_id << " stopped reporting progress (interrupted)\n";
      }
    }
    return 0;
  } catch (const CiReplyError& error) {
    if (error.code() == "nolog") {
      std::cerr << "ckgit: run " << run_id << " has no log for that step\n";
      return 1;
    }
    return reportCiReplyError(error, *project, run_id);
  }
}

int ciWatchCommand(const ckgit::ClientConfig& config, const CiOptions& options) {
  const auto project = resolveCiProject(config, options);
  if (!project.has_value()) return kUsage;
  std::string run_id;
  try {
    if (!options.positional.empty()) {
      run_id = options.positional.front();
    } else {
      const auto status = fetchCiStatus(config, *project);
      const auto& runs = status.projects.front().runs;
      if (runs.empty()) {
        std::cerr << "ckgit: " << *project << " has no CI runs to watch\n";
        return 1;
      }
      const auto [latest, active] = ciLatestAndActive(status.projects.front(), status.server_epoch_seconds);
      run_id = (active != nullptr ? active : latest)->run.run_id;
    }
    auto report = fetchCiRun(config, *project, run_id);
    std::cout << "Watching run " << run_id << " of " << *project << " (" << terminalText(ciRefLabel(report.run.ref))
              << " " << report.run.commit_id.substr(0, 12) << ")\n" << std::flush;
    std::size_t printed_steps = 0;
    std::string last_state;
    int failed_polls = 0;
    while (true) {
      const auto& run = report.run;
      const auto display = ckgit::ciRunDisplayAt(run, report.server_epoch_seconds);
      for (; printed_steps < run.steps.size(); ++printed_steps) {
        const auto& step = run.steps[printed_steps];
        std::cout << "  " << ciStepIcon(step) << " step " << printed_steps << " " << ciStepLabel(step, printed_steps)
                  << ": " << ciStepOutcome(step) << "\n";
      }
      if (!display.active) {
        const std::string timing = ckgit::ciRunTiming(display);
        std::cout << display.icon << " Run " << run_id << " " << display.name
                  << (timing.empty() ? "" : " after " + timing)
                  << (run.detail.empty() ? "" : ": " + terminalText(run.detail)) << "\n";
        return run.status == ckgit::CiRunStatus::Success && display.name == "success" ? 0 : kRunUnsuccessful;
      }
      const std::string state = display.name == "pending" ? "queued, waiting for a runner"
                                                          : "running step " + std::to_string(run.steps.size());
      if (state != last_state) {
        std::cout << "  " << display.icon << " " << state << "\n";
        last_state = state;
      }
      std::cout.flush();
      std::this_thread::sleep_for(options.interval);
      try {
        report = fetchCiRun(config, *project, run_id);
        failed_polls = 0;
      } catch (const CiReplyError&) {
        throw;
      } catch (const std::exception& error) {
        if (++failed_polls >= kCiWatchPollAttempts) throw;
        std::cerr << "ckgit: warning: " << error.what() << "; retrying\n";
      }
    }
  } catch (const CiReplyError& error) {
    return reportCiReplyError(error, *project, run_id);
  }
}

int ciCancelCommand(const ckgit::ClientConfig& config, const CiOptions& options) {
  const auto project = resolveCiProject(config, options);
  if (!project.has_value()) return kUsage;
  const std::string& run_id = options.positional.at(0);
  try {
    const auto report = fetchCiRun(config, *project, run_id);
    const auto& run = report.run;
    const auto display = ckgit::ciRunDisplayAt(run, report.server_epoch_seconds);
    if (!ckgit::ciRunStatusIsActive(run.status)) {
      std::cout << "Run " << run_id << " of " << *project << " already finished ("
                << ckgit::ciRunStatusName(run.status) << "); nothing to cancel.\n";
      return 0;
    }
    const bool queued = run.status == ckgit::CiRunStatus::Pending;
    std::cout << "Cancel run " << run_id << " of " << *project << " on " << config.server << ":\n"
              << "  status: " << display.icon << " " << display.name
              << (queued ? " (queued, not started)" : ", step " + std::to_string(run.steps.size()) + " running")
              << "\n  ref:    " << terminalText(ciRefLabel(run.ref)) << "\n  commit: " << run.commit_id << "\n"
              << (queued ? "  The runner records the run as cancelled instead of starting it.\n"
                         : "  The runner stops the current step at its next check and records the run as cancelled.\n");
    if (options.dry_run) {
      std::cout << "Preview only; nothing was cancelled.\n";
      return 0;
    }
    if (!confirmManagementPlan(options.yes, "Cancel this run?")) return 0;
    if (ciControlReply(config, "ci-cancel", {*project, run_id}) != "ok cancelling\n") {
      throw std::runtime_error("unexpected reply to the cancellation request");
    }
    std::cout << "Requested cancellation of run " << run_id << ". Follow it with ckgit ci watch " << run_id
              << (options.project.has_value() ? " --project " + *project : "") << ".\n";
    return 0;
  } catch (const CiReplyError& error) {
    if (error.code() == "finished") {
      std::cout << "Run " << run_id << " finished before the cancellation arrived; nothing was cancelled.\n";
      return 0;
    }
    return reportCiReplyError(error, *project, run_id);
  }
}

// --- ci lint -------------------------------------------------------------------

// Reads the workflow to lint: the file PATH, PATH's (or the current
// repository's) .ckgit/ci.yml, or with --rev the blob committed there.
// Returns the content and names its source; throws with a readable reason.
std::pair<std::string, std::string> readLintWorkflow(const CiOptions& options) {
  const std::optional<std::filesystem::path> path =
      options.positional.empty() ? std::nullopt : std::optional<std::filesystem::path>(options.positional.front());
  std::error_code error;
  if (path.has_value() && !std::filesystem::is_directory(*path, error)) {
    if (options.rev.has_value()) throw std::invalid_argument("--rev needs PATH to be a repository directory");
    std::ifstream file(*path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read " + path->string());
    std::string content(ckgit::kMaximumCiWorkflowBytes + 1, '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(file.gcount()));
    return {content, path->string()};
  }
  const std::filesystem::path directory = path.value_or(".");
  const auto top = ckgit::runProcess({"git", "-C", directory.string(), "rev-parse", "--show-toplevel"});
  if (top.exit_code != 0 || top.timed_out || top.output.empty()) {
    throw std::runtime_error(directory.string() + " is not inside a Git repository; pass a workflow file");
  }
  const std::filesystem::path repository = top.output.substr(0, top.output.find('\n'));
  if (!options.rev.has_value()) {
    const std::filesystem::path workflow = repository / ".ckgit" / "ci.yml";
    std::ifstream file(workflow, std::ios::binary);
    if (!file) throw std::runtime_error(workflow.string() + " does not exist; CI runs need .ckgit/ci.yml");
    std::string content(ckgit::kMaximumCiWorkflowBytes + 1, '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(file.gcount()));
    return {content, workflow.string()};
  }
  const auto commit = ckgit::runProcess(
      {"git", "-C", repository.string(), "rev-parse", "--verify", "--quiet", *options.rev + "^{commit}"});
  const std::string commit_id = commit.output.substr(0, commit.output.find('\n'));
  if (commit.exit_code != 0 || commit.timed_out || !ckgit::isValidObjectId(commit_id)) {
    throw std::runtime_error(*options.rev + " is not a commit in " + repository.string());
  }
  ckgit::ProcessOptions blob_options;
  blob_options.timeout = std::chrono::seconds(20);
  blob_options.output_limit = ckgit::kMaximumCiWorkflowBytes + 1;
  const auto blob = ckgit::runProcess(
      {"git", "-C", repository.string(), "cat-file", "blob", commit_id + ":.ckgit/ci.yml"}, blob_options);
  if (blob.exit_code != 0 && !blob.output_truncated) {
    throw std::runtime_error("commit " + commit_id.substr(0, 12) + " (" + *options.rev +
                             ") has no .ckgit/ci.yml; a push of it would be recorded as skipped");
  }
  return {blob.output, ".ckgit/ci.yml at " + *options.rev + " (" + commit_id.substr(0, 12) + ")"};
}

// Shows the lines around a parse error that names one, `(line N)`.
void printLintContext(const std::string& content, const std::string& message) {
  const std::size_t marker = message.rfind("(line ");
  if (marker == std::string::npos) return;
  std::size_t line_number = 0;
  const auto [end, error] = std::from_chars(message.data() + marker + 6, message.data() + message.size(), line_number);
  if (error != std::errc{} || *end != ')' || line_number == 0) return;
  std::size_t current = 1;
  std::size_t start = 0;
  while (start <= content.size() && current <= line_number + 1) {
    const std::size_t newline = content.find('\n', start);
    const std::string_view line(content.data() + start,
                                (newline == std::string::npos ? content.size() : newline) - start);
    if (current + 1 >= line_number) {
      std::string shown;
      for (const char character : line) shown += character == '\t' ? std::string("\\t") : std::string(1, character);
      std::cerr << (current == line_number ? "> " : "  ") << padded(std::to_string(current), 5) << "| "
                << terminalText(shown) << "\n";
    }
    if (newline == std::string::npos) break;
    start = newline + 1;
    ++current;
  }
}

std::string joined(const std::vector<std::string>& values) {
  std::string text;
  for (const auto& value : values) text += (text.empty() ? "" : ", ") + value;
  return text;
}

int ciLintCommand(const CiOptions& options) {
  std::string content;
  std::string source;
  try {
    std::tie(content, source) = readLintWorkflow(options);
  } catch (const std::invalid_argument& error) {
    std::cerr << "ckgit: " << error.what() << "\n";
    return kUsage;
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << error.what() << "\n";
    return 1;
  }
  ckgit::CiWorkflow workflow;
  try {
    workflow = ckgit::parseCiWorkflow(content);
  } catch (const std::exception& error) {
    std::cerr << source << ": invalid: " << terminalText(error.what()) << "\n";
    printLintContext(content, error.what());
    return 1;
  }
  std::cout << source << ": valid (version " << workflow.version << ")\n";
  std::string triggers;
  if (workflow.triggers_default_branch) triggers = "the repository's default branch";
  else if (!workflow.branches.empty()) triggers = "branches " + joined(workflow.branches);
  else triggers = "no branches";
  triggers += workflow.tags.empty() ? "; no tags"
              : workflow.tags == std::vector<std::string>{"*"} ? "; every tag (a release)"
                                                              : "; tags " + joined(workflow.tags) + " (releases)";
  std::cout << "  triggers: " << terminalText(triggers) << "\n  jobs: " << workflow.jobs.size() << "\n";
  for (const auto& job : workflow.jobs) {
    std::cout << "    " << terminalText(job.name) << ": " << job.steps.size()
              << (job.steps.size() == 1 ? " step" : " steps");
    if (job.artifact.has_value()) {
      const unsigned days = job.artifact->retention_days;
      std::cout << "; artifact " << terminalText(job.artifact->name) << " ("
                << terminalText(joined(job.artifact->paths)) << (days != 0 ? "; " + std::to_string(days) + " days" : "")
                << ")";
    }
    std::cout << "\n";
  }
  if (!workflow.sisters.empty()) {
    std::vector<std::string> sisters;
    for (const auto& sister : workflow.sisters) {
      sisters.push_back(sister.ref.empty() ? sister.name : sister.name + " @ " + sister.ref);
    }
    std::cout << "  sisters: " << terminalText(joined(sisters)) << "\n";
  }
  if (!workflow.caches.empty()) {
    std::vector<std::string> caches;
    for (const auto& cache : workflow.caches) {
      caches.push_back(cache.env.empty() ? cache.name : cache.name + " (" + joined(cache.env) + ")");
    }
    std::cout << "  caches: " << terminalText(joined(caches)) << "\n";
  }
  if (workflow.pages_path.has_value()) {
    std::cout << "  pages: " << terminalText(*workflow.pages_path) << " (published by default-branch builds)\n";
  }
  // What only the server knows, named for this workflow.
  std::vector<std::string> unchecked;
  if (workflow.triggers_default_branch || workflow.pages_path.has_value()) {
    unchecked.push_back("which branch is the repository's default branch on the server");
  }
  if (!workflow.sisters.empty()) unchecked.push_back("whether the sister projects exist on the server");
  unchecked.push_back("the server's limits (step timeout, log and artifact sizes, retention cap, network policy)");
  std::cout << "Not checked locally: " << joined(unchecked) << ".\n";
  return 0;
}

int ciCommand(const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    printUsage(std::cerr);
    return kUsage;
  }
  const std::string& subcommand = arguments.front();
  const CiOptions options = parseCiOptions(std::vector<std::string>(arguments.begin() + 1, arguments.end()));
  if (subcommand == "lint") return ciLintCommand(options);
  const auto config = ckgit::loadClientConfig(resolveConfigPath(options.config_path));
  try {
    if (subcommand == "status") return ciStatusCommand(config, options);
    if (subcommand == "list") return ciListCommand(config, options);
    if (subcommand == "show") return ciShowCommand(config, options);
    if (subcommand == "log") return ciLogCommand(config, options);
    if (subcommand == "watch") return ciWatchCommand(config, options);
    if (subcommand == "artifacts") return ciArtifactsCommand(config, options);
    if (subcommand == "download") return ciDownloadCommand(config, options);
    if (subcommand == "cancel") return ciCancelCommand(config, options);
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << error.what() << "\n";
    return 1;
  }
  printUsage(std::cerr);
  return kUsage;
}
