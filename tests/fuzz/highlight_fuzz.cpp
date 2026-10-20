// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/highlight.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(std::string_view input, ckgit::Language language) {
  const auto size = input.size();
  const auto continuation = [&](std::size_t at) { return at < size && (static_cast<unsigned char>(input[at]) & 0xc0) == 0x80; };
  std::size_t last = 0;
  for (const auto& token : ckgit::highlightTokens(input, language)) {
    if (token.begin < last || token.end <= token.begin || token.end > size) std::abort();
    if (continuation(token.begin) || continuation(token.end)) std::abort();
    last = token.end;
  }
  const auto html = ckgit::highlightHtml(input, language);
  if (html.find('<') != std::string::npos && html.find("<span class=\"hl-") == std::string::npos) std::abort();
  if (html.find("<script") != std::string::npos || html.size() > size * 8 + 1024) std::abort();
  for (auto at = html.find("<span"); at != std::string::npos; at = html.find("<span", at + 1)) {
    const auto close = html.find("</span>", at);
    if (close == std::string::npos || html.substr(at, close - at).find('\n') != std::string::npos) std::abort();
  }
}

}  // namespace

// The highlighter never throws, never emits markup other than its own spans,
// keeps its tokens ordered, inside the input and on UTF-8 boundaries, and
// never puts a newline inside a span, for every language it knows.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static const auto languages = ckgit::highlightLanguages();
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  check(input, ckgit::Language::None);
  for (const auto& entry : languages) check(input, entry.language);
  static_cast<void>(ckgit::detectLanguage(input.substr(0, 64), input));
  static_cast<void>(ckgit::languageForShebang(input));
  static_cast<void>(ckgit::languageForName(input.substr(0, 40)));
  return 0;
}
