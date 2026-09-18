// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/docs_site.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <cstdlib>
#include <functional>
#include <set>

#include <unistd.h>

#include "ckgit/ci_workflow.hpp"
#include "ckgit/cli_help.hpp"
#include "ckgit/docs_theme.hpp"
#include "ckgit/http_router.hpp"
#include "ckgit/markdown.hpp"
#include "ckgit/pages_store.hpp"
#include "ckgit/process.hpp"
#include "ckgit/text.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/yaml_subset.hpp"

namespace ckgit {
namespace {

namespace fs = std::filesystem;
using Node = YamlNode;

// The config shares the workflow's syntax bounds; only the names differ.
constexpr YamlDialect kDocsConfigDialect{kCiWorkflowYamlBounds, "ckdocs.yml", "the config"};
static_assert(kCiWorkflowYamlBounds.document_bytes == kMaximumDocsConfigBytes);

[[noreturn]] void malformed(const std::string& message, std::size_t line) {
  yamlMalformed(kDocsConfigDialect, message, line);
}

[[noreturn]] void tooLarge(const std::string& message) { yamlTooLarge(kDocsConfigDialect, message); }

[[noreturn]] void siteError(const std::string& message) { throw std::runtime_error("ckdocs: " + message); }

const Node& requireKind(const Node& node, Node::Kind kind, const char* what) {
  return yamlRequireKind(kDocsConfigDialect, node, kind, what);
}

const Node* findEntry(const Node& mapping, std::string_view key) { return yamlFindEntry(mapping, key); }

void rejectUnknownKeys(const Node& mapping, std::initializer_list<std::string_view> allowed) {
  yamlRejectUnknownKeys(kDocsConfigDialect, mapping, allowed);
}

bool printable(std::string_view value) {
  return std::none_of(value.begin(), value.end(),
                      [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; });
}

bool isValidDocsTitle(std::string_view value) {
  return !value.empty() && value.size() <= kMaximumDocsTitleBytes && isValidUtf8(value) && printable(value);
}

bool isValidDocsText(std::string_view value) {
  return value.size() <= kMaximumDocsTextBytes && isValidUtf8(value) && printable(value);
}

std::string lowerAscii(std::string_view value) {
  std::string result(value);
  for (char& byte : result) byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
  return result;
}

bool isValidLinkUrl(std::string_view url) {
  if (url.empty() || url.size() > kMaximumDocsUrlBytes || !printable(url)) return false;
  if (url.find_first_of(" \"<>") != std::string_view::npos) return false;
  const auto lowered = lowerAscii(url.substr(0, 8));
  return lowered.starts_with("http://") || lowered.starts_with("https://") || lowered.starts_with("mailto:");
}

// A path prefix, or a prefix with one trailing `*`.
bool isValidExcludePattern(std::string_view pattern) {
  if (pattern.empty() || pattern == "*") return false;
  if (pattern.back() == '*') pattern.remove_suffix(1);
  return !pattern.empty() && pattern != "/" && isSafeRelativePath(pattern);
}

bool hasMarkdownExtension(std::string_view path) {
  const auto lowered = lowerAscii(path);
  return lowered.ends_with(".md") || lowered.ends_with(".markdown");
}

std::string scalarOf(const Node& node, const char* what) {
  return requireKind(node, Node::Kind::Scalar, what).scalar;
}

std::string pathOf(const Node& node, const char* what) {
  const auto value = scalarOf(node, what);
  if (!isSafeRelativePath(value)) {
    malformed(std::string(what) + " must be a relative path inside the repository, not '" + value + "'", node.line);
  }
  return value;
}

std::string titleOf(const Node& node, const char* what) {
  const auto value = scalarOf(node, what);
  if (!isValidDocsTitle(value)) malformed(std::string(what) + " is empty, longer than 128 bytes, or has control characters", node.line);
  return value;
}

// `depth` is the depth of the entries in this list: tabs are 1. A group at
// depth d holds entries at d + 1, so groups stop at depth 3 and pages at 4.
void parseNavEntries(const Node& sequence, std::vector<DocsNavEntry>& out, std::size_t depth, std::size_t& count) {
  requireKind(sequence, Node::Kind::Sequence, "nav entries to be a list");
  if (sequence.items.empty()) malformed("a nav list is empty", sequence.line);
  for (const Node& item : sequence.items) {
    if (++count > kMaximumDocsNavEntries) tooLarge("nav has too many entries");
    DocsNavEntry entry;
    if (item.kind == Node::Kind::Scalar) {
      entry.page = pathOf(item, "a nav page");
    } else if (item.kind == Node::Kind::Mapping) {
      rejectUnknownKeys(item, {"title", "page", "pages"});
      if (const Node* title = findEntry(item, "title")) entry.title = titleOf(*title, "a nav title");
      const Node* page = findEntry(item, "page");
      const Node* pages = findEntry(item, "pages");
      if ((page != nullptr) == (pages != nullptr)) malformed("a nav entry needs exactly one of 'page' or 'pages'", item.line);
      if (page != nullptr) {
        entry.page = pathOf(*page, "a nav page");
      } else {
        if (entry.title.empty()) malformed("a nav group needs a 'title'", item.line);
        if (depth >= kMaximumDocsNavDepth) malformed("nav is nested more than 4 levels deep", item.line);
        parseNavEntries(*pages, entry.pages, depth + 1, count);
      }
    } else {
      malformed("each nav entry to be a page path, or a mapping with 'title' and 'page' or 'pages'", item.line);
    }
    if (!entry.page.empty() && !hasMarkdownExtension(entry.page)) {
      malformed("nav page '" + entry.page + "' is not a Markdown file", item.line);
    }
    out.push_back(std::move(entry));
  }
}

std::optional<std::string> readFile(const fs::path& path, std::size_t limit) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size > limit) return std::nullopt;
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string content(static_cast<std::size_t>(size), '\0');
  in.read(content.data(), static_cast<std::streamsize>(content.size()));
  if (in.gcount() != static_cast<std::streamsize>(content.size())) return std::nullopt;
  return content;
}

// ---- page enumeration ------------------------------------------------------

std::string_view directoryOf(std::string_view path) {
  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
}

