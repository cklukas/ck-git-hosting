// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "ckgit/ci_vm_protocol.hpp"

namespace ckgit {
// Handles one guest's result stream. Any invalid message or failed lease is a
// terminal error; the owner must stop the VM and abandon or quarantine it.
// Returns an uncommitted terminal record; the owner publishes it only after
// confirmed VM shutdown and workspace cleanup.
CiRunRecord relayCiVmResults(CiVmChannel& channel, const CiPreparedJob& assignment, const CiRemotePolicy& policy,
                             CiResultSink& sink, const std::filesystem::path& relay_root);
}
