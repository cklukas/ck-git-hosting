// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/docs_site.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
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
#include "ckgit/docs_i18n.hpp"
#include "ckgit/docs_icons.hpp"
#include "ckgit/emoji.hpp"
#include "ckgit/docs_theme.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/http_router.hpp"
#include "ckgit/markdown.hpp"
#include "ckgit/markdown_styles.hpp"
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

// The familiar data-only home layout also lets existing VitePress content
// retain its hero, calls to action and feature cards during a migration.
DocsHome parseHome(const Node& front) {
  DocsHome home;
  const auto text = [](const Node& node, const char* key) {
    // A `|` or `>` block's line breaks become spaces: one line of text.
    std::string value;
    for (const char byte : scalarOf(node, key)) {
      if (byte != '\n') value += byte;
      else if (!value.empty() && value.back() != ' ') value += ' ';
    }
    while (!value.empty() && value.back() == ' ') value.pop_back();
    if (!isValidDocsText(value)) malformed(std::string(key) + " is too long or has control characters", node.line);
    return value;
  };
  if (const Node* hero = findEntry(front, "hero")) {
    requireKind(*hero, Node::Kind::Mapping, "hero to be a mapping");
    rejectUnknownKeys(*hero, {"name", "text", "tagline", "image", "actions"});
    if (const Node* n = findEntry(*hero, "name")) home.name = text(*n, "hero.name");
    if (const Node* n = findEntry(*hero, "text")) home.text = text(*n, "hero.text");
    if (const Node* n = findEntry(*hero, "tagline")) home.tagline = text(*n, "hero.tagline");
    if (const Node* image = findEntry(*hero, "image"); image != nullptr && image->kind == Node::Kind::Scalar) {
      home.image_light = text(*image, "hero.image");  // a path alone: one image for both themes, no alternative text
    } else if (image != nullptr) {
      requireKind(*image, Node::Kind::Mapping, "hero.image to be a path or a mapping");
      rejectUnknownKeys(*image, {"src", "alt", "light", "dark"});
      const auto variant = [&](const Node& node) {
        requireKind(node, Node::Kind::Mapping, "an image variant to be a mapping");
        rejectUnknownKeys(node, {"src", "alt"});
        const Node* src = findEntry(node, "src");
        if (!src) malformed("hero image needs src", node.line);
        if (const Node* alt = findEntry(node, "alt")) home.image_alt = text(*alt, "hero.image.alt");
        return text(*src, "hero.image.src");
      };
      if (findEntry(*image, "src")) {
        if (findEntry(*image, "light") || findEntry(*image, "dark")) malformed("hero.image cannot mix src and variants", image->line);
        home.image_light = variant(*image);
      } else {
        const Node* light = findEntry(*image, "light");
        const Node* dark = findEntry(*image, "dark");
        if (!light) malformed("hero.image needs src or a light variant", image->line);
        home.image_light = variant(*light);
        const std::string light_alt = home.image_alt;
        if (dark) home.image_dark = variant(*dark);
        home.image_alt = light_alt;
        if (const Node* alt = findEntry(*image, "alt")) home.image_alt = text(*alt, "hero.image.alt");
      }
    }
    if (const Node* actions = findEntry(*hero, "actions")) {
      requireKind(*actions, Node::Kind::Sequence, "hero.actions to be a list");
      if (actions->items.size() > 8) malformed("hero.actions has more than 8 entries", actions->line);
      for (const Node& action : actions->items) {
        requireKind(action, Node::Kind::Mapping, "a hero action to be a mapping");
        rejectUnknownKeys(action, {"text", "link", "theme"});
        const Node* label = findEntry(action, "text");
        const Node* link = findEntry(action, "link");
        if (!label || !link) malformed("a hero action needs text and link", action.line);
        DocsHomeAction out{text(*label, "action.text"), text(*link, "action.link"), "alt"};
        if (const Node* theme = findEntry(action, "theme")) out.theme = scalarOf(*theme, "action.theme");
        if (out.theme != "alt" && out.theme != "brand") malformed("action.theme must be brand or alt", action.line);
        home.actions.push_back(std::move(out));
      }
    }
  }
  if (const Node* features = findEntry(front, "features")) {
    requireKind(*features, Node::Kind::Sequence, "features to be a list");
    if (features->items.size() > 16) malformed("features has more than 16 entries", features->line);
    for (const Node& feature : features->items) {
      requireKind(feature, Node::Kind::Mapping, "a feature to be a mapping");
      rejectUnknownKeys(feature, {"title", "details", "icon", "link"});
      const Node* title = findEntry(feature, "title");
      const Node* details = findEntry(feature, "details");
      if (!title || !details) malformed("a feature needs title and details", feature.line);
      DocsHomeFeature out{text(*title, "feature.title"), text(*details, "feature.details"), {}, {}};
      if (const Node* icon = findEntry(feature, "icon")) {
        // A short text or emoji; a `:shortcode:` is spelled out, as in Markdown.
        out.icon = text(*icon, "feature.icon");
        if (out.icon.size() > 64) malformed("feature.icon is longer than 64 bytes", icon->line);
        if (out.icon.size() > 2 && out.icon.front() == ':' && out.icon.back() == ':') {
          if (const auto emoji = emojiForShortcode(std::string_view(out.icon).substr(1, out.icon.size() - 2))) out.icon = *emoji;
        }
      }
      if (const Node* link = findEntry(feature, "link")) out.link = text(*link, "feature.link");
      home.features.push_back(std::move(out));
    }
  }
  return home;
}

// The front matter keys ckdocs reads itself; no project may declare one
// foreign. Several are the names other generators use for the same thing
// (Docusaurus's `sidebar_position`, `hide_table_of_contents`,
// `pagination_prev`; VitePress's `outline`), so pages written for them carry over.
constexpr std::array<std::string_view, 31> kDocsFrontMatterKeys{
    "title",          "description",           "nav_order",
    "nav_exclude",    "author",                "date",
    "slug",           "tags",                  "keywords",
    "layout",         "sidebar",               "sidebar_label",
    "sidebar_position", "toc",                 "hide_table_of_contents",
    "toc_min_heading_level", "toc_max_heading_level", "outline",
    "pager",          "prev",                  "next",
    "pagination_prev", "pagination_next",      "search",
    "draft",          "page_class",            "lang",
    "hide_title",     "hero",                  "features",
    "unlisted"};

// `front_matter: { foreign_keys: [...] }`.
std::vector<std::string> parseForeignFrontMatterKeys(const Node& front_matter) {
  requireKind(front_matter, Node::Kind::Mapping, "front_matter to be a mapping");
  rejectUnknownKeys(front_matter, {"foreign_keys"});
  const Node* list = findEntry(front_matter, "foreign_keys");
  if (list == nullptr) malformed("front_matter needs 'foreign_keys'", front_matter.line);
  requireKind(*list, Node::Kind::Sequence, "front_matter.foreign_keys to be a list");
  if (list->items.empty()) malformed("front_matter.foreign_keys is empty", list->line);
  if (list->items.size() > kMaximumDocsForeignFrontMatterKeys) tooLarge("front_matter.foreign_keys has more than 32 keys");
  std::vector<std::string> keys;
  for (const Node& item : list->items) {
    auto key = scalarOf(item, "each foreign front matter key to be a scalar");
    if (!isValidFrontMatterKey(key)) {
      malformed("foreign front matter key '" + key + "' must be 1 to 64 ASCII letters, digits, '.', '_' or '-'", item.line);
    }
    if (std::find(kDocsFrontMatterKeys.begin(), kDocsFrontMatterKeys.end(), key) != kDocsFrontMatterKeys.end()) {
      malformed("front matter key '" + key + "' is read by ckdocs itself and cannot be foreign", item.line);
    }
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
      malformed("foreign front matter key '" + key + "' is listed twice", item.line);
    }
    keys.push_back(std::move(key));
  }
  return keys;
}

bool booleanOf(const Node& node, const std::string& what) {
  const auto value = scalarOf(node, (what + " to be true or false").c_str());
  if (value != "true" && value != "false") malformed(what + " must be true or false", node.line);
  return value == "true";
}

// `ui: {key: text, ...}`: interface text overrides, checked against the keys
// DocsUiText has.
std::vector<std::pair<std::string, std::string>> parseUiText(const Node& mapping, const std::string& where) {
  requireKind(mapping, Node::Kind::Mapping, (where + " to be a mapping of interface texts").c_str());
  std::vector<std::pair<std::string, std::string>> texts;
  const auto& keys = docsUiKeys();
  for (const auto& [key, value] : mapping.entries) {
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
      malformed(where + " has no interface text '" + key + "'", value.line);
    }
    const auto text = scalarOf(value, "an interface text to be a scalar");
    if (text.empty() || !isValidDocsText(text)) malformed(where + "." + key + " is empty, longer than 1024 bytes, or has control characters", value.line);
    texts.emplace_back(key, text);
  }
  return texts;
}

std::string languageOf(const Node& node, const char* what) {
  const auto value = scalarOf(node, what);
  if (!isValidDocsLanguageTag(value)) {
    malformed(std::string(what) + " '" + value + "' is not a language tag such as en, de or pt-BR", node.line);
  }
  return value;
}

// `i18n: {label, locales: [...]}`.
void parseI18n(const Node& i18n, DocsConfig& config) {
  requireKind(i18n, Node::Kind::Mapping, "i18n to be a mapping");
  rejectUnknownKeys(i18n, {"label", "locales"});
  if (const Node* label = findEntry(i18n, "label")) config.language_label = titleOf(*label, "i18n.label");
  const Node* locales = findEntry(i18n, "locales");
  if (locales == nullptr) malformed("i18n needs 'locales'", i18n.line);
  requireKind(*locales, Node::Kind::Sequence, "i18n.locales to be a list");
  if (locales->items.empty()) malformed("i18n.locales is empty", locales->line);
  if (locales->items.size() > kMaximumDocsLocales) tooLarge("i18n.locales has more than 16 languages");
  for (const Node& item : locales->items) {
    requireKind(item, Node::Kind::Mapping, "each locale to be a mapping with 'lang'");
    rejectUnknownKeys(item, {"lang", "label", "source", "title", "description", "nav", "nav_titles", "fallback", "ui"});
    DocsLocaleEntry locale;
    const Node* lang = findEntry(item, "lang");
    if (lang == nullptr) malformed("a locale needs 'lang'", item.line);
    locale.lang = languageOf(*lang, "a locale's lang");
    for (const auto& existing : config.locales) {
      if (lowerAscii(existing.lang) == lowerAscii(locale.lang)) malformed("locale '" + locale.lang + "' is listed twice", lang->line);
    }
    if (const Node* label = findEntry(item, "label")) locale.label = titleOf(*label, "a locale's label");
    if (const Node* source = findEntry(item, "source")) locale.source = pathOf(*source, "a locale's source");
    if (const Node* title = findEntry(item, "title")) locale.title = titleOf(*title, "a locale's title");
    if (const Node* description = findEntry(item, "description")) {
      locale.description = scalarOf(*description, "a locale's description");
      if (!isValidDocsText(locale.description)) malformed("a locale's description is longer than 1024 bytes or has control characters", description->line);
    }
    if (const Node* nav = findEntry(item, "nav")) {
      std::size_t count = 0;
      parseNavEntries(*nav, locale.nav, 1, count);
    }
    if (const Node* titles = findEntry(item, "nav_titles")) {
      requireKind(*titles, Node::Kind::Mapping, "nav_titles to be a mapping from a title to its translation");
      for (const auto& [from, to] : titles->entries) locale.nav_titles.emplace(from, titleOf(to, "a translated nav title"));
    }
    if (const Node* fallback = findEntry(item, "fallback")) locale.fallback = booleanOf(*fallback, "fallback");
    if (const Node* ui = findEntry(item, "ui")) locale.ui = parseUiText(*ui, "locale '" + locale.lang + "' ui");
    config.locales.push_back(std::move(locale));
  }
}

// `toc: {min_level, max_level}`.
void parseToc(const Node& toc, DocsConfig& config) {
  requireKind(toc, Node::Kind::Mapping, "toc to be a mapping");
  rejectUnknownKeys(toc, {"min_level", "max_level"});
  for (const auto* key : {"min_level", "max_level"}) {
    const Node* level = findEntry(toc, key);
    if (level == nullptr) continue;
    const auto text = scalarOf(*level, "a heading level to be a number");
    if (text.size() != 1 || text[0] < '1' || text[0] > '6') malformed(std::string("toc.") + key + " must be a heading level from 1 to 6", level->line);
    (std::string_view(key) == "min_level" ? config.toc_min_level : config.toc_max_level) = text[0] - '0';
  }
  if (config.toc_min_level > config.toc_max_level) malformed("toc.min_level is greater than toc.max_level", toc.line);
}

// A portal site's id: its output directory and the name it is reported under.
bool isValidSiteId(std::string_view id) {
  if (id.empty() || id.size() > kMaximumDocsSiteIdBytes || id.front() == '-') return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-';
  });
}

// A site's Git ref: a branch, tag, or commit id, held to the same rule as a
// CI sister's ref -- never a git option (no leading '-'), never outside the
// object namespace (no '..', no leading '/').
bool isValidSiteRef(std::string_view ref) {
  if (ref.empty() || ref.size() > 255 || ref.front() == '-' || ref.front() == '/' || ref.find("..") != std::string_view::npos) {
    return false;
  }
  return std::all_of(ref.begin(), ref.end(), [](unsigned char byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '.' ||
           byte == '_' || byte == '-' || byte == '/';
  });
}

