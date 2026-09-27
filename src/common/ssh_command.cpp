// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ssh_command.hpp"

#include <cctype>
#include <vector>

#include "ckgit/control_rpc.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

struct Token {
  std::string value;
  bool quoted{false};
};

// `ckgit-rpc`, the protocol version, the operation, and its arguments.
constexpr std::size_t kMaximumCommandTokens = 3 + kMaximumControlArguments;

bool allowedBareTokenCharacter(unsigned char character) {
  return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.';
}

void reject(std::string_view message, std::string* reason) {
  if (reason != nullptr) {
    *reason = std::string(message);
  }
}

std::optional<std::vector<Token>> tokenize(std::string_view command, std::string* reason) {
  if (command.empty() || command.size() > 1024) {
    reject("command is empty or exceeds 1024 bytes", reason);
    return std::nullopt;
  }
  std::vector<Token> tokens;
  std::size_t position = 0;
  bool first_token = true;
  while (position < command.size()) {
    if (!first_token) {
      if (command[position] != ' ') {
        reject("command uses invalid whitespace or token syntax", reason);
        return std::nullopt;
      }
      while (position < command.size() && command[position] == ' ') {
        ++position;
      }
      if (position == command.size()) {
        reject("command has trailing whitespace", reason);
        return std::nullopt;
      }
    }
    Token token;
    if (command[position] == '\'') {
      token.quoted = true;
      ++position;
      const std::size_t start = position;
      while (position < command.size() && command[position] != '\'') {
        const unsigned char character = command[position];
        if (character < 0x20 || character == 0x7f) {
          reject("quoted token contains a control character", reason);
          return std::nullopt;
        }
        ++position;
      }
      if (position == command.size() || position == start) {
        reject("quoted token is malformed", reason);
        return std::nullopt;
      }
      token.value = std::string(command.substr(start, position - start));
      ++position;
      if (position < command.size() && command[position] != ' ') {
        reject("quoted token must end at a token boundary", reason);
        return std::nullopt;
      }
    } else {
      const std::size_t start = position;
      while (position < command.size() && command[position] != ' ') {
        if (!allowedBareTokenCharacter(static_cast<unsigned char>(command[position]))) {
          reject("command contains an unsupported character", reason);
          return std::nullopt;
        }
        ++position;
      }
      if (position == start) {
        reject("command contains an empty token", reason);
        return std::nullopt;
      }
      token.value = std::string(command.substr(start, position - start));
    }
    tokens.push_back(std::move(token));
    first_token = false;
    if (tokens.size() > kMaximumCommandTokens) {
      reject("command has too many arguments", reason);
      return std::nullopt;
    }
  }
  return tokens;
}

std::optional<std::string> projectFromRepositoryArgument(const Token& token,
                                                          std::string* reason) {
  constexpr std::string_view suffix{".git"};
  if (!token.quoted || token.value.size() <= suffix.size() ||
      token.value.substr(token.value.size() - suffix.size()) != suffix) {
    reject("Git service requires one quoted project.git argument", reason);
    return std::nullopt;
  }
  std::string project = token.value.substr(0, token.value.size() - suffix.size());
  if (project.starts_with("./-")) project.erase(0, 2);
  if (!isValidProjectName(project)) {
    reject("repository argument is not a valid project", reason);
    return std::nullopt;
  }
  return project;
}

}  // namespace

std::optional<SshCommand> parseSshOriginalCommand(std::string_view command,
                                                   std::string* reason) {
  if (reason != nullptr) {
    reason->clear();
  }
  const auto tokens = tokenize(command, reason);
  if (!tokens.has_value()) {
    return std::nullopt;
  }
  if (tokens->size() == 2 && ((*tokens)[0].value == "git-upload-pack" ||
                              (*tokens)[0].value == "git-receive-pack")) {
    const auto project = projectFromRepositoryArgument((*tokens)[1], reason);
    if (!project.has_value()) {
      return std::nullopt;
    }
    return SshCommand{(*tokens)[0].value == "git-upload-pack" ? SshCommandKind::kUploadPack
                                                                 : SshCommandKind::kReceivePack,
                      *project, {}, {}};
  }
  if (tokens->size() >= 3 && (*tokens)[0].value == "ckgit-rpc" && !(*tokens)[0].quoted) {
    if ((*tokens)[1].quoted || (*tokens)[1].value != "1") {
      reject("unsupported ckgit-rpc protocol version", reason);
      return std::nullopt;
    }
    std::vector<std::string> arguments;
    for (std::size_t index = 2; index < tokens->size(); ++index) {
      if ((*tokens)[index].quoted) {
        reject("ckgit-rpc operations take only unquoted tokens", reason);
        return std::nullopt;
      }
      if (index > 2) arguments.push_back((*tokens)[index].value);
    }
    if (!isValidControlOperation((*tokens)[2].value, arguments)) {
      reject("command is not an allowed ckgit-rpc operation", reason);
      return std::nullopt;
    }
    return SshCommand{SshCommandKind::kRpc, {}, (*tokens)[2].value, std::move(arguments)};
  }
  reject("command is not an allowed Git or ckgit-rpc operation", reason);
  return std::nullopt;
}

}  // namespace ckgit
