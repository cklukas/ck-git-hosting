// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace ckgit {

// Emoji shortcodes, the `:name:` spelling documentation borrows from chat
// tools and code hosts: a fixed table of the widely used names (faces,
// people and gestures, animals and plants, food, travel and weather,
// activities, objects, symbols, and flags as `flag_<ISO 3166 code>`, plus
// short names such as `de`, `us`, `uk` for the most common flags).
inline constexpr std::size_t kMaximumEmojiShortcodeBytes = 48;

// The emoji `name` (without the colons) stands for, as UTF-8, or nullopt.
// Names are lowercase ASCII letters, digits, `_`, `+` and `-`; the lookup is
// exact.
std::optional<std::string_view> emojiForShortcode(std::string_view name);

// How many shortcodes the table holds.
std::size_t emojiShortcodeCount();

}  // namespace ckgit
