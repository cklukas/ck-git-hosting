// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "ckgit/authorized_keys.hpp"
#include "ckgit/install_layout.hpp"
#include "ckgit/repository_store.hpp"

namespace {

constexpr int kUsage = 2;
constexpr std::size_t kMaximumKeyFileBytes = 8 * 1024;

void usage(std::ostream& output) {
  output << "Usage:\n"
         << "  ckgit-admin create NAME --repo-root ROOT [--default-branch BRANCH] [--hook-directory PATH] [--dry-run]\n"
         << "  ckgit-admin authorized-key --client-id ID --public-key FILE [--shell PATH] [--repo-root ROOT]\n"
         << "                             [--control-socket PATH] [--state-root ROOT]\n"
         << "\n"
         << "authorized-key prints one restricted OpenSSH authorized_keys line for a device key.\n"
         << "It never edits a file; append the line to the installed authorized_keys yourself.\n";
}

int create(int argc, char* argv[]) {
  if (argc < 3) {
    usage(std::cerr);
    return kUsage;
  }
  const std::string name = argv[2];
  std::filesystem::path root;
  std::optional<std::filesystem::path> hook_directory;
  std::string branch{"main"};
  bool dry_run = false;
  for (int index = 3; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--repo-root" || argument == "--default-branch" || argument == "--hook-directory") {
      if (index + 1 == argc) {
        std::cerr << "ckgit-admin: " << argument << " requires a value\n";
        return kUsage;
      }
      const std::string value = argv[++index];
      if (argument == "--repo-root") {
        root = value;
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
  if (root.empty()) {
    std::cerr << "ckgit-admin: --repo-root is required\n";
    return kUsage;
  }
  const auto repository = ckgit::createBareRepository(root, name, branch, dry_run, hook_directory);
  std::cout << (dry_run ? "Would create " : "Created ") << repository.string() << "\n";
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
    if (argc >= 2 && std::string(argv[1]) == "create") {
      return create(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "authorized-key") {
      return authorizedKey(argc, argv);
    }
    usage(std::cerr);
    return kUsage;
  } catch (const std::exception& error) {
    std::cerr << "ckgit-admin: " << error.what() << "\n";
    return 1;
  }
}
