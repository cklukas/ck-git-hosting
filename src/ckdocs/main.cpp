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
#include <chrono>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "ckgit/cli_help.hpp"
#include "ckgit/docs_site.hpp"
#include "ckgit/http_request.hpp"
#include "ckgit/pages_store.hpp"
#include "mermaid.hpp"

namespace {

namespace fs = std::filesystem;

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;
constexpr int kWarnings = 3;
constexpr unsigned short kDefaultServePort = 8422;

void usage(std::ostream& out) {
  out << "Usage:\n"
         "  ckdocs build [--root DIR] [--source DIR] [--config FILE] [--ref-tree REF=DIR]... [--out DIR] [--clean]\n"
         "               [--strict] [--quiet]\n"
         "  ckdocs check [--root DIR] [--source DIR] [--config FILE] [--ref-tree REF=DIR]... [--quiet]\n"
         "  ckdocs serve [--root DIR] [--source DIR] [--config FILE] [--ref-tree REF=DIR]... [--out DIR] [--port PORT]\n"
         "               [--no-watch] [--quiet]\n"
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
         "  Mermaid fences render to local SVG images, with light and dark variants.\n"
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
         "  the same way a published Pages site is served, until Ctrl+C. While it serves, it\n"
         "  watches the configuration, the pages, and the source tree, and rebuilds the site\n"
         "  in place about a second after a change settles (reload the browser to see it);\n"
         "  a rebuild that fails keeps the last good site. Inside a Git work tree a new page\n"
         "  appears once Git tracks it (git add). --no-watch serves the first build only.\n"
         "\n"
         "Several sites:\n"
         "  A configuration with a sites: list builds a portal: a welcome page at the output\n"
         "  root and each site under <out>/<id>/, every page with a site selector beside the\n"
         "  brand. Each site has its own configuration, read relative to its root: --root,\n"
         "  or for a site with ref: the tree of that Git ref -- the directory --ref-tree\n"
         "  REF=DIR names, else <ref> or origin/<ref> extracted from --root's repository\n"
         "  into a private directory removed afterwards. Findings are prefixed with the\n"
         "  site's id. --source does not apply to a portal.\n"
         "\n"
         "Options:\n"
         "  --root DIR      The repository to read (default: the current directory).\n"
         "  --source DIR    Override the page source tree, relative to --root.\n"
         "  --site-url URL  Override site.url for this publishing host (also for portals).\n"
         "  --config FILE   Read this file instead of <root>/ckdocs.yml.\n"
         "  --ref-tree REF=DIR\n"
         "                  Use DIR (relative to the current directory) as the tree of the\n"
         "                  Git ref REF for the portal sites that name it, instead of\n"
         "                  extracting it from the repository. Repeatable; portal only.\n"
         "  --out DIR       Where to write the site (default: <root>/public for build, a\n"
         "                  removed-on-exit directory for serve). build, serve.\n"
         "  --clean         Replace an existing site already at --out. build only.\n"
         "  --port PORT     Loopback port to serve on (default: 8422). serve only.\n"
         "  --no-watch      Do not rebuild when the sources change. serve only.\n"
         "  --strict        Fail (exit 1) on any warning: a broken link or heading fragment\n"
         "                  (reported as page:line), a front matter key that is neither\n"
         "                  ckdocs's own nor listed in front_matter.foreign_keys, an\n"
         "                  unusable front matter value, a page an explicit nav does not\n"
         "                  mention, or a Markdown file left out of the site (over the\n"
         "                  512 KiB page limit, unreadable, or not UTF-8). build only\n"
         "                  (check always implies it).\n"
         "  --quiet         Print only errors: no summary line, no warning list.\n"
         "  -h, --help      Show this help.\n"
         "  --version       Print the build version.\n"
         "\n"
         "Examples:\n"
         "  ckdocs build\n"
         "  ckdocs build --root /srv/checkout --out /srv/checkout/public --strict\n"
         "  ckdocs check --root .\n"
         "  ckdocs serve --root .\n"
         "  ckdocs build --ref-tree main=../project-main --clean\n"
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
  std::optional<std::string> site_url;
  std::optional<fs::path> config;
  std::optional<fs::path> out;
  std::map<std::string, fs::path> ref_trees;
  std::optional<unsigned short> port;
  bool clean = false;
  bool strict = false;
  bool quiet = false;
  bool no_watch = false;
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
    } else if (argument == "--site-url") {
      const auto value = next();
      if (!value) { error = "--site-url requires a value"; return false; }
      // Use the configuration's URL validation and trailing-slash policy.
      std::string quoted = "'";
      for (const char byte : *value) quoted += byte == '\'' ? "''" : std::string(1, byte);
      quoted += "'";
      try {
        options.site_url = ckgit::parseDocsConfig("version: 1\nsite:\n  title: URL\n  url: " + quoted + "\n").url;
      } catch (const std::exception& failure) {
        error = "--site-url: " + std::string(failure.what());
        return false;
      }
    } else if (argument == "--config") {
      const auto value = next();
      if (!value) { error = "--config requires a value"; return false; }
      options.config = *value;
    } else if (argument == "--ref-tree") {
      const auto value = next();
      const auto equals = value ? value->find('=') : std::string::npos;
      if (!value || equals == 0 || equals == std::string::npos || equals + 1 == value->size()) {
        error = "--ref-tree requires a value of the form REF=DIR";
        return false;
      }
      if (!options.ref_trees.emplace(value->substr(0, equals), value->substr(equals + 1)).second) {
        error = "--ref-tree names ref '" + value->substr(0, equals) + "' twice";
        return false;
      }
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
    } else if (argument == "--no-watch") {
      if (command != Command::kServe) { error = "--no-watch is only valid for serve"; return false; }
      options.no_watch = true;
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

// The files and directories a build read, as a cheap change fingerprint for
// serve's rebuild-on-change: the configuration, every page, and every entry
// (with its size and modification time) under the watched trees, skipping
// dot-names (so .git) and the output directory. A new or removed file
// changes its directory's entry list, so it is noticed too.
struct WatchSet {
  std::vector<fs::path> files;
  std::vector<fs::path> trees;
  fs::path out;
};

std::uint64_t fingerprint(const WatchSet& watch) {
  std::uint64_t hash = 1469598103934665603ull;  // FNV-1a over names, sizes, times
  const auto mix = [&hash](std::string_view bytes) {
    for (const unsigned char byte : bytes) {
      hash ^= byte;
      hash *= 1099511628211ull;
    }
  };
  const auto mixEntry = [&](const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    mix(path.native());
    if (error || !fs::exists(status)) {
      mix("-");
      return;
    }
    const auto time = fs::last_write_time(path, error).time_since_epoch().count();
    mix(std::string_view(reinterpret_cast<const char*>(&time), sizeof(time)));
    if (fs::is_regular_file(status)) {
      const auto size = fs::file_size(path, error);
      mix(std::string_view(reinterpret_cast<const char*>(&size), sizeof(size)));
    }
  };
  for (const auto& file : watch.files) mixEntry(file);
  for (const auto& tree : watch.trees) {
    std::error_code error;
    mixEntry(tree);
    fs::recursive_directory_iterator walk(tree, fs::directory_options::skip_permission_denied, error);
    for (; !error && walk != fs::recursive_directory_iterator(); walk.increment(error)) {
      const fs::path& path = walk->path();
      const std::string name = path.filename().string();
      if ((!name.empty() && name.front() == '.') || (!watch.out.empty() && path == watch.out)) {
        walk.disable_recursion_pending();
        continue;
      }
      mixEntry(path);
    }
  }
  return hash;
}

// Serves `site` on 127.0.0.1:`port` (0 = an OS-chosen port) until SIGINT or
// SIGTERM. Prints the bound port once listening, in the same "<program>:
// ... ready on <port>" shape ck-git-hostingd and ck-pagesd already use. With
// `rebuild`, it checks `watch` about twice a second between requests and,
// once a change has settled, calls `rebuild` to refresh the site in place.
int runServe(const fs::path& site, unsigned short port, const std::function<void()>& rebuild, const WatchSet& watch) {
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
  using Clock = std::chrono::steady_clock;
  constexpr auto kScanInterval = std::chrono::milliseconds(500);
  std::uint64_t built = rebuild ? fingerprint(watch) : 0;
  std::optional<std::uint64_t> pending;  // a changed fingerprint waiting to settle
  auto next_scan = Clock::now() + kScanInterval;
  while (g_serve_stop == 0) {
    pollfd waiting{listener, POLLIN, 0};
    const int ready = ::poll(&waiting, 1, rebuild ? 250 : -1);
    if (ready < 0 && errno != EINTR) break;
    if (ready > 0) {
      const int client = ::accept(listener, nullptr, nullptr);
      if (client >= 0) {
        const timeval timeout{10, 0};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        try {
          handleServeConnection(client, site);
        } catch (const std::exception&) {
        }
        ::close(client);
      }
    }
    if (!rebuild || Clock::now() < next_scan) continue;
    next_scan = Clock::now() + kScanInterval;
    const std::uint64_t now = fingerprint(watch);
    if (now == built) {
      pending.reset();
    } else if (pending != now) {
      pending = now;  // wait one more scan: an editor may still be writing
    } else {
      pending.reset();
      built = now;
      rebuild();
    }
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
  std::map<std::string, fs::path> ref_trees;
  for (const auto& [ref, directory] : options.ref_trees) {
    ref_trees.emplace(ref, fs::absolute(directory, error));
    if (error) { std::cerr << "ckdocs: cannot resolve '" << directory.string() << "'\n"; return kFailed; }
  }

  const bool strict = options.strict || command == Command::kCheck;  // check always implies --strict
  std::optional<ScopedTempDirectory> scratch;
  fs::path target;
  ckgit::DocsBuildOptions build_options;
  build_options.mermaid_renderer = ckgit::renderMermaid;
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
    // too (there is no flag for it): it rebuilds the site --out holds on
    // every change, and a second run must be able to replace it as well.
    build_options.clean = options.clean || command == Command::kServe;
  }
  if (command == Command::kServe) build_options.clean = true;  // rebuilds replace the served site

  // One complete build: configuration, model, site, report. Throws on
  // anything that must stop a build; returns the exit status otherwise and
  // records what a rebuild must watch.
  WatchSet watch;
  watch.out = target;
  const auto buildOnce = [&](bool rebuilding) -> int {
    const fs::path config_path = options.config ? fs::absolute(*options.config) : root / "ckdocs.yml";
    ckgit::DocsConfig config =
        options.config ? ckgit::parseDocsConfig(readWholeFile(*options.config)) : ckgit::readDocsConfig(root);
    const bool several = !config.sites.empty();
    if (several && options.source) {
      std::cerr << "ckdocs: --source does not apply to a configuration with sites; set source in each site's own configuration\n";
      return kUsage;
    }
    if (!several && !options.ref_trees.empty()) {
      std::cerr << "ckdocs: --ref-tree only applies to a configuration with sites\n";
      return kUsage;
    }
    if (options.source) config.source = options.source->generic_string();
    if (options.site_url) config.url = *options.site_url;

    std::vector<std::string> problems;
    std::optional<ckgit::DocsSiteModel> model;
    std::optional<ckgit::DocsPortalModel> portal;
    watch.files = {config_path};
    watch.trees.clear();
    if (several) {
      ckgit::DocsPortalOptions portal_options;
      portal_options.ref_trees = ref_trees;
      portal = ckgit::loadDocsPortal(root, config, portal_options, &problems);
      watch.trees.push_back(root);  // every site's tree and configuration lives below it
    } else {
      model = ckgit::loadDocsSite(root, config, &problems);
      watch.trees.push_back(model->source.empty() ? root : root / model->source);
      for (const auto& page : model->pages) watch.files.push_back(root / page.source);
      if (!config.logo.empty()) watch.files.push_back(root / config.logo);
      if (!config.stylesheet.empty()) watch.files.push_back(root / config.stylesheet);
    }

    ckgit::DocsBuildReport report;
    if (portal) {
      ckgit::buildDocsPortal(*portal, target, build_options, &report);
    } else {
      ckgit::buildDocsSite(*model, target, build_options, &report);
    }
    // Extracted ref trees are only read while building; serve must not keep them.
    const std::size_t sites = portal ? portal->sites.size() : 0;
    portal.reset();
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
      const std::string in_sites = sites > 0 ? " in " + std::to_string(sites) + " site(s)" : std::string();
      if (command == Command::kCheck) {
        std::cout << "Checked " << report.pages_written << " page(s)" << in_sites << ": nothing to report.\n";
      } else {
        std::cout << (rebuilding ? "Rebuilt " : "Built ") << report.pages_written << " page(s)" << in_sites << ", "
                  << report.assets_copied + report.assets_generated << " asset(s), ";
        if (report.redirects_written > 0) std::cout << report.redirects_written << " redirect(s), ";
        std::cout << report.bytes_written << " byte(s) to " << target.string() << ".\n";
      }
      std::cout << std::flush;
    }
    return problems.empty() ? kOk : kWarnings;
  };

  const int status = buildOnce(false);
  if (command != Command::kServe) return status;
  if (status == kUsage || status == kFailed) return status;
  std::function<void()> rebuild;
  if (!options.no_watch) {
    rebuild = [&]() {
      // A failed rebuild keeps serving the last good site and says why.
      try {
        buildOnce(true);
      } catch (const std::exception& failure) {
        const std::string_view text = failure.what();
        std::cerr << (text.starts_with("ckdocs: ") ? "" : "ckdocs: ") << text << "; still serving the last build\n";
      }
    };
  }
  return runServe(target, options.port.value_or(kDefaultServePort), rebuild, watch);
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
    // Library errors carry the "ckdocs: " prefix already.
    const std::string_view text = error.what();
    std::cerr << (text.starts_with("ckdocs: ") ? "" : "ckdocs: ") << text << "\n";
    return kFailed;
  }
}