std::string_view basenameOf(std::string_view path) {
  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view stemOf(std::string_view base) {
  const auto dot = base.rfind('.');
  return dot == std::string_view::npos || dot == 0 ? base : base.substr(0, dot);
}

bool isIndexName(std::string_view base) {
  const auto stem = lowerAscii(stemOf(base));
  return stem == "readme" || stem == "index";
}

bool hasDotComponent(std::string_view path) {
  for (std::size_t start = 0; start <= path.size();) {
    const auto slash = path.find('/', start);
    const auto component = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (!component.empty() && component.front() == '.') return true;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return false;
}

bool patternMatches(std::string_view pattern, std::string_view path) {
  if (pattern.empty()) return false;
  if (pattern.back() == '*') return path.starts_with(pattern.substr(0, pattern.size() - 1));
  while (pattern.size() > 1 && pattern.back() == '/') pattern.remove_suffix(1);
  return path == pattern || (path.size() > pattern.size() && path.starts_with(pattern) && path[pattern.size()] == '/');
}

bool excluded(const std::vector<std::string>& patterns, std::string_view root_relative, std::string_view source) {
  std::string_view source_relative;
  if (!source.empty() && root_relative.size() > source.size() && root_relative.starts_with(source) &&
      root_relative[source.size()] == '/') {
    source_relative = root_relative.substr(source.size() + 1);
  }
  for (const auto& pattern : patterns) {
    if (patternMatches(pattern, root_relative)) return true;
    if (!source_relative.empty() && patternMatches(pattern, source_relative)) return true;
  }
  return false;
}

// Tracked files under the source (and the root's README/index, which may be
// the home page when the source is a subdirectory).
std::vector<std::string> trackedFiles(const fs::path& root, const std::string& source) {
  std::vector<std::string> arguments{"git", "-C", root.string(), "ls-files", "-z", "--", source.empty() ? "." : source};
  if (!source.empty()) {
    arguments.push_back("README.md");
    arguments.push_back("index.md");
  }
  ProcessOptions options;
  options.timeout = std::chrono::seconds(60);
  options.output_limit = 16 * 1024 * 1024;
  const auto result = runProcess(arguments, options);
  if (result.timed_out) siteError("git ls-files timed out");
  if (result.output_truncated) siteError("git ls-files listed more files than ckdocs can consider");
  if (result.exit_code != 0) {
    auto detail = result.output;
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\0')) detail.pop_back();
    siteError("git ls-files failed: " + detail);
  }
  std::vector<std::string> files;
  for (std::size_t start = 0; start < result.output.size();) {
    const auto end = result.output.find('\0', start);
    const auto stop = end == std::string::npos ? result.output.size() : end;
    if (stop > start) files.emplace_back(result.output.substr(start, stop - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return files;
}

std::vector<std::string> walkedFiles(const fs::path& root, const std::string& source) {
  std::vector<std::string> files;
  const fs::path base = source.empty() ? root : root / source;
  std::error_code error;
  fs::recursive_directory_iterator iterator(base, fs::directory_options::skip_permission_denied, error);
  if (error) siteError("cannot read " + base.string() + ": " + error.message());
  for (const fs::recursive_directory_iterator end; iterator != end; iterator.increment(error)) {
    if (error) siteError("cannot read " + base.string() + ": " + error.message());
    const fs::directory_entry& entry = *iterator;
    const auto name = entry.path().filename().string();
    const bool hidden = name.empty() || name.front() == '.';
    if (hidden || entry.is_symlink()) {
      if (entry.is_directory()) iterator.disable_recursion_pending();
      continue;
    }
    if (!entry.is_regular_file()) continue;
    files.push_back(fs::relative(entry.path(), root).generic_string());
  }
  if (error) siteError("cannot read " + base.string() + ": " + error.message());
  if (!source.empty()) {
    for (const auto* name : {"README.md", "index.md"}) {
      const auto candidate = root / name;
      if (fs::is_regular_file(candidate) && !fs::is_symlink(candidate)) files.emplace_back(name);
    }
  }
  return files;
}

// ---- page metadata ---------------------------------------------------------

// The text of the body's first `# ` heading (outside fenced code), rendered
// to plain text the same way an outline entry is.
std::string firstHeading(std::string_view body) {
  bool fenced = false;
  std::string_view fence;
  for (std::size_t start = 0; start < body.size();) {
    const auto newline = body.find('\n', start);
    const auto end = newline == std::string_view::npos ? body.size() : newline;
    auto line = body.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    start = end + 1;
    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') ++indent;
    if (indent > 3) continue;
    const auto content = line.substr(indent);
    if (content.starts_with("```") || content.starts_with("~~~")) {
      if (!fenced) {
        fenced = true;
        fence = content.substr(0, 3);
      } else if (content.starts_with(fence)) {
        fenced = false;
      }
      continue;
    }
    if (fenced || !content.starts_with("# ")) continue;
    LinkContext context;
    context.resolver = [](std::string_view target, bool) -> std::optional<std::string> { return std::string(target); };
    std::vector<MarkdownHeading> outline;
    static_cast<void>(renderMarkdown(line, context, &outline));
    if (!outline.empty() && !outline.front().text.empty()) return outline.front().text;
    return {};
  }
  return {};
}

struct OrderKey {
  bool ordered = false;
  int order = 0;
  std::string name;
  bool operator<(const OrderKey& other) const {
    if (ordered != other.ordered) return ordered;
    if (ordered && order != other.order) return order < other.order;
    return name < other.name;
  }
};

OrderKey keyFor(const DocsPage& page, std::string name) {
  return OrderKey{page.nav_order.has_value(), page.nav_order.value_or(0), std::move(name)};
}

// The derived navigation tree: one node per directory of the source.
struct DirNode {
  std::string name;
  std::optional<std::size_t> index_page;  // its README/index page
  std::vector<std::size_t> pages;
  std::vector<DirNode> children;

  DirNode& child(std::string_view component) {
    for (auto& existing : children) {
      if (existing.name == component) return existing;
    }
    children.push_back(DirNode{std::string(component), std::nullopt, {}, {}});
    return children.back();
  }
};

void flattenInto(const DocsNavItem& item, std::vector<DocsNavItem>& out) {
  if (item.page) out.push_back(DocsNavItem{item.title, item.page, {}});
  for (const auto& child : item.children) flattenInto(child, out);
}

DocsNavItem convertNode(const DirNode& node, const std::vector<DocsPage>& pages, std::size_t depth) {
  DocsNavItem item;
  item.title = node.index_page ? pages[*node.index_page].title : docsTitleFromFilename(node.name);
  item.page = node.index_page;
  struct Child {
    OrderKey key;
    DocsNavItem item;
  };
  std::vector<Child> children;
  for (const auto index : node.pages) {
    children.push_back({keyFor(pages[index], std::string(basenameOf(pages[index].source))),
                        DocsNavItem{pages[index].title, index, {}}});
  }
  for (const auto& child : node.children) {
    OrderKey key{false, 0, child.name};
    if (child.index_page) key = keyFor(pages[*child.index_page], child.name);
    children.push_back({key, convertNode(child, pages, depth + 1)});
  }
  std::stable_sort(children.begin(), children.end(), [](const Child& a, const Child& b) { return a.key < b.key; });
  for (auto& child : children) {
    // A tab is depth 1 and groups stop at depth 3, as for a configured nav;
    // a directory that would be deeper is listed flat inside its parent.
    if (!child.item.children.empty() && depth + 1 >= kMaximumDocsNavDepth) flattenInto(child.item, item.children);
    else item.children.push_back(std::move(child.item));
  }
  return item;
}

void collectReadingOrder(const DocsNavItem& item, std::vector<std::size_t>& out) {
  if (item.page) out.push_back(*item.page);
  for (const auto& child : item.children) collectReadingOrder(child, out);
}

void resolveNav(const std::vector<DocsNavEntry>& entries, std::vector<DocsNavItem>& out,
                const std::map<std::string, std::size_t>& by_source, const std::vector<DocsPage>& pages,
                std::vector<bool>& mentioned) {
  for (const auto& entry : entries) {
    DocsNavItem item;
    if (!entry.page.empty()) {
      const auto found = by_source.find(entry.page);
      if (found == by_source.end()) siteError("nav names '" + entry.page + "', which is not a page of the site");
      item.page = found->second;
      item.title = entry.title.empty() ? pages[found->second].title : entry.title;
      mentioned[found->second] = true;
    } else {
      item.title = entry.title;
      resolveNav(entry.pages, item.children, by_source, pages, mentioned);
    }
    out.push_back(std::move(item));
  }
}

}  // namespace

std::string docsTitleFromFilename(std::string_view name) {
  auto base = basenameOf(name);
  if (hasMarkdownExtension(base)) base = stemOf(base);
  std::size_t digits = 0;
  while (digits < base.size() && std::isdigit(static_cast<unsigned char>(base[digits])) != 0) ++digits;
  if (digits > 0 && digits + 1 < base.size() && (base[digits] == '-' || base[digits] == '_')) base.remove_prefix(digits + 1);
  std::string title;
  for (const char byte : base) title.push_back(byte == '-' || byte == '_' ? ' ' : byte);
  while (!title.empty() && title.back() == ' ') title.pop_back();
  while (!title.empty() && title.front() == ' ') title.erase(title.begin());
  if (title.empty()) title = std::string(base);
  if (!title.empty() && title.front() >= 'a' && title.front() <= 'z') title.front() = static_cast<char>(title.front() - 'a' + 'A');
  return title;
}

DocsConfig parseDocsConfig(std::string_view yaml) {
  const Node root = parseYamlSubset(yaml, kDocsConfigDialect);
  rejectUnknownKeys(root, {"version", "site", "source", "home", "exclude", "nav", "search"});
  const Node* version = findEntry(root, "version");
  if (version == nullptr) malformed("the config is missing 'version'", root.line);
  const auto& version_text = scalarOf(*version, "version to be a number");
  int version_value = 0;
  const auto [end, error] = std::from_chars(version_text.data(), version_text.data() + version_text.size(), version_value);
  if (error != std::errc{} || end != version_text.data() + version_text.size() || version_value != 1) {
    malformed("version must be 1", root.line);
  }

  DocsConfig config;
  const Node* site = findEntry(root, "site");
  if (site == nullptr) malformed("the config is missing 'site'", root.line);
  requireKind(*site, Node::Kind::Mapping, "site to be a mapping");
  rejectUnknownKeys(*site, {"title", "brand", "logo", "description", "footer", "links", "stylesheet"});
  const Node* title = findEntry(*site, "title");
  if (title == nullptr) malformed("site needs a 'title'", site->line);
  config.title = titleOf(*title, "site.title");
  if (const Node* brand = findEntry(*site, "brand")) config.brand = titleOf(*brand, "site.brand");
  if (const Node* logo = findEntry(*site, "logo")) config.logo = pathOf(*logo, "site.logo");
  if (const Node* stylesheet = findEntry(*site, "stylesheet")) config.stylesheet = pathOf(*stylesheet, "site.stylesheet");
  for (const auto* key : {"description", "footer"}) {
    if (const Node* text = findEntry(*site, key)) {
      const auto value = scalarOf(*text, key);
      if (!isValidDocsText(value)) malformed(std::string("site.") + key + " is longer than 1024 bytes or has control characters", text->line);
      (std::string_view(key) == "description" ? config.description : config.footer) = value;
    }
  }
  if (const Node* links = findEntry(*site, "links")) {
    requireKind(*links, Node::Kind::Sequence, "site.links to be a list");
    if (links->items.size() > kMaximumDocsLinks) tooLarge("site has too many links");
    for (const Node& item : links->items) {
      requireKind(item, Node::Kind::Mapping, "each link to be a mapping with 'title' and 'url'");
      rejectUnknownKeys(item, {"title", "url"});
      const Node* link_title = findEntry(item, "title");
      const Node* url = findEntry(item, "url");
      if (link_title == nullptr || url == nullptr) malformed("a link needs both 'title' and 'url'", item.line);
      DocsLink link;
      link.title = titleOf(*link_title, "a link title");
      link.url = scalarOf(*url, "a link url");
      if (!isValidLinkUrl(link.url)) malformed("link url '" + link.url + "' must be http(s) or mailto", url->line);
      config.links.push_back(std::move(link));
    }
  }

  if (const Node* source = findEntry(root, "source")) config.source = pathOf(*source, "source");
  if (const Node* home = findEntry(root, "home")) {
    config.home = pathOf(*home, "home");
    if (!hasMarkdownExtension(config.home)) malformed("home must be a Markdown page", home->line);
  }
  if (const Node* exclude = findEntry(root, "exclude")) {
    requireKind(*exclude, Node::Kind::Sequence, "exclude to be a list");
    if (exclude->items.size() > kMaximumDocsExcludes) tooLarge("too many exclude patterns");
    for (const Node& item : exclude->items) {
      const auto pattern = scalarOf(item, "each exclude pattern to be a scalar");
      if (!isValidExcludePattern(pattern)) {
        malformed("exclude pattern '" + pattern + "' must be a relative path, optionally ending in '*'", item.line);
      }
      config.exclude.push_back(pattern);
    }
  }
  if (const Node* nav = findEntry(root, "nav")) {
    std::size_t count = 0;
    parseNavEntries(*nav, config.nav, 1, count);
  }
  if (const Node* search = findEntry(root, "search")) {
    const auto value = scalarOf(*search, "search to be true or false");
    if (value != "true" && value != "false") malformed("search must be true or false", search->line);
    config.search = value == "true";
  }
  return config;
}

DocsConfig readDocsConfig(const fs::path& root) {
  const auto path = root / "ckdocs.yml";
  std::error_code error;
  if (!fs::exists(path, error)) return DocsConfig{};
  if (!fs::is_regular_file(path, error)) siteError("ckdocs.yml is not a regular file");
  const auto content = readFile(path, kMaximumDocsConfigBytes + 1);
  if (!content) siteError("cannot read ckdocs.yml");
  return parseDocsConfig(*content);
}

DocsSiteModel loadDocsSite(const fs::path& root, const DocsConfig& config, std::vector<std::string>* warnings) {
  const auto warn = [warnings](const std::string& message) {
    if (warnings != nullptr) warnings->push_back(message);
  };
  DocsSiteModel model;
  model.root = root;
  model.config = config;
  std::error_code error;
  if (!fs::is_directory(root, error)) siteError("'" + root.string() + "' is not a directory");

  // The page tree.
  if (!config.source.empty()) {
    if (!fs::is_directory(root / config.source, error)) siteError("source directory '" + config.source + "' does not exist");
    model.source = config.source;
    while (model.source.size() > 1 && model.source.back() == '/') model.source.pop_back();
  } else if (fs::is_directory(root / "docs", error)) {
    model.source = "docs";
  }
  model.title = config.title.empty() ? fs::absolute(root).lexically_normal().filename().string() : config.title;
  if (model.title.empty() || model.title == "/" || model.title == ".") model.title = "Documentation";

  // Candidate files: tracked ones inside a work tree, a walk otherwise.
  const bool work_tree = fs::exists(root / ".git", error);
  auto files = work_tree ? trackedFiles(root, model.source) : walkedFiles(root, model.source);
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());

  std::vector<std::string> candidates;
  for (const auto& file : files) {
    if (!hasMarkdownExtension(file) || hasDotComponent(file) || !isSafeRelativePath(file)) continue;
    if (excluded(config.exclude, file, model.source)) continue;
    const bool inside = model.source.empty() || (file.starts_with(model.source) && file.size() > model.source.size() &&
                                                 file[model.source.size()] == '/');
    if (!inside && file != "README.md" && file != "index.md") continue;
    const auto full = root / file;
    if (fs::is_symlink(full, error) || !fs::is_regular_file(full, error)) {
      if (work_tree && !fs::exists(full, error)) warn(file + ": tracked but missing on disk; skipped");
      continue;
    }
    candidates.push_back(file);
  }

  // The home page.
  const auto has = [&candidates](std::string_view file) {
    return std::find(candidates.begin(), candidates.end(), file) != candidates.end();
  };
  // The home page: configured; else the source tree's own README/index (a
  // documentation tree's landing page); else the repository's README.
  std::string home;
  if (!config.home.empty()) {
    if (!has(config.home)) {
      siteError("home page '" + config.home + "' is not a page of the site (missing, excluded, or outside the source)");
    }
    home = config.home;
  } else {
    for (const auto* name : {"README.md", "index.md"}) {
      const auto candidate = model.source.empty() ? std::string(name) : model.source + "/" + name;
      if (has(candidate)) {
        home = candidate;
        break;
      }
    }
    if (home.empty() && !model.source.empty() && has("README.md")) home = "README.md";
  }
  const bool root_outside_source = !model.source.empty();
  std::vector<std::string> selected;
  for (const auto& file : candidates) {
    const bool inside = !root_outside_source || file.starts_with(model.source + "/");
    if (inside || file == home) selected.push_back(file);
  }
  if (selected.empty()) siteError("no Markdown pages found under '" + (model.source.empty() ? std::string(".") : model.source) + "'");
  if (selected.size() > kMaximumDocsPages) siteError("the site has more than 4096 pages");
  if (home.empty()) home = selected.front();

  // Each page's metadata and output path.
  std::map<std::string, std::size_t> by_output;
  for (const auto& file : selected) {
    DocsPage page;
    page.source = file;
    page.home = file == home;
    const auto content = readFile(root / file, kMaximumMarkdownInputBytes);
    if (!content) siteError("cannot read '" + file + "', or it exceeds the 512 KiB page limit");
    if (!isValidUtf8(*content)) siteError("'" + file + "' is not valid UTF-8");
    std::string body = *content;
    if (const auto front = splitFrontMatter(*content)) {
      body = content->substr(front->body_offset);
      if (!front->error.empty()) {
        page.front_matter_error = front->error;
        warn(file + ": " + front->error);
      }
      for (const auto& [key, value] : front->entries) {
        if (key == "title") {
          if (isValidDocsTitle(value)) page.title = value;
          else warn(file + ": front matter title is empty, too long, or has control characters; ignored");
        } else if (key == "description") {
          if (isValidDocsText(value)) page.description = value;
          else warn(file + ": front matter description is too long or has control characters; ignored");
        } else if (key == "nav_order") {
          int order = 0;
          const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), order);
          if (parse_error == std::errc{} && end == value.data() + value.size()) page.nav_order = order;
          else warn(file + ": nav_order '" + value + "' is not an integer; ignored");
        } else if (key == "nav_exclude") {
          if (value == "true" || value == "false") page.nav_exclude = value == "true";
          else warn(file + ": nav_exclude '" + value + "' is not true or false; ignored");
        } else {
          warn(file + ": unknown front matter key '" + key + "' ignored");
        }
      }
    }
    if (page.title.empty()) page.title = firstHeading(body);
    if (page.title.empty()) {
      const auto base = basenameOf(file);
      if (isIndexName(base)) {
        const auto directory = directoryOf(file);
        page.title = page.home || directory.empty() ? model.title : docsTitleFromFilename(basenameOf(directory));
      } else {
        page.title = docsTitleFromFilename(base);
      }
    }
    if (page.home) {
      page.output = "index.html";
    } else {
      const std::string relative = model.source.empty() ? file : file.substr(model.source.size() + 1);
      const auto directory = directoryOf(relative);
      const auto base = basenameOf(relative);
      const std::string prefix = directory.empty() ? std::string() : std::string(directory) + "/";
      page.output = isIndexName(base) ? prefix + "index.html" : prefix + std::string(stemOf(base)) + ".html";
    }
    const auto [existing, inserted] = by_output.try_emplace(page.output, model.pages.size());
    if (!inserted) {
      siteError("'" + model.pages[existing->second].source + "' and '" + file + "' would both be written to '" + page.output +
                "'; exclude or rename one of them");
    }
    model.pages.push_back(std::move(page));
  }
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    if (model.pages[index].home) model.home = index;
  }

  // Navigation.
  std::vector<bool> listed(model.pages.size(), false);
  if (!config.nav.empty()) {
    std::map<std::string, std::size_t> by_source;
    for (std::size_t index = 0; index < model.pages.size(); ++index) by_source.emplace(model.pages[index].source, index);
    resolveNav(config.nav, model.nav, by_source, model.pages, listed);
  } else {
    model.nav.push_back(DocsNavItem{"Home", model.home, {}});
    listed[model.home] = true;
    DirNode tree;
    for (std::size_t index = 0; index < model.pages.size(); ++index) {
      const auto& page = model.pages[index];
      if (page.home || page.nav_exclude) continue;
      const std::string relative = model.source.empty() ? page.source : page.source.substr(model.source.size() + 1);
      DirNode* node = &tree;
      std::string_view rest = relative;
      while (true) {
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos) break;
        node = &node->child(rest.substr(0, slash));
        rest = rest.substr(slash + 1);
      }
      if (isIndexName(rest) && node != &tree) node->index_page = index;
      else node->pages.push_back(index);
      listed[index] = true;
    }
    DocsNavItem top = convertNode(tree, model.pages, 0);
    for (auto& child : top.children) model.nav.push_back(std::move(child));
  }
  for (const auto& tab : model.nav) collectReadingOrder(tab, model.reading_order);
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    if (listed[index]) continue;
    if (!config.nav.empty() && index != model.home && !model.pages[index].nav_exclude) {
      warn(model.pages[index].source + ": not named by nav; reachable from the site index only");
    }
    model.unlisted.push_back(index);
  }
  return model;
}

