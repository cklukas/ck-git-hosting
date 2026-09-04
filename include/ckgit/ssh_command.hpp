// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ckgit {

enum class SshCommandKind { kUploadPack, kReceivePack, kRpc };

struct SshCommand {
  SshCommandKind kind;
  std::string project_name;
  std::string rpc_operation;
  std::string rpc_argument;
  std::string rpc_second_argument;
};

// Parses the complete SSH_ORIGINAL_COMMAND grammar without shell expansion.
// An empty optional means rejection; when supplied, `reason` is safe text for
// an audit log or an SSH client error message.
std::optional<SshCommand> parseSshOriginalCommand(std::string_view command,
                                                   std::string* reason = nullptr);

}  // namespace ckgit
