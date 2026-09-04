// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstring>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <sys/ucred.h>
#endif

#include "ckgit/repository_store.hpp"
#include "ckgit/process.hpp"
#include "ckgit/git_repository.hpp"
#include "ckgit/http_request.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/project_summary.hpp"
#include "ckgit/server_config.hpp"
#include "ckgit/web_renderer.hpp"

namespace {

constexpr std::size_t kMaximumRequestBytes = 768;
constexpr std::size_t kMaximumResponseBytes = 4096;
volatile std::sig_atomic_t stop_requested = 0;

struct Options {
  std::optional<std::filesystem::path> config_file;
  bool check_only{false};
  std::filesystem::path repo_root;
  std::filesystem::path control_socket;
  std::optional<std::filesystem::path> state_root;
  std::optional<std::filesystem::path> hook_directory;
  std::optional<unsigned short> http_port;
};

struct ControlRequest {
  std::string client_id;
  std::string operation;
  std::string argument;
  std::string second_argument;
};

void requestStop(int) {
  stop_requested = 1;
}

void installSignalHandlers() {
  struct sigaction action {};
  action.sa_handler = requestStop;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;  // Leave accept() interruptible for prompt shutdown.
  if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
    throw std::runtime_error("could not install signal handlers");
  }
}

void usage(std::ostream& output) {
  output << "Usage: ck-git-hostingd --repo-root ROOT --control-socket PATH [--state-root ROOT] [--hook-directory PATH] [--http-port PORT] [--check]\n"
         << "       ck-git-hostingd --config FILE [--check]\n"
         << "\n--config reads the strict server.ini instead of individual path options.\n"
         << "--check prints the effective configuration without opening a socket or path.\n"
         << "--http-port binds the read-only dashboard to 127.0.0.1.\n";
}

bool parseOptions(int argc, char* argv[], Options* options) {
  bool explicit_paths = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--check") {
      options->check_only = true;
    } else if ((argument == "--repo-root" || argument == "--control-socket" || argument == "--state-root" ||
                argument == "--hook-directory" || argument == "--http-port" || argument == "--config") &&
               index + 1 < argc) {
      const std::string value = argv[++index];
      if (argument == "--config") {
        if (options->config_file.has_value()) {
          return false;
        }
        options->config_file.emplace(value);
        continue;
      }
      explicit_paths = true;
      if (argument == "--repo-root") {
        options->repo_root = value;
      } else if (argument == "--control-socket") {
        options->control_socket = value;
      } else if (argument == "--state-root") {
        options->state_root.emplace(value);
      } else if (argument == "--hook-directory") {
        options->hook_directory.emplace(value);
      } else {
        unsigned int port = 0;
        const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), port);
        if (parse_error != std::errc{} || end != value.data() + value.size() || port > 65535) {
          return false;
        }
        options->http_port = static_cast<unsigned short>(port);
      }
    } else {
      return false;
    }
  }
  if (options->config_file.has_value()) {
    // A configuration file is the single source of truth: mixing it with
    // individual path options would hide which value is effective.
    return !explicit_paths;
  }
  return !options->repo_root.empty() && !options->control_socket.empty();
}

void applyServerConfig(Options* options) {
  if (!options->config_file.has_value()) {
    return;
  }
  const ckgit::ServerConfig config = ckgit::loadServerConfig(*options->config_file);
  options->repo_root = config.repo_root;
  options->control_socket = config.control_socket;
  options->state_root = config.state_root;
  options->hook_directory = config.hook_directory;
  options->http_port = config.http_port;
}

ckgit::ServerConfig effectiveConfig(const Options& options) {
  return ckgit::ServerConfig{options.repo_root, options.control_socket, options.state_root,
                             options.hook_directory, options.http_port};
}

void sendAll(int descriptor, std::string_view response) {
  std::size_t sent_total = 0;
  while (sent_total < response.size()) {
    const ssize_t sent = send(descriptor, response.data() + sent_total,
                              response.size() - sent_total, MSG_NOSIGNAL);
    if (sent <= 0) {
      return;
    }
    sent_total += static_cast<std::size_t>(sent);
  }
}

