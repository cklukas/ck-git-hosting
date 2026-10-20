// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ckgit {

// Server-side syntax highlighting for source files and fenced code blocks.
// It is a single pass over the text with no backtracking and no script on
// the page: comments, strings, keywords, numbers, keys, preprocessor lines,
// variables, type and function names and diff lines get a
// <span class="hl-…">; everything else stays plain, escaped text. Being
// "basic" on purpose, it never refuses input: text it does not understand
// simply gets fewer spans. Most languages share one table-driven tokenizer
// (one syntax description per language); markup, style sheets, Markdown,
// configuration files, diffs, Dockerfiles, Makefiles, LaTeX, batch files and
// HTTP messages have small dedicated ones. Every tokenizer runs in time
// linear in the input: a lookahead that fails is never repeated over the
// same stretch of text.

// Text beyond this size, and markup that would grow the text more than
// eightfold, render plain instead.
inline constexpr std::size_t kMaximumHighlightBytes = 128 * 1024;

// The enumerators after Json were added together; highlightLanguages() lists
// them in this order, with their display names and accepted names.
enum class Language {
  None, Cpp, Python, Shell, Yaml, Json,
  C, ObjectiveC, CSharp, Java, Kotlin, Scala, Groovy, Dart, Swift, Go, Rust, Zig, D,
  JavaScript, TypeScript, Php, Perl, Ruby, Lua, R, Julia, Nim, Crystal, Elixir, Erlang,
  Haskell, Elm, OCaml, FSharp, CommonLisp, Scheme, Clojure, Sql, VisualBasic, PowerShell,
  Batch, Fortran, Pascal, Ada, Matlab, Verilog, Vhdl, Solidity, GraphQL, Protobuf, Thrift,
  Hcl, Nix, Awk, Tcl, CMake, Nginx, Assembly, Html, Xml, Css, Scss, Less, Markdown, Ini,
  Toml, Diff, Dockerfile, Makefile, Latex, Http,
};

// The language a fenced code block's info string names ("cpp", "c++", "py",
// "bash", "yml", "ts", "c#", …), case-insensitively, or None ("text",
// "plain" and unknown names included).
Language languageForName(std::string_view name);
// The language a file name or extension suggests: special names such as
// Makefile, Dockerfile or CMakeLists.txt first, then the extension
// (.cpp/.hpp/.h/.c, .py, .sh, .yml, .json, .rs, .go, …), or None. Dotfiles
// without a further extension are None.
Language languageForPath(std::string_view path);
// The language a `#!` first line names (python, sh/bash/zsh, node, ruby,
// perl, php, lua, Rscript, pwsh, …, also behind `/usr/bin/env [-S]`), or None.
Language languageForShebang(std::string_view source);
// The extension when it is known, else the shebang, else None.
Language detectLanguage(std::string_view path, std::string_view source);

// One supported language, for documentation: its display name ("C++"), the
// fence names it accepts (the first is the canonical one), and the file
// extensions and exact file names languageForPath maps to it. Built from the
// same table the lookups use, so it cannot drift from them.
struct HighlightLanguage {
  Language language = Language::None;
  std::string_view name;
  std::vector<std::string_view> aliases;
  std::vector<std::string_view> extensions;
  std::vector<std::string_view> file_names;
};

// Every language except None, in enumerator order.
std::vector<HighlightLanguage> highlightLanguages();
// The display name of a language ("C#", "Objective-C"), or "" for None.
std::string_view languageName(Language language);

enum class TokenKind {
  Comment, String, Keyword, Number, Property, Preprocessor, Variable,
  Type, Function, Inserted, Deleted,
};

// One classified run of the source. Runs are in order, never overlap, never
// start or end inside a UTF-8 sequence; the text between them is ordinary
// code.
struct HighlightToken {
  std::size_t begin = 0;
  std::size_t end = 0;
  TokenKind kind = TokenKind::Comment;
};

std::vector<HighlightToken> highlightTokens(std::string_view source, Language language);

// The CSS class a kind renders with: "hl-c" comment, "hl-s" string, "hl-k"
// keyword, "hl-n" number, "hl-a" property/key/attribute, "hl-p"
// preprocessor/annotation/directive, "hl-v" variable, "hl-t" built-in or
// standard type name, "hl-f" function name (a call or a definition),
// "hl-i" inserted and "hl-d" deleted diff line.
std::string_view highlightClass(TokenKind kind);

// The source as HTML: `& < > " '` escaped, control bytes other than newline
// and tab replaced by U+FFFD, and each classified run wrapped in a span. A
// span never contains a newline (a run that spans lines is split), so a
// caller may cut the result at newlines to build per-line markup. Plain
// escaped text when the language is None or a bound is exceeded.
std::string highlightHtml(std::string_view source, Language language);

}  // namespace ckgit
