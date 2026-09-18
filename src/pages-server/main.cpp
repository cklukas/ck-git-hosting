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
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "ckgit/cli_help.hpp"
#include "ckgit/http_request.hpp"
#include "ckgit/pages_store.hpp"
#include "ckgit/runtime_status.hpp"
#include "ckgit/server_config.hpp"
#include "ckgit/validation.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onStop(int) { g_stop = 1; }

constexpr std::size_t kMaximumRequestBytes = 16 * 1024;
// Matches kMaximumPagesFileBytes; a served file is read fully into memory, so
// this also bounds per-request memory. 1 GiB comfortably serves large PDFs.
constexpr std::size_t kMaximumServedBytes = static_cast<std::size_t>(1) << 30;   // 1 GiB per file

int usage(std::ostream& out, int code) {
  out << "usage:\n"
         "  ck-pagesd serve --config FILE\n"
         "  ck-pagesd check --config FILE\n"
         "\n"
         "serve serves published static sites from pages_root on pages_http_port; both\n"
         "must be set in the configuration file. check exits 0 when they are both set,\n"
         "printing nothing, and 1 with a one-line reason otherwise -- used by the\n"
         "packaged unit's ExecCondition so the service stays cleanly inactive, rather\n"
         "than restart-looping, when Pages is not configured.\n";
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

  const std::optional<std::string> decoded = ckgit::decodeRequestPath(target);
  if (!decoded.has_value() || decoded->empty() || decoded->front() != '/') {
    sendError(fd, 404, "Not Found", head_only);
    return;
  }
  std::string_view remainder = std::string_view(*decoded).substr(1);  // drop leading '/'
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

// WP4/D3: whether this server is configured to serve Pages at all. Shared by
// the packaged unit's ExecCondition (see packaging/systemd/ck-pages.service)
// and by an operator running it by hand. Deliberately does not open the
// listening socket or touch pages_root: it only reads the same two keys
// serve() requires, so it is safe to run as a pre-flight check before the
// service account and its filesystem access are fully set up.
int check(const std::filesystem::path& config_path) {
  const ckgit::ServerConfig config = ckgit::loadServerConfig(config_path);
  if (!config.pages_root.has_value() || config.pages_root->empty()) {
    std::cerr << "ck-pagesd: pages_root is not set in " << config_path.string() << "\n";
    return 1;
  }
  if (!config.pages_http_port.has_value()) {
    std::cerr << "ck-pagesd: pages_http_port is not set in " << config_path.string() << "\n";
    return 1;
  }
  return 0;
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
  // Record the running version so the dashboard and `ckgit version` can report
  // it; best-effort, and only when a state root is configured and writable.
  if (config.state_root.has_value() && !config.state_root->empty()) {
    ckgit::recordRuntimeComponent(*config.state_root, "ck-pagesd", ckgit::buildVersion());
  }

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
    if (argc >= 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-V")) {
      std::cout << ckgit::versionLine("ck-pagesd");
      return 0;
    }
    if (argc < 2) return usage(std::cerr, 2);
    const std::string command = argv[1];
    if (command != "serve" && command != "check") return usage(std::cerr, 2);
    std::filesystem::path config_path;
    for (int i = 2; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--config") {
        if (++i >= argc) throw std::runtime_error("--config requires a value");
        config_path = argv[i];
      } else {
        throw std::runtime_error("unknown " + command + " option: " + option);
      }
    }
    if (config_path.empty()) {
      std::cerr << "ck-pagesd: " << command << " requires --config FILE\n";
      return usage(std::cerr, 2);
    }
    return command == "check" ? check(config_path) : serve(config_path);
  } catch (const std::exception& error) {
    std::cerr << "ck-pagesd: " << error.what() << "\n";
    return 2;
  }
}