void sendError(int descriptor, std::string_view code, std::string_view message) {
  sendAll(descriptor, "error " + std::string(code) + " " + std::string(message) + "\n");
}

void recordStateEvent(const std::optional<std::filesystem::path>& state_root,
                      std::string_view kind, std::string_view project, std::string_view client_id) {
  if (!state_root.has_value()) {
    return;
  }
  try {
    ckgit::appendStateEvent(*state_root, kind, project, client_id);
  } catch (const std::exception&) {
    // Events are observability data: a failed append must not undo a successful
    // repository creation or metadata registration.
    std::cerr << "ck-git-hostingd: warning: could not append server event\n";
  }
}

bool sameUserPeer(int descriptor) {
#ifdef __APPLE__
  uid_t peer_uid = 0;
  gid_t peer_gid = 0;
  return getpeereid(descriptor, &peer_uid, &peer_gid) == 0 && peer_uid == geteuid();
#else
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  return getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
         size == sizeof(credentials) && credentials.uid == geteuid();
#endif
}

std::optional<ControlRequest> parseRequest(std::string_view request) {
  if (request.empty() || request.size() > kMaximumRequestBytes || request.back() != '\n' ||
      request.find('\n') != request.size() - 1 || request.find('\r') != std::string_view::npos ||
      request.find('\0') != std::string_view::npos) {
    return std::nullopt;
  }
  for (const unsigned char character : request) {
    if (character != '\n' && (character < 0x20 || character > 0x7e)) {
      return std::nullopt;
    }
  }
  const std::string_view content = request.substr(0, request.size() - 1);
  constexpr std::string_view prefix{"CKGIT-CONTROL/1 "};
  if (content.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  std::vector<std::string> tokens;
  const std::string_view arguments = content.substr(prefix.size());
  std::size_t start = 0;
  while (start < arguments.size()) {
    const std::size_t end = arguments.find(' ', start);
    const std::string_view token = arguments.substr(start,
        end == std::string_view::npos ? std::string_view::npos : end - start);
    if (token.empty() || tokens.size() == 4) {
      return std::nullopt;
    }
    tokens.emplace_back(token);
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  if (tokens.size() < 2 || !ckgit::isValidClientId(tokens[0])) {
    return std::nullopt;
  }
  const std::string argument = tokens.size() >= 3 ? tokens[2] : "";
  const std::string second_argument = tokens.size() == 4 ? tokens[3] : "";
  const std::string& operation = tokens[1];
  const bool no_arguments = argument.empty() && second_argument.empty();
  if (!((operation == "ping" || operation == "list-projects") && no_arguments) &&
      !(operation == "refs" && !argument.empty() && second_argument.empty() &&
        ckgit::isValidProjectName(argument)) &&
      !(operation == "create" && !argument.empty() && !second_argument.empty() &&
        ckgit::isValidProjectName(argument) && ckgit::isValidBranchName(second_argument)) &&
      !(operation == "register" && !argument.empty() && !second_argument.empty() &&
        ckgit::isValidProjectName(argument) && ckgit::isValidCheckoutPathToken(second_argument))) {
    return std::nullopt;
  }
  return ControlRequest{tokens[0], operation, argument, second_argument};
}

std::vector<std::string> listProjects(const std::filesystem::path& root) {
  std::vector<std::string> projects;
  std::error_code error;
  std::filesystem::directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied,
                                               error);
  if (error) {
    throw std::runtime_error("could not list repository root");
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const auto entry = *iterator;
    const auto status = entry.symlink_status(error);
    if (!error && std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status)) {
      const std::string filename = entry.path().filename().string();
      constexpr std::string_view suffix{".git"};
      if (filename.size() > suffix.size() &&
          std::string_view(filename).substr(filename.size() - suffix.size()) == suffix) {
        const std::string project = filename.substr(0, filename.size() - suffix.size());
        if (ckgit::isValidProjectName(project)) {
          projects.push_back(project);
        }
      }
    }
    error.clear();
    iterator.increment(error);
    if (error) {
      error.clear();
    }
  }
  std::sort(projects.begin(), projects.end());
  return projects;
}

