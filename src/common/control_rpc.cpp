// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/control_rpc.hpp"

#include <array>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ckgit/ci_store.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/metadata_store.hpp"

namespace ckgit {
namespace {

using Deadline = std::chrono::steady_clock::time_point;

void waitForSocket(int descriptor, short events, Deadline deadline) {
  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) {
      throw std::runtime_error("control request timed out");
    }
    pollfd state{descriptor, events, 0};
    const int ready = poll(&state, 1, static_cast<int>(std::min<std::int64_t>(
        remaining, std::numeric_limits<int>::max())));
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready < 0) {
      throw std::runtime_error("could not wait for control socket");
    }
    if (ready != 0) {
      return;
    }
  }
}

void sendAll(int descriptor, std::string_view message, Deadline deadline) {
  std::size_t offset = 0;
  while (offset < message.size()) {
    waitForSocket(descriptor, POLLOUT, deadline);
    const ssize_t sent = send(descriptor, message.data() + offset, message.size() - offset,
                              MSG_NOSIGNAL);
    if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    if (sent <= 0) {
      throw std::runtime_error("could not write control request: " +
                               std::string(std::strerror(errno)));
    }
    offset += static_cast<std::size_t>(sent);
  }
}

// The kinds of positional argument an operation takes, each with one validator.
enum class ArgumentKind { kProject, kBranch, kCheckoutPath, kCiRunId, kStepIndex, kByteOffset };

struct OperationGrammar {
  std::string_view name;
  std::size_t arity;
  std::array<ArgumentKind, kMaximumControlArguments> arguments;
};

// Every version-1 operation and its exact positional arguments. Documented in
// docs/protocol/01-ssh-and-control-v1.md; an operation is added here only
// after its grammar, limits, authorization, and negative tests are there.
constexpr ArgumentKind kProject = ArgumentKind::kProject;
constexpr ArgumentKind kRunId = ArgumentKind::kCiRunId;
constexpr std::array kOperations{
    OperationGrammar{"ping", 0, {}},
    OperationGrammar{"version", 0, {}},
    OperationGrammar{"versions", 0, {}},
    OperationGrammar{"list-projects", 0, {}},
    OperationGrammar{"checkouts", 0, {}},
    OperationGrammar{"ci-overview", 0, {}},
    OperationGrammar{"refs", 1, {kProject}},
    OperationGrammar{"refresh", 1, {kProject}},
    OperationGrammar{"forget-checkout", 1, {kProject}},
    OperationGrammar{"releases", 1, {kProject}},
    OperationGrammar{"ci-status", 1, {kProject}},
    OperationGrammar{"create", 2, {kProject, ArgumentKind::kBranch}},
    OperationGrammar{"register", 2, {kProject, ArgumentKind::kCheckoutPath}},
    OperationGrammar{"replace-checkout", 2, {kProject, ArgumentKind::kCheckoutPath}},
    OperationGrammar{"ci-run", 2, {kProject, kRunId}},
    OperationGrammar{"ci-cancel", 2, {kProject, kRunId}},
    OperationGrammar{"ci-log", 4, {kProject, kRunId, ArgumentKind::kStepIndex, ArgumentKind::kByteOffset}},
};

// A canonical unsigned decimal: digits only, no sign, and no leading zero
// unless the value is exactly 0, so every number has one spelling.
bool isCanonicalDecimal(std::string_view value, std::size_t maximum_digits) {
  return !value.empty() && value.size() <= maximum_digits && (value.size() == 1 || value.front() != '0') &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return character >= '0' && character <= '9';
         });
}

bool isValidArgument(ArgumentKind kind, std::string_view value) {
  switch (kind) {
    case ArgumentKind::kProject: return isValidProjectName(value);
    case ArgumentKind::kBranch: return isValidBranchName(value);
    case ArgumentKind::kCheckoutPath: return isValidCheckoutPathToken(value);
    case ArgumentKind::kCiRunId: return isValidCiId(value);
    case ArgumentKind::kStepIndex: {
      std::size_t step = 0;
      return isCanonicalDecimal(value, 4) &&
             std::from_chars(value.data(), value.data() + value.size(), step).ec == std::errc{} &&
             step <= kMaximumCiSteps;
    }
    case ArgumentKind::kByteOffset: {
      std::uint64_t offset = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), offset);
      return isCanonicalDecimal(value, 20) && error == std::errc{} && end == value.data() + value.size();
    }
  }
  return false;
}

}  // namespace

bool isValidControlOperation(std::string_view operation, const std::vector<std::string>& arguments) {
  const auto grammar = std::find_if(kOperations.begin(), kOperations.end(),
                                    [&](const OperationGrammar& entry) { return entry.name == operation; });
  if (grammar == kOperations.end() || arguments.size() != grammar->arity) return false;
  for (std::size_t index = 0; index < grammar->arity; ++index) {
    if (!isValidArgument(grammar->arguments[index], arguments[index])) return false;
  }
  return true;
}

