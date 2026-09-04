// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ckgit {

struct ForcedCommandLayout {
  std::filesystem::path shell;
  std::filesystem::path repo_root;
  std::filesystem::path control_socket;
  std::filesystem::path state_root;
};

// Builds one complete OpenSSH authorized_keys line that pairs a device public
// key with the restricted dispatcher.  `public_key` is the content of a
// single-line OpenSSH public key file; its comment is replaced by the client
// ID so a device is named by the administrator, not by the key file.  Any
// unsupported key type, malformed key data, option prefix, or path that
// cannot be embedded verbatim in a quoted forced command is rejected.
std::string renderAuthorizedKeyLine(std::string_view client_id,
                                    std::string_view public_key,
                                    const ForcedCommandLayout& layout);

}  // namespace ckgit