// ---- building ----------------------------------------------------------------

namespace {

std::string escapeHtml(std::string_view value) {
  std::string out;
  for (const char byte : value) {
    switch (byte) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += byte;
    }
  }
  return out;
}

std::string unescapeHtml(std::string_view value) {
  std::string out;
  for (std::size_t index = 0; index < value.size();) {
    const auto rest = value.substr(index);
    bool decoded = false;
    for (const auto& [entity, character] : {std::pair<std::string_view, char>{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
                                            {"&quot;", '"'}, {"&#39;", '\''}}) {
      if (rest.starts_with(entity)) {
        out += character;
        index += entity.size();
        decoded = true;
        break;
      }
    }
    if (!decoded) out += value[index++];
  }
  return out;
}

std::vector<std::string_view> splitPath(std::string_view path) {
  std::vector<std::string_view> parts;
  for (std::size_t start = 0; start <= path.size();) {
    const auto slash = path.find('/', start);
    const auto part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (!part.empty()) parts.push_back(part);
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return parts;
}

// `..` and `.` resolved; nullopt when the path would leave the root.
std::optional<std::string> normalizePath(std::string_view path) {
  std::vector<std::string_view> parts;
  for (const auto part : splitPath(path)) {
    if (part == ".") continue;
    if (part == "..") {
      if (parts.empty()) return std::nullopt;
      parts.pop_back();
      continue;
    }
    parts.push_back(part);
  }
  std::string result;
  for (const auto part : parts) {
    if (!result.empty()) result += '/';
    result += part;
  }
  return result;
}

// The URL from a page in `from_dir` (site-relative directory, "" at the
// root) to the site-relative file `target`, percent-encoded per component.
std::string relativeUrl(std::string_view from_dir, std::string_view target) {
  const auto from = splitPath(from_dir);
  const auto to = splitPath(target);
  std::size_t common = 0;
  while (common < from.size() && common + 1 < to.size() && from[common] == to[common]) ++common;
  std::string result;
  for (std::size_t index = common; index < from.size(); ++index) result += "../";
  for (std::size_t index = common; index < to.size(); ++index) {
    if (index > common) result += '/';
    result += encodePathSegment(std::string(to[index]));
  }
  return result;
}

std::optional<std::size_t> firstPageOf(const DocsNavItem& item) {
  if (item.page) return item.page;
  for (const auto& child : item.children) {
    if (const auto found = firstPageOf(child)) return found;
  }
  return std::nullopt;
}

// The chain of nav items from a tab down to the item showing `page`.
bool findTrail(const std::vector<DocsNavItem>& items, std::size_t page, std::vector<const DocsNavItem*>& trail) {
  for (const auto& item : items) {
    trail.push_back(&item);
    if (item.page == page || findTrail(item.children, page, trail)) return true;
    trail.pop_back();
  }
  return false;
}

// Everything the page shell needs that does not change per page.
struct SiteContext {
  const DocsSiteModel* model = nullptr;
  std::string version;
  std::string logo_output;        // site-relative path of the copied logo, or ""
  std::string stylesheet_output;  // site-relative path of the copied stylesheet, or ""
  std::vector<std::vector<MarkdownHeading>> outlines;  // per page, filled while rendering
};

void renderSidebarItems(std::string& out, const std::vector<DocsNavItem>& items, const DocsSiteModel& model,
                        std::size_t current, std::string_view from_dir) {
  out += "<ul>";
  for (const auto& item : items) {
    out += "<li>";
    const auto link = [&](std::string_view title, std::optional<std::size_t> page) {
      if (!page) return escapeHtml(title);
      const bool active = *page == current;
      return "<a href=\"" + escapeHtml(relativeUrl(from_dir, model.pages[*page].output)) + "\"" +
             (active ? " aria-current=\"page\"" : "") + ">" + escapeHtml(title) + "</a>";
    };
    if (item.children.empty()) {
      out += link(item.title, item.page);
    } else {
      out += "<details open><summary>" + link(item.title, item.page) + "</summary>";
      renderSidebarItems(out, item.children, model, current, from_dir);
      out += "</details>";
    }
    out += "</li>";
  }
  out += "</ul>";
}

std::string renderOutline(const std::vector<MarkdownHeading>& outline) {
  std::string out;
  bool open_h2 = false;
  bool open_h3_list = false;
  for (const auto& heading : outline) {
    if (heading.level != 2 && heading.level != 3) continue;
    if (heading.level == 2) {
      if (open_h3_list) out += "</ul>";
      if (open_h2) out += "</li>";
      out += "<li><a href=\"#" + escapeHtml(encodePathSegment(heading.id)) + "\">" + escapeHtml(heading.text) + "</a>";
      open_h2 = true;
      open_h3_list = false;
    } else {
      if (!open_h2) {
        out += "<li>";
        open_h2 = true;
      }
      if (!open_h3_list) {
        out += "<ul>";
        open_h3_list = true;
      }
      out += "<li><a href=\"#" + escapeHtml(encodePathSegment(heading.id)) + "\">" + escapeHtml(heading.text) + "</a></li>";
    }
  }
  if (open_h3_list) out += "</ul>";
  if (open_h2) out += "</li>";
  return out.empty() ? out : "<ul>" + out + "</ul>";
}

struct ShellInput {
  std::string title;        // the page title
  std::string description;  // meta description, may be empty
  std::string output;       // the page's site-relative output path
  std::string article;      // rendered body HTML
  const std::vector<MarkdownHeading>* outline = nullptr;
  std::optional<std::size_t> page;  // the model page, when this is one
};

std::string renderShell(const SiteContext& context, const ShellInput& in) {
  const DocsSiteModel& model = *context.model;
  const std::string from_dir(directoryOf(in.output));
  const auto url = [&](std::string_view target) { return escapeHtml(relativeUrl(from_dir, target)); };
  std::vector<const DocsNavItem*> trail;
  if (in.page) findTrail(model.nav, *in.page, trail);
  const DocsNavItem* tab = trail.empty() ? nullptr : trail.front();

  std::string out = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  out += "<title>" + escapeHtml(in.title == model.title ? in.title : in.title + " · " + model.title) + "</title>";
  if (!in.description.empty()) out += "<meta name=\"description\" content=\"" + escapeHtml(in.description) + "\">";
  out += "<meta name=\"generator\" content=\"ckdocs " + escapeHtml(context.version) + "\">";
  out += "<style>";
  out += kDocsStyles;
  out += "</style>";
  if (!context.stylesheet_output.empty()) out += "<link rel=\"stylesheet\" href=\"" + url(context.stylesheet_output) + "\">";
  out += "</head><body><a class=\"skip-link\" href=\"#main-content\">Skip to content</a>";

  // Header: brand, tabs, links.
  out += "<header class=\"site\"><a class=\"brand\" href=\"" + url(model.pages[model.home].output) + "\">";
  if (!context.logo_output.empty()) out += "<img src=\"" + url(context.logo_output) + "\" alt=\"\">";
  out += escapeHtml(model.config.brand.empty() ? model.title : model.config.brand) + "</a>";
  out += "<nav class=\"tabs\" aria-label=\"Sections\">";
  for (const auto& item : model.nav) {
    const auto first = firstPageOf(item);
    if (!first) continue;
    out += "<a href=\"" + url(model.pages[*first].output) + "\"" + (tab == &item ? " aria-current=\"page\"" : "") + ">" +
           escapeHtml(item.title) + "</a>";
  }
  out += "</nav>";
  if (model.config.search) {
    // The input starts hidden; kDocsSearchScript is the only thing that
    // reveals it, so a visitor without scripting sees only the <noscript>
    // link below, never a non-functional box.
    out += "<form class=\"search\" role=\"search\" onsubmit=\"return false\">"
           "<input type=\"search\" id=\"ckdocs-search\" name=\"q\" placeholder=\"Search\" "
           "aria-label=\"Search this site\" autocomplete=\"off\" spellcheck=\"false\" hidden data-index=\"" +
           url(std::string(kDocsSearchIndexPage)) + "\">"
           "<div id=\"ckdocs-search-results\" class=\"search-results\" hidden></div>"
           "<noscript><a href=\"" + url(std::string(kDocsSiteIndexPage)) + "\">Site index</a></noscript>"
           "</form><script>" + std::string(kDocsSearchScript) + "</script>";
  }
  if (!model.config.links.empty()) {
    out += "<nav class=\"links\" aria-label=\"Links\">";
    for (const auto& link : model.config.links) {
      out += "<a href=\"" + escapeHtml(link.url) + "\" rel=\"noopener\">" + escapeHtml(link.title) + "</a>";
    }
    out += "</nav>";
  }
  out += "</header>";

  // Sidebar: the active tab's pages, then this page's outline.
  out += "<div class=\"page\"><input class=\"nav-switch\" id=\"nav-toggle\" type=\"checkbox\">"
         "<label class=\"nav-toggle\" for=\"nav-toggle\">Menu</label><aside class=\"sidebar\">";
  const auto current = in.page.value_or(model.pages.size());
  if (tab != nullptr) {
    out += "<nav aria-label=\"Pages\"><p class=\"side-title\">";
    if (tab->page) {
      out += "<a href=\"" + url(model.pages[*tab->page].output) + "\"" +
             (*tab->page == current ? " aria-current=\"page\"" : "") + ">" + escapeHtml(tab->title) + "</a>";
    } else {
      out += escapeHtml(tab->title);
    }
    out += "</p>";
    if (!tab->children.empty()) renderSidebarItems(out, tab->children, model, current, from_dir);
    out += "</nav>";
  }
  const std::string outline = in.outline ? renderOutline(*in.outline) : std::string();
  if (!outline.empty()) out += "<nav class=\"outline\" aria-label=\"On this page\"><p class=\"side-title\">On this page</p>" + outline + "</nav>";
  out += "</aside><main id=\"main-content\">";

  // Breadcrumbs, article, previous/next.
  if (trail.size() > 1) {
    out += "<nav class=\"crumbs\" aria-label=\"Breadcrumb\"><ol>";
    for (std::size_t index = 0; index < trail.size(); ++index) {
      const auto* item = trail[index];
      const bool last = index + 1 == trail.size();
      if (last) {
        out += "<li aria-current=\"page\">" + escapeHtml(item->title) + "</li>";
      } else if (const auto first = firstPageOf(*item)) {
        out += "<li><a href=\"" + url(model.pages[*first].output) + "\">" + escapeHtml(item->title) + "</a></li>";
      } else {
        out += "<li>" + escapeHtml(item->title) + "</li>";
      }
    }
    out += "</ol></nav>";
  }
  out += "<article>" + in.article + "</article>";
  if (in.page) {
    const auto& order = model.reading_order;
    const auto position = std::find(order.begin(), order.end(), *in.page);
    if (position != order.end()) {
      std::string pager;
      if (position != order.begin()) {
        const auto& previous = model.pages[*(position - 1)];
        pager += "<a class=\"previous\" rel=\"prev\" href=\"" + url(previous.output) + "\"><small>Previous</small>← " +
                 escapeHtml(previous.title) + "</a>";
      }
      if (position + 1 != order.end()) {
        const auto& next = model.pages[*(position + 1)];
        pager += "<a class=\"next\" rel=\"next\" href=\"" + url(next.output) + "\"><small>Next</small>" +
                 escapeHtml(next.title) + " →</a>";
      }
      if (!pager.empty()) out += "<nav class=\"pager\" aria-label=\"Previous and next page\">" + pager + "</nav>";
    }
  }
  out += "</main></div>";

  // Footer.
  out += "<footer class=\"site\">";
  if (!model.config.footer.empty()) out += "<span>" + escapeHtml(model.config.footer) + "</span>";
  out += "<span>Built with ckdocs " + escapeHtml(context.version) + "</span>";
  out += "<a href=\"" + url(std::string(kDocsSiteIndexPage)) + "\">Site index</a>";
  return out + "</footer></body></html>";
}

// Where an asset lands: like a page, relative to the source when inside it.
std::string assetOutputPath(const DocsSiteModel& model, std::string_view root_relative) {
  if (!model.source.empty() && root_relative.size() > model.source.size() && root_relative.starts_with(model.source) &&
      root_relative[model.source.size()] == '/') {
    return std::string(root_relative.substr(model.source.size() + 1));
  }
  return std::string(root_relative);
}

// Heading ids and the internal hrefs (with a fragment) of a rendered article.
void scanArticle(std::string_view html, std::set<std::string>& ids, std::vector<std::pair<std::string, std::string>>& links) {
  for (auto at = html.find(" id=\""); at != std::string_view::npos; at = html.find(" id=\"", at + 5)) {
    const auto end = html.find('"', at + 5);
    if (end == std::string_view::npos) break;
    ids.insert(unescapeHtml(html.substr(at + 5, end - at - 5)));
  }
  for (auto at = html.find("href=\""); at != std::string_view::npos; at = html.find("href=\"", at + 6)) {
    const auto end = html.find('"', at + 6);
    if (end == std::string_view::npos) break;
    const auto href = unescapeHtml(html.substr(at + 6, end - at - 6));
    const auto lowered = lowerAscii(href.substr(0, 8));
    if (lowered.starts_with("http://") || lowered.starts_with("https://") || lowered.starts_with("mailto:")) continue;
    const auto hash = href.find('#');
    if (hash == std::string::npos || hash + 1 == href.size()) continue;
    links.emplace_back(href.substr(0, hash), href.substr(hash + 1));
  }
}

// ---- opt-in search (site.search) -------------------------------------------

std::string jsonEscape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (const unsigned char character : value) {
    switch (character) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (character < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped += kHex[character >> 4];
          escaped += kHex[character & 0x0f];
        } else {
          escaped += static_cast<char>(character);
        }
    }
  }
  return escaped;
}

