// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

#include "ckgit/metadata_store.hpp"
#include "ckgit/validation.hpp"

namespace {

constexpr std::size_t kMaximumInputBytes = 32 * 1024;
constexpr std::size_t kMaximumRefUpdates = 128;

bool isObjectId(std::string_view value) {
  return (value.size() == 40 || value.size() == 64) &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isxdigit(character) != 0;
         });
}

bool isAllowedRef(std::string_view value) {
  const bool allowed_namespace = value.rfind("refs/heads/", 0) == 0 ||
                                 value.rfind("refs/tags/", 0) == 0;
  return allowed_namespace && value.size() <= 512 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return character >= 0x21 && character <= 0x7e && character != '\\';
         });
}

std::string readUpdates() {
  std::array<char, 1024> buffer{};
  std::string input;
  while (true) {
    const ssize_t received = read(STDIN_FILENO, buffer.data(), buffer.size());
    if (received == 0) {
      return input;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0 || input.size() + static_cast<std::size_t>(received) > kMaximumInputBytes) {
      throw std::runtime_error("post-receive input is invalid or exceeds its limit");
    }
    input.append(buffer.data(), static_cast<std::size_t>(received));
  }
}

void validateUpdates(std::string_view input) {
  if (input.empty()) {
    return;
  }
  if (input.back() != '\n') {
    throw std::runtime_error("post-receive input has invalid framing");
  }
  std::size_t updates = 0;
  std::size_t start = 0;
  while (start < input.size()) {
    const std::size_t newline = input.find('\n', start);
    if (newline == std::string_view::npos || newline == start || ++updates > kMaximumRefUpdates) {
      throw std::runtime_error("post-receive input has too many or malformed updates");
    }
    const std::string_view line = input.substr(start, newline - start);
    const std::size_t first_space = line.find(' ');
    const std::size_t second_space = first_space == std::string_view::npos ? std::string_view::npos :
        line.find(' ', first_space + 1);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
        line.find(' ', second_space + 1) != std::string_view::npos) {
      throw std::runtime_error("post-receive input has invalid update fields");
    }
    const std::string_view old_id = line.substr(0, first_space);
    const std::string_view new_id = line.substr(first_space + 1, second_space - first_space - 1);
    const std::string_view ref_name = line.substr(second_space + 1);
    if (!isObjectId(old_id) || !isObjectId(new_id) || old_id.size() != new_id.size() ||
        !isAllowedRef(ref_name)) {
      throw std::runtime_error("post-receive input contains unsafe ref data");
    }
    start = newline + 1;
  }
}

int run() {
  const char* state_root = std::getenv("CKGIT_STATE_ROOT");
  const char* client_id = std::getenv("CKGIT_CLIENT_ID");
  const char* project_name = std::getenv("CKGIT_PROJECT_NAME");
  if (state_root == nullptr && client_id == nullptr && project_name == nullptr) {
    return 0;  // A local administrative push has no authenticated SSH identity to record.
  }
  if (state_root == nullptr || client_id == nullptr || project_name == nullptr ||
      !ckgit::isValidClientId(client_id) || !ckgit::isValidProjectName(project_name)) {
    throw std::runtime_error("post-receive environment is incomplete or unsafe");
  }
  const std::string updates = readUpdates();
  validateUpdates(updates);
  if (!updates.empty()) {
    ckgit::appendStateEvent(ckgit::validatedMetadataRoot(state_root), "git-push", project_name, client_id);
  }
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const std::exception& error) {
    std::cerr << "ckgit post-receive: " << error.what() << "\n";
    return 1;
  }
}
