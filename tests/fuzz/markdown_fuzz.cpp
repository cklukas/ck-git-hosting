// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/markdown.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const ckgit::LinkContext context{"example", std::string(40, 'a'), "docs"};
  try {
    const auto html = ckgit::renderMarkdown(std::string_view(reinterpret_cast<const char*>(data), size), context);
    if (html.size() > ckgit::kMaximumMarkdownOutputBytes || html.find("<script") != std::string::npos ||
        html.find("<iframe") != std::string::npos || html.find("<svg") != std::string::npos) std::abort();
  } catch (const std::length_error&) {
    // Exceeding a documented input, output or work cap is an expected result.
  }
  return 0;
}
