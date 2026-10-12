// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "ckgit/ci_runner_protocol.hpp"
#include "ckgit/control_rpc.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/repository_store.hpp"
#include "ckgit/ssh_command.hpp"
#include "ckgit/validation.hpp"

namespace {

constexpr int kDenied = 126;
constexpr int kUsage = 2;

struct Options {
  std::string client_id;
  std::filesystem::path repo_root;
  std::filesystem::path control_socket;
  std::optional<std::filesystem::path> state_root;
  // Set for a CI runner key (see renderAuthorizedKeyLine): only ckgit-runner
  // operations, handed to the runner gateway.
  std::optional<std::filesystem::path> runner_config;
  std::filesystem::path runner_program{"/usr/bin/ck-ci-runnerd"};
  bool dry_run{false};
};

void usage(std::ostream& output) {
  output << "Usage: ck-git-shell --client-id ID --repo-root ROOT --control-socket PATH [--state-root ROOT]\n"
            "                    [--runner-config FILE [--runner-program PATH]] [--dry-run]\n";
}

bool parseOptions(int argc, char* argv[], Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--dry-run") {
      options->dry_run = true;
      continue;
    }
    if ((argument == "--client-id" || argument == "--repo-root" || argument == "--control-socket" ||
         argument == "--state-root" || argument == "--runner-config" || argument == "--runner-program") &&
        index + 1 < argc) {
      const std::string value = argv[++index];
      if (argument == "--runner-config") {
        options->runner_config.emplace(value);
      } else if (argument == "--runner-program") {
        options->runner_program = value;
      } else if (argument == "--client-id") {
        options->client_id = value;
      } else if (argument == "--repo-root") {
        options->repo_root = value;
      } else if (argument == "--state-root") {
        options->state_root.emplace(value);
      } else {
        options->control_socket = value;
      }
      continue;
    }
    return false;
  }
  return ckgit::isValidClientId(options->client_id) && !options->repo_root.empty() &&
         !options->control_socket.empty();
}

void requireExistingRepository(const std::filesystem::path& repository) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(repository, error);
  if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
    throw std::runtime_error("requested repository is unavailable");
  }
}

[[noreturn]] void execGitService(const ckgit::SshCommand& command,
                                 const std::filesystem::path& repository,
                                 std::string_view client_id,
                                 std::string_view project_name,
                                 const std::optional<std::filesystem::path>& state_root,
                                 const std::filesystem::path& control_socket) {
  const char* program = command.kind == ckgit::SshCommandKind::kUploadPack
                            ? "/usr/bin/git-upload-pack"
                            : "/usr/bin/git-receive-pack";
  char* const arguments[] = {const_cast<char*>(program),
                             const_cast<char*>(repository.c_str()), nullptr};
  std::vector<std::string> environment_values{"PATH=/usr/bin:/bin", "LANG=C",
                                               "CKGIT_CLIENT_ID=" + std::string(client_id),
                                               "CKGIT_PROJECT_NAME=" + std::string(project_name)};
  environment_values.push_back("CKGIT_CONTROL_SOCKET=" + control_socket.string());
  environment_values.push_back("CKGIT_REPOSITORY_ROOT=" + repository.parent_path().string());
  if (const char* temporary_root = std::getenv("TMPDIR")) {
    environment_values.push_back("TMPDIR=" + std::string(temporary_root));
  }
  if (state_root.has_value()) {
    environment_values.push_back("CKGIT_STATE_ROOT=" + state_root->string());
  }
  std::vector<char*> environment;
  environment.reserve(environment_values.size() + 1);
  for (auto& value : environment_values) {
    environment.push_back(value.data());
  }
  environment.push_back(nullptr);
  execve(program, arguments, environment.data());
  throw std::runtime_error("could not start Git service: " + std::string(std::strerror(errno)));
}

