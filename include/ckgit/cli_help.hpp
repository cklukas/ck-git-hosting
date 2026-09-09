// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ckgit {

// Parsing, help, and shell completion use the same command definitions. This
// entry point does not inspect configuration, Git, the filesystem, or the network.
// Pass argv without the executable name. A populated exit_code means the caller
// should print the supplied streams and exit; otherwise dispatch command with
// arguments. Normalized arguments retain nested command names for their handler.
struct CliInvocation {
  std::string command;
  std::vector<std::string> arguments;
  std::optional<int> exit_code;
  std::string standard_output;
  std::string standard_error;
};

CliInvocation prepareClientInvocation(const std::vector<std::string>& arguments);

// Empty command_path selects the global page. Unknown paths are rejected by
// prepareClientInvocation rather than being passed here.
std::string clientHelp(const std::vector<std::string>& command_path = {});
// The running binary's build identifier, shared by CLI version output and the
// dashboard. Contains neither a product prefix nor a trailing newline.
std::string buildVersion();
std::string clientVersion();

}  // namespace ckgit
