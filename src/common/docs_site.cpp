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

#include "ckgit/ci_workflow.hpp"
#include "ckgit/markdown.hpp"
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

}  // namespace ckgit
