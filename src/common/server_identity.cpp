// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/server_identity.hpp"

#include "ckgit/validation.hpp"

namespace ckgit {

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
  const std::string project = remote.path.substr(0, remote.path.size() - 4);
  if (!isValidProjectName(project)) {
    return std::nullopt;
  }
  return project;
}

}  // namespace ckgit
