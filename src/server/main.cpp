// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstring>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <fcntl.h>
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

#include "ckgit/cli_help.hpp"
#include "ckgit/control_rpc.hpp"
#include "ckgit/dashboard.hpp"
#include "ckgit/project_index.hpp"
#include "ckgit/repository_store.hpp"
#include "ckgit/runtime_status.hpp"
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
constexpr std::size_t kMaximumResponseBytes = ckgit::kMaximumControlResponseBytes;
// A repository with many tags produces far more than the process helper's
// default capture; refs are listed from a bounded but generous buffer.
constexpr std::size_t kRefListingBytes = 8 * 1024 * 1024;
// A lock-free atomic is safe in the signal handler and shared by all workers.
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stop_requested{false};

struct Options {
  std::optional<std::filesystem::path> config_file;
  bool check_only{false};
  std::filesystem::path repo_root;
  std::filesystem::path control_socket;
  std::optional<std::filesystem::path> state_root;
  std::optional<std::filesystem::path> hook_directory;
  std::optional<unsigned short> http_port;
  std::optional<std::string> ssh_clone_target;
  // Read from the shared config so the dashboard can link a project's published
  // Pages site: pages_root to tell whether a site exists, pages_http_port to
  // build the link on ck-pagesd's separate origin.
  std::optional<std::filesystem::path> pages_root;
  std::optional<unsigned short> pages_http_port;
  std::optional<std::string> pages_public_url;
};

struct ControlRequest {
  std::string client_id;
  std::string operation;
  std::string argument;
  std::string second_argument;
};

void requestStop(int) {
  stop_requested.store(true, std::memory_order_relaxed);
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
  output << "Usage: ck-git-hostingd --repo-root ROOT --control-socket PATH [--state-root ROOT] [--hook-directory PATH] [--http-port PORT] [--ssh-clone-target USER@HOST] [--check]\n"
         << "       ck-git-hostingd --config FILE [--check]\n"
         << "\n--config reads the strict server.ini instead of individual path options.\n"
         << "--check prints the effective configuration without opening a socket or path.\n"
         << "--http-port binds the read-only dashboard to 127.0.0.1.\n"
         << "--ssh-clone-target advertises the SSH destination for dashboard clone commands.\n";
}

bool parseOptions(int argc, char* argv[], Options* options) {
  bool explicit_paths = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--check") {
      options->check_only = true;
    } else if ((argument == "--repo-root" || argument == "--control-socket" || argument == "--state-root" ||
                argument == "--hook-directory" || argument == "--http-port" || argument == "--ssh-clone-target" || argument == "--config") &&
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
      } else if (argument == "--ssh-clone-target") {
        if (!ckgit::isValidSshCloneTarget(value)) return false;
        options->ssh_clone_target = value;
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
  options->ssh_clone_target = config.ssh_clone_target;
  options->pages_root = config.pages_root;
  options->pages_http_port = config.pages_http_port;
  options->pages_public_url = config.pages_public_url;
}