std::string listResponse(const std::filesystem::path& root) {
  const auto projects = listProjects(root);
  std::string response = "ok " + std::to_string(projects.size()) + "\n";
  for (const auto& project : projects) {
    if (response.size() + project.size() + 1 > kMaximumResponseBytes) {
      throw std::runtime_error("project list exceeds control response limit");
    }
    response += project;
    response += '\n';
  }
  return response;
}

std::string refsResponse(const std::filesystem::path& root, std::string_view project) {
  const auto repository = ckgit::bareRepositoryPath(root, project);
  std::error_code error;
  const auto status = std::filesystem::symlink_status(repository, error);
  if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
    throw std::runtime_error("requested repository is unavailable");
  }
  const auto result = ckgit::runProcess(
      {"git", "--git-dir", repository.string(), "for-each-ref",
       "--format=%(refname)%09%(objectname)", "refs/heads", "refs/tags"});
  if (result.exit_code != 0 || result.timed_out || result.output_truncated) {
    throw std::runtime_error("could not inspect repository refs");
  }
  std::vector<ckgit::RefTip> refs;
  std::size_t start = 0;
  while (start < result.output.size()) {
    const std::size_t newline = result.output.find('\n', start);
    const std::size_t end = newline == std::string::npos ? result.output.size() : newline;
    const std::string_view line(result.output.data() + start, end - start);
    if (!line.empty()) {
      const std::size_t tab = line.find('\t');
      if (tab == std::string_view::npos || tab == 0 || tab + 1 == line.size()) {
        throw std::runtime_error("repository returned malformed ref data");
      }
      const std::string_view name = line.substr(0, tab);
      const std::string_view object_id = line.substr(tab + 1);
      const bool allowed_namespace = name.rfind("refs/heads/", 0) == 0 ||
                                     name.rfind("refs/tags/", 0) == 0;
      const bool hex_id = (object_id.size() == 40 || object_id.size() == 64) &&
                          std::all_of(object_id.begin(), object_id.end(), [](unsigned char character) {
                            return std::isxdigit(character) != 0;
                          });
      if (!allowed_namespace || !hex_id || name.find_first_of(" \t\r\n") != std::string_view::npos) {
        throw std::runtime_error("repository returned unsafe ref data");
      }
      refs.push_back(ckgit::RefTip{std::string(name), std::string(object_id)});
    }
    if (newline == std::string::npos) {
      break;
    }
    start = newline + 1;
  }
  std::string response = "ok " + std::to_string(refs.size()) + "\n";
  for (const auto& ref : refs) {
    if (response.size() + ref.name.size() + ref.object_id.size() + 2 > kMaximumResponseBytes) {
      throw std::runtime_error("ref list exceeds control response limit");
    }
    response += ref.name;
    response += ' ';
    response += ref.object_id;
    response += '\n';
  }
  return response;
}

std::string readRequest(int descriptor) {
  std::array<char, 128> buffer{};
  std::string request;
  while (true) {
    const ssize_t received = recv(descriptor, buffer.data(), buffer.size(), 0);
    if (received == 0) {
      return request;
    }
    if (received < 0) {
      throw std::runtime_error("could not read control request");
    }
    if (request.size() + static_cast<std::size_t>(received) > kMaximumRequestBytes) {
      throw std::runtime_error("control request exceeds limit");
    }
    request.append(buffer.data(), static_cast<std::size_t>(received));
  }
}

void setSocketTimeouts(int descriptor) {
  timeval timeout{};
  timeout.tv_sec = 2;
  if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
      setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
    throw std::runtime_error("could not configure control socket timeout");
  }
}

