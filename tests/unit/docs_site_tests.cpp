// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/docs_site.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "ckgit/http_router.hpp"
#include "ckgit/process.hpp"

namespace {

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("docs site: " + message);
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// The message a rejected config carries, or "" when it parsed.
std::string rejection(const std::string& yaml) {
  try {
    static_cast<void>(ckgit::parseDocsConfig(yaml));
    return {};
  } catch (const std::exception& error) {
    return error.what();
  }
}

std::string loadError(const fs::path& root, const ckgit::DocsConfig& config) {
  try {
    static_cast<void>(ckgit::loadDocsSite(root, config, nullptr));
    return {};
  } catch (const std::exception& error) {
    return error.what();
  }
}

void write(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
  require(bool(out), "write fixture " + path.string());
}

void git(const fs::path& path, std::vector<std::string> arguments) {
  std::vector<std::string> command{"git", "-C", path.string(), "-c", "commit.gpgSign=false"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto result = ckgit::runProcess(command, std::chrono::seconds(10), 4 * 1024 * 1024);
  require(result.exit_code == 0 && !result.timed_out, "fixture git failed: " + result.output);
}

// A scratch directory beneath TMPDIR, removed when the fixture goes away.
class Scratch {
 public:
  Scratch() {
    const char* configured = std::getenv("TMPDIR");
    require(configured != nullptr && *configured != '\0', "TMPDIR must be explicitly selected");
    auto pattern = (fs::path(configured) / "ckdocs-tests-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(mkdtemp(writable.data()) != nullptr, "could not create the fixture directory");
    root = writable.data();
  }
  ~Scratch() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  fs::path root;
};

// The page tree every discovery test starts from.
void writeFixtureTree(const fs::path& root) {
  write(root / "README.md", "# Fixture home\n\nIntro.\n");
  write(root / "docs/a/01-x.md", "# X page\n\nBody.\n");
  write(root / "docs/a/02-y.md", "---\ntitle: Why\nnav_order: 1\n---\n# Y\n");
  write(root / "docs/b/README.md", "---\ntitle: Bee\ndescription: About b\n---\n# B\n");
  write(root / "docs/b/deep/inner.md", "# Inner\n");
  write(root / "docs/z.md", "```\n# not a title\n```\n# Zed *page*\n");
  write(root / "docs/hidden.md", "---\nnav_exclude: true\nlayout: post\n---\n# Hidden\n");
  write(root / "docs/.drafts/n.md", "# Draft\n");
  write(root / "docs/notes.txt", "not a page\n");
  fs::create_symlink("z.md", root / "docs/link.md");
}

const ckgit::DocsPage& pageNamed(const ckgit::DocsSiteModel& model, std::string_view source) {
  for (const auto& page : model.pages) {
    if (page.source == source) return page;
  }
  throw std::runtime_error("docs site: no page " + std::string(source));
}

std::size_t indexOf(const ckgit::DocsSiteModel& model, std::string_view source) {
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    if (model.pages[index].source == source) return index;
  }
  throw std::runtime_error("docs site: no page " + std::string(source));
}

void checkDerivedModel(const ckgit::DocsSiteModel& model, const fs::path& root) {
  require(model.source == "docs", "docs/ is the default source");
  require(model.title == root.filename().string(), "the site title defaults to the root directory's name");
  const std::vector<std::string> expected{"README.md",       "docs/a/01-x.md", "docs/a/02-y.md", "docs/b/README.md",
                                          "docs/b/deep/inner.md", "docs/hidden.md", "docs/z.md"};
  require(model.pages.size() == expected.size(), "seven pages: no symlink, dot-directory, or .txt");
  for (std::size_t index = 0; index < expected.size(); ++index) {
    require(model.pages[index].source == expected[index], "pages are sorted by source path: " + expected[index]);
  }
  require(model.home == 0 && model.pages[0].home && model.pages[0].output == "index.html" && model.pages[0].title == "Fixture home",
          "the root README is the home page, written as index.html, titled by its heading");
  require(pageNamed(model, "docs/a/01-x.md").output == "a/01-x.html" && pageNamed(model, "docs/a/01-x.md").title == "X page",
          "a page keeps its source-relative path with .html");
  require(pageNamed(model, "docs/a/02-y.md").title == "Why" && pageNamed(model, "docs/a/02-y.md").nav_order == 1,
          "front matter title and nav_order");
  require(pageNamed(model, "docs/b/README.md").output == "b/index.html" && pageNamed(model, "docs/b/README.md").title == "Bee" &&
              pageNamed(model, "docs/b/README.md").description == "About b",
          "a directory README becomes its index.html and carries its description");
  require(pageNamed(model, "docs/b/deep/inner.md").output == "b/deep/inner.html", "nested output path");
  require(pageNamed(model, "docs/z.md").title == "Zed page", "the first heading outside fences names a page, as plain text");
  require(pageNamed(model, "docs/hidden.md").nav_exclude && pageNamed(model, "docs/hidden.md").output == "hidden.html",
          "nav_exclude is read; the page is still built");

  require(model.nav.size() == 4, "tabs: Home, A, Bee, Zed");
  require(model.nav[0].title == "Home" && model.nav[0].page == model.home && model.nav[0].children.empty(), "the Home tab");
  require(model.nav[1].title == "A" && !model.nav[1].page && model.nav[1].children.size() == 2, "a directory tab titled by its name");
  require(model.nav[1].children[0].title == "Why" && model.nav[1].children[0].page == indexOf(model, "docs/a/02-y.md") &&
              model.nav[1].children[1].title == "X page",
          "nav_order puts Why before X page despite the filenames");
  require(model.nav[2].title == "Bee" && model.nav[2].page == indexOf(model, "docs/b/README.md") &&
              model.nav[2].children.size() == 1 && model.nav[2].children[0].title == "Deep" &&
              model.nav[2].children[0].children.size() == 1 && model.nav[2].children[0].children[0].title == "Inner",
          "a directory with a README is titled by it and groups its subdirectory");
  require(model.nav[3].title == "Zed page" && model.nav[3].page == indexOf(model, "docs/z.md") && model.nav[3].children.empty(),
          "a top-level page is a one-page tab, ordered by filename after the directories");
  const std::vector<std::size_t> order{0, indexOf(model, "docs/a/02-y.md"), indexOf(model, "docs/a/01-x.md"),
                                       indexOf(model, "docs/b/README.md"), indexOf(model, "docs/b/deep/inner.md"),
                                       indexOf(model, "docs/z.md")};
  require(model.reading_order == order, "reading order is depth-first through the tabs");
  require(model.unlisted == std::vector<std::size_t>{indexOf(model, "docs/hidden.md")}, "an excluded page is unlisted");
}

void testConfig() {
  const auto config = ckgit::parseDocsConfig(
      "version: 1\n"
      "site:\n"
      "  title: ck-git-hosting\n"
      "  brand: ck git hosting\n"
      "  logo: docs/assets/logo.svg\n"
      "  description: A small Git control plane\n"
      "  footer: \"© 2026 C. Klukas\"\n"
      "  links:\n"
      "    - title: GitHub\n"
      "      url: https://github.com/cklukas/ck-git-hosting\n"
      "    - title: Mail\n"
      "      url: mailto:someone@example.test\n"
      "  stylesheet: docs/assets/site.css\n"
      "source: docs\n"
      "home: README.md\n"
      "exclude: [drafts, internal/*]\n"
      "nav:\n"
      "  - page: README.md\n"
      "  - title: Operations\n"
      "    pages:\n"
      "      - docs/operations/01-installation.md\n"
      "      - title: Continuous delivery\n"
      "        pages: [docs/operations/04-ci-cd.md]\n"
      "search: true\n");
  require(config.title == "ck-git-hosting" && config.brand == "ck git hosting" && config.logo == "docs/assets/logo.svg" &&
              config.description == "A small Git control plane" && config.footer == "© 2026 C. Klukas" &&
              config.stylesheet == "docs/assets/site.css" && config.source == "docs" && config.home == "README.md" && config.search,
          "every site field is read");
  require(config.links.size() == 2 && config.links[0].title == "GitHub" && config.links[1].url == "mailto:someone@example.test",
          "links are title/url pairs");
  require(config.exclude == std::vector<std::string>{"drafts", "internal/*"}, "exclude patterns");
  require(config.nav.size() == 2 && config.nav[0].page == "README.md" && config.nav[0].title.empty() &&
              config.nav[1].title == "Operations" && config.nav[1].pages.size() == 2 &&
              config.nav[1].pages[0].page == "docs/operations/01-installation.md" &&
              config.nav[1].pages[1].title == "Continuous delivery" && config.nav[1].pages[1].pages.size() == 1 &&
              config.nav[1].pages[1].pages[0].page == "docs/operations/04-ci-cd.md",
          "nav entries: bare pages, titled pages, groups");

  const std::string minimal = "version: 1\nsite:\n  title: x\n";
  require(rejection(minimal).empty(), "a minimal config parses");
  require(contains(rejection(""), "ckdocs.yml: the config file is empty (line 1)"), "an empty file names itself");
  require(contains(rejection("site:\n  title: x\n"), "missing 'version'"), "version is required");
  require(contains(rejection("version: 2\nsite:\n  title: x\n"), "version must be 1"), "version must be 1");
  require(contains(rejection("version: 1\n"), "missing 'site'"), "site is required");
  require(contains(rejection("version: 1\nsite:\n  brand: x\n"), "site needs a 'title'"), "site.title is required");
  require(contains(rejection("version: 1\nsite:\n  title: ''\n"), "site.title is empty"), "an empty title");
  require(contains(rejection("version: 1\nsite:\n  title: \"a\\tb\"\n"), "site.title is empty, longer"), "a control character in a title");
  require(contains(rejection(minimal + "bogus: 1\n"), "unknown key 'bogus'"), "unknown top-level keys are rejected");
  require(contains(rejection("version: 1\nsite:\n  title: x\n  theme: dark\n"), "unknown key 'theme'"), "unknown site keys are rejected");
  require(contains(rejection("version: 1\nsite:\n  title: x\n  logo: ../x.svg\n"), "site.logo must be a relative path"),
          "paths may not leave the repository");
  require(contains(rejection("version: 1\nsite:\n  title: x\n  logo: /x.svg\n"), "site.logo must be a relative path"), "no absolute paths");
  require(contains(rejection("version: 1\nsite:\n  title: x\n  links:\n    - title: A\n      url: ftp://x\n"), "must be http(s) or mailto"),
          "link schemes are limited");
  require(contains(rejection("version: 1\nsite:\n  title: x\n  links:\n    - url: https://x\n"), "needs both 'title' and 'url'"),
          "a link needs a title");
  std::string many_links = "version: 1\nsite:\n  title: x\n  links:\n";
  for (int index = 0; index < 9; ++index) many_links += "    - title: L" + std::to_string(index) + "\n      url: https://x/" + std::to_string(index) + "\n";
  require(contains(rejection(many_links), "site has too many links"), "at most 8 links");
  require(contains(rejection(minimal + "exclude: ['*']\n"), "exclude pattern '*' must be"), "an exclude that matches everything");
  require(contains(rejection(minimal + "exclude: ['../x']\n"), "exclude pattern '../x' must be"), "an exclude outside the tree");
  require(contains(rejection(minimal + "home: docs/x.txt\n"), "home must be a Markdown page"), "home must be Markdown");
  require(contains(rejection(minimal + "nav:\n  - title: A\n    page: a.md\n    pages: [b.md]\n"), "exactly one of 'page' or 'pages'"),
          "a nav entry is a page or a group");
  require(contains(rejection(minimal + "nav:\n  - pages: [a.md]\n"), "a nav group needs a 'title'"), "groups are titled");
  require(contains(rejection(minimal + "nav:\n  - page: a.txt\n"), "is not a Markdown file"), "nav pages are Markdown");
  require(contains(rejection(minimal + "nav:\n  - title: A\n    pages: []\n"), "a nav list is empty"), "groups are not empty");
  require(contains(rejection(minimal + "nav:\n  - 7\n"), "is not a Markdown file"), "a bare nav entry must be a page path");
  require(contains(rejection(minimal + "nav:\n  - title: A\n    pages:\n      - title: B\n        pages:\n          - title: C\n"
                                       "            pages:\n              - title: D\n                pages: [x.md]\n"),
                   "nested more than 4 levels deep"),
          "a group at depth 4 is one too many");
  require(rejection(minimal + "nav:\n  - title: A\n    pages:\n      - title: B\n        pages:\n          - title: C\n"
                              "            pages: [x.md]\n").empty(),
          "groups to depth 3 with pages at depth 4 are fine");
  require(contains(rejection(minimal + "search: maybe\n"), "search must be true or false"), "search is a boolean");
  require(ckgit::docsTitleFromFilename("01-getting-started.md") == "Getting started" &&
              ckgit::docsTitleFromFilename("docs/ci-yml-reference.md") == "Ci yml reference" &&
              ckgit::docsTitleFromFilename("README") == "README" && ckgit::docsTitleFromFilename("2024_notes.md") == "Notes" &&
              ckgit::docsTitleFromFilename("007.md") == "007" && ckgit::docsTitleFromFilename("Über_uns.md") == "Über uns",
          "filename titles strip a numeric prefix and separators and capitalise ASCII");
}

void testDiscovery() {
  Scratch scratch;
  writeFixtureTree(scratch.root);
  std::vector<std::string> warnings;
  const auto walked = ckgit::loadDocsSite(scratch.root, ckgit::DocsConfig{}, &warnings);
  checkDerivedModel(walked, scratch.root);
  require(warnings.size() == 1 && warnings[0] == "docs/hidden.md: unknown front matter key 'layout' ignored",
          "unknown front matter keys are warned about, once each");
  require(ckgit::readDocsConfig(scratch.root).title.empty(), "no ckdocs.yml means defaults");

  // The same tree inside a Git work tree: only tracked files are pages.
  git(scratch.root, {"init", "-q", "--initial-branch=main"});
  git(scratch.root, {"add", "-A"});
  write(scratch.root / "docs/untracked.md", "# Untracked\n");
  warnings.clear();
  const auto tracked = ckgit::loadDocsSite(scratch.root, ckgit::DocsConfig{}, &warnings);
  checkDerivedModel(tracked, scratch.root);
  require(std::none_of(tracked.pages.begin(), tracked.pages.end(), [](const ckgit::DocsPage& page) { return page.source == "docs/untracked.md"; }),
          "an untracked file is never a page inside a work tree");

  // An explicit nav names pages; the rest are unlisted with a warning.
  write(scratch.root / "ckdocs.yml",
        "version: 1\nsite:\n  title: Fixture\nnav:\n  - title: Start\n    page: README.md\n  - title: Guides\n    pages:\n"
        "      - docs/a/02-y.md\n      - title: Extra\n        pages: [docs/a/01-x.md]\n");
  warnings.clear();
  const auto configured = ckgit::loadDocsSite(scratch.root, ckgit::readDocsConfig(scratch.root), &warnings);
  require(configured.title == "Fixture", "the configured title");
  require(configured.nav.size() == 2 && configured.nav[0].title == "Start" && configured.nav[0].page == configured.home &&
              configured.nav[1].title == "Guides" && configured.nav[1].children.size() == 2 &&
              configured.nav[1].children[0].title == "Why" && configured.nav[1].children[1].title == "Extra" &&
              configured.nav[1].children[1].children.size() == 1 && configured.nav[1].children[1].children[0].title == "X page",
          "explicit nav: titles from the config or the page, groups nested");
  require(configured.reading_order == std::vector<std::size_t>{0, indexOf(configured, "docs/a/02-y.md"), indexOf(configured, "docs/a/01-x.md")},
          "reading order follows the explicit nav");
  require(configured.unlisted.size() == 4, "pages the nav does not name are unlisted");
  require(std::count_if(warnings.begin(), warnings.end(), [](const std::string& warning) { return contains(warning, "not named by nav"); }) == 3,
          "unmentioned pages are warned about, except the excluded one");
  ckgit::DocsConfig missing;
  missing.nav.push_back(ckgit::DocsNavEntry{"", "docs/none.md", {}});
  require(contains(loadError(scratch.root, missing), "nav names 'docs/none.md', which is not a page"), "a nav path must be a page");

  // Excludes match root- and source-relative paths; home can be chosen.
  ckgit::DocsConfig excluding;
  excluding.exclude = {"b", "docs/a/*"};
  const auto excluded = ckgit::loadDocsSite(scratch.root, excluding, nullptr);
  require(excluded.pages.size() == 3 && excluded.nav.size() == 2, "excluded directories leave Home and Zed");
  ckgit::DocsConfig homed;
  homed.home = "docs/z.md";
  const auto rehomed = ckgit::loadDocsSite(scratch.root, homed, nullptr);
  require(rehomed.pages[rehomed.home].source == "docs/z.md" && rehomed.pages[rehomed.home].output == "index.html" &&
              std::none_of(rehomed.pages.begin(), rehomed.pages.end(), [](const ckgit::DocsPage& page) { return page.source == "README.md"; }),
          "a configured home replaces the root README, which is then outside the site");
  ckgit::DocsConfig rooted;
  rooted.source = "docs/b";
  const auto sub = ckgit::loadDocsSite(scratch.root, rooted, nullptr);
  require(sub.source == "docs/b" && sub.pages.size() == 2 && sub.pages[sub.home].source == "docs/b/README.md" &&
              sub.pages[sub.home].output == "index.html" && sub.nav.size() == 2 && sub.nav[0].title == "Home" &&
              sub.nav[1].title == "Deep" && pageNamed(sub, "docs/b/deep/inner.md").output == "deep/inner.html",
          "a source tree's own README is its home and the root README stays out");
}

void testEdges() {
  Scratch scratch;
  write(scratch.root / "README.md", "# Home\n");
  write(scratch.root / "docs/README.md", "# Docs\n");
  write(scratch.root / "docs/index.md", "# Index\n");
  const auto collision = loadError(scratch.root, ckgit::DocsConfig{});
  require(contains(collision, "'docs/README.md' and 'docs/index.md' would both be written to 'index.html'") &&
              contains(collision, "exclude or rename"),
          "two index pages in one directory are an error with a hint");
  ckgit::DocsConfig chosen;
  chosen.exclude = {"docs/index.md"};
  const auto resolved = ckgit::loadDocsSite(scratch.root, chosen, nullptr);
  require(resolved.pages.size() == 1 && resolved.pages[0].source == "docs/README.md" && resolved.pages[0].title == "Docs" &&
              resolved.pages[0].home,
          "the docs tree's README is the home ahead of the root README, which then stays out");
  ckgit::DocsConfig root_home;
  root_home.exclude = {"docs/index.md"};
  root_home.home = "README.md";
  require(contains(loadError(scratch.root, root_home), "'README.md' and 'docs/README.md' would both be written to 'index.html'"),
          "home: README.md next to a docs README is a collision, reported with both names");
  ckgit::DocsConfig bad_home;
  bad_home.home = "docs/none.md";
  require(contains(loadError(scratch.root, bad_home), "home page 'docs/none.md' is not a page"), "a missing home is an error");
  ckgit::DocsConfig bad_source;
  bad_source.source = "elsewhere";
  require(contains(loadError(scratch.root, bad_source), "source directory 'elsewhere' does not exist"), "a missing source is an error");

  Scratch empty;
  fs::create_directories(empty.root / "docs");
  write(empty.root / "docs/notes.txt", "x");
  require(contains(loadError(empty.root, ckgit::DocsConfig{}), "no Markdown pages found under 'docs'"), "a tree without pages");

  Scratch flat;
  write(flat.root / "guide.md", "---\ntitle: Guide\n---\n");
  write(flat.root / "b/c/d/e/f.md", "# F\n");
  write(flat.root / "b/c/d/e/h.md", "# H\n");
  write(flat.root / "b/c/d/g.md", "# G\n");
  const auto site = ckgit::loadDocsSite(flat.root, ckgit::DocsConfig{}, nullptr);
  require(site.source.empty() && site.pages[site.home].source == "b/c/d/e/f.md",
          "without docs/ the root is the source, and without a README the first page is the home");
  require(site.nav.size() == 3 && site.nav[0].title == "Home" && site.nav[1].title == "B" && site.nav[2].title == "Guide",
          "tabs: Home, the b directory, and the top-level page");
  require(site.nav[1].children.size() == 1 && site.nav[1].children[0].title == "C" && site.nav[1].children[0].children.size() == 1 &&
              site.nav[1].children[0].children[0].title == "D",
          "groups nest to depth 3");
  const auto& d = site.nav[1].children[0].children[0];
  require(d.children.size() == 2 && d.children[0].title == "H" && d.children[0].page == indexOf(site, "b/c/d/e/h.md") &&
              d.children[0].children.empty() && d.children[1].title == "G",
          "a directory that would be a depth-4 group is listed flat inside its parent, in its place");
}

// The repository this test suite belongs to, when the build points at it.
void testThisRepository() {
  const char* configured = std::getenv("CKGIT_SOURCE_ROOT");
  if (configured == nullptr || *configured == '\0') return;
  const fs::path root(configured);
  if (!fs::is_directory(root / "docs/operations") || !fs::is_regular_file(root / "README.md")) return;

  // Zero-config derivation on this repository's real content (WP4's own
  // acceptance case), independent of whatever ckdocs.yml the repository
  // happens to carry -- this is the fallback every project without one gets.
  {
    std::vector<std::string> warnings;
    const auto model = ckgit::loadDocsSite(root, ckgit::DocsConfig{}, &warnings);
    require(model.source == "docs" && model.pages[model.home].source == "README.md", "this repository's docs tree and README");
    require(std::none_of(model.pages.begin(), model.pages.end(), [](const ckgit::DocsPage& page) { return contains(page.source, "planning"); }),
            "gitignored planning documents are never pages");
    require(model.nav.size() == 3 && model.nav[0].title == "Home" && model.nav[1].title == "Operations" && model.nav[2].title == "Protocol",
            "derived tabs Home, Operations, Protocol");
    const auto& operations = model.nav[1].children;
    require(operations.size() == 6, "six operations pages, derived flat");
    for (std::size_t index = 0; index < operations.size(); ++index) {
      require(operations[index].page.has_value() && model.pages[*operations[index].page].source.starts_with("docs/operations/0" + std::to_string(index + 1) + "-"),
              "operations pages follow their numeric prefixes");
    }
  }

  // The repository's own committed ckdocs.yml (WP7): an explicit nav names
  // the tabs and groups the continuous-delivery pages, so this exercises the
  // config actually published on ck-git Pages and GitHub Pages.
  {
    const auto config = ckgit::readDocsConfig(root);
    require(!config.title.empty(), "this repository ships its own ckdocs.yml");
    std::vector<std::string> warnings;
    const auto model = ckgit::loadDocsSite(root, config, &warnings);
    require(model.title == "ck-git-hosting" && model.pages[model.home].source == "README.md" &&
                model.nav.size() == 3 && model.nav[0].title == "Home" && model.nav[1].title == "Operations" &&
                model.nav[2].title == "Protocol" && model.nav[2].page.has_value() &&
                model.pages[*model.nav[2].page].source == "docs/protocol/01-ssh-and-control-v1.md",
            "the configured site: title, home, tabs, and Protocol as a one-page tab");
    const auto& operations = model.nav[1].children;
    require(operations.size() == 4 && operations[0].page.has_value() &&
                model.pages[*operations[0].page].source == "docs/operations/01-installation.md" &&
                operations[3].title == "Continuous delivery" && operations[3].children.size() == 3 &&
                model.pages[*operations[3].children[0].page].source == "docs/operations/04-ci-cd.md" &&
                model.pages[*operations[3].children[2].page].source == "docs/operations/06-ci-yml-reference.md",
            "Operations: three flat pages, then a Continuous delivery group with 04-06");
    require(config.links.size() == 1 && config.links[0].title == "GitHub" && !config.footer.empty(),
            "the configured header link and footer");
  }
}

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  require(bool(in), "read " + path.string());
  return buffer.str();
}

std::size_t occurrences(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  for (auto at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) ++count;
  return count;
}

std::string between(const std::string& html, const std::string& open, const std::string& close) {
  const auto start = html.find(open);
  require(start != std::string::npos, "markup present: " + open);
  const auto end = html.find(close, start);
  require(end != std::string::npos, "markup closed: " + close);
  return html.substr(start, end - start);
}

// Every internal href/src of a written page resolves to a file of the site.
void checkInternalReferences(const fs::path& site, const std::string& page, const std::string& html) {
  const auto directory = fs::path(page).parent_path();
  for (const char* attribute : {"href=\"", "src=\""}) {
    for (auto at = html.find(attribute); at != std::string::npos; at = html.find(attribute, at + 1)) {
      const auto start = at + std::strlen(attribute);
      auto value = html.substr(start, html.find('"', start) - start);
      if (value.starts_with("http") || value.starts_with("mailto:") || value.starts_with("#")) continue;
      value = value.substr(0, value.find('#'));
      const auto decoded = ckgit::decodePathSegment(value);
      require(decoded.has_value(), "internal reference is canonically encoded: " + value);
      require(fs::exists((site / directory / *decoded).lexically_normal()), "internal reference from " + page + " resolves: " + value);
    }
  }
}

bool hasTemporaryLeftovers(const fs::path& parent, std::string_view name) {
  for (const auto& entry : fs::directory_iterator(parent)) {
    if (entry.path().filename().string().starts_with(std::string(name) + ".tmp-")) return true;
  }
  return false;
}

void testBuild() {
  Scratch scratch;
  writeFixtureTree(scratch.root);
  write(scratch.root / "docs/a/01-x.md",
        "# X page\n\n[to y](02-y.md#y) [home](../../README.md) [missing](nope.md) [bad anchor](02-y.md#nope) "
        "[dir](../b/) [abs](/docs/z.md) [pdf](../files/spec.pdf) [self](#install) [outside](../../CONTRIBUTING.md) "
        "[sp](my%20page.md) [ext](https://example.test/x#frag)\n\n![flow](../img/flow.png)\n\n"
        "## Install\n\nText.\n\n### Pair\n\nMore.\n\n## Use\n");
  write(scratch.root / "docs/a/02-y.md", "---\ntitle: Why\nnav_order: 1\n---\n# Y\n\n> [!NOTE]\n> Noted.\n\n- [ ] task\n\n```cpp\nint x;\n```\n");
  write(scratch.root / "docs/a/my page.md", "# My page\n");
  write(scratch.root / "docs/img/flow.png", "PNGDATA");
  write(scratch.root / "docs/img/unref.png", "UNREF");
  write(scratch.root / "docs/files/spec.pdf", "%PDF");
  write(scratch.root / "CONTRIBUTING.md", "# Contributing\n");
  write(scratch.root / "docs/site.css", "body{}");
  write(scratch.root / "ckdocs.yml",
        "version: 1\nsite:\n  title: Fixture\n  logo: docs/img/flow.png\n  stylesheet: docs/site.css\n  footer: \"© Fixture\"\n"
        "  links:\n    - title: Source\n      url: https://example.test/src\n");
  const auto model = ckgit::loadDocsSite(scratch.root, ckgit::readDocsConfig(scratch.root), nullptr);
  const auto site = scratch.root / "site";
  ckgit::DocsBuildReport report;
  ckgit::buildDocsSite(model, site, ckgit::DocsBuildOptions{}, &report);
  require(report.pages_written == 8 && report.assets_copied == 3, "eight pages; the logo, the stylesheet and two referenced files");
  for (const auto* file : {"index.html", "a/01-x.html", "a/02-y.html", "a/my page.html", "b/index.html", "b/deep/inner.html",
                           "z.html", "hidden.html", "site-index.html", ".ckdocs", "img/flow.png", "files/spec.pdf", "site.css"}) {
    require(fs::is_regular_file(site / file), std::string("written: ") + file);
  }
  require(!fs::exists(site / "img/unref.png"), "an unreferenced file is not copied");
  require(!hasTemporaryLeftovers(scratch.root, "site"), "no temporary directory is left behind");

  const auto x = slurp(site / "a/01-x.html");
  checkInternalReferences(site, "a/01-x.html", x);
  require(contains(x, "<title>X page · Fixture</title>") && contains(x, "<link rel=\"stylesheet\" href=\"../site.css\">"),
          "title and the extra stylesheet, relative to the page");
  require(contains(x, "href=\"02-y.html#y\"") && contains(x, "href=\"../index.html\"") && contains(x, "href=\"../b/index.html\"") &&
              contains(x, "href=\"../z.html\"") && contains(x, "href=\"../files/spec.pdf\"") && contains(x, "href=\"#install\"") &&
              contains(x, "href=\"my%20page.html\"") && contains(x, "src=\"../img/flow.png\" alt=\"flow\""),
          "page, directory, absolute, file, fragment and encoded links resolve relative to the page");
  require(contains(x, "[missing](nope.md)") && contains(x, "[outside](../../CONTRIBUTING.md)") && !contains(x, "nope.html"),
          "links to nothing stay text");
  require(report.broken_links.size() == 2 && contains(report.broken_links[0], "docs/a/01-x.md: link target 'nope.md' is not a page") &&
              contains(report.broken_links[1], "'../../CONTRIBUTING.md' is not a page"),
          "broken links are reported with their page");
  require(report.broken_anchors.size() == 1 && contains(report.broken_anchors[0], "docs/a/01-x.md: '02-y.html#nope' names no heading on docs/a/02-y.md"),
          "a fragment that names no heading is reported; #y and #install are fine");
  const auto tabs = between(x, "<nav class=\"tabs\"", "</nav>");
  require(contains(tabs, "<a href=\"../index.html\">Home</a>") && contains(tabs, "<a href=\"02-y.html\" aria-current=\"page\">A</a>") &&
              contains(tabs, "<a href=\"../b/index.html\">Bee</a>") && contains(tabs, "<a href=\"../z.html\">Zed page</a>") &&
              occurrences(tabs, "aria-current") == 1,
          "tabs link to their first page and mark the active one");
  const auto aside = between(x, "<aside class=\"sidebar\">", "</aside>");
  require(contains(aside, "<p class=\"side-title\">A</p>") && contains(aside, "<a href=\"02-y.html\">Why</a>") &&
              contains(aside, "<a href=\"01-x.html\" aria-current=\"page\">X page</a>") && occurrences(aside, "aria-current") == 1,
          "the sidebar lists the active tab's pages and marks the current one");
  require(contains(aside, "<li><a href=\"#install\">Install</a><ul><li><a href=\"#pair\">Pair</a></li></ul></li><li><a href=\"#use\">Use</a></li>"),
          "the outline nests h3 under h2");
  require(contains(x, "<nav class=\"crumbs\" aria-label=\"Breadcrumb\"><ol><li><a href=\"02-y.html\">A</a></li><li aria-current=\"page\">X page</li></ol></nav>"),
          "breadcrumbs: tab, page");
  require(contains(x, "rel=\"prev\" href=\"02-y.html\"") && contains(x, "rel=\"next\" href=\"my%20page.html\""), "previous and next follow reading order");
  require(contains(x, "<img src=\"../img/flow.png\" alt=\"\">") && contains(x, "<a href=\"https://example.test/src\" rel=\"noopener\">Source</a>") &&
              contains(x, "© Fixture") && contains(x, "Built with ckdocs") && contains(x, "<a href=\"../site-index.html\">Site index</a>"),
          "logo, header links, footer, site index link");
  require(contains(x, "<input class=\"nav-switch\" id=\"nav-toggle\" type=\"checkbox\">") && !contains(x, "<script"),
          "the sidebar toggle is a checkbox, and there is no script");

  const auto home = slurp(site / "index.html");
  checkInternalReferences(site, "index.html", home);
  require(contains(home, "<title>Fixture home · Fixture</title>") && !contains(home, "class=\"crumbs\"") &&
              !contains(home, "rel=\"prev\"") && contains(home, "rel=\"next\" href=\"a/02-y.html\"") &&
              contains(home, "<a href=\"site-index.html\">Site index</a>") && contains(home, "<a href=\"a/02-y.html\">A</a>"),
          "the home page: no breadcrumbs, no previous, links from the root");
  const auto y = slurp(site / "a/02-y.html");
  require(contains(y, "class=\"alert alert-note\"") && contains(y, "task-list-item") && contains(y, "<span class=\"hl-k\">int</span>"),
          "alerts, task lists and highlighting reach the site");
  const auto last = slurp(site / "z.html");
  require(!contains(last, "rel=\"next\"") && contains(last, "rel=\"prev\""), "the last page has no next");
  const auto index = slurp(site / "site-index.html");
  checkInternalReferences(site, "site-index.html", index);
  require(contains(index, "<title>Site index · Fixture</title>") && contains(index, "<h2>A</h2>") && contains(index, "<h2>Bee</h2>") &&
              contains(index, "<h2>Other pages</h2>") && contains(index, "href=\"hidden.html\">Hidden</a>") &&
              contains(index, "href=\"a/01-x.html#install\">Install</a>") && contains(index, "href=\"b/deep/inner.html\">Inner</a>"),
          "the site index lists every tab, page, heading, and unlisted page");
  const auto marker = slurp(site / ".ckdocs");
  require(contains(marker, "version=1\n") && contains(marker, "pages=8\n"), "the marker records the site");

  // Rebuilding: refused without --clean, replaced with it, never touching foreign directories.
  bool refused = false;
  try {
    ckgit::buildDocsSite(model, site, ckgit::DocsBuildOptions{}, nullptr);
  } catch (const std::runtime_error& error) {
    refused = contains(error.what(), "use --clean");
  }
  require(refused && fs::is_regular_file(site / "index.html") && !hasTemporaryLeftovers(scratch.root, "site"),
          "a second build without --clean is refused and leaves the site");
  write(site / "stale.html", "old");
  ckgit::DocsBuildOptions clean;
  clean.clean = true;
  ckgit::buildDocsSite(model, site, clean, &report);
  require(!fs::exists(site / "stale.html") && fs::is_regular_file(site / ".ckdocs") && report.pages_written == 8 &&
              !hasTemporaryLeftovers(scratch.root, "site"),
          "--clean replaces the previous site whole");
  fs::create_directories(scratch.root / "other");
  write(scratch.root / "other/x.txt", "keep");
  bool foreign = false;
  try {
    ckgit::buildDocsSite(model, scratch.root / "other", clean, nullptr);
  } catch (const std::runtime_error& error) {
    foreign = contains(error.what(), "not written by ckdocs");
  }
  require(foreign && fs::is_regular_file(scratch.root / "other/x.txt") && !hasTemporaryLeftovers(scratch.root, "other"),
          "a directory without the marker is refused untouched, even with --clean");
  bool not_directory = false;
  try {
    ckgit::buildDocsSite(model, scratch.root / "other/x.txt", clean, nullptr);
  } catch (const std::runtime_error& error) {
    not_directory = contains(error.what(), "is not a directory");
  }
  require(not_directory, "a file in the way is an error");
  fs::create_directories(scratch.root / "empty");
  ckgit::buildDocsSite(model, scratch.root / "empty", ckgit::DocsBuildOptions{}, nullptr);
  require(fs::is_regular_file(scratch.root / "empty/index.html"), "an empty directory is filled without --clean");
}

}  // namespace

void testDocsSite() {
  testConfig();
  testDiscovery();
  testEdges();
  testBuild();
  testThisRepository();
}
