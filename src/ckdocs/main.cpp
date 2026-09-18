// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// ckdocs turns a repository's Markdown (a README and a docs/ tree) into a
// self-contained static documentation site: relative links between pages, a
// pull model for referenced images and files, no JavaScript. It is a
// separate, dependency-free binary so it can run wherever a docs build step
// runs -- inside the server's sandboxed CI runner, on a developer's machine,
// or on a GitHub Actions runner -- without the sync client or the server
// being installed. See include/ckgit/docs_site.hpp for the model and
// rendering it drives.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "ckgit/cli_help.hpp"
#include "ckgit/docs_site.hpp"
#include "ckgit/http_request.hpp"
#include "ckgit/pages_store.hpp"

namespace {

namespace fs = std::filesystem;

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;
constexpr int kWarnings = 3;
constexpr unsigned short kDefaultServePort = 8422;

void usage(std::ostream& out) {
  out << "Usage:\n"
         "  ckdocs build [--root DIR] [--source DIR] [--config FILE] [--out DIR] [--clean] [--strict] [--quiet]\n"
         "  ckdocs check [--root DIR] [--source DIR] [--config FILE] [--quiet]\n"
         "  ckdocs serve [--root DIR] [--source DIR] [--config FILE] [--out DIR] [--port PORT] [--quiet]\n"
         "  ckdocs --version\n"
         "  ckdocs --help\n"
         "\n"
         "Scope and defaults:\n"
         "  --root defaults to the current directory. --config defaults to <root>/ckdocs.yml\n"
         "  when that file exists, else the site uses its built-in defaults (the repository's\n"
         "  own title, docs/ when present else the root as the page source). --source, when\n"
         "  given, overrides the source tree the configuration or the default would pick,\n"
         "  relative to --root. build's --out defaults to <root>/public; serve's --out\n"
         "  defaults to a private directory removed when it exits, and its --port to 8422.\n"
         "\n"
         "Effects:\n"
         "  build renders every Markdown page under the source tree into a self-contained\n"
         "  static site at --out. Pages come only from what Git tracks in a work tree (a\n"
         "  gitignored planning directory, for example, is never read) or, outside a work\n"
         "  tree, from a walk that skips dotfiles, dot-directories, and symlinks. Links\n"
         "  between pages stay relative, so the result works from file:// and under any URL\n"
         "  prefix; images and files are copied only when a page references them. --out may\n"
         "  be absent, empty, or a site this tool wrote before (its .ckdocs marker); --clean\n"
         "  is required to replace one that already holds pages, and anything else there is\n"
         "  refused untouched. A link or heading fragment that resolves to nothing stays\n"
         "  visible text and is reported rather than stopping the build, unless --strict asks\n"
         "  for it to fail instead. check builds into a private temporary directory that is\n"
         "  always removed, implies --strict, and prints the same report -- use it in CI or\n"
         "  before publishing, when only the outcome matters. serve builds like build --out\n"
         "  is given (replacing a previous ckdocs site there the way --clean would), or into\n"
         "  a private directory removed on exit otherwise, then serves it on 127.0.0.1:--port\n"
         "  the same way a published Pages site is served, until Ctrl+C; there is no rebuild\n"
         "  on change in this version -- edit, then rerun the command.\n"
         "\n"
         "Options:\n"
         "  --root DIR      The repository to read (default: the current directory).\n"
         "  --source DIR    Override the page source tree, relative to --root.\n"
         "  --config FILE   Read this file instead of <root>/ckdocs.yml.\n"
         "  --out DIR       Where to write the site (default: <root>/public for build, a\n"
         "                  removed-on-exit directory for serve). build, serve.\n"
         "  --clean         Replace an existing site already at --out. build only.\n"
         "  --port PORT     Loopback port to serve on (default: 8422). serve only.\n"
         "  --strict        Fail (exit 1) on any warning: a broken link or heading fragment,\n"
         "                  an unrecognised front matter value, or a page an explicit nav\n"
         "                  does not mention. build only (check always implies it).\n"
         "  --quiet         Print only errors: no summary line, no warning list.\n"
         "  -h, --help      Show this help.\n"
         "  --version       Print the build version.\n"
         "\n"
         "Examples:\n"
         "  ckdocs build\n"
         "  ckdocs build --root /srv/checkout --out /srv/checkout/public --strict\n"
         "  ckdocs check --root .\n"
         "  ckdocs serve --root .\n"
         "\n"
         "Exit codes:\n"
         "  0  Built (or checked) with nothing to report; serve exits 0 after Ctrl+C.\n"
         "  1  Failed to build, or a warning became a failure under --strict.\n"
         "  2  Invalid arguments; see the usage above.\n"
         "  3  Built with warnings (build only, without --strict); read the report.\n";
}

enum class Command { kBuild, kCheck, kServe };

struct Options {
  fs::path root = ".";
  std::optional<fs::path> source;
  std::optional<fs::path> config;
  std::optional<fs::path> out;
  std::optional<unsigned short> port;
  bool clean = false;
  bool strict = false;
  bool quiet = false;
};

// Parses the options for `command`, starting at argv[start]. Each option is
// only valid for the command(s) noted in --help above; anything else is
// rejected the same way an unrecognised option is. Returns false and sets
// `error` on the first problem.
bool parseOptions(int argc, char** argv, int start, Command command, Options& options, std::string& error) {
  const bool allow_out = command != Command::kCheck;
  for (int index = start; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&]() -> std::optional<std::string> {
      if (index + 1 >= argc) return std::nullopt;
      return std::string(argv[++index]);
    };
    if (argument == "--root") {
      const auto value = next();
      if (!value) { error = "--root requires a value"; return false; }
      options.root = *value;
    } else if (argument == "--source") {
      const auto value = next();
      if (!value) { error = "--source requires a value"; return false; }
      options.source = *value;
    } else if (argument == "--config") {
      const auto value = next();
      if (!value) { error = "--config requires a value"; return false; }
      options.config = *value;
    } else if (argument == "--out") {
      if (!allow_out) { error = "--out is not valid for check"; return false; }
      const auto value = next();
      if (!value) { error = "--out requires a value"; return false; }
      options.out = *value;
    } else if (argument == "--clean") {
      if (command != Command::kBuild) { error = "--clean is only valid for build"; return false; }
      options.clean = true;
    } else if (argument == "--port") {
      if (command != Command::kServe) { error = "--port is only valid for serve"; return false; }
      const auto value = next();
      unsigned long parsed = 0;
      if (!value || value->empty() || !std::all_of(value->begin(), value->end(), [](unsigned char byte) { return std::isdigit(byte) != 0; }) ||
          (parsed = std::strtoul(value->c_str(), nullptr, 10)) > 65535) {
        error = "--port requires a value from 0 to 65535";
        return false;
      }
      options.port = static_cast<unsigned short>(parsed);
    } else if (argument == "--strict") {
      options.strict = true;
    } else if (argument == "--quiet") {
      options.quiet = true;
    } else {
      error = "unknown option '" + argument + "'";
      return false;
    }
  }
  return true;
}

