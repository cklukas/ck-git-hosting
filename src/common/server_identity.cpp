// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/server_identity.hpp"

#include "ckgit/validation.hpp"
#include <stdexcept>

namespace ckgit {

std::string hostedRepositoryUrl(std::string_view server, std::string_view project) {
  if (!isValidProjectName(project)) throw std::invalid_argument("invalid hosted project name");
  return std::string(server) + ":" + (project.front() == '-' ? "./" : "") + std::string(project) + ".git";
}

std::optional<std::string> projectForConfiguredServerRemote(const RemoteUrl& remote,
                                                            const ClientConfig& config) {
  const std::size_t at = config.server.find('@');
  if (at == std::string::npos ||
      (remote.transport != RemoteTransport::kScpLikeSsh && remote.transport != RemoteTransport::kSshUrl) ||
      remote.user != config.server.substr(0, at) || remote.host != config.server.substr(at + 1) ||
      remote.port.has_value() || remote.path.size() <= 4 ||
      remote.path.substr(remote.path.size() - 4) != ".git") {
    return std::nullopt;
  }
  std::string project = remote.path.substr(0, remote.path.size() - 4);
  if (project.starts_with("./-")) project.erase(0, 2);
  if (!isValidProjectName(project)) {
    return std::nullopt;
  }
  return project;
}

}  // namespace ckgit