// `sites: [...]`, a portal's documentation sites.
std::vector<DocsSiteEntry> parseSiteEntries(const Node& sequence) {
  requireKind(sequence, Node::Kind::Sequence, "sites to be a list");
  if (sequence.items.empty()) malformed("sites is empty", sequence.line);
  if (sequence.items.size() > kMaximumDocsSites) tooLarge("sites has more than 8 entries");
  std::vector<DocsSiteEntry> entries;
  bool redirecting = false;
  for (const Node& item : sequence.items) {
    requireKind(item, Node::Kind::Mapping, "each site to be a mapping with 'id' and 'title'");
    rejectUnknownKeys(item, {"id", "title", "description", "config", "ref", "redirect_from_root"});
    const Node* id = findEntry(item, "id");
    const Node* title = findEntry(item, "title");
    if (id == nullptr || title == nullptr) malformed("a site needs both 'id' and 'title'", item.line);
    DocsSiteEntry entry;
    entry.id = scalarOf(*id, "a site id to be a scalar");
    if (!isValidSiteId(entry.id)) {
      malformed("site id '" + entry.id + "' must be 1 to 32 lowercase letters, digits or '-', not starting with '-'", id->line);
    }
    for (const auto& existing : entries) {
      if (existing.id == entry.id) malformed("site id '" + entry.id + "' is listed twice", id->line);
    }
    entry.title = titleOf(*title, "a site title");
    if (const Node* description = findEntry(item, "description")) {
      entry.description = scalarOf(*description, "a site description to be a scalar");
      if (!isValidDocsText(entry.description)) {
        malformed("a site description is longer than 1024 bytes or has control characters", description->line);
      }
    }
    if (const Node* config = findEntry(item, "config")) entry.config = pathOf(*config, "a site config");
    if (const Node* ref = findEntry(item, "ref")) {
      entry.ref = scalarOf(*ref, "a site ref to be a scalar");
      if (!isValidSiteRef(entry.ref)) malformed("invalid site ref '" + entry.ref + "'", ref->line);
    }
    if (const Node* redirect = findEntry(item, "redirect_from_root")) {
      const auto value = scalarOf(*redirect, "redirect_from_root to be true or false");
      if (value != "true" && value != "false") malformed("redirect_from_root must be true or false", redirect->line);
      entry.redirect_from_root = value == "true";
      if (entry.redirect_from_root && redirecting) malformed("only one site may set redirect_from_root", redirect->line);
      redirecting = redirecting || entry.redirect_from_root;
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

// A strict ISO 8601 calendar date, YYYY-MM-DD, that exists in the proleptic
// Gregorian calendar.
bool isCalendarDate(std::string_view value) {
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
  const auto digits = [value](std::size_t at, std::size_t length) -> std::optional<int> {
    int result = 0;
    for (const char byte : value.substr(at, length)) {
      if (byte < '0' || byte > '9') return std::nullopt;
      result = result * 10 + (byte - '0');
    }
    return result;
  };
  const auto year = digits(0, 4);
  const auto month = digits(5, 2);
  const auto day = digits(8, 2);
  if (!year || !month || !day || *month < 1 || *month > 12 || *day < 1) return false;
  constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (*year % 4 == 0 && *year % 100 != 0) || *year % 400 == 0;
  return *day <= kDays[static_cast<std::size_t>(*month - 1)] + (*month == 2 && leap ? 1 : 0);
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

bool hasPartialComponent(std::string_view path) {
  for (const auto part : splitPath(path)) {
    if (!part.empty() && part.front() == '_') return true;
  }
  return false;
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
    context.resolver = [](std::string_view target, bool, std::size_t) -> std::optional<std::string> {
      return std::string(target);
    };
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

// A page's title in navigation: its `sidebar_label`, else its title.
const std::string& navTitle(const DocsPage& page) { return page.nav_title.empty() ? page.title : page.nav_title; }

DocsNavItem convertNode(const DirNode& node, const std::vector<DocsPage>& pages, std::size_t depth) {
  DocsNavItem item;
  item.title = node.index_page ? navTitle(pages[*node.index_page]) : docsTitleFromFilename(node.name);
  item.page = node.index_page;
  struct Child {
    OrderKey key;
    DocsNavItem item;
  };
  std::vector<Child> children;
  for (const auto index : node.pages) {
    children.push_back({keyFor(pages[index], std::string(basenameOf(pages[index].source))),
                        DocsNavItem{navTitle(pages[index]), index, {}}});
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
                const std::map<std::string, std::string>& skipped, std::vector<bool>& mentioned) {
  for (const auto& entry : entries) {
    DocsNavItem item;
    if (!entry.page.empty()) {
      if (skipped.count(entry.page) != 0) continue;  // already reported as left out
      const auto found = by_source.find(entry.page);
      if (found == by_source.end()) siteError("nav names '" + entry.page + "', which is not a page of the site");
      item.page = found->second;
      item.title = entry.title.empty() ? navTitle(pages[found->second]) : entry.title;
      mentioned[found->second] = true;
    } else {
      item.title = entry.title;
      resolveNav(entry.pages, item.children, by_source, pages, skipped, mentioned);
      if (item.children.empty() && !entry.pages.empty()) continue;  // every page in it was left out
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
  const Node* sites = findEntry(root, "sites");
  if (sites != nullptr) {
    // A portal: the page-tree keys describe one site, so they live in that
    // site's own config instead.
    for (const auto* key : {"source", "nav", "search", "exclude", "front_matter", "toc", "markdown", "i18n"}) {
      if (const Node* misplaced = findEntry(root, key)) {
        malformed(std::string("'") + key + "' belongs to a site's own config when 'sites' is given", misplaced->line);
      }
    }
  }
  rejectUnknownKeys(root, {"version", "site", "source", "home", "exclude", "nav", "search", "llms", "front_matter", "sites", "i18n",
                           "ui", "toc", "markdown"});
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
  rejectUnknownKeys(*site, {"title", "brand", "logo", "favicon", "description", "footer", "links", "stylesheet", "url", "lang"});
  if (const Node* lang = findEntry(*site, "lang")) config.lang = languageOf(*lang, "site.lang");
  if (const Node* url = findEntry(*site, "url")) {
    config.url = scalarOf(*url, "site.url");
    const auto lowered = lowerAscii(config.url.substr(0, 8));
    if (!isValidLinkUrl(config.url) || (!lowered.starts_with("http://") && !lowered.starts_with("https://")) ||
        config.url.find_first_of("?#") != std::string::npos) {
      malformed("site.url must be an absolute http(s) URL without a query or fragment", url->line);
    }
    if (config.url.back() != '/') config.url += '/';
  }
  const Node* title = findEntry(*site, "title");
  if (title == nullptr) malformed("site needs a 'title'", site->line);
  config.title = titleOf(*title, "site.title");
  if (const Node* brand = findEntry(*site, "brand")) config.brand = titleOf(*brand, "site.brand");
  if (const Node* logo = findEntry(*site, "logo")) config.logo = pathOf(*logo, "site.logo");
  if (const Node* favicon = findEntry(*site, "favicon")) config.favicon = pathOf(*favicon, "site.favicon");
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
      rejectUnknownKeys(item, {"title", "url", "icon"});
      const Node* link_title = findEntry(item, "title");
      const Node* url = findEntry(item, "url");
      if (link_title == nullptr || url == nullptr) malformed("a link needs both 'title' and 'url'", item.line);
      DocsLink link;
      link.title = titleOf(*link_title, "a link title");
      link.url = scalarOf(*url, "a link url");
      if (!isValidLinkUrl(link.url)) malformed("link url '" + link.url + "' must be http(s) or mailto", url->line);
      if (const Node* icon = findEntry(item, "icon")) {
        link.icon = scalarOf(*icon, "a link icon");
        if (link.icon != "github") malformed("link icon must be github", icon->line);
      }
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
  if (const Node* llms = findEntry(root, "llms")) {
    const auto value = scalarOf(*llms, "llms to be true or false");
    if (value != "true" && value != "false") malformed("llms must be true or false", llms->line);
    config.llms = value == "true";
  }
  if (const Node* front_matter = findEntry(root, "front_matter")) {
    config.foreign_front_matter_keys = parseForeignFrontMatterKeys(*front_matter);
  }
  if (const Node* ui = findEntry(root, "ui")) config.ui = parseUiText(*ui, "ui");
  if (const Node* toc = findEntry(root, "toc")) parseToc(*toc, config);
  if (const Node* markdown = findEntry(root, "markdown")) {
    requireKind(*markdown, Node::Kind::Mapping, "markdown to be a mapping");
    rejectUnknownKeys(*markdown, {"line_numbers"});
    if (const Node* numbers = findEntry(*markdown, "line_numbers")) config.line_numbers = booleanOf(*numbers, "markdown.line_numbers");
  }
  if (const Node* i18n = findEntry(root, "i18n")) {
    parseI18n(*i18n, config);
    for (const auto& locale : config.locales) {
      if (lowerAscii(locale.lang) == lowerAscii(config.lang.empty() ? "en" : config.lang)) {
        malformed("locale '" + locale.lang + "' is the site's own language (site.lang)", i18n->line);
      }
    }
  }
  if (sites != nullptr) config.sites = parseSiteEntries(*sites);
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

namespace {

using Warn = std::function<void(const std::string&)>;

// A front matter `slug`: a path of letters, digits, `.`, `_`, `~` and `-`
// components, optionally rooted with `/` or ending in `/`.
bool isValidSlug(std::string_view slug) {
  if (slug.empty() || slug.size() > 256) return false;
  if (slug == "/") return true;
  const auto body = slug.substr(slug.front() == '/' ? 1 : 0);
  if (body.empty()) return false;
  for (const auto part : splitPath(body.back() == '/' ? body.substr(0, body.size() - 1) : body)) {
    if (part.empty() || part == "." || part == ".." || part.front() == '.') return false;
    if (!std::all_of(part.begin(), part.end(), [](unsigned char byte) {
          return std::isalnum(byte) != 0 || byte == '.' || byte == '_' || byte == '~' || byte == '-';
        })) {
      return false;
    }
  }
  return body.find("//") == std::string_view::npos;
}

// The output path a slug gives a page whose own output directory is `directory`.
std::string slugOutput(std::string_view slug, std::string_view directory) {
  std::string path;
  if (slug.front() == '/') path = std::string(slug.substr(1));
  else path = directory.empty() ? std::string(slug) : std::string(directory) + "/" + std::string(slug);
  if (path.empty() || path.back() == '/') return path + "index.html";
  if (lowerAscii(path).ends_with(".html")) return path;
  return path + ".html";
}

// Reads one page's front matter into `page`. A value ckdocs cannot use is
// reported and ignored; `draft` receives `draft: true`.
class FrontMatterReader {
 public:
  FrontMatterReader(DocsPage& page, const DocsConfig& config, const Warn& warn, const std::string& file)
      : page_(page), config_(config), warn_(warn), file_(file) {}

  void read(const YamlNode& data, bool& draft) {
    for (const auto& [key, value] : data.entries) apply(key, value, draft);
    if (page_.toc_min_level > page_.toc_max_level) {
      problem("toc_min_heading_level is greater than toc_max_heading_level; the site's levels are used");
      page_.toc_min_level = config_.toc_min_level;
      page_.toc_max_level = config_.toc_max_level;
    }
  }

 private:
  DocsPage& page_;
  const DocsConfig& config_;
  const Warn& warn_;
  const std::string& file_;

  void problem(const std::string& message) { warn_(file_ + ": " + message); }

  std::optional<std::string> scalar(const std::string& key, const YamlNode& value) {
    if (value.kind == YamlNode::Kind::Scalar) return value.scalar;
    problem("front matter " + key + " must be a single value; ignored");
    return std::nullopt;
  }

  std::optional<bool> boolean(const std::string& key, const YamlNode& value) {
    const auto text = scalar(key, value);
    if (!text) return std::nullopt;
    if (*text == "true" || *text == "false") return *text == "true";
    problem(key + " '" + *text + "' is not true or false; ignored");
    return std::nullopt;
  }

  std::optional<int> level(const std::string& key, const std::string& text) {
    if (text.size() == 1 && text[0] >= '1' && text[0] <= '6') return text[0] - '0';
    problem(key + " '" + text + "' is not a heading level from 1 to 6; ignored");
    return std::nullopt;
  }

  // A one-line text. A `|` or `>` block's line breaks become spaces, so a
  // long description may be wrapped in the file.
  std::optional<std::string> text(const std::string& key, const YamlNode& value, std::size_t limit, bool required) {
    auto result = scalar(key, value);
    if (!result) return std::nullopt;
    if (result->find('\n') != std::string::npos) {
      std::string joined;
      for (const char byte : *result) {
        if (byte == '\n') {
          if (!joined.empty() && joined.back() != ' ') joined += ' ';
        } else {
          joined += byte;
        }
      }
      while (!joined.empty() && joined.back() == ' ') joined.pop_back();
      result = std::move(joined);
    }
    if ((required && result->empty()) || result->size() > limit || !isValidUtf8(*result) || !printable(*result)) {
      problem("front matter " + key + (required ? " is empty, too long, or has control characters; ignored"
                                                 : " is too long or has control characters; ignored"));
      return std::nullopt;
    }
    return result;
  }

  // A list of short labels: a sequence, or one value.
  std::vector<std::string> labels(const std::string& key, const YamlNode& value) {
    std::vector<const YamlNode*> items;
    if (value.kind == YamlNode::Kind::Sequence) {
      for (const auto& item : value.items) items.push_back(&item);
    } else if (value.kind == YamlNode::Kind::Scalar) {
      items.push_back(&value);
    } else {
      problem("front matter " + key + " must be a list; ignored");
      return {};
    }
    std::vector<std::string> result;
    for (const YamlNode* item : items) {
      if (item->kind != YamlNode::Kind::Scalar || item->scalar.empty() || item->scalar.size() > kMaximumDocsTagBytes ||
          !isValidUtf8(item->scalar) || !printable(item->scalar)) {
        problem("a " + key + " entry is not a single line of 1 to 64 bytes; ignored");
        continue;
      }
      if (result.size() >= kMaximumDocsPageTags) {
        problem(key + " has more than 32 entries; the rest are ignored");
        break;
      }
      if (std::find(result.begin(), result.end(), item->scalar) == result.end()) result.push_back(item->scalar);
    }
    return result;
  }

  void pagerLink(const std::string& key, const YamlNode& value, DocsPagerLink& link) {
    if (value.kind == YamlNode::Kind::Scalar) {
      if (value.scalar == "false" || value.scalar == "null" || value.scalar == "~" || value.scalar.empty()) {
        link.hidden = true;
      } else if (isSafeLinkTarget(value.scalar)) {
        link.target = value.scalar;
      } else {
        problem(key + " '" + value.scalar + "' is not a page path; ignored");
      }
      return;
    }
    if (value.kind != YamlNode::Kind::Mapping) {
      problem(key + " must be false, a page, or {text, link}; ignored");
      return;
    }
    for (const auto& [name, item] : value.entries) {
      if (name == "text") {
        if (auto label = text(key + ".text", item, kMaximumDocsTitleBytes, true)) link.text = *label;
      } else if (name == "link") {
        if (item.kind == YamlNode::Kind::Scalar && isSafeLinkTarget(item.scalar)) link.target = item.scalar;
        else problem(key + ".link is not a page path; ignored");
      } else {
        problem(key + " has no '" + name + "'; ignored");
      }
    }
  }

  static bool isSafeLinkTarget(std::string_view target) {
    return !target.empty() && target.size() <= kMaximumDocsUrlBytes && printable(target) &&
           target.find_first_of(" \"<>\\") == std::string_view::npos;
  }

  void apply(const std::string& key, const YamlNode& value, bool& draft) {
    if (key == "title") {
      if (auto line = text(key, value, kMaximumDocsTitleBytes, true)) page_.title = *line;
    } else if (key == "description") {
      if (auto line = text(key, value, kMaximumDocsTextBytes, false)) page_.description = *line;
    } else if (key == "author") {
      if (auto line = text(key, value, kMaximumDocsTitleBytes, true)) page_.author = *line;
    } else if (key == "sidebar_label") {
      if (auto line = text(key, value, kMaximumDocsTitleBytes, true)) page_.nav_title = *line;
    } else if (key == "nav_order" || key == "sidebar_position") {
      if (const auto number = scalar(key, value)) {
        int order = 0;
        const auto [end, parse_error] = std::from_chars(number->data(), number->data() + number->size(), order);
        if (parse_error == std::errc{} && end == number->data() + number->size()) page_.nav_order = order;
        else problem(key + " '" + *number + "' is not an integer; ignored");
      }
    } else if (key == "nav_exclude") {
      if (const auto flag = boolean(key, value)) page_.nav_exclude = *flag;
    } else if (key == "unlisted") {
      if (const auto flag = boolean(key, value); flag && *flag) {
        page_.nav_exclude = true;
        page_.search = false;
      }
    } else if (key == "date") {
      if (const auto day = scalar(key, value)) {
        if (isCalendarDate(*day)) page_.date = *day;
        else problem("date '" + *day + "' is not a YYYY-MM-DD calendar date; ignored");
      }
    } else if (key == "slug") {
      if (const auto slug = scalar(key, value)) {
        if (isValidSlug(*slug)) page_.slug = *slug;
        else problem("slug '" + *slug + "' must be a path of letters, digits, '.', '_', '~' and '-'; ignored");
      }
    } else if (key == "tags") {
      page_.tags = labels(key, value);
    } else if (key == "keywords") {
      page_.keywords = labels(key, value);
    } else if (key == "layout") {
      if (const auto layout = scalar(key, value)) {
        if (*layout == "doc" || *layout == "page" || *layout == "home") page_.layout = *layout;
        else problem("layout '" + *layout + "' is not doc, page or home; ignored");
      }
    } else if (key == "sidebar") {
      if (const auto flag = boolean(key, value)) page_.sidebar = *flag;
    } else if (key == "toc") {
      if (const auto flag = boolean(key, value)) page_.toc = *flag;
    } else if (key == "hide_table_of_contents") {
      if (const auto flag = boolean(key, value)) page_.toc = !*flag;
    } else if (key == "toc_min_heading_level" || key == "toc_max_heading_level") {
      if (const auto number = scalar(key, value)) {
        if (const auto found = level(key, *number)) (key == "toc_min_heading_level" ? page_.toc_min_level : page_.toc_max_level) = *found;
      }
    } else if (key == "outline") {
      // VitePress's form: false, one level, [min, max], or deep (2 to 6).
      if (value.kind == YamlNode::Kind::Sequence && value.items.size() == 2 && value.items[0].kind == YamlNode::Kind::Scalar &&
          value.items[1].kind == YamlNode::Kind::Scalar) {
        const auto first = level(key, value.items[0].scalar);
        const auto last = level(key, value.items[1].scalar);
        if (first && last) {
          page_.toc_min_level = *first;
          page_.toc_max_level = *last;
        }
      } else if (const auto setting = scalar(key, value)) {
        if (*setting == "false") page_.toc = false;
        else if (*setting == "deep") page_.toc_min_level = 2, page_.toc_max_level = 6;
        else if (const auto found = level(key, *setting)) page_.toc_min_level = page_.toc_max_level = *found;
      }
    } else if (key == "pager") {
      if (const auto flag = boolean(key, value)) page_.pager = *flag;
    } else if (key == "prev" || key == "pagination_prev") {
      pagerLink(key, value, page_.prev);
    } else if (key == "next" || key == "pagination_next") {
      pagerLink(key, value, page_.next);
    } else if (key == "search") {
      if (const auto flag = boolean(key, value)) page_.search = *flag;
    } else if (key == "draft") {
      if (const auto flag = boolean(key, value)) draft = *flag;
    } else if (key == "hide_title") {
      if (const auto flag = boolean(key, value)) page_.hide_title = *flag;
    } else if (key == "page_class") {
      if (const auto classes = scalar(key, value)) {
        const bool valid = !classes->empty() && classes->size() <= 128 &&
                           std::all_of(classes->begin(), classes->end(), [](unsigned char byte) {
                             return std::isalnum(byte) != 0 || byte == '-' || byte == '_' || byte == ' ';
                           });
        if (valid) page_.page_class = *classes;
        else problem("page_class must be class names of letters, digits, '-' and '_'; ignored");
      }
    } else if (key == "lang") {
      if (const auto lang = scalar(key, value)) {
        if (isValidDocsLanguageTag(*lang)) page_.lang = *lang;
        else problem("lang '" + *lang + "' is not a language tag such as en, de or pt-BR; ignored");
      }
    } else if (key == "hero" || key == "features") {
      // Read by parseHome when the page's layout is `home`.
    } else if (std::find(config_.foreign_front_matter_keys.begin(), config_.foreign_front_matter_keys.end(), key) !=
               config_.foreign_front_matter_keys.end()) {
      // Another tool's key: accepted silently, and carried when it fits a meta tag.
      if (value.kind == YamlNode::Kind::Scalar && isValidDocsText(value.scalar)) page_.foreign_front_matter.emplace_back(key, value.scalar);
    } else {
      problem("unknown front matter key '" + key + "' ignored; list it under front_matter.foreign_keys in ckdocs.yml if another tool reads it");
    }
  }
};

// The page tree a config names: `source`, else `docs/` when it exists, else the root.
std::string effectiveSource(const fs::path& root, const DocsConfig& config) {
  std::error_code error;
  if (!config.source.empty()) {
    std::string source = config.source;
    while (source.size() > 1 && source.back() == '/') source.pop_back();
    return source;
  }
  return fs::is_directory(root / "docs", error) ? std::string("docs") : std::string();
}

std::string joinPath(std::string_view directory, std::string_view name) {
  if (directory.empty()) return std::string(name);
  if (name.empty()) return std::string(directory);
  return std::string(directory) + "/" + std::string(name);
}

// Where a page sits in its tree: its source relative to `source`, "" for the home.
std::string treeRelative(const std::string& file, const std::string& source) {
  if (source.empty()) return file;
  if (file.size() > source.size() && file.starts_with(source) && file[source.size()] == '/') return file.substr(source.size() + 1);
  return file;
}

// One language's site: the default language's (`original` null) or a
// translation of it (`locale` set), which takes over the pages it lacks
// from `original` unless its fallback is off.
DocsSiteModel loadTree(const fs::path& root, const DocsConfig& config, std::vector<std::string>* warnings,
                       const DocsSiteModel* original, const DocsLocaleEntry* locale) {
  const Warn warn = [warnings](const std::string& message) {
    if (warnings != nullptr) warnings->push_back(message);
  };
  DocsSiteModel model;
  model.root = root;
  model.config = config;
  std::error_code error;

  // The page tree.
  if (!config.source.empty() && !fs::is_directory(root / config.source, error)) {
    siteError("source directory '" + config.source + "' does not exist");
  }
  model.source = effectiveSource(root, config);
  model.title = config.title.empty() ? fs::absolute(root).lexically_normal().filename().string() : config.title;
  if (model.title.empty() || model.title == "/" || model.title == ".") model.title = "Documentation";
  model.lang = config.lang.empty() ? std::string("en") : config.lang;
  model.language_label = locale != nullptr ? locale->label : config.language_label;
  if (model.language_label.empty()) model.language_label = docsLanguageName(model.lang);
  model.ui = docsUiTextFor(model.lang);
  for (const auto& [key, text] : config.ui) setDocsUiText(model.ui, key, text);

  // Candidate files: tracked ones inside a work tree, a walk otherwise.
  const bool work_tree = fs::exists(root / ".git", error);
  auto files = work_tree ? trackedFiles(root, model.source) : walkedFiles(root, model.source);
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());

  std::vector<std::string> candidates;
  for (const auto& file : files) {
    if (!hasMarkdownExtension(file) || hasDotComponent(file) || !isSafeRelativePath(file)) continue;
    if (excluded(config.exclude, file, model.source)) continue;
    // A file or directory whose name starts with `_` holds partials: parts
    // pages include, never pages of their own.
    if (hasPartialComponent(treeRelative(file, model.source))) continue;
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

  const auto has = [&candidates](std::string_view file) {
    return std::find(candidates.begin(), candidates.end(), file) != candidates.end();
  };
  // The home page: configured; else the source tree's own README/index (a
  // documentation tree's landing page); else the repository's README -- but
  // a translation never takes the repository's README, which is in the
  // default language; it falls back to the default home instead.
  std::string home;
  if (!config.home.empty()) {
    if (!has(config.home)) {
      siteError("home page '" + config.home + "' is not a page of the site (missing, excluded, or outside the source)");
    }
    home = config.home;
  } else {
    for (const auto* name : {"README.md", "index.md"}) {
      const auto candidate = joinPath(model.source, name);
      if (has(candidate)) {
        home = candidate;
        break;
      }
    }
    if (home.empty() && !model.source.empty() && original == nullptr && has("README.md")) home = "README.md";
  }
  const bool root_outside_source = !model.source.empty();
  std::vector<std::string> selected;
  for (const auto& file : candidates) {
    const bool inside = !root_outside_source || file.starts_with(model.source + "/");
    if (inside || file == home) selected.push_back(file);
  }
  if (selected.empty() && original == nullptr) {
    siteError("no Markdown pages found under '" + (model.source.empty() ? std::string(".") : model.source) + "'");
  }
  if (selected.size() > kMaximumDocsPages) siteError("the site has more than 4096 pages");
  if (home.empty() && original == nullptr) home = selected.front();

  // Each page's metadata and output path.
  std::map<std::string, std::size_t> by_output;
  const auto place = [&](DocsPage page) {
    const auto [existing, inserted] = by_output.try_emplace(page.output, model.pages.size());
    if (!inserted) {
      siteError("'" + model.pages[existing->second].source + "' and '" + page.source + "' would both be written to '" +
                page.output + "'; exclude or rename one of them");
    }
    model.pages.push_back(std::move(page));
  };
  for (const auto& file : selected) {
    DocsPage page;
    page.source = file;
    page.home = file == home;
    page.toc_min_level = config.toc_min_level;
    page.toc_max_level = config.toc_max_level;
    const auto content = readFile(root / file, kMaximumMarkdownInputBytes);
    std::string unusable;
    if (!content) {
      unusable = fs::file_size(root / file, error) > kMaximumMarkdownInputBytes ? "exceeds the 512 KiB page limit"
                                                                                : "cannot be read";
    } else if (!isValidUtf8(*content)) {
      unusable = "is not valid UTF-8";
    }
    if (!unusable.empty()) {
      // One such file must not stop the whole site; the home page must exist.
      if (page.home) siteError("the home page '" + file + "' " + unusable);
      warn(file + ": " + unusable + "; left out of the site (split it, or list it under exclude in ckdocs.yml)");
      model.skipped.emplace(file, unusable);
      continue;
    }
    std::string body = *content;
    bool draft = false;
    if (const auto front = splitFrontMatter(*content)) {
      body = content->substr(front->body_offset);
      if (!front->error.empty()) {
        page.front_matter_error = front->error;
        warn(file + ": " + front->error);
      }
      FrontMatterReader(page, config, warn, file).read(front->structured, draft);
      // The landing page's banner and cards: on the home page only.
      if (front->error.empty() && page.layout == "home") {
        try {
          if (!page.home) throw std::runtime_error("layout: home is only valid on the home page");
          page.landing = parseHome(front->structured);
          if (page.title.empty() && !page.landing->name.empty()) page.title = page.landing->name;
          if (page.description.empty()) page.description = page.landing->tagline;
        } catch (const std::exception& failure) {
          std::string message = failure.what();
          if (message.starts_with("ckdocs.yml: ")) message.erase(0, 12);
          warn(file + ": landing page: " + message);
          page.layout = "doc";
        }
      }
    }
    if (draft && !config.drafts) {
      if (page.home) siteError("the home page '" + file + "' is a draft (draft: true)");
      model.skipped.emplace(file, "is a draft (draft: true)");
      continue;
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
    page.counterpart = page.home ? std::string() : treeRelative(file, model.source);
    if (original != nullptr) page.alias = page.home ? std::string() : joinPath(original->source, page.counterpart);
    if (page.home) {
      page.output = "index.html";
    } else {
      const auto& relative = page.counterpart;
      const auto directory = directoryOf(relative);
      const auto base = basenameOf(relative);
      const std::string prefix = directory.empty() ? std::string() : std::string(directory) + "/";
      page.output = !page.slug.empty() ? slugOutput(page.slug, directory)
                    : isIndexName(base) ? prefix + "index.html"
                                        : prefix + std::string(stemOf(base)) + ".html";
      if (!page.slug.empty() && page.output == "index.html") {
        siteError("'" + file + "' would replace the home page: its slug names the site root");
      }
    }
    place(std::move(page));
  }

  // A translation takes over every page it does not have from the default
  // language, at the same place, marked so the reader learns why.
  if (original != nullptr && locale != nullptr && locale->fallback) {
    std::set<std::string> translated;
    for (const auto& page : model.pages) translated.insert(page.counterpart);
    for (const auto& page : original->pages) {
      if (translated.count(page.counterpart) != 0) continue;
      DocsPage stand_in = page;
      stand_in.fallback = true;
      stand_in.alias = page.home ? std::string() : joinPath(model.source, page.counterpart);
      if (by_output.count(stand_in.output) != 0) continue;  // a translated page took its place under a slug
      place(std::move(stand_in));
    }
    for (const auto& [file, reason] : original->skipped) model.skipped.emplace(file, reason);
  }
  if (model.pages.empty()) siteError("no Markdown pages found under '" + (model.source.empty() ? std::string(".") : model.source) + "'");
  bool has_home = false;
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    if (model.pages[index].home) {
      model.home = index;
      has_home = true;
    }
  }
  if (!has_home) siteError("'" + model.source + "' has no README.md or index.md to be its home page, and fallback is off");

  // Pager overrides: `prev` and `next` name pages like a link does.
  std::map<std::string, std::size_t> by_source;
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    by_source.emplace(model.pages[index].source, index);
    if (!model.pages[index].alias.empty()) by_source.emplace(model.pages[index].alias, index);
  }
  for (auto& page : model.pages) {
    for (DocsPagerLink* link : {&page.prev, &page.next}) {
      if (link->target.empty()) continue;
      const auto target = link->target.front() == '/' ? std::optional<std::string>(link->target.substr(1))
                                                      : normalizePath(joinPath(directoryOf(page.source), link->target));
      const auto found = target ? by_source.find(*target) : by_source.end();
      if (found == by_source.end()) warn(page.source + ": prev/next '" + link->target + "' is not a page of the site; ignored");
      else link->page = found->second;
    }
  }

  // Tags, each with its pages in title order.
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    for (const auto& tag : model.pages[index].tags) model.tags[tag].push_back(index);
  }
  for (auto& [tag, pages] : model.tags) {
    std::stable_sort(pages.begin(), pages.end(), [&](std::size_t a, std::size_t b) { return model.pages[a].title < model.pages[b].title; });
  }

  // Navigation.
  std::vector<bool> listed(model.pages.size(), false);
  const auto translateTitles = [&](auto&& self, std::vector<DocsNavItem>& items) -> void {
    if (locale == nullptr || locale->nav_titles.empty()) return;
    for (auto& item : items) {
      if (const auto found = locale->nav_titles.find(item.title); found != locale->nav_titles.end()) item.title = found->second;
      self(self, item.children);
    }
  };
  // A translation without a nav of its own follows the default language's,
  // each page swapped for its translation (or its fallback).
  std::vector<DocsNavEntry> nav = config.nav;
  if (original != nullptr && nav.empty() && !original->config.nav.empty()) {
    const auto map = [&](auto&& self, std::vector<DocsNavEntry>& entries) -> void {
      for (auto& entry : entries) {
        if (!entry.page.empty()) {
          const auto translation = joinPath(model.source, treeRelative(entry.page, original->source));
          const bool is_home = entry.page == original->pages[original->home].source;
          if (is_home) entry.page = model.pages[model.home].source;
          else if (by_source.count(translation) != 0) entry.page = model.pages[by_source[translation]].source;
          else if (by_source.count(entry.page) == 0) entry.page = translation;  // reported below as missing
        }
        self(self, entry.pages);
      }
    };
    nav = original->config.nav;
    map(map, nav);
  }
  if (!nav.empty()) {
    std::map<std::string, std::size_t> nav_sources;
    for (std::size_t index = 0; index < model.pages.size(); ++index) nav_sources.emplace(model.pages[index].source, index);
    resolveNav(nav, model.nav, nav_sources, model.pages, model.skipped, listed);
  } else {
    model.nav.push_back(DocsNavItem{model.ui.home, model.home, {}});
    listed[model.home] = true;
    DirNode tree;
    for (std::size_t index = 0; index < model.pages.size(); ++index) {
      const auto& page = model.pages[index];
      if (page.home || page.nav_exclude) continue;
      DirNode* node = &tree;
      std::string_view rest = page.counterpart;
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
  translateTitles(translateTitles, model.nav);
  for (const auto& tab : model.nav) collectReadingOrder(tab, model.reading_order);
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    if (listed[index]) continue;
    if (!nav.empty() && index != model.home && !model.pages[index].nav_exclude && !model.pages[index].fallback) {
      warn(model.pages[index].source + ": not named by nav; reachable from the site index only");
    }
    model.unlisted.push_back(index);
  }
  return model;
}

}  // namespace

DocsSiteModel loadDocsSite(const fs::path& root, const DocsConfig& config, std::vector<std::string>* warnings) {
  if (!config.sites.empty()) siteError("the config lists 'sites', so it describes a portal of several sites, not one site");
  std::error_code error;
  if (!fs::is_directory(root, error)) siteError("'" + root.string() + "' is not a directory");
  // Each translation's tree stays out of the default language's pages.
  const std::string source = effectiveSource(root, config);
  DocsConfig base = config;
  std::vector<std::string> locale_sources;
  for (const auto& locale : config.locales) {
    locale_sources.push_back(locale.source.empty() ? joinPath(source, locale.lang) : locale.source);
    base.exclude.push_back(locale_sources.back());
  }
  DocsSiteModel model = loadTree(root, base, warnings, nullptr, nullptr);
  for (std::size_t index = 0; index < config.locales.size(); ++index) {
    const auto& locale = config.locales[index];
    const std::string& tree = locale_sources[index];
    if (!fs::is_directory(root / tree, error)) {
      siteError("locale '" + locale.lang + "': source directory '" + tree + "' does not exist");
    }
    for (const auto& page : model.pages) {
      if (page.output.starts_with(locale.lang + "/")) {
        siteError("'" + page.source + "' would be written to '" + page.output + "', inside the '" + locale.lang +
                  "/' directory of that translation; rename the page or move it out of the way");
      }
    }
    DocsConfig translation = config;
    translation.source = tree;
    translation.home.clear();
    translation.nav = locale.nav;
    translation.locales.clear();
    translation.lang = locale.lang;
    translation.ui = locale.ui;
    if (!locale.title.empty()) translation.title = locale.title;
    if (!locale.description.empty()) translation.description = locale.description;
    // Another translation's tree nested inside this one stays out of it too.
    for (std::size_t other = 0; other < locale_sources.size(); ++other) {
      if (other != index) translation.exclude.push_back(locale_sources[other]);
    }
    std::vector<std::string> translation_warnings;
    try {
      model.translations.push_back(loadTree(root, translation, &translation_warnings, &model, &locale));
    } catch (const std::exception& failure) {
      std::string message = failure.what();
      if (message.starts_with("ckdocs: ")) message.erase(0, 8);
      siteError("locale '" + locale.lang + "': " + message);
    }
    if (warnings != nullptr) {
      for (auto& warning : translation_warnings) warnings->push_back(locale.lang + ": " + warning);
    }
  }
  return model;
}

// ---- building ----------------------------------------------------------------

namespace {

// A page's broken links, one finding per target and reason however often the
// page repeats the link, with the source lines it is written on.
class LinkFindings {
 public:
  void add(const std::string& page, const std::string& message, std::size_t line) {
    const auto [at, inserted] = index_.try_emplace({page, message}, findings_.size());
    if (inserted) findings_.push_back({page, message, {}, 0});
    Finding& finding = findings_[at->second];
    ++finding.count;
    if (line != 0 && std::find(finding.lines.begin(), finding.lines.end(), line) == finding.lines.end()) {
      finding.lines.push_back(line);
    }
  }

  // "<page>:<first line>: <message>", plus " (<n> links; also line(s) ...)"
  // for a repeated one, in the order the findings were first met.
  void appendTo(std::vector<std::string>& out) const {
    constexpr std::size_t kListedLines = 8;
    for (const Finding& finding : findings_) {
      std::string text = finding.page;
      if (!finding.lines.empty()) text += ":" + std::to_string(finding.lines.front());
      text += ": " + finding.message;
      if (finding.count > 1) {
        text += " (" + std::to_string(finding.count) + " links";
        if (finding.lines.size() > 1) {
          text += finding.lines.size() == 2 ? "; also line " : "; also lines ";
          for (std::size_t at = 1; at < finding.lines.size() && at <= kListedLines; ++at) {
            text += (at == 1 ? "" : ", ") + std::to_string(finding.lines[at]);
          }
          if (finding.lines.size() > kListedLines + 1) text += ", ...";
        }
        text += ")";
      }
      out.push_back(std::move(text));
    }
  }

 private:
  struct Finding {
    std::string page;
    std::string message;
    std::vector<std::size_t> lines;
    std::size_t count;
  };
  std::vector<Finding> findings_;
  std::map<std::pair<std::string, std::string>, std::size_t> index_;
};

// Merges repeats of the same finding from `from` on into one entry that says
// how often it occurred, keeping first-seen order.
void collapseRepeats(std::vector<std::string>& findings, std::size_t from, std::string_view unit) {
  std::vector<std::string> unique;
  std::vector<std::size_t> counts;
  std::map<std::string, std::size_t> seen;
  for (std::size_t at = from; at < findings.size(); ++at) {
    const auto [found, inserted] = seen.try_emplace(findings[at], unique.size());
    if (inserted) {
      unique.push_back(findings[at]);
      counts.push_back(0);
    }
    ++counts[found->second];
  }
  findings.resize(from);
  for (std::size_t at = 0; at < unique.size(); ++at) {
    findings.push_back(counts[at] > 1 ? unique[at] + " (" + std::to_string(counts[at]) + " " + std::string(unit) + ")"
                                      : unique[at]);
  }
}

// The source line a page's body starts on, after its front matter.
std::size_t bodyFirstLine(std::string_view content) {
  const std::string_view body = markdownBody(content);
  const auto offset = static_cast<std::size_t>(body.data() - content.data());
  return 1 + static_cast<std::size_t>(std::count(content.begin(), content.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
}

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

// Serialize the renderer's bounded, trusted article fragment, never the site
// shell. This keeps the HTML and Markdown views on the same parsing and URL
// resolution rules. Code uses the ordinary renderer (not diagram SVGs).
struct MarkdownElement {
  std::string_view tag, opening, text;
  std::vector<MarkdownElement> children;
};

std::string markdownAttribute(std::string_view opening, std::string_view key) {
  const std::string start = " " + std::string(key) + "=\"";
  const auto at = opening.find(start);
  if (at == std::string_view::npos) return {};
  const auto begin = at + start.size(), end = opening.find('"', begin);
  return end == std::string_view::npos ? std::string() : unescapeHtml(opening.substr(begin, end - begin));
}

std::string markdownText(std::string_view text) {
  std::string out;
  for (const char byte : text) {
    if (std::string_view("\\`*_[]<>|#!&~").find(byte) != std::string_view::npos) out += '\\';
    out += byte;
  }
  return out;
}

std::string markdownDestination(std::string_view href) {
  std::string out;
  for (const char byte : href) {
    if (byte == '<') out += "%3C";
    else if (byte == '>') out += "%3E";
    else if (byte == ' ') out += "%20";
    else if (byte == '\\') out += "%5C";
    else out += byte;
  }
  return out;
}

std::string markdownPage(std::string_view html, const std::map<std::string, std::string>& page_links) {
  if (html.size() > kMaximumMarkdownOutputBytes) siteError("Markdown page exceeds the rendering bound");
  MarkdownElement root;
  std::vector<MarkdownElement*> stack{&root};
  std::size_t nodes = 0;
  for (std::size_t at = 0; at < html.size();) {
    if (++nodes > 131072 || stack.size() > 128) siteError("Markdown page exceeds the structure bound");
    if (html[at] != '<') {
      const auto end = html.find('<', at);
      stack.back()->children.push_back({{}, {}, html.substr(at, end == std::string_view::npos ? html.size() - at : end - at), {}});
      at = end == std::string_view::npos ? html.size() : end;
      continue;
    }
    const auto end = html.find('>', at);
    if (end == std::string_view::npos) siteError("invalid article fragment for Markdown output");
    const auto opening = html.substr(at, end + 1 - at);
    const bool closing = opening.starts_with("</");
    const auto name_at = closing ? 2U : 1U;
    const auto name_end = opening.find_first_of(" />", name_at);
    const auto tag = opening.substr(name_at, name_end - name_at);
    if (closing) {
      if (stack.size() == 1 || stack.back()->tag != tag) siteError("unbalanced article fragment for Markdown output");
      stack.pop_back();
    } else {
      stack.back()->children.push_back({tag, opening, {}, {}});
      if (tag != "br" && tag != "hr" && tag != "img" && tag != "input" && tag != "source")
        stack.push_back(&stack.back()->children.back());
    }
    at = end + 1;
  }
  if (stack.size() != 1) siteError("unclosed article fragment for Markdown output");
  const auto link = [&](const std::string& href) {
    const auto hash = href.find('#');
    const auto found = page_links.find(href.substr(0, hash));
    return found == page_links.end() ? href : found->second + (hash == std::string::npos ? "" : href.substr(hash));
  };
  const auto hasClass = [](const MarkdownElement& element, std::string_view name) {
    const auto classes = markdownAttribute(element.opening, "class");
    for (std::size_t start = 0; start <= classes.size();) {
      const auto space = classes.find(' ', start);
      if (std::string_view(classes).substr(start, space == std::string::npos ? std::string::npos : space - start) == name) return true;
      if (space == std::string::npos) break;
      start = space + 1;
    }
    return false;
  };
  // Text as written: a code line's number is the page's decoration, not code.
  const std::function<std::string(const MarkdownElement&)> plain = [&](const MarkdownElement& element) {
    if (hasClass(element, "line-number")) return std::string();
    std::string out = unescapeHtml(element.text);
    for (const auto& child : element.children) out += plain(child);
    return out;
  };
  // A formula's TeX source, from the annotation MathML carries.
  const std::function<std::string(const MarkdownElement&)> texOf = [&](const MarkdownElement& element) -> std::string {
    if (element.tag == "annotation") return plain(element);
    for (const auto& child : element.children) {
      if (auto tex = texOf(child); !tex.empty()) return tex;
    }
    return {};
  };
  const auto trimText = [](std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
  };
  const auto indent = [](const std::string& text, const std::string& first, const std::string& rest) {
    std::string out = first;
    for (std::size_t at = 0; at < text.size(); ++at) {
      out += text[at];
      if (text[at] == '\n' && at + 1 < text.size()) out += rest;
    }
    return out;
  };
  const auto codeFence = [](const std::string& text, std::size_t minimum) {
    std::size_t longest = 0, run = 0;
    for (const char byte : text) { run = byte == '`' ? run + 1 : 0; longest = std::max(longest, run); }
    return std::string(std::max(minimum, longest + 1), '`');
  };
  const std::function<std::string(const MarkdownElement&)> render = [&](const MarkdownElement& element) -> std::string {
    if (element.tag.empty() && !element.text.empty()) {
      std::string text = unescapeHtml(element.text), collapsed;
      for (const unsigned char byte : text) {
        if (byte == '\r' || byte == '\n' || byte == '\t' || byte == ' ') {
          if (collapsed.empty() || collapsed.back() != ' ') collapsed += ' ';
        } else collapsed += static_cast<char>(byte);
      }
      return markdownText(collapsed);
    }
    const auto& tag = element.tag;
    if (tag == "math") {
      const auto tex = trimText(texOf(element));
      return element.opening.find("display=\"block\"") != std::string_view::npos ? "\n\n$$\n" + tex + "\n$$\n\n" : "$" + tex + "$";
    }
    if (tag == "sup" && hasClass(element, "footnote-ref")) return "[^" + trimText(plain(element)) + "]";
    if (tag == "a" && hasClass(element, "footnote-backref")) return {};
    if (tag == "section" && hasClass(element, "footnotes")) {
      std::string out = "\n\n";
      const std::function<void(const MarkdownElement&)> notes = [&](const MarkdownElement& node) {
        if (node.tag == "li") {
          auto id = markdownAttribute(node.opening, "id");
          if (id.starts_with("fn:")) id.erase(0, 3);
          out += indent(trimText(render(node)), "[^" + id + "]: ", "    ") + "\n";
          return;
        }
        for (const auto& child : node.children) notes(child);
      };
      notes(element);
      return out + "\n";
    }
    if (tag == "dl") {
      std::string out = "\n\n";
      for (const auto& child : element.children) {
        if (child.tag == "dt") out += trimText(render(child)) + "\n";
        else if (child.tag == "dd") out += indent(trimText(render(child)), ": ", "  ") + "\n";
      }
      return out + "\n";
    }
    if (tag == "div" && hasClass(element, "tabbed")) {
      // Tabs read in order: each label in bold, then its panel.
      std::vector<std::string> labels;
      std::string out;
      for (const auto& child : element.children) {
        if (child.tag == "label") labels.push_back(trimText(render(child)));
        if (child.tag != "div") continue;
        std::size_t index = 0;
        for (const auto& panel : child.children) {
          if (panel.tag != "div") continue;
          out += "\n\n**" + (index < labels.size() ? labels[index] : std::to_string(index + 1)) + "**\n\n" + trimText(render(panel));
          ++index;
        }
      }
      return out + "\n\n";
    }
    if (tag == "input" && markdownAttribute(element.opening, "type") == "radio") return {};
    if (tag == "div" && hasClass(element, "code-title")) return "\n\n**" + trimText(plain(element)) + "**\n\n";
    if (tag == "a" && hasClass(element, "home-feature")) {
      std::string title, details;
      for (const auto& child : element.children) {
        if (child.tag == "h2") title = trimText(render(child));
        if (child.tag == "p") details = trimText(render(child));
      }
      return "\n\n## [" + title + "](<" + markdownDestination(link(markdownAttribute(element.opening, "href"))) + ">)\n\n" + details + "\n\n";
    }
    if (tag == "span" && hasClass(element, "home-feature-icon")) return {};
    if (tag == "pre") {
      const std::string text = plain(element), fence = codeFence(text, 3);
      std::string language;
      if (!element.children.empty()) {
        const auto classes = markdownAttribute(element.children.front().opening, "class");
        if (classes.starts_with("language-")) language = classes.substr(9);
      }
      return "\n\n" + fence + language + "\n" + text + (text.ends_with('\n') ? "" : "\n") + fence + "\n\n";
    }
    if (tag == "code") {
      const std::string text = plain(element), fence = codeFence(text, 1);
      const bool pad = text.starts_with('`') || text.ends_with('`') ||
                       (text.starts_with(' ') && text.ends_with(' ') && text.find_first_not_of(' ') != std::string::npos);
      return fence + (pad ? " " : "") + text + (pad ? " " : "") + fence;
    }
    if (tag == "img") {
      return "![" + markdownText(markdownAttribute(element.opening, "alt")) + "](<" +
             markdownDestination(markdownAttribute(element.opening, "src")) + ">)";
    }
    if (tag == "input") return element.opening.find(" checked") != std::string_view::npos ? "[x] " : "[ ] ";
    if (tag == "br") return "  \n";
    if (tag == "hr") return "\n\n---\n\n";
    if (tag == "source") return {};
    if (tag == "ul" || tag == "ol") {
      std::string out = "\n";
      unsigned number = 1;
      const auto start = markdownAttribute(element.opening, "start");
      if (!start.empty()) std::from_chars(start.data(), start.data() + start.size(), number);
      for (const auto& child : element.children) {
        if (child.tag != "li") continue;
        const std::string marker = tag == "ol" ? std::to_string(number++) + ". " : "- ";
        out += indent(trimText(render(child)), marker, std::string(marker.size(), ' ')) + "\n";
      }
      return out + "\n";
    }
    if (tag == "table") {
      std::vector<const MarkdownElement*> rows;
      const std::function<void(const MarkdownElement&)> findRows = [&](const MarkdownElement& node) {
        if (node.tag == "tr") rows.push_back(&node);
        else for (const auto& child : node.children) findRows(child);
      };
      findRows(element);
      std::string out = "\n\n";
      for (std::size_t row = 0; row < rows.size(); ++row) {
        std::size_t cells = 0;
        out += '|';
        for (const auto& cell : rows[row]->children) {
          if (cell.tag != "th" && cell.tag != "td") continue;
          std::string text = trimText(render(cell));
          std::replace(text.begin(), text.end(), '\n', ' ');
          // Pipes inside code spans still need escaping in a GFM table.
          for (std::size_t at = 0; at < text.size(); ++at) if (text[at] == '|' && (at == 0 || text[at - 1] != '\\')) text.insert(at++, "\\");
          out += " " + text + " |";
          ++cells;
        }
        out += '\n';
        if (row == 0) {
          out += '|';
          for (std::size_t cell = 0; cell < cells; ++cell) out += " --- |";
          out += '\n';
        }
      }
      return out + "\n";
    }
    if (tag == "div" && markdownAttribute(element.opening, "class") == "home-actions") {
      std::string out = "\n\n";
      for (const auto& child : element.children) {
        const auto text = trimText(render(child));
        if (!text.empty()) out += "- " + text + "\n";
      }
      return out + "\n";
    }
    if (tag == "div" && markdownAttribute(element.opening, "class").starts_with("alert alert-")) {
      std::string kind = markdownAttribute(element.opening, "class").substr(12), body;
      // GitHub knows five alert kinds; the containers' others map onto them.
      if (kind == "info") kind = "note";
      if (kind == "danger") kind = "caution";
      std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char byte) { return static_cast<char>(std::toupper(byte)); });
      for (const auto& child : element.children) {
        if (markdownAttribute(child.opening, "class") != "alert-title") body += render(child);
      }
      return "\n\n> [!" + kind + "]\n" + indent(trimText(body), "> ", "> ") + "\n\n";
    }
    const auto block = [](std::string_view name) {
      return name == "p" || name == "pre" || name == "blockquote" || name == "ul" || name == "ol" ||
             name == "table" || name == "div" || name == "section" || name == "details" || name == "picture" ||
             name == "hr" || (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6');
    };
    std::string inner;
    for (std::size_t index = 0; index < element.children.size(); ++index) {
      const auto& child = element.children[index];
      if (child.tag.empty() && trimText(std::string(child.text)).empty() &&
          (tag.empty() || (index > 0 && block(element.children[index - 1].tag)) ||
                          (index + 1 < element.children.size() && block(element.children[index + 1].tag)))) continue;
      const auto part = render(child);
      // Join block separators without modifying blank lines inside code.
      if (part.starts_with('\n')) while (!inner.empty() && inner.back() == '\n') inner.pop_back();
      inner += part;
    }
    if (tag == "a") return "[" + inner + "](<" + markdownDestination(link(markdownAttribute(element.opening, "href"))) + ">)";
    if (tag == "em") return "*" + inner + "*";
    if (tag == "strong" || tag == "summary") return "**" + inner + "**";
    if (tag == "del") return "~~" + inner + "~~";
    if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6')
      return "\n\n" + std::string(static_cast<std::size_t>(tag[1] - '0'), '#') + " " + trimText(inner) + "\n\n";
    if (tag == "blockquote") return "\n\n" + indent(trimText(inner), "> ", "> ") + "\n\n";
    if (tag == "span" && markdownAttribute(element.opening, "class") == "home-name") return inner + " ";
    if (tag == "p") {
      inner = trimText(inner);
      if (inner.starts_with('-') || inner.starts_with('+')) inner.insert(0, "\\");
      std::size_t digits = 0;
      while (digits < inner.size() && inner[digits] >= '0' && inner[digits] <= '9') ++digits;
      if (digits > 0 && digits + 1 < inner.size() && (inner[digits] == '.' || inner[digits] == ')') && inner[digits + 1] == ' ')
        inner.insert(digits, "\\");
      return "\n\n" + inner + "\n\n";
    }
    if (tag == "div" || tag == "section" || tag == "details" || tag == "picture") return "\n\n" + trimText(inner) + "\n\n";
    return inner;
  };
  std::string out = trimText(render(root));
  if (out.size() > kMaximumMarkdownOutputBytes) siteError("Markdown page exceeds the output bound");
  return out + "\n";
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

// What a site's pages need from the portal around them; absent in a
// single-site build. Paths are portal-relative (from the output root).
struct PortalShell {
  const DocsPortalModel* portal = nullptr;
  std::optional<std::size_t> current;  // the site being rendered; none on the welcome page
  std::string logo_output;             // the portal's copied logo, or ""
  std::string favicon_output;          // the portal's copied browser icon, or ""
  std::string stylesheet_output;       // the portal's copied stylesheet, or ""
  DocsUiText ui;                       // the portal's own interface text (its welcome page, its selector)
};

// Everything the page shell needs that does not change per page.
struct SiteContext {
  const DocsSiteModel* model = nullptr;
  std::string version;
  std::string logo_output;        // site-relative path of the copied logo, or ""
  std::string favicon_output;     // site-relative path of the copied browser icon, or ""
  std::string stylesheet_output;  // site-relative path of the copied stylesheet, or ""
  std::vector<std::vector<MarkdownHeading>> outlines;  // per page, filled while rendering
  std::string prefix;                   // where the site sits in the output: "" or "<id>/" (and "<lang>/" for a translation)
  const PortalShell* portal = nullptr;  // set when the site is one of a portal's
  // The default-language site whose translations make up the language menu,
  // and where it sits in the output; null when the site has no translations.
  const DocsSiteModel* languages = nullptr;
  std::string languages_prefix;
  std::map<std::string, std::string> tag_outputs;  // each tag's page, site-relative
};

// The portal's site selector: a native <details> dropdown, so it needs no
// script, listing every site's home with the current one marked.
// `from_dir` is the page's portal-relative directory.
std::string renderSiteSelector(const DocsPortalModel& portal, std::optional<std::size_t> current, std::string_view from_dir,
                               const DocsUiText& ui);

// Header links, as configured.
std::string renderHeaderLinks(const std::vector<DocsLink>& links, const DocsUiText& ui);

constexpr std::string_view kNavChevron =
    "<svg class=\"nav-chevron\" viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"m9 6 6 6-6 6\"/></svg>";

// Keep the category's landing page reachable without repeating its name as
// both a disclosure heading and its first link. This only changes navigation
// labels; the page title, breadcrumbs and reading order remain intact.
std::vector<DocsNavItem> menuChildren(const DocsNavItem& item, const DocsSiteModel& model) {
  auto children = item.children;
  if (item.page) {
    children.insert(children.begin(), DocsNavItem{model.ui.overview, item.page, {}});
  } else if (!children.empty()) {
    auto& first = children.front();
    if (first.page && first.children.empty() && first.title == item.title) {
      const auto name = basenameOf(model.pages[*first.page].source);
      if (name == "index.md" || name == "README.md") first.title = model.ui.overview;
    }
  }
  return children;
}

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
      std::vector<const DocsNavItem*> trail;
      const bool active = item.page == current || findTrail(item.children, current, trail);
      out += std::string("<details class=\"nav-group\"") + (active ? " open" : "") + "><summary><span>" +
             escapeHtml(item.title) + "</span>" + std::string(kNavChevron) + "</summary>";
      renderSidebarItems(out, menuChildren(item, model), model, current, from_dir);
      out += "</details>";
    }
    out += "</li>";
  }
  out += "</ul>";
}

// The headings from `min_level` to `max_level` as nested lists, each deeper
// heading inside the list of the shallower one before it. A heading deeper
// than the level just above it by more than one still nests one level only.
std::string renderOutline(const std::vector<MarkdownHeading>& outline, int min_level, int max_level) {
  std::string out;
  std::vector<int> open;  // the levels of the lists currently open
  bool item_open = false;
  for (const auto& heading : outline) {
    if (heading.level < min_level || heading.level > max_level) continue;
    if (open.empty()) {
      out += "<ul>";
      open.push_back(heading.level);
    } else if (heading.level > open.back()) {
      out += "<ul>";
      open.push_back(heading.level);
      item_open = false;
    } else {
      if (item_open) out += "</li>";
      while (open.size() > 1 && heading.level < open.back()) {
        out += "</ul></li>";
        open.pop_back();
      }
    }
    out += "<li><a href=\"#" + escapeHtml(encodePathSegment(heading.id)) + "\">" + escapeHtml(heading.text) + "</a>";
    item_open = true;
  }
  if (open.empty()) return out;
  out += "</li>";
  for (std::size_t index = 1; index < open.size(); ++index) out += "</ul></li>";
  return out + "</ul>";
}

// The page's author and date as a byline, placed right under the article's
// leading `<h1>` -- or first, when the article does not open with one.
std::string withByline(std::string article, const DocsPage& page) {
  if (page.author.empty() && page.date.empty()) return article;
  std::string byline = "<p class=\"byline\">";
  if (!page.author.empty()) byline += "<span class=\"author\">" + escapeHtml(page.author) + "</span>";
  if (!page.author.empty() && !page.date.empty()) byline += " · ";
  if (!page.date.empty()) byline += "<time datetime=\"" + escapeHtml(page.date) + "\">" + escapeHtml(page.date) + "</time>";
  byline += "</p>\n";
  std::size_t at = 0;
  if (article.starts_with("<h1")) {
    constexpr std::string_view kClose = "</h1>";
    if (const auto close = article.find(kClose); close != std::string::npos) {
      at = close + kClose.size();
      if (at < article.size() && article[at] == '\n') ++at;
    }
  }
  article.insert(at, byline);
  return article;
}

std::string renderHome(const DocsHome& home, const MarkdownLinkResolver& resolver,
                       const std::function<void(const std::string&)>& warning, const std::string& features_label = "Features") {
  const auto destination = [&](const std::string& target, bool image) -> std::optional<std::string> {
    if (isValidLinkUrl(target) && !image) return target;
    if (target.empty() || target.starts_with("//") || target.find(':') != std::string::npos ||
        target.find_first_of("\\\r\n") != std::string::npos) {
      warning("unsafe landing page destination");
      return std::nullopt;
    }
    const auto hash = target.find('#');
    const auto path = target.substr(0, hash);
    if (path.empty() && hash == 0 && !image) return target;
    const auto resolved = resolver(path, image, 0);
    if (!resolved) return std::nullopt;
    return *resolved + (hash == std::string::npos ? "" : target.substr(hash));
  };
  std::string out = "<section class=\"home-hero\"><div class=\"home-copy\"><h1>";
  if (!home.name.empty()) out += "<span class=\"home-name\">" + escapeHtml(home.name) + "</span>";
  out += escapeHtml(home.text) + "</h1>";
  if (!home.tagline.empty()) out += "<p class=\"home-tagline\">" + escapeHtml(home.tagline) + "</p>";
  out += "<div class=\"home-actions\">";
  for (const auto& action : home.actions) {
    if (const auto url = destination(action.link, false))
      out += "<a class=\"home-action " + action.theme + "\" href=\"" + escapeHtml(*url) + "\">" + escapeHtml(action.text) + "</a>";
    else out += "<span>" + escapeHtml(action.text) + "</span>";
  }
  out += "</div></div>";
  if (!home.image_light.empty()) {
    const auto light = destination(home.image_light, true);
    const auto dark = home.image_dark.empty() ? std::optional<std::string>{} : destination(home.image_dark, true);
    if (light) {
      out += "<picture class=\"home-image\">";
      if (dark) out += "<source media=\"(prefers-color-scheme: dark)\" srcset=\"" + escapeHtml(*dark) + "\">";
      out += "<img src=\"" + escapeHtml(*light) + "\" alt=\"" + escapeHtml(home.image_alt) + "\"></picture>";
    }
  }
  out += "</section>";
  if (!home.features.empty()) {
    out += "<section class=\"home-features\" aria-label=\"" + escapeHtml(features_label) + "\">";
    for (const auto& feature : home.features) {
      std::string card;
      if (!feature.icon.empty()) card += "<span class=\"home-feature-icon\" aria-hidden=\"true\">" + escapeHtml(feature.icon) + "</span>";
      card += "<h2>" + escapeHtml(feature.title) + "</h2><p>" + escapeHtml(feature.details) + "</p>";
      // A card with a link is the link: the whole card is the target.
      const auto href = feature.link.empty() ? std::nullopt : destination(feature.link, false);
      out += href ? "<a class=\"home-feature\" href=\"" + escapeHtml(*href) + "\">" + card + "</a>"
                  : "<div class=\"home-feature\">" + card + "</div>";
    }
    out += "</section>";
  }
  return out;
}

// v2 permits an appended .md suffix. Relative URLs work on file:// and under
// any hosting prefix; portal pages all point at the portal's root index.
std::string llmsDiscovery(std::string_view from, std::string_view page) {
  return "<link rel=\"describedby\" href=\"" + escapeHtml(relativeUrl(from, kDocsLlmsFile)) +
         "\" type=\"text/plain\"><link rel=\"alternate\" type=\"text/markdown\" href=\"" +
         escapeHtml(relativeUrl(directoryOf(page), std::string(page) + ".md")) + "\">";
}

std::map<std::string, std::string> markdownPageLinks(const DocsSiteModel& model, const std::string& prefix,
                                                   const PortalShell* portal, std::string_view from) {
  std::map<std::string, std::string> links;
  const auto add = [&](const std::string& path) { links.emplace(relativeUrl(from, path), relativeUrl(from, path + ".md")); };
  if (portal != nullptr) {
    add("index.html");
    for (const auto& site : portal->portal->sites) {
      for (const auto& page : site.model.pages) add(site.entry.id + "/" + page.output);
      add(site.entry.id + "/" + std::string(kDocsSiteIndexPage));
    }
  } else {
    for (const auto& page : model.pages) add(prefix + page.output);
    add(prefix + std::string(kDocsSiteIndexPage));
  }
  return links;
}

// The article without its leading `<h1>` (front matter `hide_title`).
std::string withoutTitle(std::string article) {
  if (!article.starts_with("<h1")) return article;
  constexpr std::string_view kClose = "</h1>";
  const auto close = article.find(kClose);
  if (close == std::string::npos) return article;
  auto end = close + kClose.size();
  if (end < article.size() && article[end] == '\n') ++end;
  return article.substr(end);
}

struct ShellInput {
  std::string title;        // the page title
  std::string description;  // meta description, may be empty
  std::string output;       // the page's site-relative output path
  std::string article;      // rendered body HTML
  const std::vector<MarkdownHeading>* outline = nullptr;
  std::optional<std::size_t> page;  // the model page, when this is one
};

// The page carrying `counterpart` in `site`, or its home when it has none.
const DocsPage& counterpartIn(const DocsSiteModel& site, const std::string& counterpart, bool* translated = nullptr) {
  for (const auto& page : site.pages) {
    if (page.counterpart == counterpart) {
      if (translated != nullptr) *translated = !page.fallback;
      return page;
    }
  }
  if (translated != nullptr) *translated = false;
  return site.pages[site.home];
}

// The language menu: every language of the site, each linking to this
// page's counterpart there (or that language's home), the current one marked.
std::string renderLanguageMenu(const SiteContext& context, const std::string& counterpart, std::string_view portal_dir) {
  const DocsSiteModel& root = *context.languages;
  std::string out = "<details class=\"lang-select\"><summary><span aria-hidden=\"true\">\xf0\x9f\x8c\x90</span> " +
                    escapeHtml(context.model->language_label) + "</summary><nav aria-label=\"" +
                    escapeHtml(context.model->ui.languages) + "\"><ul>";
  const auto entry = [&](const DocsSiteModel& site, const std::string& prefix) {
    const DocsPage& target = counterpartIn(site, counterpart);
    out += "<li><a href=\"" + escapeHtml(relativeUrl(portal_dir, prefix + target.output)) + "\" hreflang=\"" + escapeHtml(site.lang) +
           "\" lang=\"" + escapeHtml(site.lang) + "\"" + (&site == context.model ? " aria-current=\"true\"" : "") + ">" +
           escapeHtml(site.language_label) + "</a></li>";
  };
  entry(root, context.languages_prefix);
  for (const auto& translation : root.translations) entry(translation, context.languages_prefix + translation.lang + "/");
  return out + "</ul></nav></details>";
}

// `<link rel="alternate" hreflang>` for each language this page exists in
// (translated, not standing in), plus x-default for the default language.
std::string renderAlternates(const SiteContext& context, const std::string& counterpart, std::string_view portal_dir) {
  const DocsSiteModel& root = *context.languages;
  const std::string& base = root.config.url;
  std::string out;
  const auto link = [&](const DocsSiteModel& site, const std::string& prefix, std::string_view hreflang) {
    bool translated = false;
    const DocsPage& target = counterpartIn(site, counterpart, &translated);
    if (!translated || target.counterpart != counterpart) return;
    const std::string path = prefix + target.output;
    const std::string href = base.empty() ? relativeUrl(portal_dir, path) : base + encodePathSegment(path);
    out += "<link rel=\"alternate\" hreflang=\"" + escapeHtml(hreflang) + "\" href=\"" + escapeHtml(href) + "\">";
  };
  link(root, context.languages_prefix, root.lang);
  for (const auto& translation : root.translations) link(translation, context.languages_prefix + translation.lang + "/", translation.lang);
  link(root, context.languages_prefix, "x-default");
  return out;
}

// The colour theme picker: hidden until kDocsUiScript reveals it, so a
// visitor without scripting never sees a control that does nothing.
std::string renderThemePicker(const DocsUiText& ui) {
  constexpr std::string_view kSystem =
      "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\"><rect x=\"3\" y=\"4\" width=\"18\" height=\"13\" rx=\"2\"/><path d=\"M8 21h8m-4-4v4\"/></svg>";
  constexpr std::string_view kLight =
      "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\"><circle cx=\"12\" cy=\"12\" r=\"4\"/><path d=\"M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.4 "
      "1.4m11.2 11.2L19 19M5 19l1.4-1.4M17.6 6.4L19 5\"/></svg>";
  constexpr std::string_view kDark =
      "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"M20.5 14A8.5 8.5 0 0 1 10 3.5 8.5 8.5 0 1 0 20.5 14Z\"/></svg>";
  const std::string theme = escapeHtml(ui.theme);
  const auto option = [](std::string_view choice, const std::string& label, std::string_view icon, bool pressed) {
    const std::string text = escapeHtml(label);
    return "<button type=\"button\" class=\"theme-toggle\" data-theme-choice=\"" + std::string(choice) + "\" aria-label=\"" + text +
           "\" title=\"" + text + "\" aria-pressed=\"" + (pressed ? "true" : "false") + "\">" + std::string(icon) + "</button>";
  };
  return "<details class=\"theme-picker\" hidden data-label=\"" + theme + "\"><summary class=\"theme-trigger\" aria-label=\"" + theme +
         "\" title=\"" + theme + "\">" + std::string(kSystem) + "</summary><div class=\"theme-options\" role=\"group\" aria-label=\"" +
         theme + "\">" + option("system", ui.theme_system, kSystem, true) + option("light", ui.theme_light, kLight, false) +
         option("dark", ui.theme_dark, kDark, false) + "</div></details>";
}

std::string renderShell(const SiteContext& context, const ShellInput& in) {
  const DocsSiteModel& model = *context.model;
  const DocsUiText& ui = model.ui;
  const DocsPage* page = in.page ? &model.pages[*in.page] : nullptr;
  const std::string from_dir(directoryOf(in.output));
  const auto url = [&](std::string_view target) { return escapeHtml(relativeUrl(from_dir, target)); };
  // A portal-relative target (the portal's welcome page, logo, stylesheet,
  // another site), from this page's place in the whole output.
  const std::string portal_dir = context.prefix + from_dir;
  const auto portalUrl = [&](std::string_view target) { return escapeHtml(relativeUrl(portal_dir, target)); };
  const PortalShell* portal = context.portal;
  std::vector<const DocsNavItem*> trail;
  if (in.page) findTrail(model.nav, *in.page, trail);
  const DocsNavItem* tab = trail.empty() ? nullptr : trail.front();
  const std::string layout = page != nullptr ? page->layout : std::string("doc");
  const bool doc = layout == "doc";
  const std::string lang = page != nullptr && !page->lang.empty() ? page->lang : model.lang;

  std::string out = "<!doctype html><html lang=\"" + escapeHtml(lang) + "\"" +
                    (docsLanguageIsRightToLeft(lang) ? " dir=\"rtl\"" : "") +
                    "><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  out += "<title>" + escapeHtml(in.title == model.title ? in.title : in.title + " · " + model.title) + "</title>";
  if (!in.description.empty()) out += "<meta name=\"description\" content=\"" + escapeHtml(in.description) + "\">";
  if (page != nullptr) {
    const auto meta = [&out](std::string_view name, std::string_view content) {
      out += "<meta name=\"" + escapeHtml(name) + "\" content=\"" + escapeHtml(content) + "\">";
    };
    if (!page->author.empty()) meta("author", page->author);
    if (!page->date.empty()) meta("date", page->date);
    if (!page->keywords.empty()) {
      std::string keywords;
      for (const auto& keyword : page->keywords) keywords += (keywords.empty() ? "" : ", ") + keyword;
      meta("keywords", keywords);
    }
    for (const auto& [key, value] : page->foreign_front_matter) meta(std::string(kDocsForeignMetaPrefix) + key, value);
    if (context.languages != nullptr) out += renderAlternates(context, page->counterpart, portal_dir);
  }
  out += "<meta name=\"generator\" content=\"ckdocs " + escapeHtml(context.version) + "\">";
  if (portal != nullptr ? portal->portal->config.llms : model.config.llms)
    out += llmsDiscovery(portal_dir, in.output);
  if (portal != nullptr && !portal->favicon_output.empty()) {
    out += "<link rel=\"icon\" href=\"" + portalUrl(portal->favicon_output) + "\">";
  } else if (!context.favicon_output.empty()) {
    out += "<link rel=\"icon\" href=\"" + url(context.favicon_output) + "\">";
  }
  out += "<style>";
  out += kDocsStyles;
  out += kMarkdownStyles;
  out += "</style>";
  if (!context.stylesheet_output.empty()) {
    out += "<link rel=\"stylesheet\" href=\"" + url(context.stylesheet_output) + "\">";
  } else if (portal != nullptr && !portal->stylesheet_output.empty()) {
    out += "<link rel=\"stylesheet\" href=\"" + portalUrl(portal->stylesheet_output) + "\">";
  }
  const bool landing = page != nullptr && page->landing.has_value();
  std::string body_class = "layout-" + layout;
  if (page != nullptr && !page->page_class.empty()) body_class += " " + page->page_class;
  out += "</head><body class=\"" + escapeHtml(body_class) + "\" data-copy-label=\"" + escapeHtml(ui.copy_code) +
         "\" data-copied-label=\"" + escapeHtml(ui.copied) + "\" data-diagram-expand=\"" + escapeHtml(ui.diagram_expand) +
         "\" data-diagram-close=\"" + escapeHtml(ui.diagram_close) + "\"><a class=\"skip-link\" href=\"#main-content\">" +
         escapeHtml(ui.skip_to_content) + "</a>";

  // Header: brand (the portal's, in a portal), the site selector, tabs, links.
  if (portal != nullptr) {
    const DocsConfig& portal_config = portal->portal->config;
    out += "<header class=\"site\"><a class=\"brand\" href=\"" + portalUrl("index.html") + "\">";
    if (!portal->logo_output.empty()) out += "<img src=\"" + portalUrl(portal->logo_output) + "\" alt=\"\">";
    out += escapeHtml(portal_config.brand.empty() ? portal_config.title : portal_config.brand) + "</a>";
    out += renderSiteSelector(*portal->portal, portal->current, portal_dir, ui);
  } else {
    out += "<header class=\"site\"><a class=\"brand\" href=\"" + url(model.pages[model.home].output) + "\">";
    if (!context.logo_output.empty()) out += "<img src=\"" + url(context.logo_output) + "\" alt=\"\">";
    out += escapeHtml(model.config.brand.empty() ? model.title : model.config.brand) + "</a>";
  }
  out += "<nav class=\"tabs\" aria-label=\"" + escapeHtml(ui.sections) + "\">";
  for (const auto& item : model.nav) {
    const auto first = firstPageOf(item);
    if (!first) continue;
    out += "<div class=\"tab-group\">";
    if (!item.children.empty()) {
      out += std::string("<details class=\"tab-menu") + (tab == &item ? " is-active" : "") +
             "\"><summary><span>" + escapeHtml(item.title) + "</span>" + std::string(kNavChevron) + "</summary>";
      renderSidebarItems(out, menuChildren(item, model), model, in.page.value_or(model.pages.size()), from_dir);
      out += "</details>";
    } else {
      out += "<a class=\"tab-link\" href=\"" + url(model.pages[*first].output) + "\"" +
             (tab == &item ? " aria-current=\"page\"" : "") + ">" + escapeHtml(item.title) + "</a>";
    }
    out += "</div>";
  }
  out += "</nav>";
  if (model.config.search) {
    // The input starts hidden; kDocsSearchScript is the only thing that
    // reveals it, so a visitor without scripting sees only the <noscript>
    // link below, never a non-functional box.
    out += "<form class=\"search\" role=\"search\" onsubmit=\"return false\">"
           "<button type=\"button\" class=\"icon-button header-icon search-toggle\" aria-label=\"" + escapeHtml(ui.search_open) +
           "\" title=\"" + escapeHtml(ui.search) + " (\xe2\x8c\x98/Ctrl+K)\" data-open-label=\"" + escapeHtml(ui.search_open) +
           "\" data-close-label=\"" + escapeHtml(ui.search_close) +
           "\" aria-expanded=\"false\" aria-controls=\"ckdocs-search\" hidden><svg viewBox=\"0 0 24 24\" aria-hidden=\"true\">"
           "<circle cx=\"10.5\" cy=\"10.5\" r=\"6.5\"/><path d=\"m16 16 5 5\"/></svg></button>"
           "<input type=\"search\" id=\"ckdocs-search\" name=\"q\" placeholder=\"" + escapeHtml(ui.search) +
           "\" aria-label=\"" + escapeHtml(ui.search_label) + "\" autocomplete=\"off\" spellcheck=\"false\" hidden data-index=\"" +
           url(std::string(kDocsSearchIndexPage)) + "\">"
           "<div id=\"ckdocs-search-results\" class=\"search-results\" hidden></div>"
           "<noscript><a href=\"" + url(std::string(kDocsSiteIndexPage)) + "\">" + escapeHtml(ui.site_index) + "</a></noscript>"
           "</form><script>" + std::string(kDocsSearchScript) + "</script>";
  }
  // A portal site without links of its own shows the portal's.
  out += renderHeaderLinks(!model.config.links.empty() || portal == nullptr ? model.config.links : portal->portal->config.links, ui);
  if (context.languages != nullptr) out += renderLanguageMenu(context, page != nullptr ? page->counterpart : std::string(), portal_dir);
  out += renderThemePicker(ui);
  out += "</header>";

  // Sidebar: the active tab's pages, then this page's outline -- in the doc
  // layout, as far as the page keeps them.
  std::string sidebar;
  const auto current = in.page.value_or(model.pages.size());
  if (doc && tab != nullptr && (page == nullptr || page->sidebar)) {
    sidebar += "<nav aria-label=\"" + escapeHtml(ui.pages) + "\"><p class=\"side-title\">";
    if (tab->page) {
      sidebar += "<a href=\"" + url(model.pages[*tab->page].output) + "\"" +
                 (*tab->page == current ? " aria-current=\"page\"" : "") + ">" + escapeHtml(tab->title) + "</a>";
    } else {
      sidebar += escapeHtml(tab->title);
    }
    sidebar += "</p>";
    if (!tab->children.empty()) renderSidebarItems(sidebar, menuChildren(*tab, model), model, current, from_dir);
    sidebar += "</nav>";
  }
  if (doc && in.outline != nullptr && (page == nullptr || page->toc)) {
    const int min_level = page != nullptr ? page->toc_min_level : model.config.toc_min_level;
    const int max_level = page != nullptr ? page->toc_max_level : model.config.toc_max_level;
    const std::string outline = renderOutline(*in.outline, min_level, max_level);
    if (!outline.empty()) {
      sidebar += "<nav class=\"outline\" aria-label=\"" + escapeHtml(ui.on_this_page) + "\"><p class=\"side-title\">" +
                 escapeHtml(ui.on_this_page) + "</p>" + outline + "</nav>";
    }
  }
  if (sidebar.empty()) {
    out += std::string("<div class=\"page ") + (landing ? "landing" : "no-sidebar") + "\"><main id=\"main-content\">";
  } else {
    out += "<div class=\"page\"><input class=\"nav-switch\" id=\"nav-toggle\" type=\"checkbox\">"
           "<label class=\"nav-toggle\" for=\"nav-toggle\">" + escapeHtml(ui.menu) + "</label><aside class=\"sidebar\">" +
           sidebar + "</aside><main id=\"main-content\">";
  }

  // Breadcrumbs, the fallback notice, the article, its tags, previous/next.
  if (doc && trail.size() > 1) {
    out += "<nav class=\"crumbs\" aria-label=\"" + escapeHtml(ui.breadcrumb) + "\"><ol>";
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
  std::string article_lang;
  if (page != nullptr && page->fallback && context.languages != nullptr) {
    const DocsSiteModel& root = *context.languages;
    article_lang = root.lang;
    out += "<p class=\"fallback-notice\" role=\"note\">" +
           escapeHtml(fillDocsUiText(ui.not_translated, {{"language", model.language_label}, {"default", root.language_label}})) +
           "</p>";
  }
  out += article_lang.empty() ? "<article>" : "<article lang=\"" + escapeHtml(article_lang) + "\"" +
                                                  (docsLanguageIsRightToLeft(article_lang) ? " dir=\"rtl\"" : " dir=\"ltr\"") + ">";
  out += in.article + "</article>";
  if (page != nullptr && !page->tags.empty()) {
    out += "<nav class=\"page-tags\" aria-label=\"" + escapeHtml(ui.tags) + "\"><span>" + escapeHtml(ui.tags) + "</span><ul>";
    for (const auto& tag : page->tags) {
      const auto found = context.tag_outputs.find(tag);
      out += "<li>" + (found == context.tag_outputs.end() ? escapeHtml(tag)
                                                          : "<a href=\"" + url(found->second) + "\">" + escapeHtml(tag) + "</a>") +
             "</li>";
    }
    out += "</ul></nav>";
  }
  if (page != nullptr && doc && page->pager) {
    const auto& order = model.reading_order;
    const auto position = std::find(order.begin(), order.end(), *in.page);
    // The neighbours in reading order, unless front matter says otherwise;
    // `none` stands for no link.
    const std::size_t none = model.pages.size();
    std::size_t previous = none;
    std::size_t next = none;
    if (position != order.end() && position != order.begin()) previous = *(position - 1);
    if (position != order.end() && position + 1 != order.end()) next = *(position + 1);
    if (page->prev.hidden) previous = none;
    else if (page->prev.page) previous = *page->prev.page;
    if (page->next.hidden) next = none;
    else if (page->next.page) next = *page->next.page;
    std::string pager;
    if (previous != none) {
      const auto& target = model.pages[previous];
      const std::string& label = page->prev.text.empty() ? target.title : page->prev.text;
      pager += "<a class=\"previous\" rel=\"prev\" href=\"" + url(target.output) + "\"><small>" + escapeHtml(ui.previous) +
               "</small>\xe2\x86\x90 " + escapeHtml(label) + "</a>";
    }
    if (next != none) {
      const auto& target = model.pages[next];
      const std::string& label = page->next.text.empty() ? target.title : page->next.text;
      pager += "<a class=\"next\" rel=\"next\" href=\"" + url(target.output) + "\"><small>" + escapeHtml(ui.next) + "</small>" +
               escapeHtml(label) + " \xe2\x86\x92</a>";
    }
    if (!pager.empty()) out += "<nav class=\"pager\" aria-label=\"" + escapeHtml(ui.previous_next) + "\">" + pager + "</nav>";
  }
  out += "</main></div>";

  // Footer.
  out += "<footer class=\"site\">";
  const std::string& footer = !model.config.footer.empty() || portal == nullptr ? model.config.footer : portal->portal->config.footer;
  if (!footer.empty()) out += "<span>" + escapeHtml(footer) + "</span>";
  out += "<span>" + escapeHtml(fillDocsUiText(ui.built_with, {{"generator", "ckdocs " + context.version}})) + "</span>";
  out += "<a href=\"" + url(std::string(kDocsSiteIndexPage)) + "\">" + escapeHtml(ui.site_index) + "</a>";
  if (portal != nullptr ? portal->portal->config.llms : model.config.llms) {
    out += "<a href=\"" + url(in.output + ".md") + "\" title=\"" + escapeHtml(ui.markdown_version) + "\">Markdown</a>";
  }
  out += "</footer><script>";
  out += kDocsUiScript;
  // The copy buttons' script, only where there is code to copy.
  if (in.article.find("<pre") != std::string::npos) out += kDocsCodeScript;
  return out + "</script></body></html>";
}

std::string renderSiteSelector(const DocsPortalModel& portal, std::optional<std::size_t> current, std::string_view from_dir,
                               const DocsUiText& ui) {
  std::string out = "<details class=\"site-select\"><summary>";
  out += current ? escapeHtml(portal.sites[*current].entry.title) : escapeHtml(ui.sites);
  out += "</summary><nav aria-label=\"" + escapeHtml(ui.documentation_sites) + "\"><ul>";
  for (std::size_t index = 0; index < portal.sites.size(); ++index) {
    const auto& site = portal.sites[index];
    out += "<li><a href=\"" + escapeHtml(relativeUrl(from_dir, site.entry.id + "/" + site.model.pages[site.model.home].output)) +
           "\"" + (current == index ? " aria-current=\"true\"" : "") + ">" + escapeHtml(site.entry.title) + "</a></li>";
  }
  return out + "</ul></nav></details>";
}

std::string renderHeaderLinks(const std::vector<DocsLink>& links, const DocsUiText& ui) {
  if (links.empty()) return {};
  std::string out = "<nav class=\"links\" aria-label=\"" + escapeHtml(ui.links) + "\">";
  for (const auto& link : links) {
    if (link.icon == "github") {
      out += "<a class=\"icon-button header-icon github-link\" href=\"" + escapeHtml(link.url) +
             "\" rel=\"noopener\" aria-label=\"" + escapeHtml(link.title) + "\" title=\"" + escapeHtml(link.title) + "\">" +
             std::string(kDocsGithubIcon) + "</a>";
    } else {
      out += "<a href=\"" + escapeHtml(link.url) + "\" rel=\"noopener\">" + escapeHtml(link.title) + "</a>";
    }
  }
  return out + "</nav>";
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
  // Inline elements join their text to its neighbours ("<a>x</a>." reads
  // "x."); every other tag separates. A footnote's number, a formula's TeX
  // source, line numbers and the footnote list are not part of the text.
  static constexpr std::array<std::string_view, 24> kInline{"a",    "abbr", "b",  "code",   "del",  "em",  "i",  "ins",
                                                            "kbd",  "mark", "s",  "small",  "span", "strong", "sub", "sup",
                                                            "time", "u",    "mi", "mn",     "mo",   "mtext", "mrow", "math"};
  std::string raw;
  raw.reserve(html.size());
  for (std::size_t at = 0; at < html.size();) {
    if (html[at] != '<') {
      raw += html[at++];
      continue;
    }
    const auto close = html.find('>', at);
    if (close == std::string_view::npos) break;
    const auto tag = html.substr(at, close + 1 - at);
    const auto skipTo = [&](std::string_view ending) {
      const auto stop = html.find(ending, close);
      return stop == std::string_view::npos ? html.size() : stop + ending.size();
    };
    if (tag.starts_with("<sup class=\"footnote-ref\"")) { at = skipTo("</sup>"); continue; }
    if (tag.starts_with("<annotation")) { at = skipTo("</annotation>"); continue; }
    if (tag.starts_with("<span class=\"line-number\"")) { at = skipTo("</span>"); continue; }
    if (tag.starts_with("<section class=\"footnotes\"")) { at = skipTo("</section>"); continue; }
    auto name = tag.substr(tag.size() > 1 && tag[1] == '/' ? 2 : 1);
    name = name.substr(0, name.find_first_of(" >/"));
    if (std::find(kInline.begin(), kInline.end(), name) == kInline.end()) raw += ' ';
    at = close + 1;
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
std::string searchPageEntryJson(const std::string& title, const std::string& url, const std::string& description,
                                const std::vector<std::string>& tags, const std::vector<SearchSection>& sections) {
  std::string entry = "{\"title\":\"" + jsonEscape(title) + "\",\"url\":\"" + jsonEscape(url) + "\",";
  if (!description.empty()) entry += "\"description\":\"" + jsonEscape(description) + "\",";
  if (!tags.empty()) {
    entry += "\"tags\":[";
    for (std::size_t index = 0; index < tags.size(); ++index) entry += (index ? ",\"" : "\"") + jsonEscape(tags[index]) + "\"";
    entry += "],";
  }
  entry += "\"sections\":[";
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

// The one temporary tree a build writes into, a fresh sibling of the output,
// and its swap into place. Construction refuses early what the final swap
// would refuse, before any work; nothing reaches `out` before commit().
class OutputTree {
 public:
  OutputTree(const fs::path& out_path, bool clean, DocsBuildReport& report)
      : out_(fs::absolute(out_path).lexically_normal()), report_(report) {
    std::error_code error;
    if (fs::exists(out_, error)) {
      if (!fs::is_directory(out_, error)) siteError("output path '" + out_.string() + "' exists and is not a directory");
      const bool empty = fs::is_empty(out_, error);
      const bool marked = fs::is_regular_file(out_ / kDocsSiteMarker, error);
      if (!empty && !marked) siteError("output directory '" + out_.string() + "' exists and was not written by ckdocs; refusing to touch it");
      if (!empty && !clean) siteError("output directory '" + out_.string() + "' already holds a site; use --clean to replace it");
    }
    auto pattern = (out_.parent_path() / (out_.filename().string() + ".tmp-XXXXXX")).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    if (mkdtemp(writable.data()) == nullptr) siteError("cannot create a temporary directory beside '" + out_.string() + "'");
    temporary_.path = writable.data();
  }

  // Writes one file at its output-relative path; the Pages size and entry
  // bounds apply to everything written so far, plus the marker to come.
  void write(const std::string& path, const std::string& content) {
    const auto target = temporary_.path / path;
    for (auto parent = fs::path(path).parent_path(); !parent.empty(); parent = parent.parent_path()) {
      if (directories_.insert(parent.generic_string()).second) fs::create_directories(temporary_.path / parent);
    }
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) siteError("cannot write '" + target.string() + "'");
    files_.insert(path);
    report_.bytes_written += content.size();
    if (report_.bytes_written > kMaximumPagesSiteBytes) siteError("the site exceeds the size limit for a Pages site");
    if (directories_.size() + files_.size() + 1 > kMaximumPagesEntries) siteError("the site exceeds the entry limit for a Pages site");
  }

  bool written(const std::string& path) const { return files_.count(path) != 0; }

  // The marker, then the swap; an existing site is kept until the new one is in.
  void commit(const std::string& version) {
    write(std::string(kDocsSiteMarker),
          "version=1\ngenerator=ckdocs " + version + "\npages=" + std::to_string(report_.pages_written) + "\n");
    std::error_code error;
    if (fs::exists(out_, error)) {
      const fs::path old = fs::path(temporary_.path.string() + ".old");
      fs::rename(out_, old, error);
      if (error) siteError("cannot move the previous site aside: " + error.message());
      fs::rename(temporary_.path, out_, error);
      if (error) {
        std::error_code restore;
        fs::rename(old, out_, restore);
        siteError("cannot move the new site into place: " + error.message());
      }
      fs::remove_all(old, error);
    } else {
      fs::create_directories(out_.parent_path(), error);
      fs::rename(temporary_.path, out_, error);
      if (error) siteError("cannot move the new site into place: " + error.message());
    }
    temporary_.keep = true;
  }

 private:
  fs::path out_;
  DocsBuildReport& report_;
  TemporaryDirectory temporary_;
  std::set<std::string> directories_;
  std::set<std::string> files_;
};

// Renders one site's pages, site index, search index and assets into `tree`
// below `prefix` ("" for a single site, "<id>/" in a portal), counting into
// `result`.
// A tag's file name: letters and digits lowercased, every other run of ASCII
// characters one `-`; other scripts are kept (and percent-encoded in links).
std::string tagSlug(std::string_view tag) {
  std::string slug;
  bool dash = false;
  for (const unsigned char byte : tag) {
    if (std::isalnum(byte) != 0 || byte >= 0x80) {
      if (dash && !slug.empty()) slug += '-';
      slug += static_cast<char>(std::tolower(byte));
      dash = false;
    } else {
      dash = true;
    }
  }
  return slug.empty() ? std::string("tag") : slug;
}

// The words renderMarkdown writes itself, in the site's language.
MarkdownLabels markdownLabels(const DocsUiText& ui) {
  MarkdownLabels labels;
  labels.note = ui.note;
  labels.tip = ui.tip;
  labels.important = ui.important;
  labels.warning = ui.warning;
  labels.caution = ui.caution;
  labels.info = ui.info;
  labels.danger = ui.danger;
  labels.details = ui.details;
  labels.footnotes = ui.footnotes;
  labels.back_to_reference = ui.back_to_reference;
  return labels;
}

// The directory `to` as seen from the directory `from` (both repository-
// relative): "" when they are the same, else a path with `..` as needed.
std::string relativeDirectory(std::string_view from, std::string_view to) {
  const auto source = splitPath(from);
  const auto target = splitPath(to);
  std::size_t common = 0;
  while (common < source.size() && common < target.size() && source[common] == target[common]) ++common;
  std::string result;
  for (std::size_t index = common; index < source.size(); ++index) result += result.empty() ? ".." : "/..";
  for (std::size_t index = common; index < target.size(); ++index) {
    if (!result.empty()) result += '/';
    result += target[index];
  }
  return result;
}

// The opening of a page's text for a listing that has no description for it:
// the first sentence of its first non-empty section, else that excerpt as is.
std::string pageLead(const std::vector<SearchSection>& sections) {
  for (const auto& section : sections) {
    if (section.excerpt.empty()) continue;
    const auto stop = section.excerpt.find(". ");
    return stop == std::string::npos ? section.excerpt : section.excerpt.substr(0, stop + 1);
  }
  return {};
}

// `leads`, when given, receives each page's opening text (pageLead), indexed
// like model.pages, for llms.txt entries of pages without a description.
//
// `languages` is the default-language site when `model` is it or one of its
// translations (null when there are none), at `languages_prefix`; each
// translation is rendered below `<prefix><lang>/` after the site itself.
void renderSite(const DocsSiteModel& model, OutputTree& tree, const std::string& prefix, const PortalShell* portal,
                const DocsBuildOptions& options, DocsBuildReport& result, std::vector<std::string>* leads = nullptr,
                const DocsSiteModel* languages = nullptr, const std::string& languages_prefix = {}) {
  std::error_code error;
  SiteContext context;
  context.model = &model;
  context.version = buildVersion();
  context.outlines.resize(model.pages.size());
  context.prefix = prefix;
  context.portal = portal;
  if (languages == nullptr && !model.translations.empty()) languages = &model;
  context.languages = languages;
  context.languages_prefix = languages == &model ? prefix : languages_prefix;

  // Site-relative outputs and their sources: pages, assets, the logo and
  // stylesheet. A translation's page is also found under its alias, so a
  // link written in either language's tree reaches it.
  std::map<std::string, std::size_t> page_by_source;
  std::map<std::string, std::size_t> page_by_output;
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    page_by_source.emplace(model.pages[index].source, index);
    page_by_output.emplace(model.pages[index].output, index);
  }
  const bool llms_enabled = portal != nullptr ? portal->portal->config.llms : model.config.llms;
  std::set<std::string> markdown_outputs;
  if (llms_enabled) {
    for (const auto& page : model.pages) markdown_outputs.insert(page.output + ".md");
    markdown_outputs.insert(std::string(kDocsSiteIndexPage) + ".md");
  }
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    if (!model.pages[index].alias.empty()) page_by_source.emplace(model.pages[index].alias, index);
  }
  // Tag pages: one per tag under tags/, plus tags/index.html.
  if (!model.tags.empty()) {
    for (const auto& [output, index] : page_by_output) {
      if (output.starts_with("tags/")) {
        siteError("page '" + model.pages[index].source + "' is written to '" + output +
                  "', where the tag pages go; rename it, or remove the site's tags");
      }
    }
    std::set<std::string> slugs;
    for (const auto& [tag, pages] : model.tags) {
      const std::string base = tagSlug(tag);
      std::string slug = base;
      for (std::size_t number = 2; !slugs.insert(slug).second; ++number) slug = base + "-" + std::to_string(number);
      context.tag_outputs.emplace(tag, "tags/" + slug + ".html");
    }
  }
  std::map<std::string, std::string> assets;  // output path -> root-relative source
  const auto diagramPath = [](std::string_view path) { return path == "_ckdocs-mermaid" || path.starts_with("_ckdocs-mermaid/"); };
  if (options.mermaid_renderer) {
    for (const auto& [output, index] : page_by_output) {
      if (diagramPath(output)) siteError("page '" + model.pages[index].source + "' uses the reserved _ckdocs-mermaid output directory");
    }
  }
  // Copies a root-relative file into the site: its output path, or why it
  // cannot be copied (empty `output`).
  const auto placeAsset = [&](const std::string& root_relative, std::string& output) -> std::string {
    const auto full = model.root / root_relative;
    if (fs::is_symlink(full, error) || !fs::is_regular_file(full, error)) return "is not a regular file";
    if (fs::file_size(full, error) > kMaximumPagesFileBytes) return "exceeds the file size limit";
    const std::string public_prefix = model.source.empty() ? "public/" : model.source + "/public/";
    const auto path = root_relative.starts_with(public_prefix) ? root_relative.substr(public_prefix.size())
                                                              : assetOutputPath(model, root_relative);
    if (options.mermaid_renderer && diagramPath(path)) return "uses the reserved _ckdocs-mermaid output directory";
    if (page_by_output.count(path) != 0) return "would overwrite the page '" + path + "'";
    if (markdown_outputs.count(path) != 0) return "would overwrite the generated Markdown page '" + path + "'";
    if (prefix.empty() && (path == kDocsSitemapFile || (llms_enabled && path == kDocsLlmsFile) || path == kDocsSearchIndexPage ||
                           path == kDocsSiteIndexPage)) {
      return "would overwrite the generated '" + path + "'";
    }
    if (!model.tags.empty() && path.starts_with("tags/")) return "would be written among the tag pages under tags/";
    const auto [existing, inserted] = assets.try_emplace(path, root_relative);
    if (!inserted && existing->second != root_relative) {
      return "would be written to '" + path + "', like '" + existing->second + "'";
    }
    output = path;
    return {};
  };
  // The logo and stylesheet the configuration names must be copied.
  const auto addAsset = [&](const std::string& root_relative, const char* what) {
    std::string output;
    const std::string problem = placeAsset(root_relative, output);
    if (!problem.empty()) siteError(std::string(what) + " '" + root_relative + "' " + problem);
    return output;
  };
  // In a portal the header shows the portal's brand and logo, not the site's.
  if (portal == nullptr && !model.config.logo.empty()) context.logo_output = addAsset(model.config.logo, "site.logo");
  if (portal == nullptr && !model.config.favicon.empty()) context.favicon_output = addAsset(model.config.favicon, "site.favicon");
  if (!model.config.stylesheet.empty()) context.stylesheet_output = addAsset(model.config.stylesheet, "site.stylesheet");

  const auto writeFile = [&](const std::string& site_path, const std::string& content) { tree.write(prefix + site_path, content); };

  // Render and write every page; remember ids and links for the anchor check,
  // and, when search is on, each page's own sections for the index below.
  std::vector<std::set<std::string>> ids(model.pages.size());
  std::vector<std::vector<std::pair<std::string, std::string>>> links(model.pages.size());
  std::vector<std::string> search_entries;
  struct RenderedDiagram {
    std::string light_path, dark_path, alternative_text, error;
    std::vector<std::string> warnings;
  };
  std::map<std::string, RenderedDiagram> diagram_cache;
  std::set<std::string> diagram_assets;
  const auto writeDiagram = [&](const std::string& svg) {
    if (svg.empty() || svg.size() > kMaximumPagesFileBytes) siteError("Mermaid SVG exceeds the file size limit or is empty");
    const std::string path = "_ckdocs-mermaid/" + sha256Hex(svg) + ".svg";
    if (diagram_assets.insert(path).second) {
      ++result.assets_generated;
      writeFile(path, svg);
    }
    return path;
  };
  if (model.config.search) search_entries.reserve(model.pages.size());
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    const DocsPage& page = model.pages[index];
    const auto content = readFile(model.root / page.source, kMaximumMarkdownInputBytes);
    if (!content) siteError("cannot read '" + page.source + "', or it exceeds the 512 KiB page limit");
    const std::string page_dir(directoryOf(page.source));
    const std::string out_dir(directoryOf(page.output));
    LinkFindings broken_links;
    LinkContext link_context;
    link_context.first_line = bodyFirstLine(*content);
    link_context.resolver = [&](std::string_view target, bool image, std::size_t line) -> std::optional<std::string> {
      const auto broken = [&](const std::string& reason) {
        broken_links.add(page.source, std::string(image ? "image" : "link") + " target '" + std::string(target) + "' " + reason,
                         line);
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
      // The source-relative clean URL lookup below also handles directories.
      if (const auto left_out = model.skipped.find(*normalized); left_out != model.skipped.end()) {
        return broken("was left out of the site: it " + left_out->second);
      }
      if (hasMarkdownExtension(*normalized) && !target.starts_with('/')) return broken("is not a page of this site");
      // A real repository file takes precedence over a clean page alias,
      // including the existing asset/page collision check.
      const auto original_file = model.root / *normalized;
      if (fs::is_regular_file(original_file, error) || fs::is_symlink(original_file, error)) {
        std::string output;
        const auto problem = placeAsset(*normalized, output);
        if (!problem.empty()) return broken(problem);
        return relativeUrl(out_dir, output);
      }
      // Clean URLs from documentation tools: /guide, /guide.html and
      // /section/ are interpreted against the source tree only after the
      // existing repository-relative lookup has had first refusal.
      std::vector<std::string> candidates{*normalized};
      if (target.starts_with('/') && !model.source.empty()) candidates.push_back(model.source + "/" + *normalized);
      for (const auto& candidate : candidates) {
        if (const auto found = page_by_source.find(candidate); found != page_by_source.end())
          return relativeUrl(out_dir, model.pages[found->second].output);
        if (!image && !hasMarkdownExtension(candidate)) {
          std::string clean = candidate;
          if (clean.ends_with(".html")) clean.resize(clean.size() - 5);
          if (const auto found = page_by_source.find(clean + ".md"); found != page_by_source.end())
            return relativeUrl(out_dir, model.pages[found->second].output);
        }
        for (const auto* name : {"README.md", "index.md"}) {
          if (const auto found = page_by_source.find(candidate + "/" + name); found != page_by_source.end())
            return relativeUrl(out_dir, model.pages[found->second].output);
        }
      }
      if (target == "/") return relativeUrl(out_dir, model.pages[model.home].output);
      if (target.starts_with('/') && !model.source.empty()) {
        const auto public_file = model.source + "/public/" + *normalized;
        if (!fs::is_symlink(model.root / public_file, error) && fs::is_regular_file(model.root / public_file, error)) {
          std::string output;
          const auto problem = placeAsset(public_file, output);
          if (!problem.empty()) return broken(problem);
          return relativeUrl(out_dir, output);
        }
      }
      if (directory_target || normalized->empty()) return broken("is a directory without an index page");
      if (hasMarkdownExtension(*normalized)) return broken("is not a page of this site");
      const auto full = model.root / *normalized;
      if (fs::is_symlink(full, error) || !fs::is_regular_file(full, error)) return broken("does not exist");
      if (fs::file_size(full, error) > kMaximumPagesFileBytes) return broken("exceeds the file size limit");
      // A file a link cannot bring along is that link's problem, not the site's.
      std::string output;
      const std::string problem = placeAsset(*normalized, output);
      if (!problem.empty()) return broken(problem);
      return relativeUrl(out_dir, output);
    };
    link_context.labels = markdownLabels(model.ui);
    link_context.line_numbers = model.config.line_numbers;
    link_context.reporter = [&](const std::string& message, std::size_t line) {
      result.warnings.push_back(page.source + (line != 0 ? ":" + std::to_string(line) : std::string()) + ": " + message);
    };
    // `<<<` snippets and `<!--@include:-->` parts: files of the repository,
    // found like a link's target, never hidden ones.
    link_context.include_reader = [&](std::string_view target, bool markdown, std::size_t line) -> std::optional<MarkdownInclude> {
      const auto broken = [&](const std::string& reason) {
        broken_links.add(page.source, std::string(markdown ? "include" : "snippet") + " '" + std::string(target) + "' " + reason, line);
        return std::optional<MarkdownInclude>{};
      };
      const auto normalized = normalizePath(target.starts_with('/') ? std::string(target.substr(1)) : joinPath(page_dir, target));
      if (!normalized || normalized->empty()) return broken("leaves the repository");
      if (hasDotComponent(*normalized)) return broken("is a hidden file or inside a hidden directory");
      const auto full = model.root / *normalized;
      if (fs::is_symlink(full, error) || !fs::is_regular_file(full, error)) return broken("does not exist");
      if (fs::file_size(full, error) > kMaximumMarkdownInputBytes) return broken("exceeds the 512 KiB include limit");
      auto content = readFile(full, kMaximumMarkdownInputBytes);
      if (!content) return broken("cannot be read");
      if (!isValidUtf8(*content)) return broken("is not valid UTF-8 text");
      if (std::find(result.inputs.begin(), result.inputs.end(), *normalized) == result.inputs.end()) result.inputs.push_back(*normalized);
      return MarkdownInclude{std::move(*content), relativeDirectory(page_dir, directoryOf(*normalized))};
    };
    std::size_t diagram_number = 0;
    const MarkdownCodeRenderer code_renderer = [&](std::string_view language, std::string_view source) -> std::optional<std::string> {
      if (!options.mermaid_renderer || language != "mermaid") return std::nullopt;
      const std::string location = page.source + ": Mermaid diagram " + std::to_string(++diagram_number) + ": ";
      if (diagram_number > 128) {
        result.warnings.push_back(location + "exceeds the limit of 128 diagrams per page");
        return std::nullopt;
      }
      const std::string key = sha256Hex(source);
      auto found = diagram_cache.find(key);
      if (found == diagram_cache.end()) {
        if (diagram_cache.size() >= 1024) {
          result.warnings.push_back(location + "exceeds the limit of 1024 distinct diagrams per site");
          return std::nullopt;
        }
        RenderedDiagram rendered;
        std::optional<DocsDiagram> diagram;
        if (options.diagram_cache != nullptr) {
          if (const auto cached = options.diagram_cache->diagrams.find(key); cached != options.diagram_cache->diagrams.end()) {
            diagram = cached->second;
          }
        }
        if (!diagram) {
          try {
            diagram = options.mermaid_renderer(source);
            if (options.diagram_cache != nullptr) {
              if (options.diagram_cache->diagrams.size() >= kMaximumDocsCachedDiagrams) options.diagram_cache->diagrams.clear();
              options.diagram_cache->diagrams.emplace(key, *diagram);
            }
          } catch (const std::exception& failure) {
            rendered.error = failure.what();
          }
        }
        // Filesystem/output-limit failures must abort the atomic build, never
        // masquerade as a source error and publish a partially written SVG.
        if (diagram) {
          rendered.light_path = writeDiagram(diagram->light_svg);
          rendered.dark_path = writeDiagram(diagram->dark_svg);
          rendered.alternative_text = std::move(diagram->alternative_text);
          rendered.warnings = std::move(diagram->warnings);
        }
        found = diagram_cache.emplace(key, std::move(rendered)).first;
      }
      const auto& rendered = found->second;
      if (!rendered.error.empty()) {
        result.warnings.push_back(location + rendered.error);
        return std::nullopt;
      }
      for (const auto& warning : rendered.warnings) result.warnings.push_back(location + warning);
      ++result.diagrams_rendered;
      const std::string light = escapeHtml(relativeUrl(out_dir, rendered.light_path));
      const std::string dark = escapeHtml(relativeUrl(out_dir, rendered.dark_path));
      return "<figure class=\"mermaid\"><picture><source media=\"(prefers-color-scheme: dark)\" srcset=\"" + dark +
             "\"><img src=\"" + light + "\" alt=\"" + escapeHtml(rendered.alternative_text) +
             "\" loading=\"lazy\"></picture><div class=\"diagram-tools\"><a class=\"icon-button diagram-open\" href=\"" + light +
             "\" aria-label=\"" + escapeHtml(model.ui.diagram_open) + "\" title=\"" + escapeHtml(model.ui.diagram_open) + "\"><svg viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"M14 3h7v7m0-7L11 13M10 5H5a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-5\"/></svg></a></div></figure>\n";
    };
    std::string article;
    try {
      article = renderMarkdown(markdownBody(*content), link_context, &context.outlines[index], code_renderer);
    } catch (const std::length_error& bound) {
      siteError("'" + page.source + "' exceeds a rendering bound: " + bound.what());
    }
    if (page.hide_title) article = withoutTitle(std::move(article));
    std::string banner;  // a home page's hero and features, kept out of its search text
    if (page.landing) {
      banner = renderHome(*page.landing, link_context.resolver,
                          [&](const std::string& warning) { result.warnings.push_back(page.source + ": " + warning); },
                          model.ui.features);
    }
    broken_links.appendTo(result.broken_links);
    scanArticle(banner, ids[index], links[index]);
    scanArticle(article, ids[index], links[index]);
    if (leads != nullptr) {
      leads->resize(model.pages.size());
      (*leads)[index] = pageLead(pageSearchSections(article, context.outlines[index]));
    }
    if (model.config.search && page.search) {
      search_entries.push_back(searchPageEntryJson(page.title, page.output,
                                                   page.description.empty() && page.home ? model.config.description
                                                                                         : page.description,
                                                   page.tags, pageSearchSections(article, context.outlines[index])));
    }
    ShellInput input;
    input.title = page.title;
    input.description = page.description.empty() && page.home ? model.config.description : page.description;
    input.output = page.output;
    input.article = banner + withByline(std::move(article), page);
    input.outline = &context.outlines[index];
    input.page = index;
    if (llms_enabled) {
      try {
        // The same page again, without diagrams; what it reports was
        // reported by the render above already.
        LinkContext quiet = link_context;
        quiet.reporter = {};
        std::string clean = renderMarkdown(markdownBody(*content), quiet);
        if (page.hide_title) clean = withoutTitle(std::move(clean));
        if (page.landing) clean = renderHome(*page.landing, link_context.resolver, [](const std::string&) {}, model.ui.features) + clean;
        writeFile(page.output + ".md", markdownPage(withByline(std::move(clean), page),
            markdownPageLinks(model, prefix, portal, prefix + out_dir)));
      } catch (const std::length_error& bound) {
        siteError("'" + page.source + "' exceeds a Markdown rendering bound: " + bound.what());
      }
    }
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
  const std::size_t anchors_before = result.broken_anchors.size();
  for (std::size_t index = 0; index < model.pages.size(); ++index) {
    // A fallback page's fragments were checked in its own language; the
    // translations it now links to may name their headings differently.
    if (model.pages[index].fallback) continue;
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
  collapseRepeats(result.broken_anchors, anchors_before, "links");

  // The site index: every tab, page, and h2/h3, as plain nested lists.
  {
    std::string body = "<h1>" + escapeHtml(model.ui.site_index) + "</h1>";
    const std::string from_dir;  // the index page sits at the site root
    const auto pageEntry = [&](std::size_t index) {
      const auto& page = model.pages[index];
      std::string entry = "<a href=\"" + escapeHtml(relativeUrl(from_dir, page.output)) + "\">" + escapeHtml(page.title) + "</a>";
      // The page's front matter description, as a one-line summary under its link.
      const std::string& summary = page.description.empty() && page.home ? model.config.description : page.description;
      if (!summary.empty()) entry += "<p class=\"index-description\">" + escapeHtml(summary) + "</p>";
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
      body += "<h2>" + escapeHtml(model.ui.other_pages) + "</h2><ul>";
      for (const auto index : model.unlisted) body += "<li>" + pageEntry(index) + "</li>";
      body += "</ul>";
    }
    ShellInput input;
    input.title = model.ui.site_index;
    input.output = std::string(kDocsSiteIndexPage);
    input.article = "<div class=\"site-index\">" + body + "</div>";
    if (llms_enabled) writeFile(input.output + ".md", markdownPage(input.article, markdownPageLinks(model, prefix, portal, prefix)));
    writeFile(input.output, renderShell(context, input));
  }

  // The tag pages: every tag with its count, and each tag's pages.
  if (!model.tags.empty()) {
    const auto countText = [&](std::size_t count) {
      return count == 1 ? model.ui.page_count_one : fillDocsUiText(model.ui.page_count, {{"count", std::to_string(count)}});
    };
    std::string index_body = "<h1>" + escapeHtml(model.ui.tags) + "</h1><ul class=\"tag-list\">";
    for (const auto& [tag, pages] : model.tags) {
      const std::string& output = context.tag_outputs.at(tag);
      index_body += "<li><a href=\"" + escapeHtml(relativeUrl("tags", output)) + "\">" + escapeHtml(tag) + "</a> <span class=\"tag-count\">" +
                    escapeHtml(countText(pages.size())) + "</span></li>";
      const std::string title = fillDocsUiText(model.ui.tagged, {{"tag", tag}});
      std::string body = "<h1>" + escapeHtml(title) + "</h1><ul class=\"tagged-pages\">";
      for (const auto page_index : pages) {
        const DocsPage& page = model.pages[page_index];
        body += "<li><a href=\"" + escapeHtml(relativeUrl("tags", page.output)) + "\">" + escapeHtml(page.title) + "</a>";
        if (!page.description.empty()) body += "<p class=\"index-description\">" + escapeHtml(page.description) + "</p>";
        body += "</li>";
      }
      body += "</ul><p class=\"all-tags\"><a href=\"index.html\">" + escapeHtml(model.ui.tags) + "</a></p>";
      ShellInput input;
      input.title = title;
      input.output = output;
      input.article = "<div class=\"tag-page\">" + body + "</div>";
      if (llms_enabled) writeFile(output + ".md", markdownPage(input.article, markdownPageLinks(model, prefix, portal, prefix + "tags")));
      writeFile(output, renderShell(context, input));
      ++result.pages_written;
    }
    index_body += "</ul>";
    ShellInput input;
    input.title = model.ui.tags;
    input.output = "tags/index.html";
    input.article = "<div class=\"tag-page\">" + index_body + "</div>";
    if (llms_enabled) writeFile(input.output + ".md", markdownPage(input.article, markdownPageLinks(model, prefix, portal, prefix + "tags")));
    writeFile(input.output, renderShell(context, input));
    ++result.pages_written;
  }

  // Assets.
  for (const auto& [output, source] : assets) {
    const auto asset = readFile(model.root / source, kMaximumPagesFileBytes);
    if (!asset) siteError("cannot read '" + source + "'");
    writeFile(output, *asset);
    ++result.assets_copied;
  }

  // The translations, each below its language's directory.
  for (const auto& translation : model.translations) {
    renderSite(translation, tree, prefix + translation.lang + "/", portal, options, result, nullptr, languages,
               context.languages_prefix);
  }
}

// One site's pages as sitemap.xml and llms.txt list them, below `prefix`
// ("" or "<id>/"): a section per tab in nav order, then the pages outside the
// nav under "Optional".
struct ListingPage {
  std::string title;
  std::string path;  // output-root-relative
  std::string description;
  std::string date;
};
struct ListingSection {
  std::string title;
  std::vector<ListingPage> pages;
};

// Pages without a description fall back to their opening text, `leads`.
std::vector<ListingSection> siteListing(const DocsSiteModel& model, const std::string& prefix,
                                        const std::string& title_prefix, bool with_unlisted,
                                        const std::vector<std::string>& leads) {
  std::vector<ListingSection> sections;
  const auto entry = [&](std::size_t index) {
    const DocsPage& page = model.pages[index];
    std::string summary = page.description.empty() && page.home ? model.config.description : page.description;
    if (summary.empty() && index < leads.size()) summary = leads[index];
    return ListingPage{page.title, prefix + page.output, summary, page.date};
  };
  for (const auto& tab : model.nav) {
    std::vector<std::size_t> order;
    collectReadingOrder(tab, order);
    ListingSection section{title_prefix + tab.title, {}};
    for (const auto index : order) section.pages.push_back(entry(index));
    if (!section.pages.empty()) sections.push_back(std::move(section));
  }
  if (with_unlisted && !model.unlisted.empty()) {
    ListingSection optional{"Optional", {}};
    for (const auto index : model.unlisted) optional.pages.push_back(entry(index));
    sections.push_back(std::move(optional));
  }
  return sections;
}

// The translations' own pages, in their reading order, below `prefix`.
std::vector<ListingPage> translatedListing(const DocsSiteModel& model, const std::string& prefix) {
  std::vector<ListingPage> pages;
  for (const auto& translation : model.translations) {
    const auto entry = [&](std::size_t index) {
      const DocsPage& page = translation.pages[index];
      if (!page.fallback) pages.push_back({page.title, prefix + translation.lang + "/" + page.output, page.description, page.date});
    };
    for (const auto index : translation.reading_order) entry(index);
    for (const auto index : translation.unlisted) entry(index);
  }
  return pages;
}

std::string escapeXml(std::string_view value) {
  std::string out;
  for (const char byte : value) {
    switch (byte) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += byte;
    }
  }
  return out;
}

// Markdown link text and descriptions on one line, with the characters that
// would end a link label escaped.
std::string llmsText(std::string_view value) {
  std::string out;
  for (const char byte : value) {
    if (byte == '\n' || byte == '\r') {
      out += ' ';
    } else {
      if (byte == '[' || byte == ']' || byte == '\\') out += '\\';
      out += byte;
    }
  }
  return out;
}

// sitemap.xml (only with a base URL) and llms.txt at the output root.
// `translated` lists the translations' own pages (not fallbacks), which the
// sitemap holds too; llms.txt keeps to the default language.
void writeListings(OutputTree& tree, const std::string& title, const std::string& description, const std::string& base_url,
                   const std::vector<ListingSection>& sections, bool llms_enabled,
                   const std::vector<ListingPage>& translated = {}) {
  const auto href = [&](const std::string& path) {
    const std::string encoded = encodePathSegment(path);
    return base_url.empty() ? encoded : base_url + encoded;
  };
  for (const std::string_view name : {kDocsSitemapFile, kDocsLlmsFile}) {
    if (name == kDocsLlmsFile && !llms_enabled) continue;
    if (tree.written(std::string(name))) siteError("'" + std::string(name) + "' would overwrite another output");
  }
  if (!base_url.empty()) {
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n";
    std::set<std::string> listed;
    for (const auto& section : sections) {
      for (const auto& page : section.pages) {
        if (!listed.insert(page.path).second) continue;
        xml += "  <url><loc>" + escapeXml(href(page.path)) + "</loc>";
        if (!page.date.empty()) xml += "<lastmod>" + page.date + "</lastmod>";
        xml += "</url>\n";
      }
    }
    for (const auto& page : translated) {
      if (!listed.insert(page.path).second) continue;
      xml += "  <url><loc>" + escapeXml(href(page.path)) + "</loc>";
      if (!page.date.empty()) xml += "<lastmod>" + page.date + "</lastmod>";
      xml += "</url>\n";
    }
    xml += "</urlset>\n";
    tree.write(std::string(kDocsSitemapFile), xml);
  }
  if (!llms_enabled) return;
  std::string text = "# " + llmsText(title) + "\n";
  if (!description.empty()) text += "\n> " + llmsText(description) + "\n";
  for (const auto& section : sections) {
    text += "\n## " + llmsText(section.title) + "\n\n";
    for (const auto& page : section.pages) {
      text += "- [" + llmsText(page.title) + "](" + href(page.path + ".md") + ")";
      if (!page.description.empty()) text += ": " + llmsText(page.description);
      text += "\n";
    }
  }
  tree.write(std::string(kDocsLlmsFile), text);
}

}  // namespace

void buildDocsSite(const DocsSiteModel& model, const fs::path& out_path, const DocsBuildOptions& options,
                   DocsBuildReport* report) {
  DocsBuildReport local_report;
  DocsBuildReport& result = report != nullptr ? *report : local_report;
  result = DocsBuildReport{};
  if (model.pages.empty()) siteError("the site has no pages");
  OutputTree tree(out_path, options.clean, result);
  std::vector<std::string> leads;
  renderSite(model, tree, "", nullptr, options, result, &leads);
  writeListings(tree, model.title, model.config.description, model.config.url, siteListing(model, "", "", true, leads),
                model.config.llms, translatedListing(model, ""));
  tree.commit(buildVersion());
}


// ---- several sites in one build (a portal) ----------------------------------

namespace {

// A failure inside one site, restated with the site's id.
[[noreturn]] void rethrowForSite(const std::string& id, const std::exception& failure) {
  std::string message = failure.what();
  if (message.starts_with("ckdocs: ")) message.erase(0, 8);
  siteError("site '" + id + "': " + message);
}

std::string trimmedOutput(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '\0')) text.pop_back();
  return text;
}

// The committed tree of `entry.ref` -- or of `origin/<ref>`, the form a
// CI clone that fetched the branch holds -- extracted with `git archive`
// into a fresh directory below the portal's private scratch directory.
fs::path extractRef(const fs::path& root, const DocsSiteEntry& entry, std::shared_ptr<const fs::path>& scratch,
                    std::size_t number) {
  const auto unavailable = [&]() {
    siteError("site '" + entry.id + "': ref '" + entry.ref +
              "' is not available (no Git repository here, or a clone without that branch); fetch it, or pass --ref-tree " +
              entry.ref + "=DIR");
  };
  std::error_code error;
  if (!fs::exists(root / ".git", error)) unavailable();
  ProcessOptions resolve;
  resolve.timeout = std::chrono::seconds(20);
  resolve.output_limit = 256;
  std::string commit;
  for (const auto& candidate : {entry.ref, "origin/" + entry.ref}) {
    const auto resolved = runProcess({"git", "-C", root.string(), "rev-parse", "--verify", "--quiet", "--end-of-options",
                                      candidate + "^{commit}"},
                                     resolve);
    if (resolved.exit_code == 0 && !resolved.timed_out) {
      commit = trimmedOutput(resolved.output);
      break;
    }
  }
  if (commit.empty() || !std::all_of(commit.begin(), commit.end(), [](unsigned char byte) { return std::isxdigit(byte) != 0; })) {
    unavailable();
  }
  if (!scratch) {
    auto pattern = (fs::temp_directory_path() / "ckdocs-refs-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    if (mkdtemp(writable.data()) == nullptr) siteError("cannot create a temporary directory for ref trees");
    scratch = std::shared_ptr<const fs::path>(new fs::path(writable.data()), [](const fs::path* path) {
      std::error_code ignored;
      fs::remove_all(*path, ignored);
      delete path;
    });
  }
  const fs::path tree = *scratch / ("tree-" + std::to_string(number));
  const fs::path archive = *scratch / ("tree-" + std::to_string(number) + ".tar");
  fs::create_directories(tree, error);
  if (error) siteError("cannot create a directory for ref '" + entry.ref + "': " + error.message());
  ProcessOptions extract;
  extract.timeout = std::chrono::seconds(120);
  extract.output_limit = 64 * 1024;
  // --output writes the archive itself; runProcess merges stdout and stderr,
  // so the tar stream never passes through it.
  const auto archived =
      runProcess({"git", "-C", root.string(), "archive", "--format=tar", "--output=" + archive.string(), commit}, extract);
  if (archived.exit_code != 0 || archived.timed_out) {
    siteError("site '" + entry.id + "': git archive of ref '" + entry.ref + "' failed: " + trimmedOutput(archived.output));
  }
  const auto unpacked = runProcess({"tar", "-xf", archive.string(), "-C", tree.string()}, extract);
  if (unpacked.exit_code != 0 || unpacked.timed_out) {
    siteError("site '" + entry.id + "': unpacking ref '" + entry.ref + "' failed: " + trimmedOutput(unpacked.output));
  }
  fs::remove(archive, error);
  return tree;
}

// A small page standing at a site's old, root-level URL: an immediate
// refresh to the page's new place, its canonical link, and a visible link
// for anything that does not follow the refresh.
std::string renderRedirectPage(const std::string& target, const std::string& title, const std::string& output, bool llms,
                               const DocsSiteModel& site) {
  const auto href = escapeHtml(target);
  return "<!doctype html><html lang=\"" + escapeHtml(site.lang) + "\"" + (docsLanguageIsRightToLeft(site.lang) ? " dir=\"rtl\"" : "") +
         "><head><meta charset=\"utf-8\"><title>" + escapeHtml(title) +
         "</title><meta http-equiv=\"refresh\" content=\"0; url=" + href + "\"><link rel=\"canonical\" href=\"" + href +
         "\"><meta name=\"robots\" content=\"noindex\">" + (llms ? llmsDiscovery(directoryOf(output), output) : "") +
         "</head><body><p>" + escapeHtml(site.ui.moved) + " <a href=\"" + href + "\">" + escapeHtml(title) +
         "</a>.</p></body></html>\n";
}

// The welcome page: the portal's own shell (brand, a selector with no
// current site, the portal's links; no tabs, sidebar or search), its title,
// description and intro, then a card per site.
std::string renderWelcomePage(const DocsPortalModel& portal, const PortalShell& shell, const std::string& version,
                              const std::string& intro) {
  const DocsConfig& config = portal.config;
  const DocsUiText& ui = shell.ui;
  const std::string lang = config.lang.empty() ? std::string("en") : config.lang;
  std::string out = "<!doctype html><html lang=\"" + escapeHtml(lang) + "\"" + (docsLanguageIsRightToLeft(lang) ? " dir=\"rtl\"" : "") +
                    "><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  out += "<title>" + escapeHtml(config.title) + "</title>";
  if (!config.description.empty()) out += "<meta name=\"description\" content=\"" + escapeHtml(config.description) + "\">";
  out += "<meta name=\"generator\" content=\"ckdocs " + escapeHtml(version) + "\">";
  if (config.llms) out += llmsDiscovery("", "index.html");
  if (!shell.favicon_output.empty()) out += "<link rel=\"icon\" href=\"" + escapeHtml(relativeUrl("", shell.favicon_output)) + "\">";
  out += "<style>";
  out += kDocsStyles;
  out += kMarkdownStyles;
  out += "</style>";
  if (!shell.stylesheet_output.empty()) out += "<link rel=\"stylesheet\" href=\"" + escapeHtml(relativeUrl("", shell.stylesheet_output)) + "\">";
  out += "</head><body><a class=\"skip-link\" href=\"#main-content\">" + escapeHtml(ui.skip_to_content) + "</a>";
  out += "<header class=\"site\"><a class=\"brand\" href=\"index.html\">";
  if (!shell.logo_output.empty()) out += "<img src=\"" + escapeHtml(relativeUrl("", shell.logo_output)) + "\" alt=\"\">";
  out += escapeHtml(config.brand.empty() ? config.title : config.brand) + "</a>";
  out += renderSiteSelector(portal, std::nullopt, "", ui);
  out += renderHeaderLinks(config.links, ui);
  out += "</header><div class=\"page portal\"><main id=\"main-content\"><article><h1>" + escapeHtml(config.title) + "</h1>";
  if (!config.description.empty()) out += "<p class=\"portal-description\">" + escapeHtml(config.description) + "</p>";
  out += intro;
  out += "<ul class=\"site-cards\">";
  for (const auto& site : portal.sites) {
    out += "<li><a href=\"" + escapeHtml(relativeUrl("", site.entry.id + "/" + site.model.pages[site.model.home].output)) + "\">" +
           escapeHtml(site.entry.title) + "</a>";
    if (!site.entry.description.empty()) out += "<p>" + escapeHtml(site.entry.description) + "</p>";
    out += "</li>";
  }
  out += "</ul></article></main></div><footer class=\"site\">";
  if (!config.footer.empty()) out += "<span>" + escapeHtml(config.footer) + "</span>";
  out += "<span>" + escapeHtml(fillDocsUiText(ui.built_with, {{"generator", "ckdocs " + version}})) + "</span>";
  if (config.llms) out += "<a href=\"index.html.md\" title=\"" + escapeHtml(ui.markdown_version) + "\">Markdown</a>";
  return out + "</footer></body></html>";
}

}  // namespace

DocsPortalModel loadDocsPortal(const fs::path& root, const DocsConfig& config, const DocsPortalOptions& options,
                               std::vector<std::string>* warnings) {
  if (config.sites.empty()) siteError("the config lists no 'sites'");
  std::error_code error;
  if (!fs::is_directory(root, error)) siteError("'" + root.string() + "' is not a directory");
  DocsPortalModel portal;
  portal.root = root;
  portal.config = config;
  if (!config.home.empty()) {
    const auto intro = root / config.home;
    if (fs::is_symlink(intro, error) || !fs::is_regular_file(intro, error)) {
      siteError("home page '" + config.home + "' (the welcome page's intro) is not a regular file");
    }
  }

  std::map<std::string, fs::path> trees;  // ref -> the site root holding its tree
  for (const DocsSiteEntry& entry : config.sites) {
    fs::path site_root = root;
    if (!entry.ref.empty()) {
      if (const auto known = trees.find(entry.ref); known != trees.end()) {
        site_root = known->second;
      } else if (const auto given = options.ref_trees.find(entry.ref); given != options.ref_trees.end()) {
        site_root = fs::absolute(given->second, error);
        if (error || !fs::is_directory(site_root, error)) {
          siteError("site '" + entry.id + "': --ref-tree " + entry.ref + "=" + given->second.string() + " is not a directory");
        }
      } else {
        site_root = extractRef(root, entry, portal.extracted, trees.size());
      }
      trees.emplace(entry.ref, site_root);
    }

    // The site's own config, relative to its root.
    const std::string config_path = entry.config.empty() ? std::string("ckdocs.yml") : entry.config;
    const std::string where = entry.ref.empty() ? std::string() : " in ref '" + entry.ref + "'";
    DocsConfig site_config;
    const auto file = site_root / config_path;
    if (fs::exists(file, error)) {
      if (fs::is_symlink(file, error) || !fs::is_regular_file(file, error)) {
        siteError("site '" + entry.id + "': config '" + config_path + "'" + where + " is not a regular file");
      }
      const auto content = readFile(file, kMaximumDocsConfigBytes + 1);
      if (!content) siteError("site '" + entry.id + "': cannot read config '" + config_path + "'" + where);
      try {
        site_config = parseDocsConfig(*content);
      } catch (const std::exception& failure) {
        siteError("site '" + entry.id + "': config '" + config_path + "'" + where + ": " + failure.what());
      }
      if (!site_config.sites.empty()) {
        siteError("site '" + entry.id + "': config '" + config_path + "'" + where +
                  " lists 'sites' itself; a site needs an ordinary single-site config (set 'config' to one)");
      }
    } else if (!entry.config.empty()) {
      siteError("site '" + entry.id + "': config '" + config_path + "'" + where + " does not exist");
    }
    if (site_config.title.empty()) site_config.title = entry.title;
    site_config.drafts = options.drafts;

    std::vector<std::string> site_warnings;
    DocsPortalSite site;
    site.entry = entry;
    try {
      site.model = loadDocsSite(site_root, site_config, &site_warnings);
    } catch (const std::exception& failure) {
      rethrowForSite(entry.id, failure);
    }
    if (warnings != nullptr) {
      for (const auto& warning : site_warnings) warnings->push_back(entry.id + ": " + warning);
    }
    portal.sites.push_back(std::move(site));
  }
  return portal;
}

void buildDocsPortal(const DocsPortalModel& portal, const fs::path& out_path, const DocsBuildOptions& options,
                     DocsBuildReport* report) {
  DocsBuildReport local_report;
  DocsBuildReport& result = report != nullptr ? *report : local_report;
  result = DocsBuildReport{};
  if (portal.sites.empty()) siteError("the portal has no sites");
  for (const auto& site : portal.sites) {
    if (site.model.pages.empty()) siteError("site '" + site.entry.id + "': the site has no pages");
  }
  std::error_code error;
  OutputTree tree(out_path, options.clean, result);
  const std::string version = buildVersion();

  // The portal's own files (logo, stylesheet, what the intro references)
  // land at their root-relative path, which must stay out of every site's
  // directory.
  std::set<std::string> portal_assets;
  const auto siteOwning = [&portal](std::string_view path) -> const DocsPortalSite* {
    const auto first = path.substr(0, path.find('/'));
    for (const auto& site : portal.sites) {
      if (site.entry.id == first) return &site;
    }
    return nullptr;
  };
  const auto addPortalAsset = [&](const std::string& root_relative, const std::string& what) {
    const auto full = portal.root / root_relative;
    if (fs::is_symlink(full, error) || !fs::is_regular_file(full, error)) siteError(what + " '" + root_relative + "' is not a regular file");
    if (fs::file_size(full, error) > kMaximumPagesFileBytes) siteError(what + " '" + root_relative + "' exceeds the file size limit");
    if (const auto* owner = siteOwning(root_relative)) {
      siteError(what + " '" + root_relative + "' would be written inside site '" + owner->entry.id + "'s directory");
    }
    portal_assets.insert(root_relative);
    return root_relative;
  };
  PortalShell shell;
  shell.portal = &portal;
  shell.ui = docsUiTextFor(portal.config.lang.empty() ? std::string("en") : portal.config.lang);
  for (const auto& [key, text] : portal.config.ui) setDocsUiText(shell.ui, key, text);
  if (!portal.config.logo.empty()) shell.logo_output = addPortalAsset(portal.config.logo, "site.logo");
  if (!portal.config.favicon.empty()) shell.favicon_output = addPortalAsset(portal.config.favicon, "site.favicon");
  if (!portal.config.stylesheet.empty()) shell.stylesheet_output = addPortalAsset(portal.config.stylesheet, "site.stylesheet");

  // Every site below its id, its findings prefixed with it.
  std::vector<std::vector<std::string>> site_leads(portal.sites.size());
  for (std::size_t index = 0; index < portal.sites.size(); ++index) {
    const auto& site = portal.sites[index];
    shell.current = index;
    const auto links_before = result.broken_links.size();
    const auto anchors_before = result.broken_anchors.size();
    const auto warnings_before = result.warnings.size();
    try {
      renderSite(site.model, tree, site.entry.id + "/", &shell, options, result, &site_leads[index]);
    } catch (const std::exception& failure) {
      rethrowForSite(site.entry.id, failure);
    }
    for (auto at = links_before; at < result.broken_links.size(); ++at) result.broken_links[at].insert(0, site.entry.id + ": ");
    for (auto at = anchors_before; at < result.broken_anchors.size(); ++at) result.broken_anchors[at].insert(0, site.entry.id + ": ");
    for (auto at = warnings_before; at < result.warnings.size(); ++at) result.warnings[at].insert(0, site.entry.id + ": ");
  }

  // The welcome page, with the optional intro. Its links resolve within the
  // work tree root (a file there is copied like any page's asset) or to a
  // page of one of the sites, as `<id>/<page>.html`; a fragment-only link
  // must name one of its own headings.
  std::string intro;
  if (!portal.config.home.empty()) {
    const std::string& source = portal.config.home;
    const auto content = readFile(portal.root / source, kMaximumMarkdownInputBytes);
    if (!content) siteError("cannot read '" + source + "', or it exceeds the 512 KiB page limit");
    if (!isValidUtf8(*content)) siteError("'" + source + "' is not valid UTF-8");
    const std::string page_dir(directoryOf(source));
    LinkFindings broken_links;
    LinkContext link_context;
    link_context.first_line = bodyFirstLine(*content);
    link_context.resolver = [&](std::string_view target, bool image, std::size_t line) -> std::optional<std::string> {
      const auto broken = [&](const std::string& reason) {
        broken_links.add(source, std::string(image ? "image" : "link") + " target '" + std::string(target) + "' " + reason, line);
        return std::optional<std::string>{};
      };
      std::string joined;
      if (target.starts_with('/')) joined = std::string(target.substr(1));
      else joined = page_dir.empty() ? std::string(target) : page_dir + "/" + std::string(target);
      const auto normalized = normalizePath(joined);
      if (!normalized) return broken("leaves the repository");
      if (const auto* owner = siteOwning(*normalized)) {
        const auto rest = normalized->size() > owner->entry.id.size() ? normalized->substr(owner->entry.id.size() + 1) : std::string();
        const std::string page = rest.empty() ? std::string("index.html") : rest;
        for (const auto& candidate : owner->model.pages) {
          if (candidate.output == page) return relativeUrl("", owner->entry.id + "/" + page);
        }
        return broken("is not a page of site '" + owner->entry.id + "'");
      }
      if (normalized->empty() || target.ends_with('/')) return broken("is a directory");
      if (hasMarkdownExtension(*normalized)) return broken("is a Markdown file; link to a site's page as <id>/<page>.html");
      const auto full = portal.root / *normalized;
      if (fs::is_symlink(full, error) || !fs::is_regular_file(full, error)) return broken("does not exist");
      if (fs::file_size(full, error) > kMaximumPagesFileBytes) return broken("exceeds the file size limit");
      return relativeUrl("", addPortalAsset(*normalized, "file"));
    };
    std::vector<MarkdownHeading> outline;
    try {
      intro = renderMarkdown(markdownBody(*content), link_context, &outline);
    } catch (const std::length_error& bound) {
      siteError("'" + source + "' exceeds a rendering bound: " + bound.what());
    }
    broken_links.appendTo(result.broken_links);
    std::set<std::string> ids;
    std::vector<std::pair<std::string, std::string>> links;
    scanArticle(intro, ids, links);
    for (const auto& [path, fragment] : links) {
      const auto anchor = decodePathSegment(fragment).value_or(fragment);
      if (path.empty() && ids.count(anchor) == 0) {
        result.broken_anchors.push_back(source + ": '#" + fragment + "' names no heading on " + source);
      }
    }
  }
  if (portal.config.llms) {
    std::string clean = "<h1>" + escapeHtml(portal.config.title) + "</h1>";
    if (!portal.config.description.empty()) clean += "<p>" + escapeHtml(portal.config.description) + "</p>";
    clean += intro + "<ul>";
    for (const auto& site : portal.sites) {
      clean += "<li><a href=\"" + escapeHtml(site.entry.id + "/" + site.model.pages[site.model.home].output) + "\">" +
               escapeHtml(site.entry.title) + "</a>";
      if (!site.entry.description.empty()) clean += "<p>" + escapeHtml(site.entry.description) + "</p>";
      clean += "</li>";
    }
    clean += "</ul>";
    tree.write("index.html.md", markdownPage(clean, markdownPageLinks(portal.sites.front().model, "", &shell, "")));
  }
  tree.write("index.html", renderWelcomePage(portal, shell, version, intro));
  ++result.pages_written;
  for (const auto& asset : portal_assets) {
    if (tree.written(asset)) siteError("'" + asset + "' would overwrite another output of the portal");
    const auto content = readFile(portal.root / asset, kMaximumPagesFileBytes);
    if (!content) siteError("cannot read '" + asset + "'");
    tree.write(asset, *content);
    ++result.assets_copied;
  }

  // Old root-level URLs of the site that used to be the whole build.
  for (const auto& site : portal.sites) {
    if (!site.entry.redirect_from_root) continue;
    for (const auto& page : site.model.pages) {
      if (page.output == "index.html") continue;  // the portal's welcome page stands there now
      const auto fail = [&](const std::string& reason) {
        siteError("site '" + site.entry.id + "': redirect_from_root would write '" + page.output + "', " + reason +
                  "; rename the page or drop redirect_from_root");
      };
      if (const auto* owner = siteOwning(page.output)) fail("inside site '" + owner->entry.id + "'s directory");
      if (tree.written(page.output)) fail("which another output of the portal already uses");
      const auto target = relativeUrl(directoryOf(page.output), site.entry.id + "/" + page.output);
      tree.write(page.output, renderRedirectPage(target, page.title, page.output, portal.config.llms, site.model));
      if (portal.config.llms) {
        if (tree.written(page.output + ".md")) fail("which another Markdown output of the portal already uses");
        tree.write(page.output + ".md", "# " + markdownText(page.title) + "\n\n" + markdownText(site.model.ui.moved) + " [" +
                   markdownText(page.title) + "](<" + target + ".md>).\n");
      }
      ++result.redirects_written;
    }
  }
  std::vector<ListingSection> sections;
  sections.push_back({"Overview", {{portal.config.title, "index.html", portal.config.description, ""}}});
  for (std::size_t index = 0; index < portal.sites.size(); ++index) {
    const auto& site = portal.sites[index];
    // Each site's pages outside its nav join one "Optional" section below.
    for (auto& section : siteListing(site.model, site.entry.id + "/", site.entry.title + ": ", false, site_leads[index])) {
      sections.push_back(std::move(section));
    }
  }
  ListingSection optional{"Optional", {}};
  for (std::size_t site_index = 0; site_index < portal.sites.size(); ++site_index) {
    const auto& site = portal.sites[site_index];
    for (const auto index : site.model.unlisted) {
      const DocsPage& page = site.model.pages[index];
      const auto& leads = site_leads[site_index];
      const std::string summary = !page.description.empty() ? page.description : index < leads.size() ? leads[index] : "";
      optional.pages.push_back({site.entry.title + ": " + page.title, site.entry.id + "/" + page.output, summary, page.date});
    }
  }
  if (!optional.pages.empty()) sections.push_back(std::move(optional));
  std::vector<ListingPage> translated;
  for (const auto& site : portal.sites) {
    for (auto& page : translatedListing(site.model, site.entry.id + "/")) translated.push_back(std::move(page));
  }
  writeListings(tree, portal.config.title, portal.config.description, portal.config.url, sections, portal.config.llms, translated);
  tree.commit(version);
}

}  // namespace ckgit