// Reads a whole file into a string. A size well beyond ckdocs.yml's own bound
// (kMaximumDocsConfigBytes) is refused here so a huge file gets a clear
// message instead of a slow, pointless read; the exact bound is enforced by
// parseDocsConfig itself, with its own wording.
std::string readWholeFile(const fs::path& path) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error) throw std::runtime_error("cannot read '" + path.string() + "': " + error.message());
  if (size > 8 * ckgit::kMaximumDocsConfigBytes) {
    throw std::runtime_error("'" + path.string() + "' is far larger than a ckdocs configuration file should be");
  }
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (!in) throw std::runtime_error("cannot read '" + path.string() + "'");
  return buffer.str();
}

// Removes its directory on destruction, regardless of what happened inside
// it. check never leaves anything behind, on success or failure.
struct ScopedTempDirectory {
  fs::path path;

  explicit ScopedTempDirectory(std::string_view prefix) {
    auto pattern = (fs::temp_directory_path() / (std::string(prefix) + "-XXXXXX")).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    if (::mkdtemp(writable.data()) == nullptr) throw std::runtime_error("cannot create a temporary directory for check");
    path = writable.data();
  }
  ~ScopedTempDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }
  ScopedTempDirectory(const ScopedTempDirectory&) = delete;
  ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;
};

