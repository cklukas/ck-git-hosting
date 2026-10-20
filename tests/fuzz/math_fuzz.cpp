// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/math.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Every element the converter may emit; anything else is a failure.
bool allowedTag(std::string_view tag) {
  static constexpr std::string_view kTags[] = {
      "math", "semantics", "annotation", "mrow", "mi", "mn", "mo", "mtext", "mspace", "msub", "msup", "msubsup",
      "munder", "mover", "munderover", "mfrac", "msqrt", "mroot", "mtable", "mtr", "mtd", "mstyle", "mpadded",
      "mphantom", "merror", "ms",
  };
  for (const auto known : kTags) {
    if (known == tag) return true;
  }
  return false;
}

// The markup is a single, properly nested tree of allowed elements.
bool wellFormed(const std::string& html) {
  std::vector<std::string_view> open;
  const std::string_view text(html);
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '<') continue;
    const bool closing = i + 1 < text.size() && text[i + 1] == '/';
    const std::size_t start = i + (closing ? 2 : 1);
    std::size_t end = start;
    while (end < text.size() && text[end] != '>' && text[end] != ' ') ++end;
    const auto tag = text.substr(start, end - start);
    if (!allowedTag(tag)) return false;
    if (closing) {
      if (open.empty() || open.back() != tag) return false;
      open.pop_back();
      if (open.empty() && end + 1 != text.size()) return false;
    } else {
      open.push_back(tag);
    }
  }
  return open.empty();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  for (const bool display : {false, true}) {
    // Never throws; one <math> element, bounded linearly in the input.
    const auto result = ckgit::renderTexMath(input, display);
    if (result.html.size() > 256 * size + 512 || result.errors.size() > ckgit::kMaximumMathErrors + 1) std::abort();
    if (result.html.rfind(display ? "<math display=\"block\">" : "<math>", 0) != 0 || !wellFormed(result.html)) {
      std::abort();
    }
    if (result.html.find("<script") != std::string::npos || result.html.find("<svg") != std::string::npos) std::abort();
  }
  return 0;
}
