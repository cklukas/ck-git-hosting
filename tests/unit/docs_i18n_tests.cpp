// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ckgit/docs_i18n.hpp"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("docs i18n: " + message);
}

}  // namespace

void testDocsI18n() {
  using namespace ckgit;
  const DocsUiText english;
  require(docsUiTextFor("en").on_this_page == "On this page" && docsUiTextFor("xx").on_this_page == "On this page",
          "English is the default and the fallback");
  require(docsUiTextFor("de").on_this_page == "Auf dieser Seite" && docsUiTextFor("de-CH").on_this_page == "Auf dieser Seite" &&
              docsUiTextFor("DE").home == "Startseite",
          "a regional tag and any case find the language");
  require(docsUiTextFor("zh-TW").search == "搜尋" && docsUiTextFor("zh-Hant").search == "搜尋" && docsUiTextFor("zh-Hans").search == "搜索" &&
              docsUiTextFor("zh").search == "搜索",
          "Chinese in traditional and simplified script");
  require(docsUiTextFor("nn").next == docsUiTextFor("nb").next && docsUiTextFor("no").next == "Neste", "Norwegian tags share a set");
  // Every built-in language sets every text, and keeps every placeholder.
  for (const auto lang : docsUiLanguages()) {
    const auto text = docsUiTextFor(lang);
    for (const auto key : docsUiKeys()) {
      DocsUiText probe = text;
      require(setDocsUiText(probe, key, "probe"), "a listed key can be set: " + std::string(key));
    }
    if (lang == "en") continue;
    require(text.on_this_page != english.on_this_page && text.home != english.home && text.copy_code != english.copy_code &&
                text.back_to_reference != english.back_to_reference,
            "a translation translates: " + std::string(lang));
    require(text.not_translated.find("{language}") != std::string::npos && text.not_translated.find("{default}") != std::string::npos &&
                text.built_with.find("{generator}") != std::string::npos && text.tagged.find("{tag}") != std::string::npos &&
                text.page_count.find("{count}") != std::string::npos,
            "a translation keeps its placeholders: " + std::string(lang));
  }
  DocsUiText unknown;
  require(docsUiKeys().size() == 49 && !setDocsUiText(unknown, "nope", "x"), "49 keys; an unknown one is refused");

  require(docsLanguageName("de") == "Deutsch" && docsLanguageName("pt-BR") == "Português (Brasil)" && docsLanguageName("de-AT") == "Deutsch" &&
              docsLanguageName("zh-Hant-TW") == "繁體中文" && docsLanguageName("tlh") == "tlh",
          "languages name themselves; an unknown tag names itself");
  require(docsLanguageIsRightToLeft("ar") && docsLanguageIsRightToLeft("he-IL") && docsLanguageIsRightToLeft("fa") &&
              !docsLanguageIsRightToLeft("en") && !docsLanguageIsRightToLeft("de"),
          "right-to-left scripts");
  for (const auto* tag : {"en", "de-CH", "zh-Hant-TW", "pt-BR", "gsw", "sr-Latn"}) require(isValidDocsLanguageTag(tag), std::string("valid: ") + tag);
  for (const auto* tag : {"", "e", "english", "de_CH", "de-", "-de", "de--CH", "de-C", "de/x", "d1"}) {
    require(!isValidDocsLanguageTag(tag), std::string("invalid: ") + tag);
  }
  require(fillDocsUiText("{count} of {count} in {nope} {", {{"count", "3"}}) == "3 of 3 in {nope} {", "placeholders fill; unknown ones stay");
}
