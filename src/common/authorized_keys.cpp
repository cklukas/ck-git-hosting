// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/authorized_keys.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

constexpr std::size_t kMaximumKeyBytes = 8 * 1024;
constexpr std::size_t kMaximumPathBytes = 256;

constexpr std::array<std::string_view, 7> kSupportedKeyTypes{
    "ssh-ed25519",
    "sk-ssh-ed25519@openssh.com",
    "ecdsa-sha2-nistp256",
    "ecdsa-sha2-nistp384",
    "ecdsa-sha2-nistp521",
    "sk-ecdsa-sha2-nistp256@openssh.com",
    "ssh-rsa",
};

bool isBase64Body(std::string_view value) {
  if (value.size() < 16 || value.size() % 4 != 0) {
    return false;
  }
  std::size_t end = value.size();
  while (end > 0 && value[end - 1] == '=') {
    --end;
  }
  if (value.size() - end > 2) {
    return false;
  }
  return std::all_of(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(end),
                     [](unsigned char character) {
                       return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                              (character >= '0' && character <= '9') || character == '+' || character == '/';
                     });
}

// The forced command is written inside double quotes.  Instead of escaping,
// only a narrow portable character set is accepted so the printed line means
// exactly what sshd will parse.
std::string embeddablePath(const std::filesystem::path& path, std::string_view description) {
  const std::string value = path.string();
  if (value.empty() || value.size() > kMaximumPathBytes || value.front() != '/' ||
      !std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '/' || character == '.' ||
               character == '_' || character == '-';
      }) ||
      value.find("/../") != std::string::npos || value.find("/./") != std::string::npos) {
    throw std::invalid_argument(std::string(description) +
                                " must be an absolute path using only letters, digits, '/', '.', '_', and '-'");
  }
  return value;
}

}  // namespace

std::string renderAuthorizedKeyLine(std::string_view client_id, std::string_view public_key,
                                    const ForcedCommandLayout& layout) {
  if (!isValidClientId(client_id)) {
    throw std::invalid_argument("client ID must use only letters, digits, '-', and '_'");
  }
  if (public_key.empty() || public_key.size() > kMaximumKeyBytes) {
    throw std::invalid_argument("public key file is empty or exceeds its limit");
  }
  std::string_view line = public_key;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.remove_suffix(1);
  }
  if (line.empty() || line.find('\n') != std::string_view::npos ||
      std::any_of(line.begin(), line.end(), [](unsigned char character) {
        return character < 0x20 || character > 0x7e;
      })) {
    throw std::invalid_argument("public key file must contain exactly one printable ASCII line");
  }
  const std::size_t first_space = line.find(' ');
  if (first_space == std::string_view::npos) {
    throw std::invalid_argument("public key line must contain a key type and key data");
  }
  const std::string_view key_type = line.substr(0, first_space);
  if (std::find(kSupportedKeyTypes.begin(), kSupportedKeyTypes.end(), key_type) == kSupportedKeyTypes.end()) {
    throw std::invalid_argument("public key type is unsupported; the line must begin with a plain key type, not options");
  }
  std::string_view remainder = line.substr(first_space + 1);
  const std::size_t second_space = remainder.find(' ');
  const std::string_view key_data = remainder.substr(0, second_space);
  if (!isBase64Body(key_data)) {
    throw std::invalid_argument("public key data is not valid base64");
  }
  const std::string command = embeddablePath(layout.shell, "dispatcher path") + " --client-id " +
                              std::string(client_id) + " --repo-root " +
                              embeddablePath(layout.repo_root, "repository root") + " --control-socket " +
                              embeddablePath(layout.control_socket, "control socket") + " --state-root " +
                              embeddablePath(layout.state_root, "state root");
  return "restrict,command=\"" + command + "\" " + std::string(key_type) + " " + std::string(key_data) + " " +
         std::string(client_id) + "\n";
}

}  // namespace ckgit