// A runner key reaches nothing but the gateway: the validated command line is
// handed over whole, with a minimal environment.
int dispatchRunner(const Options& options, const char* original) {
  std::string reason;
  const auto command = ckgit::parseCiRunnerCommand(original, &reason);
  if (!command.has_value()) {
    std::cerr << "ck-git-shell: denied: " << reason << "\n";
    return kDenied;
  }
  if (options.client_id == ckgit::kCiPrimaryRunnerName) {
    std::cerr << "ck-git-shell: denied: reserved runner name\n";
    return kDenied;
  }
  if (options.dry_run) {
    std::cout << "runner " << original << " for " << options.client_id << "\n";
    return 0;
  }
  const std::string program = options.runner_program.string();
  const std::string config = options.runner_config->string();
  std::vector<std::string> arguments{program, "gateway", "--config", config, "--runner", options.client_id,
                                     "--command", original};
  std::vector<std::string> environment_values{"PATH=/usr/bin:/bin", "LANG=C"};
  if (const char* temporary_root = std::getenv("TMPDIR")) {
    environment_values.push_back("TMPDIR=" + std::string(temporary_root));
  }
  // Git's protocol negotiation, when sshd passes it through (AcceptEnv).
  if (const char* protocol = std::getenv("GIT_PROTOCOL")) {
    const std::string_view value(protocol);
    if (value.size() < 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '=' || c == ':';
        })) {
      environment_values.push_back("GIT_PROTOCOL=" + std::string(value));
    }
  }
  std::vector<char*> argv_raw;
  for (auto& argument : arguments) argv_raw.push_back(argument.data());
  argv_raw.push_back(nullptr);
  std::vector<char*> environment;
  for (auto& value : environment_values) environment.push_back(value.data());
  environment.push_back(nullptr);
  execve(program.c_str(), argv_raw.data(), environment.data());
  throw std::runtime_error("could not start the runner gateway: " + std::string(std::strerror(errno)));
}

int dispatch(const Options& options) {
  const char* original = std::getenv("SSH_ORIGINAL_COMMAND");
  if (original == nullptr) {
    std::cerr << "ck-git-shell: SSH_ORIGINAL_COMMAND is not set\n";
    return kDenied;
  }
  if (options.runner_config.has_value()) return dispatchRunner(options, original);
  std::string reason;
  const auto command = ckgit::parseSshOriginalCommand(original, &reason);
  if (!command.has_value()) {
    std::cerr << "ck-git-shell: denied: " << reason << "\n";
    return kDenied;
  }
  if (command->kind == ckgit::SshCommandKind::kRpc) {
    if (options.dry_run) {
      std::cout << "rpc " << command->rpc_operation;
      for (const auto& argument : command->rpc_arguments) {
        std::cout << " " << argument;
      }
      std::cout << " for " << options.client_id << "\n";
      return 0;
    }
    std::string response;
    const bool success = ckgit::forwardControlRpc(options.control_socket, options.client_id,
                                                  command->rpc_operation, command->rpc_arguments,
                                                  &response);
    std::cout << response;
    return success ? 0 : 1;
  }

  const auto root = ckgit::validatedRepositoryRoot(options.repo_root);
  const std::optional<std::filesystem::path> state_root = options.state_root.has_value()
      ? std::optional<std::filesystem::path>(ckgit::validatedMetadataRoot(*options.state_root))
      : std::nullopt;
  const auto repository = ckgit::bareRepositoryPath(root, command->project_name);
  requireExistingRepository(repository);
  if (options.dry_run) {
    std::cout << (command->kind == ckgit::SshCommandKind::kUploadPack ? "git-upload-pack "
                                                                        : "git-receive-pack ")
              << repository.string() << " for " << options.client_id << "\n";
    return 0;
  }
  execGitService(*command, repository, options.client_id, command->project_name, state_root,
                  options.control_socket);
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    Options options;
    if (!parseOptions(argc, argv, &options)) {
      usage(std::cerr);
      return kUsage;
    }
    return dispatch(options);
  } catch (const std::exception& error) {
    std::cerr << "ck-git-shell: denied: " << error.what() << "\n";
    return kDenied;
  }
}
