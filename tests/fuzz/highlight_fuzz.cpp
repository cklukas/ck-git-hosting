// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/highlight.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

// The highlighter never throws and never emits markup other than its own
// spans; its tokens stay ordered and inside the input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  for (const auto language : {ckgit::Language::Cpp, ckgit::Language::Python, ckgit::Language::Shell,
                              ckgit::Language::Yaml, ckgit::Language::Json, ckgit::Language::None}) {
    std::size_t last = 0;
    for (const auto& token : ckgit::highlightTokens(input, language)) {
      if (token.begin < last || token.end <= token.begin || token.end > size) std::abort();
      last = token.end;
    }
    const auto html = ckgit::highlightHtml(input, language);
    if (html.find('<') != std::string::npos && html.find("<span class=\"hl-") == std::string::npos) std::abort();
    if (html.find("<script") != std::string::npos || html.size() > size * 8 + 1024) std::abort();
  }
  static_cast<void>(ckgit::detectLanguage("script", input));
  static_cast<void>(ckgit::languageForName(input.substr(0, 40)));
  return 0;
}
