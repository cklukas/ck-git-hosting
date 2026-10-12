// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include "ckgit/ci_runner_protocol.hpp"
#include "ckgit/server_config.hpp"

namespace ckgit {

// The main host's side of the runner protocol (ci_runner_protocol.hpp). It runs
// as the service account, started by the SSH dispatcher for a runner key as
// `ck-ci-runnerd gateway`, and works on the queue and the state root with the
// same store functions the primary runner uses. It never executes project code:
// it claims and prepares jobs, relays a run's progress into its run record and
// step logs, stages uploaded outputs, and commits a finished run.
//
// `runner` is the runner's client id (from its key's forced command). `in` and
// `out` are the SSH session's stdin and stdout. Returns the process exit code:
// 0 on success, 1 when the request was refused (the reason is on `out`).
// `upload-pack` does not return on success: it executes git-upload-pack.
int runCiGateway(const ServerConfig& config, const std::string& runner, const CiRunnerCommand& command, int in,
                 int out);

}  // namespace ckgit
