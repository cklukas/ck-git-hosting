// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/control_rpc.hpp"

#include <array>
#include <algorithm>
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

bool validControlToken(std::string_view token) {
  if (token.empty() || token.size() > 64) {
    return false;
  }
  for (const unsigned char character : token) {
    if (!(character >= 'a' && character <= 'z') &&
        !(character >= 'A' && character <= 'Z') &&
        !(character >= '0' && character <= '9') && character != '-' &&
        character != '_') {
      return false;
    }
  }
  return true;
}

}  // namespace

bool forwardControlRpc(const std::filesystem::path& socket_path,
                       std::string_view client_id,
                       std::string_view operation,
                       std::string_view argument,
                       std::string_view second_argument,
                       std::string* response,
                       std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("control timeout must be positive");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  if (!validControlToken(client_id) || !validControlToken(operation) ||
      (!argument.empty() && !isValidProjectName(argument))) {
    throw std::invalid_argument("invalid control request token");
  }
  const bool no_arguments = argument.empty() && second_argument.empty();
  if (!((operation == "ping" || operation == "list-projects" || operation == "checkouts" ||
         operation == "version" || operation == "versions") && no_arguments) &&
      !((operation == "refs" || operation == "refresh" || operation == "forget-checkout" ||
         operation == "releases") && !argument.empty() && second_argument.empty()) &&
      !(operation == "create" && !argument.empty() && !second_argument.empty() &&
        isValidBranchName(second_argument)) &&
      !((operation == "register" || operation == "replace-checkout") && !argument.empty() &&
        !second_argument.empty() && isValidCheckoutPathToken(second_argument))) {
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
    if (!argument.empty()) {
      request += " ";
      request += argument;
    }
    if (!second_argument.empty()) {
      request += " ";
      request += second_argument;
    }
    request += '\n';
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
