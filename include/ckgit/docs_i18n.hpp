// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ckgit {

// The words a ckdocs page writes around its content -- navigation labels,
// the search box, the footer, alert and container titles, the copy button --
// in the page's language. Built in for the languages listed by
// docsUiLanguages(); a site may override any of them per language with
// `ui:` in ckdocs.yml, and a language without a built-in set starts from
// English. `{language}`, `{default}`, `{tag}`, `{count}` and `{generator}`
// in a text are placeholders the renderer fills in.
struct DocsUiText {
  std::string skip_to_content = "Skip to content";
  std::string menu = "Menu";
  std::string home = "Home";
  std::string on_this_page = "On this page";
  std::string previous = "Previous";
  std::string next = "Next";
  std::string previous_next = "Previous and next page";
  std::string search = "Search";
  std::string search_label = "Search this site";
  std::string site_index = "Site index";
  std::string other_pages = "Other pages";
  std::string built_with = "Built with {generator}";
  std::string sections = "Sections";
  std::string pages = "Pages";
  std::string breadcrumb = "Breadcrumb";
  std::string links = "Links";
  std::string sites = "Sites";
  std::string documentation_sites = "Documentation sites";
  std::string languages = "Languages";
  std::string not_translated =
      "This page has not been translated into {language} yet; it is shown in the original language ({default}).";
  std::string tags = "Tags";
  std::string tagged = "Tagged “{tag}”";
  std::string page_count = "{count} pages";
  std::string page_count_one = "1 page";
  std::string copy_code = "Copy code";
  std::string copied = "Copied";
  std::string moved = "This page has moved to";
  std::string note = "Note";
  std::string tip = "Tip";
  std::string important = "Important";
  std::string warning = "Warning";
  std::string caution = "Caution";
  std::string info = "Info";
  std::string danger = "Danger";
  std::string details = "Details";
  std::string footnotes = "Footnotes";
  std::string back_to_reference = "Back to reference";
  std::string overview = "Overview";
  std::string features = "Features";
  std::string search_open = "Open search";
  std::string search_close = "Close search";
  std::string theme = "Colour theme";
  std::string theme_system = "System theme";
  std::string theme_light = "Light theme";
  std::string theme_dark = "Dark theme";
  std::string diagram_open = "Open diagram";
  std::string diagram_expand = "Expand diagram";
  std::string diagram_close = "Close diagram";
  std::string markdown_version = "Read this page as plain Markdown";
  std::string llms_index = "Read the documentation index for agents";
};

// The keys `ui:` accepts, in the order DocsUiText declares them.
const std::vector<std::string_view>& docsUiKeys();

// Sets the text `key` names; false when no such key exists.
bool setDocsUiText(DocsUiText& text, std::string_view key, std::string value);

// The built-in text for a BCP 47 language tag: an exact match ("pt-BR",
// "zh-TW"), else its primary language ("de" for "de-CH"), else English.
DocsUiText docsUiTextFor(std::string_view lang);

// The languages with a built-in set, as their tags.
std::vector<std::string_view> docsUiLanguages();

// The language's own name for itself ("Deutsch" for "de"), for a language
// menu that has no configured label; the tag itself when unknown.
std::string docsLanguageName(std::string_view lang);

// Whether the language is written right to left (Arabic, Hebrew, Persian,
// Urdu, ...), for the page's `dir` attribute.
bool docsLanguageIsRightToLeft(std::string_view lang);

// A well-formed BCP 47 tag as ckdocs accepts it: a primary language of two
// or three letters, then subtags of 2 to 8 letters or digits, separated by
// `-`, at most 35 bytes ("en", "de-CH", "zh-Hant-TW", "pt-BR").
bool isValidDocsLanguageTag(std::string_view lang);

// `text` with each `{name}` replaced by its value.
std::string fillDocsUiText(std::string_view text, const std::map<std::string, std::string>& values);

}  // namespace ckgit