// Every `<hN id="...">` tag in a rendered article whose id is one of the
// page's own top-level headings (`outline`, from renderMarkdown) -- a
// heading nested inside a quote or a list item still gets an id in the
// markup but starts no search section of its own; its words simply count
// toward whichever enclosing section contains it.
struct HeadingMark {
  std::size_t begin = 0;       // the opening tag's own start, "<hN ..."
  std::size_t content_end = 0;  // just past the matching closing "</hN>", so a
                                // section's own excerpt never repeats its heading's text
  std::string id;
};

std::vector<HeadingMark> findSectionHeadings(std::string_view html, const std::vector<MarkdownHeading>& outline) {
  std::set<std::string> known;
  for (const auto& heading : outline) known.insert(heading.id);
  std::vector<HeadingMark> marks;
  for (std::size_t at = html.find("<h"); at != std::string_view::npos; at = html.find("<h", at)) {
    if (at + 2 >= html.size() || html[at + 2] < '1' || html[at + 2] > '6' || html.substr(at + 3, 5) != " id=\"") {
      at += 2;
      continue;
    }
    const char level = html[at + 2];
    const auto id_start = at + 8;
    const auto id_end = html.find('"', id_start);
    if (id_end == std::string_view::npos) break;
    const auto tag_end = html.find('>', id_end);
    if (tag_end == std::string_view::npos) break;
    const std::string closing = std::string("</h") + level + ">";
    auto content_end = html.find(closing, tag_end + 1);
    content_end = content_end == std::string_view::npos ? tag_end + 1 : content_end + closing.size();
    auto id = unescapeHtml(html.substr(id_start, id_end - id_start));
    if (known.count(id) != 0) marks.push_back({at, content_end, std::move(id)});
    at = content_end;
  }
  return marks;
}