ckgit::ServerConfig effectiveConfig(const Options& options) {
  ckgit::ServerConfig config;
  config.repo_root = options.repo_root;
  config.control_socket = options.control_socket;
  config.state_root = options.state_root;
  config.hook_directory = options.hook_directory;
  config.http_port = options.http_port;
  config.ssh_clone_target = options.ssh_clone_target;
  return config;
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

void recordStateEvent(const std::filesystem::path& repository_root, const std::optional<std::filesystem::path>& state_root,
                      std::string_view kind, std::string_view project, std::string_view client_id) {
  if (!state_root.has_value()) {
    return;
  }
  try {
    static_cast<void>(ckgit::appendHostedStateEvent(repository_root, *state_root, kind, project, client_id));
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
  if (!((operation == "ping" || operation == "list-projects" || operation == "checkouts" ||
         operation == "version" || operation == "versions") && no_arguments) &&
      !((operation == "refs" || operation == "refresh" || operation == "forget-checkout" ||
         operation == "ci-status") && !argument.empty() && second_argument.empty() &&
        ckgit::isValidProjectName(argument)) &&
      !(operation == "create" && !argument.empty() && !second_argument.empty() &&
        ckgit::isValidProjectName(argument) && ckgit::isValidBranchName(second_argument)) &&
      !((operation == "register" || operation == "replace-checkout") && !argument.empty() &&
        !second_argument.empty() && ckgit::isValidProjectName(argument) &&
        ckgit::isValidCheckoutPathToken(second_argument)) &&
      !(operation == "cancel" && !argument.empty() && !second_argument.empty() &&
        ckgit::isValidProjectName(argument) && ckgit::isValidCiId(second_argument))) {
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
       "--format=%(refname)%09%(objectname)", "refs/heads", "refs/tags"},
      std::chrono::seconds(30), kRefListingBytes);
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

// The requesting host's own registrations, as `project path-hex` lines.
std::string checkoutsResponse(const std::filesystem::path& state_root, std::string_view client_id) {
  const auto checkouts = ckgit::loadClientCheckouts(state_root, client_id);
  std::string response = "ok " + std::to_string(checkouts.size()) + "\n";
  for (const auto& checkout : checkouts) {
    const std::string line = checkout.project_name + " " + ckgit::encodeCheckoutPath(checkout.reported_path) + "\n";
    if (response.size() + line.size() > kMaximumResponseBytes) {
      throw std::runtime_error("checkout list exceeds control response limit");
    }
    response += line;
  }
  return response;
}

// The newest CI runs for a project as `run_id status started heartbeat finished steps`
// lines, so a script can watch progress over the same control socket the cancel
// operation uses. Bounded to a handful of runs, well within the response limit.
std::string ciStatusResponse(const std::filesystem::path& state_root, std::string_view project) {
  const auto runs = ckgit::loadCiRuns(state_root, project, 8);
  std::string response = "ok " + std::to_string(runs.size()) + "\n";
  for (const auto& run : runs) {
    const std::string line = run.run_id + " " + std::string(ckgit::ciRunStatusName(run.status)) + " " +
        std::to_string(run.started_epoch_seconds) + " " + std::to_string(run.heartbeat_epoch_seconds) + " " +
        std::to_string(run.finished_epoch_seconds) + " " + std::to_string(run.steps.size()) + "\n";
    if (response.size() + line.size() > kMaximumResponseBytes) {
      throw std::runtime_error("CI status exceeds control response limit");
    }
    response += line;
  }
  return response;
}

// The suite's four versions come from one build, but a running process can lag
// an install that did not restart it, so this reports the recorded version and
// liveness of every daemon that has registered under the state root. The
// hosting daemon knows its own running version in-process, authoritatively.
std::string versionsResponse(const std::optional<std::filesystem::path>& state_root) {
  std::vector<ckgit::RuntimeComponent> components =
      state_root.has_value() ? ckgit::readRuntimeComponents(*state_root) : std::vector<ckgit::RuntimeComponent>{};
  bool have_host = false;
  for (auto& component : components) {
    if (component.name == "ck-git-hostingd") {
      component.version = ckgit::buildVersion();
      component.running = true;
      have_host = true;
    }
  }
  if (!have_host) {
    ckgit::RuntimeComponent host;
    host.name = "ck-git-hostingd";
    host.version = ckgit::buildVersion();
    host.running = true;
    components.insert(components.begin(), std::move(host));
  }
  std::string response = "ok versions\n";
  for (const auto& component : components) {
    response += component.name + " " + (component.version.empty() ? "unknown" : component.version) + " " +
                (component.running ? "running" : "stopped") + " " +
                std::to_string(component.started_epoch_seconds) + "\n";
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
  fcntl(descriptor, F_SETFD, FD_CLOEXEC);
  *bound_port = ntohs(bound.sin_port);
  return descriptor;
}

using Deadline = std::chrono::steady_clock::time_point;

bool waitHttp(int descriptor, short events, Deadline deadline) {
  while (!stop_requested) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) return false;
    pollfd ready{descriptor, events, 0};
    const int result = poll(&ready, 1, static_cast<int>(std::min<std::int64_t>(remaining.count(), 250)));
    if (result > 0) return (ready.revents & events) != 0;
    if (result < 0 && errno != EINTR) return false;
  }
  return false;
}
bool sendHttpBytes(int descriptor, std::string_view bytes, Deadline deadline) {
  while (!bytes.empty()) {
    if (!waitHttp(descriptor, POLLOUT, deadline)) return false;
    const auto sent = send(descriptor, bytes.data(), bytes.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent > 0) bytes.remove_prefix(static_cast<std::size_t>(sent));
    else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
    else return false;
  }
  return true;
}
void sendHttp(int descriptor, const ckgit::DashboardResponse& response, bool head_only, Deadline deadline) {
  const auto reason = response.status == 200 ? "OK" : response.status == 302 ? "Found" : response.status == 303 ? "See Other" :
      response.status == 400 ? "Bad Request" : response.status == 403 ? "Forbidden" : response.status == 404 ? "Not Found" :
      response.status == 405 ? "Method Not Allowed" : response.status == 413 ? "Content Too Large" : "Service Unavailable";
  std::string policy;
  if (!response.csp.empty()) {
    policy = response.csp;  // a page (the log follow view) that needs a scoped relaxation
  } else {
    policy = "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'";
    if (response.raw) policy += "; sandbox";
    else if (response.body.find("<img ") != std::string::npos) policy += "; img-src 'self'";
  }
  std::string headers = "HTTP/1.1 " + std::to_string(response.status) + " " + reason + "\r\nContent-Type: " +
      response.content_type + "\r\nContent-Security-Policy: " + policy;
  headers += "\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n";
  if (response.raw) {
    std::string filename;
    for (const unsigned char c : response.filename) {
      if (filename.size() == 150) break;
      filename += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ? c : '_';
    }
    if (filename.empty()) filename = "download";
    headers += "Content-Disposition: attachment; filename=\"" + filename + "\"\r\nCache-Control: public, max-age=31536000, immutable\r\n";
  } else headers += "Cache-Control: no-store\r\n";
  // Redirects are generated by the dashboard from canonical local routes;
  // validate again at the header boundary to exclude external URLs and CRLF.
  if ((response.status == 302 || response.status == 303) && !response.raw && !response.location.empty() &&
      ckgit::parseHttpRoute(response.location).kind != ckgit::RouteKind::kNotFound) {
    headers += "Location: " + response.location + "\r\n";
  }
  headers += "Connection: close\r\nContent-Length: " + std::to_string(response.body.size()) + "\r\n\r\n";
  if (sendHttpBytes(descriptor, headers, deadline) && !head_only) sendHttpBytes(descriptor, response.body, deadline);
}
// True for an Origin the dashboard will accept on its one mutating endpoint:
// its own loopback origin. The server binds only to 127.0.0.1, so a legitimate
// same-origin form POST carries http://127.0.0.1[:port], http://localhost[:port],
// or http://[::1][:port]; anything else is a cross-site attempt and is refused.
bool isLoopbackOrigin(std::string_view origin) {
  constexpr std::string_view kScheme = "http://";
  if (origin.rfind(kScheme, 0) != 0) return false;
  std::string_view authority = origin.substr(kScheme.size());
  const auto slash = authority.find('/');
  if (slash != std::string_view::npos) authority = authority.substr(0, slash);
  if (!authority.empty() && authority.front() == '[') {  // [IPv6]:port
    const auto close = authority.find(']');
    return close != std::string_view::npos && authority.substr(1, close - 1) == "::1";
  }
  const auto colon = authority.rfind(':');
  const std::string_view host = colon == std::string_view::npos ? authority : authority.substr(0, colon);
  return host == "127.0.0.1" || host == "localhost";
}

// Live log streams occupy a worker for their whole duration, so cap how many run
// at once; the fixed worker pool always keeps headroom for ordinary requests.
std::atomic<int> g_active_log_streams{0};
constexpr int kMaxLogStreams = 4;

// A 96-bit hex token for a per-response CSP script nonce.
std::string generateNonce() {
  std::random_device device;
  static constexpr char hex[] = "0123456789abcdef";
  std::string nonce;
  nonce.reserve(24);
  for (int i = 0; i < 24; ++i) nonce += hex[device() & 0x0f];
  return nonce;
}

// Streams one CI step's captured log to the client as Server-Sent Events: it
// tails only the newly appended bytes (from `start_offset`, which a reconnecting
// client supplies via Last-Event-ID) while that step is the run's currently
// executing one, and ends with an `event: done` once the step completes or the
// run reaches a terminal state. Each event carries an `id:` byte offset so a
// reconnect resumes exactly where it left off — no replay, no duplicates. Runs
// inline on the calling worker (bounded by kMaxLogStreams) and never throws.
void streamCiLog(int descriptor, ckgit::ProjectIndex& index, const std::string& project,
                 const std::string& run_id, std::size_t step, std::uint64_t start_offset) noexcept {
  if (g_active_log_streams.fetch_add(1, std::memory_order_relaxed) >= kMaxLogStreams) {
    g_active_log_streams.fetch_sub(1, std::memory_order_relaxed);
    const std::string busy =
        "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\n"
        "Connection: close\r\nContent-Length: 22\r\n\r\ntoo many live streams\n";
    sendHttpBytes(descriptor, busy, std::chrono::steady_clock::now() + std::chrono::seconds(5));
    return;
  }
  struct Release {
    ~Release() { g_active_log_streams.fetch_sub(1, std::memory_order_relaxed); }
  } release;

  const Deadline deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30);
  // `retry` sets the client's reconnect backoff; the opening comment flushes headers.
  if (!sendHttpBytes(descriptor,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\nX-Accel-Buffering: no\r\nConnection: close\r\n\r\n"
                     "retry: 2000\n: stream open\n\n",
                     deadline)) {
    return;
  }

  const auto clean = [](std::string_view line) {
    std::string out;  // one SSE event per log line; no stray CR/LF can split a frame
    out.reserve(line.size());
    for (const char c : line) {
      if (c != '\r' && c != '\n') out += c;
    }
    return out;
  };

  constexpr std::size_t kChunkBytes = 256 * 1024;
  std::uint64_t offset = start_offset;   // next unread byte in the log file
  std::uint64_t emitted = start_offset;  // bytes emitted as whole lines (the resume point)
  std::string pending;                   // bytes read but not yet ending in a newline

  // Appends the log's growth since `offset` into `pending`; returns the number of
  // bytes read (0 when caught up, the step has not started writing, or on error).
  const auto pull = [&]() -> std::size_t {
    const auto chunk = index.readCiLogChunk(project, run_id, step, offset, kChunkBytes);
    if (!chunk || chunk->empty()) return 0;
    offset += chunk->size();
    pending += *chunk;
    return chunk->size();
  };
  // Moves every complete line out of `pending` into `out` as an id'd SSE event.
  const auto drainLines = [&](std::string& out) {
    std::size_t newline;
    while ((newline = pending.find('\n')) != std::string::npos) {
      emitted += newline + 1;
      out += "id: " + std::to_string(emitted) + "\ndata: " +
             clean(std::string_view(pending).substr(0, newline)) + "\n\n";
      pending.erase(0, newline + 1);
    }
  };

  int since_status = 0;
  for (;;) {
    if (stop_requested) return;
    bool wrote = false;
    try {
      pull();
      std::string batch;
      drainLines(batch);
      if (!batch.empty()) {
        if (!sendHttpBytes(descriptor, batch, deadline)) return;
        wrote = true;
      }
      // The run status is heavier to read, so check it about once a second while
      // the log itself is tailed several times faster.
      if (++since_status >= 4) {
        since_status = 0;
        const auto record = index.readCiRun(project, run_id);
        const bool active =
            record && ckgit::ciRunStatusIsActive(record->status) && step == record->steps.size();
        if (!active) {
          // The step's whole log is on disk before its result is recorded, so
          // drain until caught up — no trailing bytes are lost to the final poll.
          std::string final_batch;
          for (int guard = 0; guard < 256 && pull() != 0; ++guard) drainLines(final_batch);
          if (!pending.empty()) {  // a final line with no trailing newline
            emitted += pending.size();
            final_batch += "id: " + std::to_string(emitted) + "\ndata: " + clean(pending) + "\n\n";
            pending.clear();
          }
          if (!final_batch.empty() && !sendHttpBytes(descriptor, final_batch, deadline)) return;
          sendHttpBytes(descriptor, "event: done\ndata: end\n\n", deadline);
          return;
        }
      }
    } catch (const std::exception&) {
      return;  // a read failure ends the stream rather than spinning on the error
    }
    // When idle, a keep-alive comment holds the connection and surfaces a gone
    // client; active writes already do that.
    if (!wrote && !sendHttpBytes(descriptor, ": ping\n\n", deadline)) return;
    if (std::chrono::steady_clock::now() >= deadline) return;
    for (int tick = 0; tick < 5 && !stop_requested; ++tick) {  // ~250 ms, shutdown-responsive
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

void handleHttpClient(int descriptor, const std::filesystem::path& root, ckgit::ProjectIndex& index, Deadline deadline,
                      const std::string& ssh_clone_target, const std::optional<std::filesystem::path>& state_root,
                      const std::optional<std::filesystem::path>& pages_root,
                      const std::optional<unsigned short>& pages_http_port,
                      const std::optional<std::string>& pages_public_url) {
  // Refresh the versions the About dialog lists, so every page shows the running
  // host and service builds. Best-effort and per request (the dashboard is
  // loopback-only and low-traffic), reflecting a service restart immediately.
  ckgit::setAboutServerComponents(state_root.has_value() ? ckgit::readRuntimeComponents(*state_root)
                                                         : std::vector<ckgit::RuntimeComponent>{});
  std::string request;
  std::array<char, 1024> buffer{};
  const auto header_deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(5));
  while (request.size() < 16384 && request.find("\r\n\r\n") == std::string::npos) {
    if (!waitHttp(descriptor, POLLIN, header_deadline)) return;
    const ssize_t received = recv(descriptor, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (received <= 0) return;
    request.append(buffer.data(), static_cast<std::size_t>(received));
  }
  const auto parsed = ckgit::parseReadOnlyHttpRequest(request);
  ckgit::DashboardResponse response;
  if (!parsed) {
    response.status = 400; response.body = ckgit::pageLayout("Bad Request", "<h1>Bad Request</h1>");
    sendHttp(descriptor, response, false, deadline); return;
  }
  const bool is_head = parsed->method == ckgit::HttpMethod::kHead;
  const bool is_post = parsed->method == ckgit::HttpMethod::kPost;
  ckgit::Route route;
  std::optional<ckgit::ProjectSummary> project;
  try {
    route = ckgit::parseHttpRoute(parsed->target);
    // The dashboard is read-only apart from the CI cancel endpoint; a POST to
    // anything else is refused rather than silently treated as a GET.
    if (is_post && route.kind != ckgit::RouteKind::kCiCancel) {
      throw ckgit::WebError(405, "This resource is read-only.");
    }
    if (route.kind == ckgit::RouteKind::kTable) {
      auto projects = index.tableSnapshot();
      if (route.sort_by_name) std::sort(projects.begin(), projects.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
      response.body = ckgit::renderProjectTable(projects, route.sort_by_name);
    } else {
      project = index.find(route.project);
      if (route.kind == ckgit::RouteKind::kNotFound || !project) throw ckgit::WebError(404, "Page was not found.");
      project->ssh_clone_target = ssh_clone_target;
      // Link the project's published Pages site when one exists. It lives on the
      // separate ck-pagesd origin (a distinct port, often a distinct public
      // name). Prefer the configured public Pages URL; otherwise derive the
      // origin from the advertised clone host (a LAN name that also serves
      // Pages) and pages_http_port, since the request Host is usually a loopback
      // tunnel to the dashboard that cannot reach the Pages port; fall back to
      // the request Host only when neither is configured.
      if (pages_root.has_value()) {
        std::error_code pages_ec;
        if (std::filesystem::exists(*pages_root / route.project / "current", pages_ec)) {
          if (pages_public_url.has_value() && !pages_public_url->empty()) {
            std::string base = *pages_public_url;
            while (!base.empty() && base.back() == '/') base.pop_back();
            project->pages_site_url = base + "/" + route.project + "/";
          } else if (pages_http_port.has_value()) {
            std::string host;
            if (!ssh_clone_target.empty()) {
              const auto at = ssh_clone_target.rfind('@');
              host = at == std::string::npos ? ssh_clone_target : ssh_clone_target.substr(at + 1);
            }
            if (host.empty()) host = parsed->host;
            if (!host.empty()) {
              std::size_t port_colon;
              if (host.front() == '[') {  // [IPv6]:port
                const auto bracket = host.find(']');
                port_colon = bracket == std::string::npos ? std::string::npos : host.find(':', bracket);
              } else {
                port_colon = host.rfind(':');
              }
              if (port_colon != std::string::npos) host.erase(port_colon);
              project->pages_site_url = "http://" + host + ":" + std::to_string(*pages_http_port) + "/" + route.project + "/";
            }
          }
        }
      }
      // A hand-deleted repository is hidden immediately, even before the sweep.
      const auto repository = ckgit::bareRepositoryPath(root, route.project);
      const auto status = std::filesystem::symlink_status(repository);
      if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) throw ckgit::WebError(404, "Repository was not found.");
      if (route.kind == ckgit::RouteKind::kCiLogStream) {
        // A live Server-Sent-Events tail of one step's log. GET only; verify the
        // run exists, then stream directly (the stream writes its own response)
        // and return without the buffered sendHttp path.
        const auto run = index.readCiRun(route.project, route.run_id);
        if (!run) throw ckgit::WebError(404, "CI run was not found.");
        if (is_head) {
          response.content_type = "text/event-stream";
        } else {
          // A reconnecting EventSource resumes from the byte offset it last saw.
          std::uint64_t start_offset = 0;
          if (!parsed->last_event_id.empty()) {
            const auto& id = parsed->last_event_id;
            const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), start_offset);
            if (error != std::errc{} || end != id.data() + id.size()) start_offset = 0;
          }
          streamCiLog(descriptor, index, route.project, route.run_id,
                      static_cast<std::size_t>(std::max(0, route.step)), start_offset);
          return;
        }
      } else if (route.kind == ckgit::RouteKind::kCiCancel) {
        // The single mutating endpoint: POST only, same-origin only. It drops a
        // cancel marker (via the index) that the runner honours; the browser is
        // sent back to the live run page, which shows the result.
        if (!is_post) throw ckgit::WebError(405, "Cancelling a run requires a POST request.");
        // Reject a request that declares a foreign origin. An absent or opaque
        // ("null", which some embedded and privacy-mode browsers send for a
        // same-origin form post) Origin is allowed: this endpoint is loopback-
        // only and carries no credentials, so a run's unguessable id is the real
        // barrier to a cross-site cancel.
        if (!parsed->origin.empty() && parsed->origin != "null" && !isLoopbackOrigin(parsed->origin)) {
          throw ckgit::WebError(403, "Cross-origin request refused.");
        }
        index.requestCiCancel(route.project, route.run_id);
        index.refresh(route.project);
        response.status = 303;
        response.location = "/project/" + route.project + "/ci/" + route.run_id;
        response.body = ckgit::pageLayout("Cancelling", "<h1>Cancelling\xe2\x80\xa6</h1><p><a href=\"" +
            ckgit::htmlEscape(response.location) + "\">Return to the run</a></p>", &*project);
      } else if (route.kind == ckgit::RouteKind::kCiRun) {
        // The live status page reads the run fresh from disk (not the cached
        // snapshot) plus the running step's log tail, and auto-refreshes while
        // the run is active.
        const auto run = index.readCiRun(route.project, route.run_id);
        if (!run) throw ckgit::WebError(404, "CI run was not found.");
        const ckgit::CiRunDisplay display = ckgit::ciRunDisplay(*run);
        const std::size_t live_step = run->steps.size();
        std::optional<std::string> live_log;
        if (display.active) live_log = index.readCiLog(route.project, route.run_id, live_step);
        ckgit::PageContext ci_context{{}, {}, "ci", {}, 0, 0};
        response.body = ckgit::pageLayout(project->name + " \xc2\xb7 CI run",
            ckgit::renderCiRunDetail(*project, *run, live_log, live_step), &*project, &ci_context,
            display.active ? 3u : 0u);
      } else if (route.kind == ckgit::RouteKind::kCiArtifact) {
        const auto blob = index.readCiArtifact(route.project, route.run_id, route.path);
        if (!blob) throw ckgit::WebError(404, "Artifact was not found.");
        response.raw = true;
        response.content_type = "application/x-tar";
        response.filename = route.path + ".tar";
        response.body = *blob;
      } else if (route.kind == ckgit::RouteKind::kReleaseAsset) {
        const auto blob = index.readReleaseAsset(route.project, route.run_id, route.path);
        if (!blob) throw ckgit::WebError(404, "Release asset was not found.");
        response.raw = true;
        response.content_type = "application/x-tar";
        response.filename = route.path + ".tar";
        response.body = *blob;
      } else if (route.kind == ckgit::RouteKind::kCiLog) {
        const int step = std::max(0, route.step);
        const auto log = index.readCiLog(route.project, route.run_id, static_cast<std::size_t>(step));
        if (!log) throw ckgit::WebError(404, "CI log was not found.");
        const std::string nonce = generateNonce();
        ckgit::PageContext ci_context{{}, {}, "ci", {}, 0, 0};
        response.body = ckgit::pageLayout(project->name + " \xc2\xb7 CI log",
            ckgit::renderCiLogView(*project, route.run_id, step, *log, nonce), &*project, &ci_context);
        // Scope the relaxation to this page only: its nonce'd follow script and a
        // same-origin EventSource; every other page keeps the strict default.
        response.csp = "default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-" + nonce +
                       "'; connect-src 'self'";
      } else {
        response = ckgit::renderDashboard(route, *project, repository, deadline);
        if (response.status == 302 && (response.raw || response.location.empty() ||
            ckgit::parseHttpRoute(response.location).kind == ckgit::RouteKind::kNotFound)) {
          throw ckgit::WebError(503, "The destination could not be prepared. Try again shortly.");
        }
      }
    }
  } catch (const ckgit::WebError& error) {
    if (project.has_value()) {
      response = ckgit::renderDashboardError(route, *project, error.status, error.what());
    } else {
      response.status = error.status;
      response.body = ckgit::pageLayout("Repository view unavailable", "<h1>Repository view unavailable</h1><p>" + ckgit::htmlEscape(error.what()) + "</p>");
    }
  } catch (const std::exception&) {
    const std::string message = "Repository data exceeded a limit or could not be read. Try again shortly.";
    if (project.has_value()) {
      response = ckgit::renderDashboardError(route, *project, 503, message);
    } else {
      response.status = 503;
      response.body = ckgit::pageLayout("View unavailable", "<h1>View unavailable</h1><p>" + message + "</p>");
    }
  }
  sendHttp(descriptor, response, is_head, deadline);
}

void serveHttp(int listener, const std::filesystem::path& root, ckgit::ProjectIndex& index,
                const std::string& ssh_clone_target, std::optional<std::filesystem::path> state_root,
                std::optional<std::filesystem::path> pages_root, std::optional<unsigned short> pages_http_port,
                std::optional<std::string> pages_public_url) {
  // Live log streams (SSE) hold a worker for their duration, bounded by
  // kMaxLogStreams; the extra workers keep ordinary requests responsive.
  constexpr std::size_t kHttpWorkerCount = 8;
  constexpr std::size_t kMaximumQueuedHttpClients = 12;
  struct Client { int descriptor; Deadline deadline; };
  std::deque<Client> queue;
  std::mutex mutex;
  std::condition_variable ready;
  bool stopping = false;
  std::vector<std::thread> workers;
  for (std::size_t i = 0; i < kHttpWorkerCount; ++i) workers.emplace_back([&] {
    while (true) {
      Client client;
      {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return stopping || !queue.empty(); });
        if (stopping) return;
        client = queue.front(); queue.pop_front();
      }
      try { handleHttpClient(client.descriptor, root, index, client.deadline, ssh_clone_target, state_root, pages_root, pages_http_port, pages_public_url); } catch (...) {}
      close(client.descriptor);
    }
  });
  while (!stop_requested) {
    pollfd pending{listener, POLLIN, 0};
    if (poll(&pending, 1, 250) <= 0) continue;
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) continue;
    fcntl(client, F_SETFD, FD_CLOEXEC);
    {
      std::lock_guard lock(mutex);
      if (queue.size() >= kMaximumQueuedHttpClients) { close(client); continue; }
      queue.push_back({client, std::chrono::steady_clock::now() + std::chrono::seconds(30)});
    }
    ready.notify_one();
  }
  {
    std::lock_guard lock(mutex); stopping = true;
    for (const auto& client : queue) close(client.descriptor);
    queue.clear();
  }
  ready.notify_all();
  for (auto& worker : workers) worker.join();
  close(listener);
}

