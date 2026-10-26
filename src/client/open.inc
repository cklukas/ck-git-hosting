// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// `ckgit open`: the dashboard page for this checkout's hosted project -- its
// overview, a file or folder at the current branch, a commit, or CI -- opened
// through the same loopback SSH tunnel `ckgit web` uses. Included by main.cpp.

// The dashboard path `ckgit open` asks for, or std::nullopt after explaining why
// there is none.
std::optional<std::string> openPagePath(const ckgit::RepositoryAudit& audit, const ckgit::ClientConfig& config,
                                        const std::string& project, const std::optional<std::string>& path,
                                        const std::optional<std::string>& commit, bool ci,
                                        const std::optional<std::string>& run) {
  const std::string base = "/project/" + project;
  if (ci) return base + "/ci";
  if (run.has_value()) return base + "/ci/" + *run;
  const auto git = [&](std::vector<std::string> arguments) {
    std::vector<std::string> command{"git", "-C", audit.path.string()};
    command.insert(command.end(), arguments.begin(), arguments.end());
    ckgit::ProcessResult result = ckgit::runProcess(command, std::chrono::seconds(20));
    while (!result.output.empty() && (result.output.back() == '\n' || result.output.back() == '\r')) {
      result.output.pop_back();
    }
    return result;
  };
  if (commit.has_value()) {
    const auto resolved = git({"rev-parse", "--verify", "--quiet", "--end-of-options", *commit + "^{commit}"});
    if (resolved.exit_code != 0 || !ckgit::isValidObjectId(resolved.output)) {
      std::cerr << "ckgit: " << *commit << " names no commit in " << audit.path.string() << "\n";
      return std::nullopt;
    }
    // The dashboard knows only what reached the server; a local commit 404s.
    const auto published = git({"for-each-ref", "--count=1", "--contains", resolved.output, "--format=%(refname)",
                                "refs/remotes/" + config.remote_name + "/"});
    if (published.exit_code == 0 && published.output.empty()) {
      std::cerr << "ckgit: note: " << resolved.output.substr(0, 12) << " is in no " << config.remote_name
                << " branch this checkout knows of; publish it (ckgit sync) if the page is missing\n";
    }
    return base + "/commit/" + resolved.output;
  }
  if (!path.has_value()) return base;

  // A file or folder of this checkout, shown at the current branch (or, with
  // a detached HEAD, at its commit).
  std::error_code error;
  const auto target = std::filesystem::weakly_canonical(std::filesystem::absolute(*path), error);
  const auto root = std::filesystem::weakly_canonical(audit.path, error);
  const auto relative = target.lexically_relative(root);
  if (error || relative.empty() || *relative.begin() == "..") {
    std::cerr << "ckgit: " << *path << " is not inside " << audit.path.string() << "\n";
    return std::nullopt;
  }
  const std::string inside = relative == "." ? std::string() : relative.generic_string();
  std::string ref = audit.current_branch;
  if (audit.detached_head || ref.empty()) {
    const auto head = git({"rev-parse", "--verify", "HEAD"});
    if (head.exit_code != 0) {
      std::cerr << "ckgit: " << audit.path.string() << " has no commit yet\n";
      return std::nullopt;
    }
    ref = head.output;
  }
  const bool directory = inside.empty() || std::filesystem::is_directory(target, error);
  return base + (directory ? "/tree/" : "/blob/") + ckgit::encodePathSegment(ref) + ":" +
         ckgit::encodePathSegment(inside);
}

int openCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> config_path;
  std::optional<std::filesystem::path> repository;
  std::optional<std::string> commit;
  std::optional<std::string> run;
  std::optional<std::string> path;
  bool ci = false;
  std::vector<std::string> web_arguments;  // passed on to the tunnel
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string& argument = arguments[index];
    const bool has_value = index + 1 < arguments.size();
    if (argument == "--config" && has_value) {
      config_path.emplace(arguments[++index]);
      web_arguments.insert(web_arguments.end(), {argument, arguments[index]});
    } else if (argument == "--repo" && has_value) {
      repository.emplace(arguments[++index]);
    } else if (argument == "--commit" && has_value) {
      commit = arguments[++index];
    } else if (argument == "--run" && has_value) {
      run = arguments[++index];
    } else if (argument == "--ci") {
      ci = true;
    } else if ((argument == "--port" || argument == "--remote-port") && has_value) {
      web_arguments.insert(web_arguments.end(), {argument, arguments[++index]});
    } else if (argument == "--no-open") {
      web_arguments.push_back(argument);
    } else if (argument == "--" && index + 1 == arguments.size() - 1) {
      path = arguments[++index];
    } else if (!argument.empty() && argument.front() != '-' && !path.has_value()) {
      path = argument;
    } else {
      std::cerr << "ckgit: invalid open option; see ckgit open --help\n";
      return kUsage;
    }
  }
  if (static_cast<int>(path.has_value()) + static_cast<int>(commit.has_value()) + static_cast<int>(ci) +
          static_cast<int>(run.has_value()) > 1) {
    std::cerr << "ckgit: choose one of PATH, --commit, --ci, and --run\n";
    return kUsage;
  }
  const auto config = loadConfiguredClient(resolveConfigPath(config_path));
  std::optional<ckgit::RepositoryAudit> audit;
  try {
    audit = ckgit::inspectRepository(repository.value_or("."));
  } catch (const std::exception&) {
    std::cerr << "ckgit: " << (repository ? repository->string() : std::string("this folder"))
              << " is not inside a Git repository; use ckgit web --project NAME for a hosted project\n";
    return 1;
  }
  bool invalid = false;
  const auto project = pairedProjectForAudit(*audit, config, &invalid);
  if (!project.has_value()) {
    std::cerr << "ckgit: " << audit->path.string() << " is not paired with a project on " << config.server
              << (invalid ? " (its " + config.remote_name + " remote is unsafe or ambiguous)" : "")
              << "; publish it first, or use ckgit web --project NAME\n";
    return 1;
  }
  const auto page = openPagePath(*audit, config, *project, path, commit, ci, run);
  if (!page.has_value()) return 1;
  return webCommand(web_arguments, *page);
}