// ---- serve: a small, loopback-only static file server ---------------------
//
// This is deliberately independent of ck-pagesd's own accept loop (which
// carries systemd/ServerConfig/runtime-recording concerns that do not apply
// to an ephemeral CLI preview); it shares only the two pieces that matter for
// two servers to behave identically to a reader -- the HTTP request parser
// (ckgit::parseReadOnlyHttpRequest) and the traversal-safe file resolution
// (ckgit::readSiteFile / ckgit::decodeRequestPath), both already used by
// ck-pagesd (src/pages-server/main.cpp).

volatile std::sig_atomic_t g_serve_stop = 0;
void onServeStop(int) { g_serve_stop = 1; }

constexpr std::size_t kMaximumServeRequestBytes = 16 * 1024;
constexpr std::size_t kMaximumServedFileBytes = static_cast<std::size_t>(1) << 30;  // matches kMaximumPagesFileBytes

bool readServeHeaders(int fd, std::string* out) {
  char buffer[4096];
  while (out->find("\r\n\r\n") == std::string::npos) {
    if (out->size() > kMaximumServeRequestBytes) return false;
    const ssize_t received = ::read(fd, buffer, sizeof(buffer));
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) return false;
    out->append(buffer, static_cast<std::size_t>(received));
  }
  return true;
}

bool writeServeAll(int fd, std::string_view data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

void sendServeResponse(int fd, int status, std::string_view reason, std::string_view content_type,
                       std::string_view body, bool head_only) {
  const std::string headers = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
                              "\r\nContent-Type: " + std::string(content_type) +
                              "\r\nContent-Length: " + std::to_string(body.size()) +
                              "\r\nX-Content-Type-Options: nosniff"
                              "\r\nReferrer-Policy: no-referrer"
                              "\r\nCache-Control: no-cache"
                              "\r\nConnection: close\r\n\r\n";
  if (!writeServeAll(fd, headers)) return;
  if (!head_only) writeServeAll(fd, body);
}

void sendServeError(int fd, int status, std::string_view reason, bool head_only) {
  const std::string body =
      "<!doctype html><meta charset=utf-8><title>" + std::to_string(status) + "</title><p>" + std::string(reason) + "</p>\n";
  sendServeResponse(fd, status, reason, "text/html; charset=utf-8", body, head_only);
}

void handleServeConnection(int fd, const fs::path& site) {
  std::string raw;
  if (!readServeHeaders(fd, &raw)) return;
  const std::optional<ckgit::HttpRequest> request = ckgit::parseReadOnlyHttpRequest(raw);
  if (!request.has_value()) {
    sendServeError(fd, 400, "Bad Request", false);
    return;
  }
  const bool head_only = request->method == ckgit::HttpMethod::kHead;
  std::string_view target = request->target;
  const auto query = target.find_first_of("?#");
  if (query != std::string_view::npos) target = target.substr(0, query);
  const std::optional<std::string> decoded = ckgit::decodeRequestPath(target);
  if (!decoded.has_value() || decoded->empty() || decoded->front() != '/') {
    sendServeError(fd, 404, "Not Found", head_only);
    return;
  }
  const std::optional<ckgit::PageFile> page =
      ckgit::readSiteFile(site, std::string_view(*decoded).substr(1), kMaximumServedFileBytes);
  if (!page.has_value()) {
    sendServeError(fd, 404, "Not Found", head_only);
    return;
  }
  sendServeResponse(fd, 200, "OK", page->content_type, page->content, head_only);
}

// Serves `site` on 127.0.0.1:`port` (0 = an OS-chosen port) until SIGINT or
// SIGTERM. Prints the bound port once listening, in the same "<program>:
// ... ready on <port>" shape ck-git-hostingd and ck-pagesd already use.
int runServe(const fs::path& site, unsigned short port) {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) throw std::runtime_error("could not create the listening socket");
  const int enabled = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // loopback only: a local preview, never LAN-exposed
  address.sin_port = htons(port);
  if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(listener, 16) != 0) {
    const int bind_errno = errno;
    ::close(listener);
    throw std::runtime_error("could not bind 127.0.0.1:" + std::to_string(port) + ": " + std::strerror(bind_errno));
  }

  struct sigaction action {};
  action.sa_handler = onServeStop;
  ::sigaction(SIGTERM, &action, nullptr);
  ::sigaction(SIGINT, &action, nullptr);

  unsigned short bound_port = port;
  sockaddr_in bound{};
  socklen_t bound_size = sizeof(bound);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_size) == 0 && bound_size == sizeof(bound)) {
    bound_port = ntohs(bound.sin_port);
  }
  std::cout << "ckdocs: serve ready on " << bound_port << "\n" << std::flush;
  while (g_serve_stop == 0) {
    const int client = ::accept(listener, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      break;
    }
    const timeval timeout{10, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    try {
      handleServeConnection(client, site);
    } catch (const std::exception&) {
    }
    ::close(client);
  }
  ::close(listener);
  return kOk;
}

