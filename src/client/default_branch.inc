// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

// Included inside main.cpp's anonymous namespace, after ci.inc, whose project
// selection and control-reply helpers it shares. `ckgit default-branch`: show
// a hosted project's default branch, or point it at another existing branch
// over the restricted SSH control channel (docs/protocol/01-ssh-and-control-v1.md).

// The server's `default-branch` reply: the branch HEAD names, and whether it
// has a commit yet. std::nullopt branch when HEAD names no branch.
struct ServerDefaultBranch {
  std::optional<std::string> branch;
  bool exists{false};
};

ServerDefaultBranch fetchDefaultBranch(const ckgit::ClientConfig& config, const std::string& project) {
  std::string reply;
  try {
    reply = checkedControlReply(config, "default-branch", {project}, "read the default branch");
  } catch (const ControlReplyError& error) {
    if (error.code() == "nohead") return {};
    throw;
  }
  // ok <branch> <present|unborn>
  const std::size_t space = reply.find(' ', 3);
  if (reply.rfind("ok ", 0) != 0 || reply.back() != '\n' || space == std::string::npos ||
      reply.find('\n') != reply.size() - 1) {
    throw std::runtime_error("default branch reply has invalid framing");
  }
  const std::string branch = reply.substr(3, space - 3);
  const std::string state = reply.substr(space + 1, reply.size() - space - 2);
  if (!ckgit::isValidBranchName(branch) || (state != "present" && state != "unborn")) {
    throw std::runtime_error("default branch reply is malformed");
  }
  return {branch, state == "present"};
}

// The SSH dispatcher accepts only [A-Za-z0-9._-] in ckgit-rpc tokens, so a
// branch name with a slash cannot travel over the control channel.
bool isControlChannelToken(std::string_view value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.';
  });
}

int defaultBranchCommand(const std::vector<std::string>& arguments) {
  const CiOptions options = parseCiOptions(arguments);
  if (options.positional.size() > 1) {
    std::cerr << "ckgit: usage: ckgit default-branch [--project NAME] [BRANCH] [--yes] [--dry-run]\n";
    return kUsage;
  }
  const auto config = ckgit::loadClientConfig(resolveConfigPath(options.config_path));
  const auto project = resolveCiProject(config, options);
  if (!project.has_value()) return kUsage;
  try {
    const auto current = fetchDefaultBranch(config, *project);
    if (options.positional.empty()) {
      if (!current.branch.has_value()) {
        std::cout << *project << " on " << config.server << ": HEAD does not name a branch.\n"
                  << "Select one with ckgit default-branch BRANCH.\n";
        return 1;
      }
      std::cout << *current.branch << (current.exists ? "" : " (no commits yet)") << "\n";
      return 0;
    }
    const std::string& branch = options.positional.front();
    if (current.branch.has_value() && *current.branch == branch) {
      std::cout << branch << " is already the default branch of " << *project << "; nothing to change.\n";
      return 0;
    }
    if (!isControlChannelToken(branch)) {
      std::cerr << "ckgit: the control channel accepts only letters, digits, '.', '_', and '-' in a branch name;\n"
                << "  set " << branch << " on the server instead: ckgit-admin default-branch " << *project << " "
                << branch << " --config FILE\n";
      return 1;
    }
    const auto refs = fetchServerRefs(config, *project);
    const auto tip = std::find_if(refs.begin(), refs.end(), [&](const ckgit::RefTip& ref) {
      return ref.name == "refs/heads/" + branch;
    });
    if (tip == refs.end()) {
      std::cerr << "ckgit: " << *project << " on " << config.server << " has no branch " << branch
                << "; push it first, for example with ckgit sync\n";
      return 1;
    }
    std::cout << "Change the default branch of " << *project << " on " << config.server << ":\n"
              << "  current: " << current.branch.value_or("(none)") << "\n"
              << "  new:     " << branch << " (" << tip->object_id.substr(0, 12) << ")\n"
              << "  New clones check out " << branch << ", CI workflows without on: build pushes to " << branch
              << ",\n  and ck-git Pages publishes only from " << branch
              << " builds. Branches and commits are not changed.\n";
    if (options.dry_run) {
      std::cout << "Preview only; nothing changed.\n";
      return 0;
    }
    if (!confirmManagementPlan(options.yes, "Change the default branch?")) return 0;
    const std::string reply =
        checkedControlReply(config, "set-default-branch", {*project, branch}, "change the default branch");
    if (reply == "ok unchanged\n") {
      std::cout << branch << " already was the default branch of " << *project << "; nothing changed.\n";
    } else if (reply.rfind("ok changed ", 0) == 0) {
      std::cout << "Changed the default branch of " << *project << " to " << branch << ".\n";
    } else {
      throw std::runtime_error("unexpected reply to the default branch change");
    }
    return 0;
  } catch (const ControlReplyError& error) {
    if (error.code() == "noproject") {
      std::cerr << "ckgit: " << *project << " is not a hosted project\n";
    } else if (error.code() == "nobranch") {
      std::cerr << "ckgit: " << *project << " on " << config.server << " has no branch "
                << options.positional.front() << "; push it first, for example with ckgit sync\n";
    } else {
      std::cerr << "ckgit: the server refused the request: " << error.what() << "\n";
    }
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "ckgit: " << error.what() << "\n";
    return 1;
  }
}
