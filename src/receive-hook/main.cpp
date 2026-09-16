// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

#include "ckgit/ci_store.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/control_rpc.hpp"
#include "ckgit/repository_store.hpp"
#include "ckgit/validation.hpp"

namespace {

// One push may update every branch and tag of a large history at once; the
// bounds only need to exclude absurd input, not a real first publish.
constexpr std::size_t kMaximumInputBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumRefUpdates = 200000;

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

// A CI-enabled push queues one job per updated branch head (never deletions or
// tags) for the separate runner. Like a dashboard event, a failure here must
// never fail an already-accepted push.
void enqueueCiJobs(const char* state_root, const std::string& project, const std::string& client_id,
                   std::string_view updates) {
  if (state_root == nullptr || !ckgit::isProjectCiEnabled(state_root, project)) return;
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  std::size_t start = 0;
  while (start < updates.size()) {
    const std::size_t newline = updates.find('\n', start);
    const std::string_view line = updates.substr(start, newline - start);
    start = newline + 1;
    const std::size_t first = line.find(' ');
    const std::size_t second = line.find(' ', first + 1);
    const std::string_view new_id = line.substr(first + 1, second - first - 1);
    const std::string_view ref = line.substr(second + 1);
    if (ref.rfind("refs/heads/", 0) != 0) continue;                        // branch heads only
    if (new_id.find_first_not_of('0') == std::string_view::npos) continue;  // a deletion
    ckgit::CiJobRequest job;
    job.job_id = ckgit::generateCiId();
    job.project_name = project;
    job.ref = std::string(ref);
    job.commit_id = std::string(new_id);
    job.client_id = client_id;
    job.queued_epoch_seconds = now;
    ckgit::enqueueCiJob(state_root, job);
  }
}

int run() {
  const char* state_root = std::getenv("CKGIT_STATE_ROOT");
  const char* client_id = std::getenv("CKGIT_CLIENT_ID");
  const char* project_name = std::getenv("CKGIT_PROJECT_NAME");
  const char* control_socket = std::getenv("CKGIT_CONTROL_SOCKET");
  const char* repository_root = std::getenv("CKGIT_REPOSITORY_ROOT");
  if (state_root == nullptr && client_id == nullptr && project_name == nullptr) {
    return 0;  // A local administrative push has no authenticated SSH identity to record.
  }
  if (client_id == nullptr || project_name == nullptr ||
      !ckgit::isValidClientId(client_id) || !ckgit::isValidProjectName(project_name)) {
    throw std::runtime_error("post-receive environment is incomplete or unsafe");
  }
  const std::string updates = readUpdates();
  validateUpdates(updates);
  if (!updates.empty()) {
    std::exception_ptr event_failure;
    if (state_root != nullptr) {
      try {
        if (repository_root != nullptr) {
          ckgit::appendHostedStateEvent(repository_root, ckgit::validatedMetadataRoot(state_root),
                                         "git-push", project_name, client_id);
        } else {
          ckgit::appendStateEvent(ckgit::validatedMetadataRoot(state_root), "git-push", project_name, client_id);
        }
      } catch (...) {
        event_failure = std::current_exception();
      }
    }
    if (control_socket != nullptr) {
      try {
        ckgit::forwardControlRpc(control_socket, client_id, "refresh", project_name, {}, nullptr,
                                 std::chrono::seconds(2));
      } catch (const std::exception&) {
        // Index availability must never delay or fail an already accepted push.
      }
    }
    try {
      enqueueCiJobs(state_root, project_name, client_id, updates);
    } catch (const std::exception& error) {
      std::cerr << "ckgit post-receive: CI enqueue failed: " << error.what() << "\n";
    }
    if (event_failure) std::rethrow_exception(event_failure);
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