int run(const std::string& command_name, int argc, char** argv) {
  const Command command = command_name == "build"   ? Command::kBuild
                          : command_name == "check" ? Command::kCheck
                                                     : Command::kServe;
  Options options;
  std::string parse_error;
  if (!parseOptions(argc, argv, 2, command, options, parse_error)) {
    std::cerr << "ckdocs: " << parse_error << "\n";
    usage(std::cerr);
    return kUsage;
  }

  std::error_code error;
  const auto root = fs::absolute(options.root, error);
  if (error) { std::cerr << "ckdocs: cannot resolve '" << options.root.string() << "'\n"; return kFailed; }

  ckgit::DocsConfig config =
      options.config ? ckgit::parseDocsConfig(readWholeFile(*options.config)) : ckgit::readDocsConfig(root);
  if (options.source) config.source = options.source->generic_string();

  std::vector<std::string> problems;
  const auto model = ckgit::loadDocsSite(root, config, &problems);

  const bool strict = options.strict || command == Command::kCheck;  // check always implies --strict
  std::optional<ScopedTempDirectory> scratch;
  fs::path target;
  ckgit::DocsBuildOptions build_options;
  if (command == Command::kCheck) {
    scratch.emplace("ckdocs-check");
    target = scratch->path;
  } else if (command == Command::kServe && !options.out) {
    // "into a temporary directory when --out is absent": unlike build, serve
    // without --out never touches <root>/public.
    scratch.emplace("ckdocs-serve");
    target = scratch->path;
  } else {
    target = options.out ? fs::absolute(*options.out, error) : root / "public";
    // serve with an explicit --out always behaves as if --clean were given
    // too (there is no flag for it): "rerun the command" after an edit is
    // the whole workflow, so a second run must be able to replace the site
    // --out already holds instead of refusing it.
    build_options.clean = options.clean || command == Command::kServe;
  }

  ckgit::DocsBuildReport report;
  ckgit::buildDocsSite(model, target, build_options, &report);
  for (const auto& broken : report.broken_links) problems.push_back(broken);
  for (const auto& broken : report.broken_anchors) problems.push_back(broken);
  for (const auto& warning : report.warnings) problems.push_back(warning);

  if (!problems.empty() && strict) {
    for (const auto& problem : problems) std::cerr << "ckdocs: " << problem << "\n";
    std::cerr << "ckdocs: " << problems.size() << " warning(s) failed under --strict\n";
    return kFailed;
  }
  if (!options.quiet) {
    for (const auto& problem : problems) std::cout << "warning: " << problem << "\n";
    if (command == Command::kCheck) {
      std::cout << "Checked " << report.pages_written << " page(s): nothing to report.\n";
    } else {
      std::cout << "Built " << report.pages_written << " page(s), " << report.assets_copied << " asset(s), "
                << report.bytes_written << " byte(s) to " << target.string() << ".\n";
    }
  }
  if (command == Command::kServe) return runServe(target, options.port.value_or(kDefaultServePort));
  return problems.empty() ? kOk : kWarnings;
}

}  // namespace

int main(int argc, char* argv[]) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h" || (index == 1 && argument == "help")) {
      usage(std::cout);
      return kOk;
    }
    if (argument == "--version") {
      std::cout << ckgit::versionLine("ckdocs");
      return kOk;
    }
  }
  if (argc < 2) {
    usage(std::cerr);
    return kUsage;
  }
  const std::string command = argv[1];
  if (command != "build" && command != "check" && command != "serve") {
    std::cerr << "ckdocs: unknown command '" << command << "'\n";
    usage(std::cerr);
    return kUsage;
  }
  try {
    return run(command, argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "ckdocs: " << error.what() << "\n";
    return kFailed;
  }
}