// Strips tags (each becomes a space, so "a</p><p>b" reads "a b" rather than
// "ab"), decodes entities, collapses whitespace runs to single spaces, trims,
// and caps at kMaximumDocsSearchExcerptChars bytes without splitting a UTF-8
// sequence (a byte-based bound, not a strict codepoint count).
std::string plainTextExcerpt(std::string_view html) {
  std::string raw;
  raw.reserve(html.size());
  bool in_tag = false;
  for (const char byte : html) {
    if (byte == '<') { in_tag = true; continue; }
    if (byte == '>') { in_tag = false; raw += ' '; continue; }
    if (!in_tag) raw += byte;
  }
  const std::string text = unescapeHtml(raw);
  std::string collapsed;
  collapsed.reserve(text.size());
  bool space = true;  // trims leading whitespace too
  for (const unsigned char byte : text) {
    if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r') {
      if (!space) collapsed += ' ';
      space = true;
    } else {
      collapsed += static_cast<char>(byte);
      space = false;
    }
  }
  while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
  if (collapsed.size() > kMaximumDocsSearchExcerptChars) {
    collapsed.resize(kMaximumDocsSearchExcerptChars);
    while (!collapsed.empty() && (static_cast<unsigned char>(collapsed.back()) & 0xc0) == 0x80) collapsed.pop_back();
  }
  return collapsed;
}

