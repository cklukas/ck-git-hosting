// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "ckgit/client_config.hpp"
#include "ckgit/remote_url.hpp"

namespace ckgit {

// Git rejects a transport path beginning with '-'. The narrow './' escape is
// understood by the restricted dispatcher without admitting arbitrary paths.
std::string hostedRepositoryUrl(std::string_view server, std::string_view project);

// Returns a project only when a configured remote matches the paired SSH user
// and host exactly and has the first-release `project.git` path shape.
std::optional<std::string> projectForConfiguredServerRemote(const RemoteUrl& remote,
                                                            const ClientConfig& config);

}  // namespace ckgit
