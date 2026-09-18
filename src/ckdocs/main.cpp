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

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "ckgit/cli_help.hpp"
#include "ckgit/docs_site.hpp"

namespace {

namespace fs = std::filesystem;

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;
constexpr int kWarnings = 3;

void usage(std::ostream& out) {
  out << "Usage:\n"
         "  ckdocs build [--root DIR] [--source DIR] [--config FILE] [--out DIR] [--clean] [--strict] [--quiet]\n"
         "  ckdocs check [--root DIR] [--source DIR] [--config FILE] [--quiet]\n"
         "  ckdocs --version\n"
         "  ckdocs --help\n"
         "\n"
         "Scope and defaults:\n"
         "  --root defaults to the current directory. --config defaults to <root>/ckdocs.yml\n"
         "  when that file exists, else the site uses its built-in defaults (the repository's\n"
         "  own title, docs/ when present else the root as the page source). --source, when\n"
         "  given, overrides the source tree the configuration or the default would pick,\n"
         "  relative to --root. build's --out defaults to <root>/public.\n"
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
         "  before publishing, when only the outcome matters.\n"
         "\n"
         "Options:\n"
         "  --root DIR      The repository to read (default: the current directory).\n"
         "  --source DIR    Override the page source tree, relative to --root.\n"
         "  --config FILE   Read this file instead of <root>/ckdocs.yml.\n"
         "  --out DIR       Where to write the site (default: <root>/public). build only.\n"
         "  --clean         Replace an existing site already at --out. build only.\n"
         "  --strict        Fail (exit 1) on any warning: a broken link or heading fragment,\n"
         "                  an unrecognised front matter value, or a page an explicit nav\n"
         "                  does not mention.\n"
         "  --quiet         Print only errors: no summary line, no warning list.\n"
         "  -h, --help      Show this help.\n"
         "  --version       Print the build version.\n"
         "\n"
         "Examples:\n"
         "  ckdocs build\n"
         "  ckdocs build --root /srv/checkout --out /srv/checkout/public --strict\n"
         "  ckdocs check --root .\n"
         "\n"
         "Exit codes:\n"
         "  0  Built (or checked) with nothing to report.\n"
         "  1  Failed to build, or a warning became a failure under --strict.\n"
         "  2  Invalid arguments; see the usage above.\n"
         "  3  Built with warnings (build only, without --strict); read the report.\n";
}

struct Options {
  fs::path root = ".";
  std::optional<fs::path> source;
  std::optional<fs::path> config;
  std::optional<fs::path> out;
  bool clean = false;
  bool strict = false;
  bool quiet = false;
};

// Parses the options common to both commands, starting at argv[start].
// `allow_out_and_clean` restricts --out/--clean to `build`. Returns false and
// sets `error` on the first problem.
bool parseOptions(int argc, char** argv, int start, bool allow_out_and_clean, Options& options, std::string& error) {
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
      if (!allow_out_and_clean) { error = "--out is only valid for build"; return false; }
      const auto value = next();
      if (!value) { error = "--out requires a value"; return false; }
      options.out = *value;
    } else if (argument == "--clean") {
      if (!allow_out_and_clean) { error = "--clean is only valid for build"; return false; }
      options.clean = true;
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

int run(const std::string& command, int argc, char** argv) {
  const bool is_build = command == "build";
  Options options;
  std::string parse_error;
  if (!parseOptions(argc, argv, 2, is_build, options, parse_error)) {
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

  const bool strict = options.strict || !is_build;  // check always implies --strict
  std::optional<ScopedTempDirectory> scratch;
  fs::path target;
  ckgit::DocsBuildOptions build_options;
  if (is_build) {
    target = options.out ? fs::absolute(*options.out, error) : root / "public";
    build_options.clean = options.clean;
  } else {
    scratch.emplace("ckdocs-check");
    target = scratch->path;
  }

  ckgit::DocsBuildReport report;
  ckgit::buildDocsSite(model, target, build_options, &report);
  for (const auto& broken : report.broken_links) problems.push_back(broken);
  for (const auto& broken : report.broken_anchors) problems.push_back(broken);

  if (!problems.empty() && strict) {
    for (const auto& problem : problems) std::cerr << "ckdocs: " << problem << "\n";
    std::cerr << "ckdocs: " << problems.size() << " warning(s) failed under --strict\n";
    return kFailed;
  }
  if (!options.quiet) {
    for (const auto& problem : problems) std::cout << "warning: " << problem << "\n";
    if (is_build) {
      std::cout << "Built " << report.pages_written << " page(s), " << report.assets_copied << " asset(s), "
                << report.bytes_written << " byte(s) to " << target.string() << ".\n";
    } else {
      std::cout << "Checked " << report.pages_written << " page(s): nothing to report.\n";
    }
  }
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
  if (command != "build" && command != "check") {
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
