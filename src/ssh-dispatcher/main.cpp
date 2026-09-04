// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

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
  bool dry_run{false};
};

void usage(std::ostream& output) {
  output << "Usage: ck-git-shell --client-id ID --repo-root ROOT --control-socket PATH [--state-root ROOT] [--dry-run]\n";
}

bool parseOptions(int argc, char* argv[], Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--dry-run") {
      options->dry_run = true;
      continue;
    }
    if ((argument == "--client-id" || argument == "--repo-root" ||
         argument == "--control-socket" || argument == "--state-root") && index + 1 < argc) {
      const std::string value = argv[++index];
      if (argument == "--client-id") {
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
                                 const std::optional<std::filesystem::path>& state_root) {
  const char* program = command.kind == ckgit::SshCommandKind::kUploadPack
                            ? "/usr/bin/git-upload-pack"
                            : "/usr/bin/git-receive-pack";
  char* const arguments[] = {const_cast<char*>(program),
                             const_cast<char*>(repository.c_str()), nullptr};
  std::vector<std::string> environment_values{"PATH=/usr/bin:/bin", "LANG=C",
                                               "CKGIT_CLIENT_ID=" + std::string(client_id),
                                               "CKGIT_PROJECT_NAME=" + std::string(project_name)};
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

int dispatch(const Options& options) {
  const char* original = std::getenv("SSH_ORIGINAL_COMMAND");
  if (original == nullptr) {
    std::cerr << "ck-git-shell: SSH_ORIGINAL_COMMAND is not set\n";
    return kDenied;
  }
  std::string reason;
  const auto command = ckgit::parseSshOriginalCommand(original, &reason);
  if (!command.has_value()) {
    std::cerr << "ck-git-shell: denied: " << reason << "\n";
    return kDenied;
  }
  if (command->kind == ckgit::SshCommandKind::kRpc) {
    if (options.dry_run) {
      std::cout << "rpc " << command->rpc_operation;
      if (!command->rpc_argument.empty()) {
        std::cout << " " << command->rpc_argument;
      }
      if (!command->rpc_second_argument.empty()) {
        std::cout << " " << command->rpc_second_argument;
      }
      std::cout << " for " << options.client_id << "\n";
      return 0;
    }
    std::string response;
    const bool success = ckgit::forwardControlRpc(options.control_socket, options.client_id,
                                                  command->rpc_operation, command->rpc_argument,
                                                  command->rpc_second_argument,
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
  execGitService(*command, repository, options.client_id, command->project_name, state_root);
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
