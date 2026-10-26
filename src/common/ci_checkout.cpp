// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_runner.hpp"
#include "ckgit/process.hpp"
#include "ckgit/path_utf8.hpp"
#include "ckgit/validation.hpp"
#include <stdexcept>
namespace ckgit {
void cloneCiCheckout(const std::string& url, const CiCheckoutRequest& request, const std::filesystem::path& destination,
                     const std::vector<std::string>& environment, const std::string& upload_pack) {
  if (!isValidObjectId(request.commit)) throw std::runtime_error("invalid commit for checkout");
  // Every git process runs with a scrubbed configuration: no system or user
  // config, no prompts, and no hooks (fetch and checkout run none here anyway).
  ProcessOptions common;
#ifdef _WIN32
  const std::string null_device = "NUL";
#else
  const std::string null_device = "/dev/null";
#endif
  for (const auto* key : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
                         "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_CONFIG_COUNT", "GIT_CONFIG_PARAMETERS",
                         "GIT_TEMPLATE_DIR"}) common.environment[key] = std::nullopt;
  for (const auto& assignment : environment) {
    const auto equals = assignment.find('=');
    if (equals == std::string::npos || equals == 0) throw std::invalid_argument("invalid checkout environment");
    common.environment[assignment.substr(0, equals)] = assignment.substr(equals + 1);
  }
  common.environment["GIT_CONFIG_NOSYSTEM"] = "1";
  common.environment["GIT_CONFIG_GLOBAL"] = null_device;
  common.environment["GIT_TERMINAL_PROMPT"] = "0";
  common.environment["LANG"] = "C";
  // An empty template disables host-configured hooks and template files.
  std::vector<std::string> base = {"git", "-c", "core.hooksPath=" + null_device,
      "-c", "init.templateDir=", "-c", "advice.detachedHead=false", "-c", "core.autocrlf=false"};
  const auto git = [&](std::vector<std::string> arguments, std::chrono::seconds timeout, const std::string& what) {
    std::vector<std::string> command = base;
    command.insert(command.end(), arguments.begin(), arguments.end());
    ProcessOptions options = common;
    options.timeout = timeout;
    options.output_limit = 64 * 1024;
    const ProcessResult result = runProcess(command, options);
    if (result.timed_out || result.exit_code != 0) {
      std::string reason = result.output;
      while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r')) reason.pop_back();
      const std::size_t last = reason.rfind('\n');
      if (last != std::string::npos) reason = reason.substr(last + 1);
      throw std::runtime_error(what + (result.timed_out ? " timed out" : " failed") +
                               (reason.empty() ? "" : ": " + reason.substr(0, 200)));
    }
  };
  const std::string dir = pathToUtf8(destination);
  git({"init", "--quiet", "--initial-branch=ckgit-ci", dir}, std::chrono::seconds(30), "git init");
  std::vector<std::string> fetch = {"-C", dir, "fetch", "--quiet", "--no-tags", "--no-recurse-submodules"};
  if (!request.spec.since.empty()) {
    fetch.push_back("--shallow-since=" + request.spec.since);
  } else if (request.spec.depth != 0) {
    fetch.push_back("--depth=" + std::to_string(request.spec.depth));
  }
  if (!upload_pack.empty()) fetch.push_back("--upload-pack=" + upload_pack);
  fetch.push_back("--end-of-options");
  fetch.push_back(url);
  fetch.push_back(request.commit);
  if (request.spec.tags) fetch.push_back("+refs/tags/*:refs/tags/*");
  git(fetch, std::chrono::seconds(1800), "git fetch of " + request.project);
  if (!request.spec.tags && request.ref.rfind("refs/tags/", 0) == 0) {
    // The build's own tag too, so `git describe` names it even at depth 1.
    // Best-effort: the commit is what the run needs, and the tag may already
    // have been deleted or moved since the push.
    fetch.pop_back();
    fetch.push_back("+" + request.ref + ":" + request.ref);
    try {
      git(fetch, std::chrono::seconds(300), "git fetch of " + request.ref);
    } catch (const std::exception&) {
    }
  }
  git({"-C", dir, "checkout", "--quiet", "--detach", request.commit}, std::chrono::seconds(600), "git checkout");
  if (request.ref.rfind("refs/heads/", 0) == 0) {
    const std::string branch = request.ref.substr(11);
    if (isValidBranchName(branch)) {
      git({"-C", dir, "update-ref", "refs/remotes/origin/" + branch, request.commit}, std::chrono::seconds(30),
          "git update-ref");
    }
  }
  if (request.spec.archive) {
    std::error_code error;
    std::filesystem::remove_all(destination / ".git", error);
    if (error) throw std::runtime_error("could not drop .git for an archive checkout");
  }
}

}  // namespace ckgit