struct SearchSection {
  std::string heading;  // empty for the lead section (before the first heading)
  std::string anchor;   // empty for the lead section
  std::string excerpt;
};

// One page's sections: a lead section (the article's content before its
// first top-level heading, omitted when it has no excerpt at all) plus one
// section per top-level heading, in document order.
std::vector<SearchSection> pageSearchSections(std::string_view article, const std::vector<MarkdownHeading>& outline) {
  const auto marks = findSectionHeadings(article, outline);
  std::vector<SearchSection> sections;
  const auto headingText = [&](const std::string& id) -> std::string {
    for (const auto& heading : outline) {
      if (heading.id == id) return heading.text;
    }
    return {};
  };
  const std::size_t lead_end = marks.empty() ? article.size() : marks.front().begin;
  if (auto excerpt = plainTextExcerpt(article.substr(0, lead_end)); !excerpt.empty()) {
    sections.push_back({"", "", std::move(excerpt)});
  }
  for (std::size_t index = 0; index < marks.size(); ++index) {
    const auto section_end = index + 1 < marks.size() ? marks[index + 1].begin : article.size();
    sections.push_back({headingText(marks[index].id), marks[index].id,
                        plainTextExcerpt(article.substr(marks[index].content_end, section_end - marks[index].content_end))});
  }
  return sections;
}

