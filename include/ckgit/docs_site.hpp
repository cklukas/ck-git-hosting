// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
inline constexpr std::size_t kMaximumDocsForeignFrontMatterKeys = 32;
inline constexpr std::size_t kMaximumDocsConfigBytes = 64 * 1024;
inline constexpr std::size_t kMaximumDocsTitleBytes = 128;
inline constexpr std::size_t kMaximumDocsTextBytes = 1024;  // description, footer, a foreign front matter value
inline constexpr std::size_t kMaximumDocsUrlBytes = 1024;
inline constexpr std::size_t kMaximumDocsSites = 8;
inline constexpr std::size_t kMaximumDocsSiteIdBytes = 32;

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

// One `sites:` entry of a portal config: a documentation site built into
// `<out>/<id>/` from its own, ordinary single-site config. The file form:
//
//   sites:
//     - id: cpp                      # [a-z0-9][a-z0-9-]*, at most 32 bytes
//       title: C++ edition
//       description: The native port # optional, shown on the welcome page
//       config: docs/ckdocs.yml      # optional, default ckdocs.yml, site-root-relative
//       ref: main                    # optional: the site's root is this Git ref's tree
//       redirect_from_root: true     # optional, at most one site
struct DocsSiteEntry {
  std::string id;
  std::string title;
  std::string description;
  std::string config;  // relative to the site's root; empty = ckdocs.yml
  std::string ref;     // a branch, tag, or commit; empty = the work tree root itself
  bool redirect_from_root = false;
};

// `ckdocs.yml` at the repository root. Every path is repository-relative
// and must be a safe relative path (no leading `/`, no `.`/`..` component,
// no backslash). Unknown keys are rejected: this file is ours.
struct DocsConfig {
  std::string title;  // site.title; empty when there is no config file
  // site.url: where the site is published, an absolute http(s) URL ending in
  // `/` (one is added). With it, a build also writes sitemap.xml and uses
  // absolute links in llms.txt; without it, llms.txt links stay relative.
  std::string url;
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
  // front_matter.foreign_keys: page front matter keys that belong to other
  // tools. ckdocs never interprets them; a page carrying one is not warned
  // about, and its value is passed through as a `front-matter:<key>` meta tag.
  std::vector<std::string> foreign_front_matter_keys;
  // `sites:` makes this a portal config: one build holding several sites
  // and a generated welcome page. Then only `site` and `home` (the welcome
  // page's Markdown intro) may sit beside it; source, nav, search, exclude
  // and front_matter belong to each site's own config.
  std::vector<DocsSiteEntry> sites;
};

// The meta-tag name a foreign front matter key is carried under. Namespaced
// so a key meant for another tool can never act as a standard meta name
// (`robots`, `referrer`, `viewport`, ...) or shadow one ckdocs writes itself.
inline constexpr std::string_view kDocsForeignMetaPrefix = "front-matter:";

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
  std::string author;  // front matter `author`, shown in the byline
  std::string date;    // front matter `date`, a validated YYYY-MM-DD calendar date
  std::optional<int> nav_order;
  bool nav_exclude = false;
  bool home = false;
  // The config's foreign keys this page carries with a single-line value of
  // at most kMaximumDocsTextBytes, in file order, values exactly as parsed.
  std::vector<std::pair<std::string, std::string>> foreign_front_matter;
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
  // Markdown files left out because they cannot be pages (over the 512 KiB
  // page limit, unreadable, or not UTF-8), each with the reason; links to
  // them are reported with it.
  std::map<std::string, std::string> skipped;
};

// Enumerates pages (`git ls-files` inside a Git work tree, so nothing
// untracked is ever published; a directory walk that skips symlinks and
// dot-names otherwise), reads each page's front matter and first heading,
// derives output paths and the navigation, and checks the bounds. Throws
// std::runtime_error ("ckdocs: ...") on anything that must stop a build;
// appends advisory findings (front-matter keys neither ckdocs nor the
// config's foreign keys name, front-matter values ckdocs cannot use, pages an
// explicit nav does not mention, front matter that did not parse, and pages
// left out because they exceed the page limit, cannot be read, or are not
// UTF-8 -- unless that page is the home page, which stops the build) to
// `warnings`. A nav entry naming a left-out page is dropped with it.
DocsSiteModel loadDocsSite(const std::filesystem::path& root, const DocsConfig& config,
                           std::vector<std::string>* warnings);

// "01-getting-started.md" -> "Getting started".
std::string docsTitleFromFilename(std::string_view name);

// ---- building ----------------------------------------------------------------

