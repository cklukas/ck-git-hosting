// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// ck-pagesd serves published static sites ("Pages") on their own port, a
// distinct origin from the dashboard. It is a separate, minimal process on
// purpose: it serves arbitrary project-generated content, so it is kept away
// from the daemon's control socket, admin surface, and metadata store. It only
// ever reads regular files under pages_root and never executes anything.

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "ckgit/http_request.hpp"
#include "ckgit/pages_store.hpp"
#include "ckgit/server_config.hpp"
#include "ckgit/validation.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onStop(int) { g_stop = 1; }

constexpr std::size_t kMaximumRequestBytes = 16 * 1024;
constexpr std::size_t kMaximumServedBytes = static_cast<std::size_t>(64) << 20;

int usage(std::ostream& out, int code) {
  out << "usage: ck-pagesd serve --config FILE\n\n"
         "Serves published static sites from pages_root on pages_http_port. Both\n"
         "must be set in the configuration file.\n";
  return code;
}

// Reads the request header block (up to the blank line), bounded.
bool readHeaders(int fd, std::string* out) {
  char buffer[4096];
  while (out->find("\r\n\r\n") == std::string::npos) {
    if (out->size() > kMaximumRequestBytes) return false;
    const ssize_t received = ::read(fd, buffer, sizeof(buffer));
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) return false;
    out->append(buffer, static_cast<std::size_t>(received));
  }
  return true;
}

bool writeAll(int fd, std::string_view data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

int hexNibble(unsigned char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

// Decodes %XX escapes; returns false on a malformed or control-byte result.
bool percentDecode(std::string_view input, std::string* out) {
  for (std::size_t index = 0; index < input.size(); ++index) {
    if (input[index] == '%') {
      if (index + 2 >= input.size()) return false;
      const int high = hexNibble(static_cast<unsigned char>(input[index + 1]));
      const int low = hexNibble(static_cast<unsigned char>(input[index + 2]));
      if (high < 0 || low < 0) return false;
      const unsigned char byte = static_cast<unsigned char>((high << 4) | low);
      if (byte < 0x20 || byte == 0x7f) return false;
      out->push_back(static_cast<char>(byte));
      index += 2;
    } else {
      out->push_back(input[index]);
    }
  }
  return true;
}

void sendResponse(int fd, int status, std::string_view reason, std::string_view content_type,
                  std::string_view body, bool head_only) {
  std::string headers = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
                        "\r\nContent-Type: " + std::string(content_type) +
                        "\r\nContent-Length: " + std::to_string(body.size()) +
                        "\r\nX-Content-Type-Options: nosniff"
                        "\r\nReferrer-Policy: no-referrer"
                        "\r\nCache-Control: no-cache"
                        "\r\nConnection: close\r\n\r\n";
  if (!writeAll(fd, headers)) return;
  if (!head_only) writeAll(fd, body);
}

void sendError(int fd, int status, std::string_view reason, bool head_only) {
  const std::string body = "<!doctype html><meta charset=utf-8><title>" + std::to_string(status) +
                           "</title><p>" + std::string(reason) + "</p>\n";
  sendResponse(fd, status, reason, "text/html; charset=utf-8", body, head_only);
}

void handle(int fd, const std::filesystem::path& pages_root) {
  std::string raw;
  if (!readHeaders(fd, &raw)) return;
  const std::optional<ckgit::HttpRequest> request = ckgit::parseReadOnlyHttpRequest(raw);
  if (!request.has_value()) {
    sendError(fd, 400, "Bad Request", false);
    return;
  }
  const bool head_only = request->method == ckgit::HttpMethod::kHead;

  std::string_view target = request->target;
  const auto query = target.find_first_of("?#");
  if (query != std::string_view::npos) target = target.substr(0, query);

  std::string decoded;
  if (!percentDecode(target, &decoded) || decoded.empty() || decoded.front() != '/') {
    sendError(fd, 404, "Not Found", head_only);
    return;
  }
  std::string_view remainder = std::string_view(decoded).substr(1);  // drop leading '/'
  const auto slash = remainder.find('/');
  const std::string_view project = remainder.substr(0, slash);
  const std::string_view path = slash == std::string_view::npos ? std::string_view{} : remainder.substr(slash + 1);
  if (!ckgit::isValidProjectName(project)) {
    sendError(fd, 404, "Not Found", head_only);
    return;
  }
  const std::optional<ckgit::PageFile> page =
      ckgit::readCurrentPage(pages_root, project, path, kMaximumServedBytes);
  if (!page.has_value()) {
    sendError(fd, 404, "Not Found", head_only);
    return;
  }
  sendResponse(fd, 200, "OK", page->content_type, page->content, head_only);
}

int serve(const std::filesystem::path& config_path) {
  const ckgit::ServerConfig config = ckgit::loadServerConfig(config_path);
  if (!config.pages_root.has_value() || config.pages_root->empty()) {
    std::cerr << "ck-pagesd: serve requires pages_root in the configuration\n";
    return 2;
  }
  if (!config.pages_http_port.has_value()) {
    std::cerr << "ck-pagesd: serve requires pages_http_port in the configuration\n";
    return 2;
  }
  const std::filesystem::path pages_root = *config.pages_root;
  const unsigned short port = *config.pages_http_port;

  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    std::cerr << "ck-pagesd: could not create the listening socket\n";
    return 1;
  }
  const int enabled = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);  // LAN-exposable, a distinct origin
  address.sin_port = htons(port);
  if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(listener, 16) != 0) {
    std::cerr << "ck-pagesd: could not bind port " << port << "\n";
    ::close(listener);
    return 1;
  }

  struct sigaction action {};
  action.sa_handler = onStop;
  ::sigaction(SIGTERM, &action, nullptr);
  ::sigaction(SIGINT, &action, nullptr);

  unsigned short bound_port = port;
  sockaddr_in bound{};
  socklen_t bound_size = sizeof(bound);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_size) == 0 &&
      bound_size == sizeof(bound)) {
    bound_port = ntohs(bound.sin_port);
  }
  std::cout << "ck-pagesd: pages ready on " << bound_port << "\n" << std::flush;
  while (g_stop == 0) {
    const int client = ::accept(listener, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      break;
    }
    const timeval timeout{10, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    try {
      handle(client, pages_root);
    } catch (const std::exception&) {
    }
    ::close(client);
  }
  ::close(listener);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc >= 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
      return usage(std::cout, 0);
    }
    if (argc < 2 || std::string(argv[1]) != "serve") return usage(std::cerr, 2);
    std::filesystem::path config_path;
    for (int i = 2; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--config") {
        if (++i >= argc) throw std::runtime_error("--config requires a value");
        config_path = argv[i];
      } else {
        throw std::runtime_error("unknown serve option: " + option);
      }
    }
    if (config_path.empty()) {
      std::cerr << "ck-pagesd: serve requires --config FILE\n";
      return usage(std::cerr, 2);
    }
    return serve(config_path);
  } catch (const std::exception& error) {
    std::cerr << "ck-pagesd: " << error.what() << "\n";
    return 2;
  }
}