int serve(const Options& options) {
  const auto repository_root = ckgit::validatedRepositoryRoot(options.repo_root);
  const std::optional<std::filesystem::path> state_root = options.state_root.has_value()
      ? std::optional<std::filesystem::path>(ckgit::validatedMetadataRoot(*options.state_root))
      : std::nullopt;
  if (state_root.has_value()) ckgit::recordRuntimeComponent(*state_root, "ck-git-hostingd", ckgit::buildVersion());
  ckgit::ProjectIndex index(repository_root, state_root);
  index.start();
  const int listener = bindSocket(options.control_socket);
  fcntl(listener, F_SETFD, FD_CLOEXEC);
  unsigned short bound_http_port = 0;
  const int http_listener = options.http_port.has_value() ? bindHttpSocket(*options.http_port, &bound_http_port) : -1;
  std::cout << "ck-git-hostingd: control socket ready\n" << std::flush;
  std::thread http_thread;
  if (http_listener >= 0) {
    std::cout << "ck-git-hostingd: loopback HTTP ready on " << bound_http_port << "\n" << std::flush;
    http_thread = std::thread(serveHttp, http_listener, repository_root, std::ref(index), options.ssh_clone_target.value_or(""), state_root, options.pages_root, options.pages_http_port, options.pages_public_url);
  }
  while (!stop_requested) {
    pollfd ready{listener, POLLIN, 0};
    if (poll(&ready, 1, 250) <= 0) continue;
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(listener);
      throw std::runtime_error("could not accept control connection");
    }
    fcntl(client, F_SETFD, FD_CLOEXEC);
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
        } else if (request->operation == "version") {
          sendAll(client, "ok " + ckgit::buildVersion() + "\n");
        } else if (request->operation == "versions") {
          sendAll(client, versionsResponse(state_root));
        } else if (request->operation == "list-projects") {
          sendAll(client, listResponse(repository_root));
        } else if (request->operation == "refresh") {
          index.refresh(request->argument);
          sendAll(client, "ok refreshed\n");
        } else if (request->operation == "create") {
          static_cast<void>(ckgit::createBareRepository(repository_root, request->argument,
                                                         request->second_argument, false,
                                                         options.hook_directory));
          recordStateEvent(repository_root, state_root, "project-created", request->argument, request->client_id);
          index.refresh(request->argument);
          index.refreshMetadata(request->argument);
          sendAll(client, "ok created\n");
        } else if (request->operation == "register" || request->operation == "replace-checkout") {
          if (!state_root.has_value()) {
            throw std::runtime_error("checkout metadata is not configured");
          }
          const auto repository_status = std::filesystem::symlink_status(ckgit::bareRepositoryPath(repository_root, request->argument));
          if (!std::filesystem::is_directory(repository_status) || std::filesystem::is_symlink(repository_status))
            throw std::runtime_error("cannot register a missing project");
          try {
            ckgit::registerHostedCheckout(repository_root, *state_root, request->argument, request->client_id,
                                    request->second_argument, request->operation == "replace-checkout");
          } catch (const ckgit::CheckoutConflict&) {
            sendError(client, "conflict", "this host already registered a different checkout; use replace-checkout");
            close(client);
            continue;
          }
          index.refreshMetadata(request->argument);
          index.refresh(request->argument);
          sendAll(client, "ok registered\n");
        } else if (request->operation == "forget-checkout") {
          if (!state_root.has_value()) throw std::runtime_error("checkout metadata is not configured");
          ckgit::forgetHostedCheckout(repository_root, *state_root, request->argument, request->client_id);
          index.refreshMetadata(request->argument);
          sendAll(client, "ok forgotten\n");
        } else if (request->operation == "checkouts") {
          if (!state_root.has_value()) {
            throw std::runtime_error("checkout metadata is not configured");
          }
          sendAll(client, checkoutsResponse(*state_root, request->client_id));
        } else if (request->operation == "cancel") {
          if (!state_root.has_value()) throw std::runtime_error("CI state is not configured");
          if (ckgit::requestCiCancel(*state_root, request->argument, request->second_argument)) {
            index.refresh(request->argument);
            sendAll(client, "ok cancelling\n");
          } else {
            sendError(client, "norun", "no such CI run");
          }
        } else if (request->operation == "ci-status") {
          if (!state_root.has_value()) throw std::runtime_error("CI state is not configured");
          sendAll(client, ciStatusResponse(*state_root, request->argument));
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
    if (argc >= 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-V")) {
      std::cout << ckgit::versionLine("ck-git-hostingd");
      return 0;
    }
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
