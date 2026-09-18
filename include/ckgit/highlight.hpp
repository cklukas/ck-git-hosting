// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ckgit {

// Server-side syntax highlighting for the languages this product's own
// sources and configuration are written in. It is a single pass over the
// text with no backtracking and no script on the page: comments, strings,
// keywords, numbers, keys, preprocessor lines and shell variables get a
// <span class="hl-…">; everything else stays plain, escaped text. Being
// "basic" on purpose, it never refuses input — text it does not understand
// simply gets fewer spans.

// Text beyond this size, and markup that would grow the text more than
// eightfold, render plain instead.
inline constexpr std::size_t kMaximumHighlightBytes = 128 * 1024;

enum class Language { None, Cpp, Python, Shell, Yaml, Json };

// The language a fenced code block's info string names ("cpp", "c++", "py",
// "bash", "yml", …), or None.
Language languageForName(std::string_view name);
// The language a file's extension suggests (.cpp/.hpp/.h/.c/.inc, .py,
// .sh/.bash/.zsh, .yml/.yaml, .json), or None.
Language languageForPath(std::string_view path);
// The language a `#!` first line names (python, sh/bash/zsh/dash/ksh), or None.
Language languageForShebang(std::string_view source);
// The extension when it is known, else the shebang, else None.
Language detectLanguage(std::string_view path, std::string_view source);

enum class TokenKind { Comment, String, Keyword, Number, Property, Preprocessor, Variable };

// One classified run of the source. Runs are in order and never overlap;
// the text between them is ordinary code.
struct HighlightToken {
  std::size_t begin = 0;
  std::size_t end = 0;
  TokenKind kind = TokenKind::Comment;
};

std::vector<HighlightToken> highlightTokens(std::string_view source, Language language);

// The CSS class a kind renders with: "hl-c" comment, "hl-s" string, "hl-k"
// keyword, "hl-n" number, "hl-a" property/key, "hl-p" preprocessor, "hl-v"
// variable.
std::string_view highlightClass(TokenKind kind);

// The source as HTML: `& < > " '` escaped, control bytes other than newline
// and tab replaced by U+FFFD, and each classified run wrapped in a span. A
// span never contains a newline (a run that spans lines is split), so a
// caller may cut the result at newlines to build per-line markup. Plain
// escaped text when the language is None or a bound is exceeded.
std::string highlightHtml(std::string_view source, Language language);

}  // namespace ckgit
