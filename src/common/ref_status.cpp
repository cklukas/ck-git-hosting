// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ref_status.hpp"

#include "ckgit/control_rpc.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <stdexcept>
#include <string>

namespace ckgit {
namespace {

bool isSafeRefName(std::string_view name) {
  if (!(name.rfind("refs/heads/", 0) == 0 || name.rfind("refs/tags/", 0) == 0) ||
      name.size() > 1024) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char character) {
    return character > 0x20 && character < 0x7f;
  });
}

bool isObjectId(std::string_view object_id) {
  return (object_id.size() == 40 || object_id.size() == 64) &&
         std::all_of(object_id.begin(), object_id.end(), [](unsigned char character) {
           return std::isxdigit(character) != 0;
         });
}

std::map<std::string, std::string> asRefMap(const std::vector<RefTip>& refs,
                                             std::string_view source) {
  std::map<std::string, std::string> result;
  for (const auto& ref : refs) {
    if (!isSafeRefName(ref.name) || !isObjectId(ref.object_id) ||
        !result.emplace(ref.name, ref.object_id).second) {
      throw std::invalid_argument("invalid or duplicate " + std::string(source) + " ref data");
    }
  }
  return result;
}

}  // namespace

std::vector<RefTip> parseRefsControlResponse(std::string_view response) {
  if (response.empty() || response.size() > kMaximumControlResponseBytes || response.back() != '\n' ||
      response.find('\r') != std::string_view::npos || response.find('\0') != std::string_view::npos) {
    throw std::invalid_argument("invalid refs response framing");
  }
  const std::size_t first_newline = response.find('\n');
  const std::string_view first_line = response.substr(0, first_newline);
  constexpr std::string_view prefix{"ok "};
  if (first_line.rfind(prefix, 0) != 0 || first_line.size() == prefix.size()) {
    throw std::invalid_argument("refs response has no valid ok count");
  }
  std::size_t expected = 0;
  const auto [count_end, count_error] = std::from_chars(
      first_line.data() + prefix.size(), first_line.data() + first_line.size(), expected);
  if (count_error != std::errc{} || count_end != first_line.data() + first_line.size() ||
      expected > kMaximumControlRefs) {
    throw std::invalid_argument("refs response has an invalid count");
  }

  std::vector<RefTip> refs;
  std::size_t start = first_newline + 1;
  while (start < response.size()) {
    const std::size_t newline = response.find('\n', start);
    if (newline == std::string_view::npos || newline == start) {
      throw std::invalid_argument("refs response has an empty or unterminated record");
    }
    const std::string_view line = response.substr(start, newline - start);
    const std::size_t separator = line.find(' ');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 == line.size() ||
        line.find(' ', separator + 1) != std::string_view::npos) {
      throw std::invalid_argument("refs response has an invalid ref record");
    }
    const std::string_view name = line.substr(0, separator);
    const std::string_view object_id = line.substr(separator + 1);
    if (!isSafeRefName(name) || !isObjectId(object_id)) {
      throw std::invalid_argument("refs response has unsafe ref data");
    }
    refs.push_back(RefTip{std::string(name), std::string(object_id)});
    start = newline + 1;
  }
  if (refs.size() != expected) {
    throw std::invalid_argument("refs response count does not match records");
  }
  static_cast<void>(asRefMap(refs, "server"));
  return refs;
}

std::vector<RefStatus> compareRefTips(const std::vector<RefTip>& local,
                                      const std::vector<RefTip>& server) {
  const auto local_refs = asRefMap(local, "local");
  const auto server_refs = asRefMap(server, "server");
  std::vector<RefStatus> status;
  auto local_it = local_refs.begin();
  auto server_it = server_refs.begin();
  while (local_it != local_refs.end() || server_it != server_refs.end()) {
    if (server_it == server_refs.end() ||
        (local_it != local_refs.end() && local_it->first < server_it->first)) {
      status.push_back(RefStatus{local_it->first, RefRelation::kLocalOnly, local_it->second, {}});
      ++local_it;
    } else if (local_it == local_refs.end() || server_it->first < local_it->first) {
      status.push_back(RefStatus{server_it->first, RefRelation::kServerOnly, {}, server_it->second});
      ++server_it;
    } else {
      status.push_back(RefStatus{local_it->first,
                                 local_it->second == server_it->second ? RefRelation::kEqual
                                                                       : RefRelation::kMismatched,
                                 local_it->second, server_it->second});
      ++local_it;
      ++server_it;
    }
  }
  return status;
}

}  // namespace ckgit
