// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "ckgit/ci_runner.hpp"

namespace ckgit {
// Materialise exactly the host assignment into a new `source/<ordinal>` tree
// beneath the private VM exchange directory before it is shared read-only.
// On failure the owning VM workspace remains responsible for cleanup.
void stageCiVmSources(const CiPreparedJob& assignment, CiSourceProvider& source,
                      const std::filesystem::path& exchange, const std::function<bool()>& should_continue = {});
}