int bindSocket(const std::filesystem::path& requested_path) {
  const std::filesystem::path parent = requested_path.parent_path();
  if (requested_path.filename().empty() || requested_path.filename() == "." ||
      requested_path.filename() == "..") {
    throw std::invalid_argument("control socket must have a filename");
  }
  static_cast<void>(ckgit::validatedRepositoryRoot(parent));
  const std::string path = requested_path.string();
  sockaddr_un address{};
  if (path.size() >= sizeof(address.sun_path)) {
    throw std::invalid_argument("control socket path is too long");
  }
  struct stat existing {};
  if (lstat(path.c_str(), &existing) == 0) {
    if (!S_ISSOCK(existing.st_mode)) {
      throw std::runtime_error("control socket path exists and is not a socket");
    }
    if (unlink(path.c_str()) != 0) {
      throw std::runtime_error("could not remove stale control socket");
    }
  } else if (errno != ENOENT) {
    throw std::runtime_error("could not inspect control socket path");
  }

  const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0) {
    throw std::runtime_error("could not create control socket");
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.data(), path.size());
  const mode_t old_mask = umask(0077);
  const int bind_result = bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  umask(old_mask);
  if (bind_result != 0) {
    close(descriptor);
    throw std::runtime_error("could not bind control socket");
  }
  if (chmod(path.c_str(), 0600) != 0 || listen(descriptor, 16) != 0) {
    close(descriptor);
    throw std::runtime_error("could not activate control socket");
  }
  return descriptor;
}

int bindHttpSocket(unsigned short port, unsigned short* bound_port) {
  const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) {
    throw std::runtime_error("could not create HTTP socket");
  }
  const int enabled = 1;
  if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
    close(descriptor);
    throw std::runtime_error("could not configure HTTP socket");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(descriptor, 16) != 0) {
    close(descriptor);
    throw std::runtime_error("could not bind loopback HTTP socket");
  }
  sockaddr_in bound{};
  socklen_t bound_size = sizeof(bound);
  if (getsockname(descriptor, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0 ||
      bound_size != sizeof(bound)) {
    close(descriptor);
    throw std::runtime_error("could not read bound HTTP port");
  }
  *bound_port = ntohs(bound.sin_port);
  return descriptor;
}

void sendHttp(int descriptor, int status, std::string_view reason, std::string_view body, bool head_only) {
  const std::string headers = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n"
      "Content-Type: text/html; charset=utf-8\r\nContent-Security-Policy: default-src 'none'; style-src 'unsafe-inline'\r\n"
      "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"
      "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  sendAll(descriptor, headers);
  if (!head_only) {
    sendAll(descriptor, body);
  }
}

void handleHttpClient(int descriptor, const std::filesystem::path& root,
                      const std::optional<std::filesystem::path>& state_root) {
  setSocketTimeouts(descriptor);
  std::string request;
  std::array<char, 1024> buffer{};
  while (request.size() < 16384 && request.find("\r\n\r\n") == std::string::npos) {
    const ssize_t received = recv(descriptor, buffer.data(), buffer.size(), 0);
    if (received <= 0) {
      return;
    }
    request.append(buffer.data(), static_cast<std::size_t>(received));
  }
  const auto parsed = ckgit::parseReadOnlyHttpRequest(request);
  if (!parsed.has_value()) {
    sendHttp(descriptor, 400, "Bad Request", "<!doctype html><title>Bad Request</title>", false);
    return;
  }
  const bool is_head = parsed->method == ckgit::HttpMethod::kHead;
  const std::string_view target = parsed->target;
  try {
    const auto projects = ckgit::inspectHostedProjects(root, state_root);
    if (target == "/") {
      sendHttp(descriptor, 200, "OK", ckgit::renderProjectTable(projects), is_head);
      return;
    }
    constexpr std::string_view project_prefix{"/project/"};
    if (target.rfind(project_prefix, 0) == 0) {
      const std::string name(target.substr(project_prefix.size()));
      if (ckgit::isValidProjectName(name)) {
        const auto project = std::find_if(projects.begin(), projects.end(), [&](const ckgit::ProjectSummary& item) {
          return item.name == name;
        });
        if (project != projects.end()) {
          sendHttp(descriptor, 200, "OK", ckgit::renderProjectDetail(*project), is_head);
          return;
        }
      }
    }
    sendHttp(descriptor, 404, "Not Found", "<!doctype html><title>Not Found</title>", is_head);
  } catch (const std::exception&) {
    sendHttp(descriptor, 500, "Internal Server Error", "<!doctype html><title>Server Error</title>", is_head);
  }
}