// One page's title/url/sections, serialized as one JSON object.
std::string searchPageEntryJson(const std::string& title, const std::string& url,
                                const std::vector<SearchSection>& sections) {
  std::string entry = "{\"title\":\"" + jsonEscape(title) + "\",\"url\":\"" + jsonEscape(url) + "\",\"sections\":[";
  for (std::size_t index = 0; index < sections.size(); ++index) {
    if (index) entry += ',';
    entry += "{\"heading\":\"" + jsonEscape(sections[index].heading) + "\",\"anchor\":\"" +
             jsonEscape(sections[index].anchor) + "\",\"excerpt\":\"" + jsonEscape(sections[index].excerpt) + "\"}";
  }
  entry += "]}";
  return entry;
}

// Joins pre-serialized per-page JSON objects into the final index,
// {"version":1,"pages":[...]}, dropping whole entries from the end (never a
// partial one) until the whole file fits kMaximumDocsSearchIndexBytes.
// *kept is the number of entries actually kept.
std::string buildSearchIndexJson(const std::vector<std::string>& page_entries, std::size_t* kept) {
  const auto render = [&](std::size_t count) {
    std::string json = "{\"version\":1,\"pages\":[";
    for (std::size_t index = 0; index < count; ++index) {
      if (index) json += ',';
      json += page_entries[index];
    }
    json += "]}";
    return json;
  };
  std::size_t count = page_entries.size();
  std::string json = render(count);
  while (json.size() > kMaximumDocsSearchIndexBytes && count > 0) {
    --count;
    json = render(count);
  }
  *kept = count;
  return json;
}

// Removes the temporary directory unless the build succeeded.
struct TemporaryDirectory {
  fs::path path;
  bool keep = false;
  ~TemporaryDirectory() {
    if (!keep && !path.empty()) {
      std::error_code error;
      fs::remove_all(path, error);
    }
  }
};

}  // namespace

