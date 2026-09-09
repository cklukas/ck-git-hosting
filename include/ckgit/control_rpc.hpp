// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace ckgit {

// Version-1 responses are bounded: a ref or project listing for a large
// repository must fit, while an unbounded reply can never exhaust a client.
inline constexpr std::size_t kMaximumControlResponseBytes = 256 * 1024;
inline constexpr std::size_t kMaximumControlRefs = 65536;

// Sends the fixed, line-based version-1 request to the local control socket.
// It throws for transport and framing failures, and returns true only for an
// `ok` response.  The response is safe, bounded text for the caller to print.
bool forwardControlRpc(const std::filesystem::path& socket_path,
                       std::string_view client_id,
                       std::string_view operation,
                       std::string_view argument,
                       std::string_view second_argument,
                       std::string* response,
                       std::chrono::milliseconds timeout = std::chrono::seconds(10));

}  // namespace ckgit
