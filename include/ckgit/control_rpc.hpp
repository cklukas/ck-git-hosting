// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ckgit {

// Sends the fixed, line-based version-1 request to the local control socket.
// It throws for transport and framing failures, and returns true only for an
// `ok` response.  The response is safe, bounded text for the caller to print.
bool forwardControlRpc(const std::filesystem::path& socket_path,
                       std::string_view client_id,
                       std::string_view operation,
                       std::string_view argument,
                       std::string_view second_argument,
                       std::string* response);

}  // namespace ckgit