std::optional<ControlRequest> parseControlRequest(std::string_view line) {
  if (line.empty() || line.size() > kMaximumControlRequestBytes || line.back() != '\n' ||
      line.find('\n') != line.size() - 1) {
    return std::nullopt;
  }
  for (const unsigned char character : line.substr(0, line.size() - 1)) {
    if (character < 0x20 || character > 0x7e) return std::nullopt;
  }
  constexpr std::string_view kPrefix{"CKGIT-CONTROL/1 "};
  const std::string_view content = line.substr(0, line.size() - 1);
  if (content.rfind(kPrefix, 0) != 0) return std::nullopt;
  std::vector<std::string> tokens;
  const std::string_view rest = content.substr(kPrefix.size());
  std::size_t start = 0;
  while (start <= rest.size()) {
    const std::size_t end = rest.find(' ', start);
    const std::string_view token =
        rest.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (token.empty() || tokens.size() == 2 + kMaximumControlArguments) return std::nullopt;
    tokens.emplace_back(token);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  if (tokens.size() < 2 || !isValidClientId(tokens[0])) return std::nullopt;
  ControlRequest request{tokens[0], tokens[1], std::vector<std::string>(tokens.begin() + 2, tokens.end())};
  if (!isValidControlOperation(request.operation, request.arguments)) return std::nullopt;
  return request;
}

bool forwardControlRpc(const std::filesystem::path& socket_path,
                       std::string_view client_id,
                       std::string_view operation,
                       const std::vector<std::string>& arguments,
                       std::string* response,
                       std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("control timeout must be positive");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  if (!isValidClientId(client_id) || !isValidControlOperation(operation, arguments)) {
    throw std::invalid_argument("invalid control operation or argument");
  }
  const std::string path = socket_path.string();
  sockaddr_un address{};
  if (path.empty() || path.size() >= sizeof(address.sun_path)) {
    throw std::invalid_argument("invalid control socket path");
  }

  const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0) {
    throw std::runtime_error("could not create control socket: " +
                             std::string(std::strerror(errno)));
  }
  bool closed = false;
  try {
    if (fcntl(descriptor, F_SETFL, O_NONBLOCK) != 0 || fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
      throw std::runtime_error("could not configure control socket");
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    if (connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      if (errno != EINPROGRESS) {
        throw std::runtime_error("could not connect to control socket: " + std::string(std::strerror(errno)));
      }
      waitForSocket(descriptor, POLLOUT, deadline);
      int socket_error = 0;
      socklen_t error_size = sizeof(socket_error);
      if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0 || socket_error != 0) {
        throw std::runtime_error("could not connect to control socket");
      }
    }
    std::string request = "CKGIT-CONTROL/1 " + std::string(client_id) + " " +
                          std::string(operation);
    for (const auto& argument : arguments) {
      request += ' ';
      request += argument;
    }
    request += '\n';
    if (request.size() > kMaximumControlRequestBytes) {
      throw std::invalid_argument("control request exceeds its size limit");
    }
    sendAll(descriptor, request, deadline);
    if (shutdown(descriptor, SHUT_WR) != 0) {
      throw std::runtime_error("could not finish control request: " +
                               std::string(std::strerror(errno)));
    }
    std::array<char, 4096> buffer{};
    std::string reply;
    while (true) {
      waitForSocket(descriptor, POLLIN, deadline);
      const ssize_t received = recv(descriptor, buffer.data(), buffer.size(), 0);
      if (received == 0) {
        break;
      }
      if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      if (received < 0) {
        throw std::runtime_error("could not read control response: " +
                                 std::string(std::strerror(errno)));
      }
      if (reply.size() + static_cast<std::size_t>(received) > kMaximumControlResponseBytes) {
        throw std::runtime_error("control response exceeds its size limit");
      }
      reply.append(buffer.data(), static_cast<std::size_t>(received));
    }
    close(descriptor);
    closed = true;
    if (reply.empty() || reply.back() != '\n' || reply.find('\r') != std::string::npos ||
        reply.find('\0') != std::string::npos) {
      throw std::runtime_error("control response has invalid framing");
    }
    for (const unsigned char character : reply) {
      if (character != '\n' && (character < 0x20 || character > 0x7e)) {
        throw std::runtime_error("control response contains unsafe text");
      }
    }
    if (response != nullptr) {
      *response = std::move(reply);
    }
    const std::string& verified_response = response == nullptr ? reply : *response;
    return verified_response == "ok\n" || verified_response.rfind("ok ", 0) == 0;
  } catch (...) {
    if (!closed) {
      close(descriptor);
    }
    throw;
  }
}

}  // namespace ckgit
