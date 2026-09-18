// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ckgit {

// The model behind a `ckdocs` documentation site: which Markdown files of a
// repository are pages, what each is called, how they are ordered and
// grouped into tabs, and where each lands in the output. Rendering (WP5)
// builds on this without touching the disk here; loadDocsSite only reads.

inline constexpr std::size_t kMaximumDocsPages = 4096;
inline constexpr std::size_t kMaximumDocsNavDepth = 4;     // tab = 1, groups below it
inline constexpr std::size_t kMaximumDocsNavEntries = 4096;
inline constexpr std::size_t kMaximumDocsLinks = 8;
inline constexpr std::size_t kMaximumDocsExcludes = 64;
inline constexpr std::size_t kMaximumDocsConfigBytes = 64 * 1024;
inline constexpr std::size_t kMaximumDocsTitleBytes = 128;
inline constexpr std::size_t kMaximumDocsTextBytes = 1024;  // description, footer
inline constexpr std::size_t kMaximumDocsUrlBytes = 1024;

struct DocsLink {
  std::string title;
  std::string url;  // http(s) or mailto
};

// One `nav:` entry as configured: a page (`page` set; `title` optional and
// otherwise the page's own) or a group (`title` and `pages`). The file form:
//
//   nav:
//     - page: README.md                       # a tab showing one page
//     - title: Operations                     # a tab with pages and groups
//       pages:
//         - docs/operations/01-installation.md
//         - title: Continuous delivery
//           pages: [docs/operations/04-ci-cd.md]
//
// (`- Title: path` is not used because the YAML subset keeps keys plain.)
struct DocsNavEntry {
  std::string title;
  std::string page;
  std::vector<DocsNavEntry> pages;
};

// `ckdocs.yml` at the repository root. Every path is repository-relative
// and must be a safe relative path (no leading `/`, no `.`/`..` component,
// no backslash). Unknown keys are rejected: this file is ours.
struct DocsConfig {
  std::string title;  // site.title; empty when there is no config file
  std::string brand;  // header wordmark; empty = title
  std::string logo;   // an image file shown before the brand
  std::string description;
  std::string footer;
  std::vector<DocsLink> links;  // header links
  std::string stylesheet;       // an extra stylesheet, copied as-is
  std::string source;           // the page tree; empty = `docs/` when present, else the root
  std::string home;             // the home page; empty = the source's README.md/index.md, else README.md at the root
  std::vector<std::string> exclude;  // path prefixes or trailing-`*` patterns, root- or source-relative
  std::vector<DocsNavEntry> nav;     // empty = derived from the tree
  bool search = false;
};

// Parses the file's content. Throws std::length_error when a bound is
// exceeded and std::runtime_error ("ckdocs.yml: ... (line N)") otherwise.
DocsConfig parseDocsConfig(std::string_view yaml);
// The root's `ckdocs.yml`, or defaults when there is none.
DocsConfig readDocsConfig(const std::filesystem::path& root);

struct DocsPage {
  std::string source;  // repository-relative, `/` separators
  std::string output;  // site-relative output path, e.g. "operations/01-installation.html"
  std::string title;
  std::string description;
  std::optional<int> nav_order;
  bool nav_exclude = false;
  bool home = false;
  std::string front_matter_error;  // when the page's front matter did not parse
};

struct DocsNavItem {
  std::string title;
  std::optional<std::size_t> page;  // index into DocsSiteModel::pages; a group may have one (its README)
  std::vector<DocsNavItem> children;
};

struct DocsSiteModel {
  std::filesystem::path root;
  std::string source;  // the effective page tree, root-relative; empty is the root itself
  std::string title;   // the site title: config, else the root directory's name
  DocsConfig config;
  std::vector<DocsPage> pages;  // sorted by source path
  std::size_t home = 0;         // index of the home page
  std::vector<DocsNavItem> nav;  // the tabs, the Home tab first
  std::vector<std::size_t> reading_order;  // page indices depth-first through nav, for previous/next
  std::vector<std::size_t> unlisted;       // pages outside nav: nav_exclude, or not named by an explicit nav
};

// Enumerates pages (`git ls-files` inside a Git work tree, so nothing
// untracked is ever published; a directory walk that skips symlinks and
// dot-names otherwise), reads each page's front matter and first heading,
// derives output paths and the navigation, and checks the bounds. Throws
// std::runtime_error ("ckdocs: ...") on anything that must stop a build;
// appends advisory findings (unknown front-matter keys, pages an explicit
// nav does not mention, front matter that did not parse) to `warnings`.
DocsSiteModel loadDocsSite(const std::filesystem::path& root, const DocsConfig& config,
                           std::vector<std::string>* warnings);

// "01-getting-started.md" -> "Getting started".
std::string docsTitleFromFilename(std::string_view name);

// ---- building ----------------------------------------------------------------

// The file every site carries at its root, so a later build can tell an
// output directory it wrote from one it must not touch.
inline constexpr std::string_view kDocsSiteMarker = ".ckdocs";
inline constexpr std::string_view kDocsSiteIndexPage = "site-index.html";

struct DocsBuildOptions {
  bool clean = false;  // replace an existing output directory that carries the marker
};

struct DocsBuildReport {
  std::size_t pages_written = 0;
  std::size_t assets_copied = 0;
  std::size_t bytes_written = 0;
  std::vector<std::string> broken_links;    // "<page>: link target '<target>' <reason>"
  std::vector<std::string> broken_anchors;  // "<page>: '<href>' names no heading on <target page>"
};

// Renders every page of the model into `out`: the site's HTML (each page
// with the embedded stylesheet, header tabs, the active tab's sidebar,
// "On this page", breadcrumbs, previous/next, footer), the site index page,
// the marker, and every asset a page references (regular files only,
// mirrored at their source-relative path — or root-relative outside the
// source). Relative links between pages become relative links between their
// output files, so the site works from `file://` as well as under any URL
// prefix. Everything is written to a fresh sibling temporary directory and
// swapped into place at the end: `out` may be absent, empty, or a site with
// the marker when `clean` is set; anything else is refused and left alone,
// and no failure ever leaves a partial `out`. Throws std::runtime_error
// ("ckdocs: …") on failure; broken links and anchors are reported, not fatal.
void buildDocsSite(const DocsSiteModel& model, const std::filesystem::path& out, const DocsBuildOptions& options,
                   DocsBuildReport* report);

}  // namespace ckgit
