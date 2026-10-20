// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <stdexcept>
#include <string>
#include <string_view>

#include "ckgit/emoji.hpp"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("emoji: " + message);
}

}  // namespace

void testEmoji() {
  using ckgit::emojiForShortcode;
  require(emojiForShortcode("tada") == std::string_view("\xf0\x9f\x8e\x89"), "tada");
  require(emojiForShortcode("+1") == emojiForShortcode("thumbsup") && emojiForShortcode("+1").has_value(), "aliases share an emoji");
  require(emojiForShortcode("warning") == std::string_view("\xe2\x9a\xa0\xef\xb8\x8f"),
          "a character that needs it carries the emoji presentation selector");
  require(emojiForShortcode("flag_de") == std::string_view("\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa") &&
              emojiForShortcode("de") == emojiForShortcode("flag_de") && emojiForShortcode("uk") == emojiForShortcode("flag_gb"),
          "flags by ISO code, and the short names");
  require(emojiForShortcode("keycap_ten").has_value() && emojiForShortcode("hash") == std::string_view("#\xef\xb8\x8f\xe2\x83\xa3"),
          "keycaps");
  require(emojiForShortcode("technologist") == std::string_view("\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x92\xbb"), "a joined sequence");
  require(!emojiForShortcode("").has_value() && !emojiForShortcode("Tada").has_value() && !emojiForShortcode("nope").has_value() &&
              !emojiForShortcode(std::string(64, 'a')).has_value(),
          "an empty, differently cased, unknown or over-long name is not a shortcode");
  require(ckgit::emojiShortcodeCount() > 1500, "the table holds the common vocabulary");
}
