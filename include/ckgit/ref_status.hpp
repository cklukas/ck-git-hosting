// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ckgit/git_repository.hpp"

namespace ckgit {

enum class RefRelation { kEqual, kLocalOnly, kServerOnly, kMismatched };

struct RefStatus {
  std::string name;
  RefRelation relation;
  std::string local_object_id;
  std::string server_object_id;
};

// Parses a bounded, version-1 `refs` control response.  It accepts only branch
// and tag names plus SHA-1 or SHA-256 object IDs.
std::vector<RefTip> parseRefsControlResponse(std::string_view response);

// Compares exact tips only.  A mismatched tip is deliberately not called
// fast-forward or divergent: the client has not fetched any server objects.
std::vector<RefStatus> compareRefTips(const std::vector<RefTip>& local,
                                      const std::vector<RefTip>& server);

}  // namespace ckgit