void serveHttp(int listener, const std::filesystem::path& root,
               const std::optional<std::filesystem::path>& state_root) {
  while (!stop_requested) {
    pollfd ready{listener, POLLIN, 0};
    const int result = poll(&ready, 1, 250);
    if (result <= 0) {
      continue;
    }
    const int client = accept(listener, nullptr, nullptr);
    if (client >= 0) {
      handleHttpClient(client, root, state_root);
      close(client);
    }
  }
  close(listener);
}

int serve(const Options& options) {
  const auto repository_root = ckgit::validatedRepositoryRoot(options.repo_root);
  const std::optional<std::filesystem::path> state_root = options.state_root.has_value()
      ? std::optional<std::filesystem::path>(ckgit::validatedMetadataRoot(*options.state_root))
      : std::nullopt;
  const int listener = bindSocket(options.control_socket);
  unsigned short bound_http_port = 0;
  const int http_listener = options.http_port.has_value() ? bindHttpSocket(*options.http_port, &bound_http_port) : -1;
  std::cout << "ck-git-hostingd: control socket ready\n" << std::flush;
  std::thread http_thread;
  if (http_listener >= 0) {
    std::cout << "ck-git-hostingd: loopback HTTP ready on " << bound_http_port << "\n" << std::flush;
    http_thread = std::thread(serveHttp, http_listener, repository_root, state_root);
  }
  while (!stop_requested) {
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(listener);
      throw std::runtime_error("could not accept control connection");
    }
    try {
      setSocketTimeouts(client);
      if (!sameUserPeer(client)) {
        sendError(client, "peer", "untrusted local peer");
      } else {
        const auto request = parseRequest(readRequest(client));
        if (!request.has_value()) {
          sendError(client, "request", "invalid control request");
        } else if (request->operation == "ping") {
          sendAll(client, "ok\n");
        } else if (request->operation == "list-projects") {
          sendAll(client, listResponse(repository_root));
        } else if (request->operation == "create") {
          static_cast<void>(ckgit::createBareRepository(repository_root, request->argument,
                                                         request->second_argument, false,
                                                         options.hook_directory));
          recordStateEvent(state_root, "project-created", request->argument, request->client_id);
          sendAll(client, "ok created\n");
        } else if (request->operation == "register") {
          if (!state_root.has_value()) {
            throw std::runtime_error("checkout metadata is not configured");
          }
          ckgit::registerCheckout(*state_root, request->argument, request->client_id,
                                  request->second_argument);
          recordStateEvent(state_root, "checkout-registered", request->argument, request->client_id);
          sendAll(client, "ok registered\n");
        } else {
          sendAll(client, refsResponse(repository_root, request->argument));
        }
      }
    } catch (const std::exception&) {
      sendError(client, "internal", "control operation failed");
    }
    close(client);
  }
  close(listener);
  if (http_thread.joinable()) {
    http_thread.join();
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    Options options;
    if (!parseOptions(argc, argv, &options)) {
      usage(std::cerr);
      return 2;
    }
    applyServerConfig(&options);
    if (options.check_only) {
      std::cout << ckgit::renderServerConfig(effectiveConfig(options));
      return 0;
    }
    installSignalHandlers();
    return serve(options);
  } catch (const std::exception& error) {
    std::cerr << "ck-git-hostingd: " << error.what() << "\n";
    return 1;
  }
}
