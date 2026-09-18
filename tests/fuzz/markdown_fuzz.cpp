// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/markdown.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const ckgit::LinkContext context{"example", std::string(40, 'a'), "docs"};
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  // Front matter splitting never throws; the entries it returns stay within
  // their documented bounds.
  if (const auto front = ckgit::splitFrontMatter(input)) {
    if (front->body_offset > size || front->entries.size() > ckgit::kMaximumFrontMatterLines) std::abort();
    for (const auto& [key, value] : front->entries) {
      if (key.size() > ckgit::kMaximumFrontMatterKeyBytes || value.size() > ckgit::kMaximumFrontMatterValueBytes) std::abort();
    }
  }
  try {
    std::vector<ckgit::MarkdownHeading> outline;
    const auto html = ckgit::renderMarkdown(ckgit::markdownBody(input), context, &outline);
    if (html.size() > ckgit::kMaximumMarkdownOutputBytes || html.find("<script") != std::string::npos ||
        html.find("<iframe") != std::string::npos || html.find("<svg") != std::string::npos) std::abort();
    for (const auto& heading : outline) {
      if (heading.level < 1 || heading.level > 6 || heading.id.empty()) std::abort();
    }
  } catch (const std::length_error&) {
    // Exceeding a documented input, output or work cap is an expected result.
  }
  return 0;
}