// The file every site carries at its root, so a later build can tell an
// output directory it wrote from one it must not touch.
inline constexpr std::string_view kDocsSiteMarker = ".ckdocs";
inline constexpr std::string_view kDocsSiteIndexPage = "site-index.html";
// Generated at the output root for crawlers and AI tools: a sitemap of every
// page (only with site.url, since a sitemap needs absolute URLs) and an
// llms.txt index (llmstxt.org): the title, the description, and each tab's
// pages as Markdown links with their descriptions, pages outside the nav
// under "Optional". In a portal one of each covers every site.
inline constexpr std::string_view kDocsSitemapFile = "sitemap.xml";
inline constexpr std::string_view kDocsLlmsFile = "llms.txt";

// Opt-in search (`search: true` in ckdocs.yml): a generated index, read by a
// small inline script the theme embeds. `kMaximumDocsSearchIndexBytes` bounds
// the whole file; a site whose full index would exceed it keeps entries for
// as many pages as fit (dropped from the end) rather than failing the build,
// reported as a build warning. `kMaximumDocsSearchExcerptChars` bounds each
// section's plain-text excerpt (bytes, cut at a UTF-8 boundary, not a strict
// codepoint count).
inline constexpr std::string_view kDocsSearchIndexPage = "search-index.json";
inline constexpr std::size_t kMaximumDocsSearchIndexBytes = 2 * 1024 * 1024;
inline constexpr std::size_t kMaximumDocsSearchExcerptChars = 200;

struct DocsDiagram {
  std::string light_svg;
  std::string dark_svg;
  std::string alternative_text;
  std::vector<std::string> warnings;
};

// Internal application callback: SVG bytes must come from a trusted renderer.
// Keeping the callback here lets the other binaries omit the diagram engine.
using DocsDiagramRenderer = std::function<DocsDiagram(std::string_view source)>;

struct DocsBuildOptions {
  bool clean = false;  // replace an existing output directory that carries the marker
  DocsDiagramRenderer mermaid_renderer{};
};

struct DocsBuildReport {
  std::size_t pages_written = 0;
  std::size_t assets_copied = 0;
  std::size_t diagrams_rendered = 0;
  std::size_t assets_generated = 0;
  std::size_t bytes_written = 0;
  std::size_t redirects_written = 0;        // a portal's redirect_from_root pages
  // "<page>:<line>: link target '<target>' <reason>", one entry for every
  // link of a page to the same target with the same reason; repeats add
  // " (<n> links; also line(s) ...)".
  std::vector<std::string> broken_links;
  // "<page>: '<href>' names no heading on <target page>", repeats once with " (<n> links)"
  std::vector<std::string> broken_anchors;
  std::vector<std::string> warnings;        // other build-time findings, e.g. a truncated search index
};

// Renders every page of the model into `out`: the site's HTML (each page
// with the embedded stylesheet, its front matter's description, author, date
// and foreign keys as meta tags, an author/date byline under its leading
// heading, header tabs, the active tab's sidebar,
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

// ---- several sites in one build (a portal) ----------------------------------

struct DocsPortalSite {
  DocsSiteEntry entry;
  DocsSiteModel model;  // rooted at the work tree, an extracted ref tree, or a --ref-tree directory
};

struct DocsPortalModel {
  std::filesystem::path root;  // the work tree root: the portal's logo, stylesheet and intro live here
  DocsConfig config;           // the portal config itself
  std::vector<DocsPortalSite> sites;
  // The private directory ref trees were extracted into, if any; removed when
  // the last copy of the model goes, so it outlives the build that reads it.
  std::shared_ptr<const std::filesystem::path> extracted;
};

struct DocsPortalOptions {
  // `--ref-tree REF=DIR`: a directory that already holds REF's tree, used
  // instead of extracting the ref from the work tree's Git repository.
  std::map<std::string, std::filesystem::path> ref_trees;
};

// Loads every site of a portal config (`config.sites` non-empty). A site
// with a `ref` is rooted at options.ref_trees[ref] when given; else, inside
// a Git work tree, at the committed tree of `<ref>` or `origin/<ref>`,
// extracted with `git archive` into a private temporary directory; else the
// load fails naming the ref. Each site's config is read relative to its own
// root and must not list `sites` itself; one without a title takes its
// entry's. Problems are those of loadDocsSite, prefixed "site '<id>': ";
// warnings are prefixed "<id>: ".
DocsPortalModel loadDocsPortal(const std::filesystem::path& root, const DocsConfig& config,
                               const DocsPortalOptions& options, std::vector<std::string>* warnings);

// Builds a portal into `out` with the same refusal, temporary-tree and atomic
// swap rules as buildDocsSite, and one marker: the welcome page at
// `index.html` (the portal title, description, the optional `home` intro,
// then a card per site), every site under `<id>/` exactly as buildDocsSite
// would lay it out there (with a site selector after the brand in its
// header), and, for the site with redirect_from_root, a small redirect page
// at each of its page paths but its home. The report covers all of it;
// each site's broken links, anchors and warnings are prefixed "<id>: ".
void buildDocsPortal(const DocsPortalModel& portal, const std::filesystem::path& out, const DocsBuildOptions& options,
                     DocsBuildReport* report);

}  // namespace ckgit