void buildDocsSite(const DocsSiteModel& model, const fs::path& out_path, const DocsBuildOptions& options,
                   DocsBuildReport* report) {
  DocsBuildReport local_report;
  DocsBuildReport& result = report != nullptr ? *report : local_report;
  result = DocsBuildReport{};
  if (model.pages.empty()) siteError("the site has no pages");
  const fs::path out = fs::absolute(out_path).lexically_normal();
  std::error_code error;

  // Refuse early what the final swap would refuse, before any work.
  if (fs::exists(out, error)) {
    if (!fs::is_directory(out, error)) siteError("output path '" + out.string() + "' exists and is not a directory");
    const bool empty = fs::is_empty(out, error);
    const bool marked = fs::is_regular_file(out / kDocsSiteMarker, error);
    if (!empty && !marked) siteError("output directory '" + out.string() + "' exists and was not written by ckdocs; refusing to touch it");
    if (!empty && !options.clean) siteError("output directory '" + out.string() + "' already holds a site; use --clean to replace it");
  }

  SiteContext context;
  context.model = &model;
  context.version = buildVersion();
  context.outlines.resize(model.pages.size());

  // Site-relative outputs and their sources: pages, assets, the logo and stylesheet.
  std::map<std::string, std::size_t> page_by_source;
  std::map<std::string, std::size_t> page_by_output;
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    page_by_source.emplace(model.pages[index].source, index);
    page_by_output.emplace(model.pages[index].output, index);
  }
  std::map<std::string, std::string> assets;  // output path -> root-relative source
  const auto addAsset = [&](const std::string& root_relative, const char* what) {
    const auto full = model.root / root_relative;
    if (fs::is_symlink(full, error) || !fs::is_regular_file(full, error)) {
      siteError(std::string(what) + " '" + root_relative + "' is not a regular file");
    }
    if (fs::file_size(full, error) > kMaximumPagesFileBytes) siteError(std::string(what) + " '" + root_relative + "' exceeds the file size limit");
    const auto output = assetOutputPath(model, root_relative);
    if (page_by_output.count(output) != 0) siteError(std::string(what) + " '" + root_relative + "' would overwrite the page '" + output + "'");
    const auto [existing, inserted] = assets.try_emplace(output, root_relative);
    if (!inserted && existing->second != root_relative) {
      siteError("'" + existing->second + "' and '" + root_relative + "' would both be written to '" + output + "'");
    }
    return output;
  };
  if (!model.config.logo.empty()) context.logo_output = addAsset(model.config.logo, "site.logo");
  if (!model.config.stylesheet.empty()) context.stylesheet_output = addAsset(model.config.stylesheet, "site.stylesheet");

  // The temporary directory beside the output.
  auto pattern = (out.parent_path() / (out.filename().string() + ".tmp-XXXXXX")).string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  if (mkdtemp(writable.data()) == nullptr) siteError("cannot create a temporary directory beside '" + out.string() + "'");
  TemporaryDirectory temporary{fs::path(writable.data())};
  std::set<std::string> directories;
  const auto writeFile = [&](const std::string& site_path, const std::string& content) {
    const auto target = temporary.path / site_path;
    for (auto parent = fs::path(site_path).parent_path(); !parent.empty(); parent = parent.parent_path()) {
      if (directories.insert(parent.generic_string()).second) fs::create_directories(temporary.path / parent);
    }
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) siteError("cannot write '" + target.string() + "'");
    result.bytes_written += content.size();
    if (result.bytes_written > kMaximumPagesSiteBytes) siteError("the site exceeds the size limit for a Pages site");
    if (directories.size() + result.pages_written + result.assets_copied + 2 > kMaximumPagesEntries) {
      siteError("the site exceeds the entry limit for a Pages site");
    }
  };

  // Render and write every page; remember ids and links for the anchor check,
  // and, when search is on, each page's own sections for the index below.
  std::vector<std::set<std::string>> ids(model.pages.size());
  std::vector<std::vector<std::pair<std::string, std::string>>> links(model.pages.size());
  std::vector<std::string> search_entries;
  if (model.config.search) search_entries.reserve(model.pages.size());
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    const DocsPage& page = model.pages[index];
    const auto content = readFile(model.root / page.source, kMaximumMarkdownInputBytes);
    if (!content) siteError("cannot read '" + page.source + "', or it exceeds the 512 KiB page limit");
    const std::string page_dir(directoryOf(page.source));
    const std::string out_dir(directoryOf(page.output));
    LinkContext link_context;
    link_context.resolver = [&](std::string_view target, bool image) -> std::optional<std::string> {
      const auto broken = [&](const std::string& reason) {
        result.broken_links.push_back(page.source + ": " + (image ? "image" : "link") + " target '" + std::string(target) + "' " + reason);
        return std::optional<std::string>{};
      };
      std::string joined;
      if (target.starts_with('/')) joined = std::string(target.substr(1));
      else joined = page_dir.empty() ? std::string(target) : page_dir + "/" + std::string(target);
      const auto normalized = normalizePath(joined);
      if (!normalized) return broken("leaves the repository");
      const bool directory_target = target.ends_with('/') || target == "." || target == ".." || target.ends_with("/.") ||
                                    target.ends_with("/..");
      if (!directory_target) {
        if (const auto found = page_by_source.find(*normalized); found != page_by_source.end()) {
          return relativeUrl(out_dir, model.pages[found->second].output);
        }
      }
      for (const auto* name : {"README.md", "index.md"}) {
        const auto candidate = normalized->empty() ? std::string(name) : *normalized + "/" + name;
        if (const auto found = page_by_source.find(candidate); found != page_by_source.end()) {
          return relativeUrl(out_dir, model.pages[found->second].output);
        }
      }
      if (directory_target || normalized->empty()) return broken("is a directory without an index page");
      if (hasMarkdownExtension(*normalized)) return broken("is not a page of this site");
      const auto full = model.root / *normalized;
      if (fs::is_symlink(full, error) || !fs::is_regular_file(full, error)) return broken("does not exist");
      if (fs::file_size(full, error) > kMaximumPagesFileBytes) return broken("exceeds the file size limit");
      return relativeUrl(out_dir, addAsset(*normalized, "file"));
    };
    std::string article;
    try {
      article = renderMarkdown(markdownBody(*content), link_context, &context.outlines[index]);
    } catch (const std::length_error& bound) {
      siteError("'" + page.source + "' exceeds a rendering bound: " + bound.what());
    }
    scanArticle(article, ids[index], links[index]);
    if (model.config.search) {
      search_entries.push_back(searchPageEntryJson(page.title, page.output, pageSearchSections(article, context.outlines[index])));
    }
    ShellInput input;
    input.title = page.title;
    input.description = page.description.empty() && page.home ? model.config.description : page.description;
    input.output = page.output;
    input.article = std::move(article);
    input.outline = &context.outlines[index];
    input.page = index;
    writeFile(page.output, renderShell(context, input));
    ++result.pages_written;
  }

  if (model.config.search) {
    std::size_t kept = 0;
    const std::string json = buildSearchIndexJson(search_entries, &kept);
    if (kept < search_entries.size()) {
      result.warnings.push_back("search-index.json would exceed its 2 MiB bound with all " +
                                std::to_string(search_entries.size()) + " page(s); kept the first " +
                                std::to_string(kept) + " and dropped the rest");
    }
    writeFile(std::string(kDocsSearchIndexPage), json);
  }

  // Fragments must name a heading on their target page.
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    const std::string out_dir(directoryOf(model.pages[index].output));
    for (const auto& [path, fragment] : links[index]) {
      std::size_t target = index;
      if (!path.empty()) {
        const auto decoded = decodePathSegment(path);
        const auto normalized = decoded ? normalizePath(out_dir.empty() ? *decoded : out_dir + "/" + *decoded) : std::nullopt;
        if (!normalized) continue;
        const auto found = page_by_output.find(*normalized);
        if (found == page_by_output.end()) continue;  // an asset, or already a broken link
        target = found->second;
      }
      const auto anchor = decodePathSegment(fragment).value_or(fragment);
      if (ids[target].count(anchor) == 0) {
        result.broken_anchors.push_back(model.pages[index].source + ": '" + path + "#" + fragment + "' names no heading on " +
                                        model.pages[target].source);
      }
    }
  }

  // The site index: every tab, page, and h2/h3, as plain nested lists.
  {
    std::string body = "<h1>Site index</h1>";
    const std::string from_dir;  // the index page sits at the site root
    const auto pageEntry = [&](std::size_t index) {
      const auto& page = model.pages[index];
      std::string entry = "<a href=\"" + escapeHtml(relativeUrl(from_dir, page.output)) + "\">" + escapeHtml(page.title) + "</a>";
      std::string headings;
      for (const auto& heading : context.outlines[index]) {
        if (heading.level != 2 && heading.level != 3) continue;
        headings += "<li><a href=\"" + escapeHtml(relativeUrl(from_dir, page.output)) + "#" + escapeHtml(encodePathSegment(heading.id)) + "\">" +
                    escapeHtml(heading.text) + "</a></li>";
      }
      if (!headings.empty()) entry += "<ul class=\"index-headings\">" + headings + "</ul>";
      return entry;
    };
    const std::function<void(const std::vector<DocsNavItem>&)> renderItems = [&](const std::vector<DocsNavItem>& items) {
      body += "<ul>";
      for (const auto& item : items) {
        body += "<li>";
        body += item.page ? pageEntry(*item.page) : escapeHtml(item.title);
        if (!item.children.empty()) renderItems(item.children);
        body += "</li>";
      }
      body += "</ul>";
    };
    for (const auto& tab : model.nav) {
      body += "<h2>" + escapeHtml(tab.title) + "</h2>";
      if (tab.page && tab.children.empty()) body += "<ul><li>" + pageEntry(*tab.page) + "</li></ul>";
      else if (tab.page) body += "<ul><li>" + pageEntry(*tab.page) + "</li></ul>", renderItems(tab.children);
      else renderItems(tab.children);
    }
    if (!model.unlisted.empty()) {
      body += "<h2>Other pages</h2><ul>";
      for (const auto index : model.unlisted) body += "<li>" + pageEntry(index) + "</li>";
      body += "</ul>";
    }
    ShellInput input;
    input.title = "Site index";
    input.output = std::string(kDocsSiteIndexPage);
    input.article = "<div class=\"site-index\">" + body + "</div>";
    writeFile(input.output, renderShell(context, input));
  }

  // Assets, then the marker.
  for (const auto& [output, source] : assets) {
    const auto asset = readFile(model.root / source, kMaximumPagesFileBytes);
    if (!asset) siteError("cannot read '" + source + "'");
    writeFile(output, *asset);
    ++result.assets_copied;
  }
  writeFile(std::string(kDocsSiteMarker), "version=1\ngenerator=ckdocs " + context.version + "\npages=" +
                                              std::to_string(result.pages_written) + "\n");

  // Swap into place; an existing site is kept until the new one is in.
  if (fs::exists(out, error)) {
    const fs::path old = fs::path(temporary.path.string() + ".old");
    fs::rename(out, old, error);
    if (error) siteError("cannot move the previous site aside: " + error.message());
    fs::rename(temporary.path, out, error);
    if (error) {
      std::error_code restore;
      fs::rename(old, out, restore);
      siteError("cannot move the new site into place: " + error.message());
    }
    fs::remove_all(old, error);
  } else {
    fs::create_directories(out.parent_path(), error);
    fs::rename(temporary.path, out, error);
    if (error) siteError("cannot move the new site into place: " + error.message());
  }
  temporary.keep = true;
}

}  // namespace ckgit
