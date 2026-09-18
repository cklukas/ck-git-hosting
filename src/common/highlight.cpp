// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/highlight.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ckgit {
namespace {

using Tokens = std::vector<HighlightToken>;
constexpr auto npos = std::string_view::npos;

bool identStart(unsigned char byte) { return std::isalpha(byte) != 0 || byte == '_' || byte >= 0x80; }
bool identChar(unsigned char byte) { return identStart(byte) || std::isdigit(byte) != 0; }
bool digit(unsigned char byte) { return std::isdigit(byte) != 0; }
bool space(char byte) { return byte == ' ' || byte == '\t'; }

void add(Tokens& out, std::size_t begin, std::size_t end, TokenKind kind) {
  if (end > begin) out.push_back({begin, end, kind});
}

std::size_t lineEnd(std::string_view s, std::size_t i) {
  const auto newline = s.find('\n', i);
  return newline == npos ? s.size() : newline;
}

std::size_t identEnd(std::string_view s, std::size_t i) {
  while (i < s.size() && identChar(static_cast<unsigned char>(s[i]))) ++i;
  return i;
}

// The end of a quoted run that starts at the quote s[i]: just past the
// closing quote, or at the newline (single-line strings) or the end of input
// when it never closes. Backslash escapes are honoured when `escapes`.
std::size_t quotedEnd(std::string_view s, std::size_t i, char quote, bool escapes, bool single_line) {
  for (std::size_t j = i + 1; j < s.size(); ++j) {
    const char byte = s[j];
    if (escapes && byte == '\\' && j + 1 < s.size()) {
      ++j;
      continue;
    }
    if (byte == quote) return j + 1;
    if (single_line && byte == '\n') return j;
  }
  return s.size();
}

bool closedQuote(std::string_view s, std::size_t i, std::size_t end, char quote) {
  return end > i + 1 && end <= s.size() && s[end - 1] == quote;
}

// A number starting at the digit s[i]: digits, letters (hex, suffixes,
// exponents), '.', '_' and C++ digit separators, plus a sign right after an
// exponent letter.
std::size_t numberEnd(std::string_view s, std::size_t i) {
  const bool hex = i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X');
  std::size_t j = i;
  while (j < s.size()) {
    const unsigned char byte = s[j];
    if (std::isalnum(byte) != 0 || byte == '.' || byte == '_' || byte == '\'') {
      ++j;
      continue;
    }
    if ((byte == '+' || byte == '-') && !hex && j > i && (s[j - 1] == 'e' || s[j - 1] == 'E')) {
      ++j;
      continue;
    }
    break;
  }
  return j;
}

template <std::size_t N>
bool listed(std::string_view word, const std::string_view (&words)[N]) {
  return std::find(std::begin(words), std::end(words), word) != std::end(words);
}

std::string lower(std::string_view value) {
  std::string result(value);
  for (char& byte : result) byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
  return result;
}

// ---- C and C++ -------------------------------------------------------------

constexpr std::string_view kCppKeywords[] = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
    "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr",
    "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
    "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "final", "float",
    "for", "friend", "goto", "if", "import", "inline", "int", "long", "module", "mutable", "namespace", "new",
    "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "override", "private", "protected",
    "public", "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
    "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true",
    "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
    "wchar_t", "while", "xor", "xor_eq",
};

constexpr std::string_view kCppStringPrefixes[] = {"L", "u", "U", "u8", "R", "LR", "uR", "UR", "u8R"};

void tokenizeCpp(std::string_view s, Tokens& out) {
  bool line_start = true;  // nothing but whitespace since the last newline
  for (std::size_t i = 0; i < s.size();) {
    const char byte = s[i];
    if (byte == '\n') {
      line_start = true;
      ++i;
      continue;
    }
    if (space(byte)) {
      ++i;
      continue;
    }
    if (byte == '#' && line_start) {
      // A directive runs to the end of its line, continued by a trailing
      // backslash; a // comment on the line ends it.
      std::size_t j = i;
      while (true) {
        const auto end = lineEnd(s, j);
        const auto comment = s.substr(0, end).find("//", j);
        if (comment != npos) {
          add(out, i, comment, TokenKind::Preprocessor);
          i = comment;
          break;
        }
        if (end > j && s[end - 1] == '\\' && end < s.size()) {
          j = end + 1;
          continue;
        }
        add(out, i, end, TokenKind::Preprocessor);
        i = end;
        break;
      }
      line_start = false;
      continue;
    }
    line_start = false;
    if (byte == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      const auto end = lineEnd(s, i);
      add(out, i, end, TokenKind::Comment);
      i = end;
      continue;
    }
    if (byte == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      auto end = s.find("*/", i + 2);
      end = end == npos ? s.size() : end + 2;
      add(out, i, end, TokenKind::Comment);
      i = end;
      continue;
    }
    if (byte == '"') {
      const auto end = quotedEnd(s, i, '"', true, true);
      add(out, i, end, TokenKind::String);
      i = end;
      continue;
    }
    if (byte == '\'') {
      const auto end = quotedEnd(s, i, '\'', true, true);
      if (closedQuote(s, i, end, '\'')) {
        add(out, i, end, TokenKind::String);
        i = end;
      } else {
        ++i;
      }
      continue;
    }
    if (identStart(static_cast<unsigned char>(byte))) {
      const auto j = identEnd(s, i);
      const auto word = s.substr(i, j - i);
      if (j < s.size() && (s[j] == '"' || s[j] == '\'') && listed(word, kCppStringPrefixes)) {
        if (word.back() == 'R' && s[j] == '"') {
          const auto open = s.find('(', j + 1);
          if (open != npos && open - j - 1 <= 16) {
            const std::string closer = ")" + std::string(s.substr(j + 1, open - j - 1)) + "\"";
            auto end = s.find(closer, open + 1);
            end = end == npos ? s.size() : end + closer.size();
            add(out, i, end, TokenKind::String);
            i = end;
            continue;
          }
        } else {
          const auto end = quotedEnd(s, j, s[j], true, true);
          add(out, i, end, TokenKind::String);
          i = end;
          continue;
        }
      }
      if (listed(word, kCppKeywords)) add(out, i, j, TokenKind::Keyword);
      i = j;
      continue;
    }
    if (digit(static_cast<unsigned char>(byte))) {
      const auto end = numberEnd(s, i);
      add(out, i, end, TokenKind::Number);
      i = end;
      continue;
    }
    ++i;
  }
}

// ---- Python ----------------------------------------------------------------

constexpr std::string_view kPythonKeywords[] = {
    "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue", "def",
    "del", "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda",
    "nonlocal", "not", "or", "pass", "raise", "return", "try", "while", "with", "yield",
};

constexpr std::string_view kPythonStringPrefixes[] = {"r", "b", "f", "u", "rb", "br", "fr", "rf"};

// A string whose opening quote is s[quote]; the token starts at `begin` so a
// prefix such as f"…" is part of it.
std::size_t pythonString(std::string_view s, std::size_t begin, std::size_t quote, Tokens& out) {
  const char mark = s[quote];
  std::size_t end = s.size();
  if (quote + 2 < s.size() && s[quote + 1] == mark && s[quote + 2] == mark) {
    for (std::size_t j = quote + 3; j < s.size(); ++j) {
      if (s[j] == '\\') {
        ++j;
        continue;
      }
      if (s[j] == mark && j + 2 < s.size() && s[j + 1] == mark && s[j + 2] == mark) {
        end = j + 3;
        break;
      }
    }
  } else {
    end = quotedEnd(s, quote, mark, true, true);
  }
  add(out, begin, end, TokenKind::String);
  return end;
}

void tokenizePython(std::string_view s, Tokens& out) {
  bool line_start = true;
  for (std::size_t i = 0; i < s.size();) {
    const char byte = s[i];
    if (byte == '\n') {
      line_start = true;
      ++i;
      continue;
    }
    if (space(byte)) {
      ++i;
      continue;
    }
    if (byte == '#') {
      const auto end = lineEnd(s, i);
      add(out, i, end, TokenKind::Comment);
      i = end;
      continue;
    }
    if (byte == '@' && line_start) {
      std::size_t j = i + 1;
      while (j < s.size() && (identChar(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
      add(out, i, j, TokenKind::Property);
      i = j;
      line_start = false;
      continue;
    }
    line_start = false;
    if (byte == '"' || byte == '\'') {
      i = pythonString(s, i, i, out);
      continue;
    }
    if (identStart(static_cast<unsigned char>(byte))) {
      const auto j = identEnd(s, i);
      const auto word = s.substr(i, j - i);
      if (j < s.size() && (s[j] == '"' || s[j] == '\'') && word.size() <= 2 && listed(lower(word), kPythonStringPrefixes)) {
        i = pythonString(s, i, j, out);
        continue;
      }
      if (listed(word, kPythonKeywords)) add(out, i, j, TokenKind::Keyword);
      i = j;
      continue;
    }
    if (digit(static_cast<unsigned char>(byte))) {
      const auto end = numberEnd(s, i);
      add(out, i, end, TokenKind::Number);
      i = end;
      continue;
    }
    ++i;
  }
}

// ---- Shell -----------------------------------------------------------------

constexpr std::string_view kShellKeywords[] = {
    "break", "case", "continue", "declare", "do", "done", "elif", "else", "esac", "eval", "exec", "exit",
    "export", "fi", "for", "function", "if", "in", "local", "readonly", "return", "select", "set", "shift",
    "source", "then", "trap", "typeset", "unset", "until", "while",
};

// A keyword is one only as a whole word: `in/if`, `--set`, `exit=1` are not.
bool shellWordStart(std::string_view s, std::size_t i) {
  if (i == 0) return true;
  const char before = s[i - 1];
  return space(before) || before == '\n' || before == ';' || before == '(' || before == ')' || before == '|' ||
         before == '&' || before == '{' || before == '}' || before == '!';
}

bool shellWordEnd(std::string_view s, std::size_t j) {
  if (j >= s.size()) return true;
  const char after = s[j];
  return space(after) || after == '\n' || after == ';' || after == '(' || after == ')' || after == '|' ||
         after == '&' || after == '{' || after == '}' || after == '#';
}

void tokenizeShell(std::string_view s, Tokens& out) {
  std::string heredoc;  // a pending here-document delimiter
  bool heredoc_strip = false;
  for (std::size_t i = 0; i < s.size();) {
    const char byte = s[i];
    if (byte == '\n') {
      ++i;
      if (heredoc.empty()) continue;
      // The body runs up to the line that is exactly the delimiter.
      const std::size_t begin = i;
      std::size_t body_end = s.size();
      std::size_t after = s.size();
      for (std::size_t j = i; j < s.size();) {
        const auto end = lineEnd(s, j);
        auto line = s.substr(j, end - j);
        if (heredoc_strip) {
          while (!line.empty() && line.front() == '\t') line.remove_prefix(1);
        }
        if (line == heredoc) {
          body_end = j;
          after = end;
          break;
        }
        j = end + 1;
      }
      add(out, begin, body_end, TokenKind::String);
      i = after;
      heredoc.clear();
      continue;
    }
    if (byte == '#' && (i == 0 || space(s[i - 1]) || s[i - 1] == '\n' || s[i - 1] == ';' || s[i - 1] == '(' ||
                        s[i - 1] == '|' || s[i - 1] == '&')) {
      const auto end = lineEnd(s, i);
      add(out, i, end, TokenKind::Comment);
      i = end;
      continue;
    }
    if (byte == '"' || byte == '\'') {
      const auto end = quotedEnd(s, i, byte, byte == '"', false);
      add(out, i, end, TokenKind::String);
      i = end;
      continue;
    }
    if (byte == '$') {
      if (i + 1 < s.size()) {
        const char next = s[i + 1];
        if (next == '{') {
          const auto eol = lineEnd(s, i);
          auto end = s.substr(0, eol).find('}', i + 2);
          end = end == npos ? eol : end + 1;
          add(out, i, end, TokenKind::Variable);
          i = end;
          continue;
        }
        if (identStart(static_cast<unsigned char>(next))) {
          const auto end = identEnd(s, i + 1);
          add(out, i, end, TokenKind::Variable);
          i = end;
          continue;
        }
        if (digit(static_cast<unsigned char>(next)) || next == '@' || next == '*' || next == '#' || next == '?' ||
            next == '$' || next == '!' || next == '-') {
          add(out, i, i + 2, TokenKind::Variable);
          i += 2;
          continue;
        }
      }
      ++i;
      continue;
    }
    if (byte == '<' && s.substr(i, 2) == "<<" && s.substr(i, 3) != "<<<") {
      std::size_t j = i + 2;
      bool strip = false;
      if (j < s.size() && s[j] == '-') {
        strip = true;
        ++j;
      }
      while (j < s.size() && space(s[j])) ++j;
      char quote = 0;
      if (j < s.size() && (s[j] == '\'' || s[j] == '"')) quote = s[j++];
      std::size_t k = j;
      while (k < s.size() && (identChar(static_cast<unsigned char>(s[k])) || s[k] == '-')) ++k;
      if (k > j && (quote == 0 || (k < s.size() && s[k] == quote))) {
        heredoc = std::string(s.substr(j, k - j));
        heredoc_strip = strip;
        i = quote == 0 ? k : k + 1;
        continue;
      }
      i += 2;
      continue;
    }
    if (identStart(static_cast<unsigned char>(byte))) {
      const auto j = identEnd(s, i);
      if (shellWordStart(s, i) && shellWordEnd(s, j) && listed(s.substr(i, j - i), kShellKeywords)) {
        add(out, i, j, TokenKind::Keyword);
      }
      i = j;
      continue;
    }
    ++i;
  }
}

// ---- YAML ------------------------------------------------------------------

constexpr std::string_view kYamlWords[] = {"true", "false", "yes", "no", "on", "off", "null", "~"};

bool yamlWord(std::string_view value) { return listed(lower(value), kYamlWords); }

bool yamlNumber(std::string_view value) {
  std::size_t i = 0;
  if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
  if (value.size() > i + 2 && value[i] == '0' && (value[i + 1] == 'x' || value[i + 1] == 'o')) {
    return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(i) + 2, value.end(),
                       [](unsigned char byte) { return std::isalnum(byte) != 0; });
  }
  bool digits = false;
  while (i < value.size() && digit(static_cast<unsigned char>(value[i]))) {
    ++i;
    digits = true;
  }
  if (i < value.size() && value[i] == '.') {
    ++i;
    while (i < value.size() && digit(static_cast<unsigned char>(value[i]))) {
      ++i;
      digits = true;
    }
  }
  if (!digits) return false;
  if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
    ++i;
    if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
    bool exponent = false;
    while (i < value.size() && digit(static_cast<unsigned char>(value[i]))) {
      ++i;
      exponent = true;
    }
    if (!exponent) return false;
  }
  return i == value.size();
}

// A `key:` at s[p] within the line [p, end): {key_end, colon}, or {npos, npos}.
std::pair<std::size_t, std::size_t> yamlKey(std::string_view s, std::size_t p, std::size_t end) {
  const char first = s[p];
  if (first == '"' || first == '\'') {
    const auto quote_end = quotedEnd(s.substr(0, end), p, first, first == '"', true);
    std::size_t j = quote_end;
    while (j < end && space(s[j])) ++j;
    if (j < end && s[j] == ':' && (j + 1 == end || space(s[j + 1]))) return {quote_end, j};
    return {npos, npos};
  }
  if (std::string_view("[{#&*!|>%@`").find(first) != npos) return {npos, npos};
  for (std::size_t j = p; j < end; ++j) {
    if (s[j] == '#' && j > p && space(s[j - 1])) return {npos, npos};
    if (s[j] == ':' && (j + 1 == end || space(s[j + 1]))) {
      std::size_t key_end = j;
      while (key_end > p && space(s[key_end - 1])) --key_end;
      return {key_end, j};
    }
  }
  return {npos, npos};
}

void yamlFlow(std::string_view s, std::size_t p, std::size_t end, Tokens& out) {
  while (p < end) {
    const char byte = s[p];
    if (space(byte) || byte == ',' || byte == '[' || byte == ']' || byte == '{' || byte == '}' || byte == ':') {
      ++p;
      continue;
    }
    if (byte == '#' && space(s[p - 1])) {
      add(out, p, end, TokenKind::Comment);
      return;
    }
    if (byte == '"' || byte == '\'') {
      const auto quote_end = quotedEnd(s.substr(0, end), p, byte, byte == '"', true);
      std::size_t j = quote_end;
      while (j < end && space(s[j])) ++j;
      add(out, p, quote_end, j < end && s[j] == ':' ? TokenKind::Property : TokenKind::String);
      p = quote_end;
      continue;
    }
    std::size_t j = p;
    while (j < end && s[j] != ',' && s[j] != ']' && s[j] != '}' && s[j] != '[' && s[j] != '{') {
      if (s[j] == ':' && (j + 1 == end || space(s[j + 1]) || s[j + 1] == ',' || s[j + 1] == '}')) break;
      ++j;
    }
    std::size_t value_end = j;
    while (value_end > p && space(s[value_end - 1])) --value_end;
    const auto value = s.substr(p, value_end - p);
    if (j < end && s[j] == ':') add(out, p, value_end, TokenKind::Property);
    else if (yamlWord(value)) add(out, p, value_end, TokenKind::Keyword);
    else if (yamlNumber(value)) add(out, p, value_end, TokenKind::Number);
    p = j;
  }
}

// The value part of a line, [p, end); `owner_indent` is the indent a block
// scalar's content must exceed, recorded into `block_indent` on `|` or `>`.
void yamlValue(std::string_view s, std::size_t p, std::size_t end, std::size_t owner_indent, Tokens& out,
               std::size_t& block_indent) {
  while (p < end && space(s[p])) ++p;
  if (p >= end) return;
  const char byte = s[p];
  if (byte == '#') {
    add(out, p, end, TokenKind::Comment);
    return;
  }
  if (byte == '&' || byte == '*' || byte == '!') {
    std::size_t j = p + 1;
    while (j < end && !space(s[j])) ++j;
    if (byte != '!') add(out, p, j, TokenKind::Variable);
    yamlValue(s, j, end, owner_indent, out, block_indent);
    return;
  }
  if (byte == '|' || byte == '>') {
    std::size_t j = p + 1;
    while (j < end && (s[j] == '+' || s[j] == '-' || digit(static_cast<unsigned char>(s[j])))) ++j;
    if (j == end || space(s[j])) {
      add(out, p, j, TokenKind::Keyword);
      block_indent = owner_indent;
      yamlValue(s, j, end, owner_indent, out, block_indent);
      return;
    }
  }
  if (byte == '"' || byte == '\'') {
    const auto quote_end = quotedEnd(s.substr(0, end), p, byte, byte == '"', true);
    add(out, p, quote_end, TokenKind::String);
    yamlValue(s, quote_end, end, owner_indent, out, block_indent);
    return;
  }
  if (byte == '[' || byte == '{') {
    yamlFlow(s, p, end, out);
    return;
  }
  std::size_t stop = end;
  for (std::size_t j = p; j + 1 < end; ++j) {
    if (space(s[j]) && s[j + 1] == '#') {
      stop = j;
      break;
    }
  }
  std::size_t value_end = stop;
  while (value_end > p && space(s[value_end - 1])) --value_end;
  const auto value = s.substr(p, value_end - p);
  if (yamlWord(value)) add(out, p, value_end, TokenKind::Keyword);
  else if (yamlNumber(value)) add(out, p, value_end, TokenKind::Number);
  if (stop < end) add(out, stop + 1, end, TokenKind::Comment);
}

void tokenizeYaml(std::string_view s, Tokens& out) {
  std::size_t block_indent = npos;  // set while inside a `|` or `>` scalar
  for (std::size_t i = 0; i < s.size();) {
    const auto end = lineEnd(s, i);
    std::size_t indent = 0;
    while (i + indent < end && s[i + indent] == ' ') ++indent;
    bool blank = true;
    for (std::size_t k = i; k < end && blank; ++k) blank = space(s[k]);
    if (block_indent != npos) {
      if (blank || indent > block_indent) {
        i = end + 1;
        continue;
      }
      block_indent = npos;
    }
    std::size_t p = i + indent;
    if (indent == 0 && end - i >= 3 && (s.substr(i, 3) == "---" || s.substr(i, 3) == "...") &&
        (end - i == 3 || space(s[i + 3]))) {
      add(out, i, i + 3, TokenKind::Keyword);
      p = i + 3;
    }
    std::size_t owner_indent = indent;
    while (p < end) {
      while (p < end && space(s[p])) ++p;
      if (p >= end) break;
      const char byte = s[p];
      if (byte == '#' && (p == i || space(s[p - 1]))) {
        add(out, p, end, TokenKind::Comment);
        break;
      }
      if (byte == '-' && (p + 1 == end || space(s[p + 1]))) {
        owner_indent = p - i;
        ++p;
        continue;
      }
      const auto [key_end, colon] = yamlKey(s, p, end);
      if (colon != npos) {
        // A block scalar's content must be indented past its key's column,
        // as the configuration parser (yaml_subset.cpp) also requires; a bare
        // `- |` item anchors to the dash instead.
        add(out, p, key_end, TokenKind::Property);
        yamlValue(s, colon + 1, end, p - i, out, block_indent);
      } else {
        yamlValue(s, p, end, owner_indent, out, block_indent);
      }
      break;
    }
    i = end + 1;
  }
}

// ---- JSON ------------------------------------------------------------------

void tokenizeJson(std::string_view s, Tokens& out) {
  for (std::size_t i = 0; i < s.size();) {
    const char byte = s[i];
    if (byte == '"') {
      const auto end = quotedEnd(s, i, '"', true, true);
      std::size_t j = end;
      while (j < s.size() && (space(s[j]) || s[j] == '\n' || s[j] == '\r')) ++j;
      add(out, i, end, j < s.size() && s[j] == ':' ? TokenKind::Property : TokenKind::String);
      i = end;
      continue;
    }
    if (byte == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      const auto end = lineEnd(s, i);
      add(out, i, end, TokenKind::Comment);
      i = end;
      continue;
    }
    if (byte == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      auto end = s.find("*/", i + 2);
      end = end == npos ? s.size() : end + 2;
      add(out, i, end, TokenKind::Comment);
      i = end;
      continue;
    }
    if (digit(static_cast<unsigned char>(byte)) ||
        (byte == '-' && i + 1 < s.size() && digit(static_cast<unsigned char>(s[i + 1])))) {
      std::size_t j = i + 1;
      while (j < s.size() && (digit(static_cast<unsigned char>(s[j])) || s[j] == '.' || s[j] == 'e' || s[j] == 'E' ||
                              s[j] == '+' || s[j] == '-')) {
        ++j;
      }
      add(out, i, j, TokenKind::Number);
      i = j;
      continue;
    }
    if (identStart(static_cast<unsigned char>(byte))) {
      const auto j = identEnd(s, i);
      const auto word = s.substr(i, j - i);
      if (word == "true" || word == "false" || word == "null") add(out, i, j, TokenKind::Keyword);
      i = j;
      continue;
    }
    ++i;
  }
}

void escapeInto(std::string& out, std::string_view value) {
  for (const unsigned char byte : value) {
    switch (byte) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:
        if ((byte < 0x20 && byte != '\n' && byte != '\t') || byte == 0x7f) out += "\xef\xbf\xbd";
        else out += static_cast<char>(byte);
    }
  }
}

}  // namespace

Language languageForName(std::string_view name) {
  if (name.empty() || name.size() > 32) return Language::None;
  const auto lowered = lower(name);
  for (const auto* cpp : {"cpp", "c++", "cxx", "cc", "c", "h", "hpp", "hh", "hxx"}) {
    if (lowered == cpp) return Language::Cpp;
  }
  for (const auto* python : {"python", "py", "python3", "py3"}) {
    if (lowered == python) return Language::Python;
  }
  for (const auto* shell : {"bash", "sh", "shell", "zsh", "ksh", "dash", "console", "shell-script"}) {
    if (lowered == shell) return Language::Shell;
  }
  if (lowered == "yaml" || lowered == "yml") return Language::Yaml;
  if (lowered == "json" || lowered == "jsonc") return Language::Json;
  return Language::None;
}

Language languageForPath(std::string_view path) {
  const auto slash = path.rfind('/');
  const auto name = slash == npos ? path : path.substr(slash + 1);
  const auto dot = name.rfind('.');
  if (dot == npos || dot == 0) return Language::None;
  const auto extension = lower(name.substr(dot + 1));
  if (extension == "inc") return Language::Cpp;
  if (extension == "pyi") return Language::Python;
  return languageForName(extension);
}

Language languageForShebang(std::string_view source) {
  if (!source.starts_with("#!")) return Language::None;
  const auto line = source.substr(0, lineEnd(source, 0));
  const auto slash = line.rfind('/');
  auto interpreter = slash == npos ? line.substr(2) : line.substr(slash + 1);
  // "#!/usr/bin/env python3" names the interpreter after env.
  if (interpreter.starts_with("env ")) {
    interpreter.remove_prefix(4);
    while (!interpreter.empty() && space(interpreter.front())) interpreter.remove_prefix(1);
    const auto inner_slash = interpreter.rfind('/');
    if (inner_slash != npos) interpreter = interpreter.substr(inner_slash + 1);
  }
  const auto stop = interpreter.find_first_of(" \t\r");
  if (stop != npos) interpreter = interpreter.substr(0, stop);
  if (interpreter.starts_with("python")) return Language::Python;
  for (const auto* shell : {"sh", "bash", "zsh", "dash", "ksh", "ash"}) {
    if (interpreter == shell) return Language::Shell;
  }
  return Language::None;
}

Language detectLanguage(std::string_view path, std::string_view source) {
  const auto by_path = languageForPath(path);
  return by_path != Language::None ? by_path : languageForShebang(source);
}

std::vector<HighlightToken> highlightTokens(std::string_view source, Language language) {
  Tokens out;
  if (source.size() > kMaximumHighlightBytes) return out;
  switch (language) {
    case Language::Cpp: tokenizeCpp(source, out); break;
    case Language::Python: tokenizePython(source, out); break;
    case Language::Shell: tokenizeShell(source, out); break;
    case Language::Yaml: tokenizeYaml(source, out); break;
    case Language::Json: tokenizeJson(source, out); break;
    case Language::None: break;
  }
  return out;
}

std::string_view highlightClass(TokenKind kind) {
  switch (kind) {
    case TokenKind::Comment: return "hl-c";
    case TokenKind::String: return "hl-s";
    case TokenKind::Keyword: return "hl-k";
    case TokenKind::Number: return "hl-n";
    case TokenKind::Property: return "hl-a";
    case TokenKind::Preprocessor: return "hl-p";
    case TokenKind::Variable: return "hl-v";
  }
  return "hl-c";
}

std::string highlightHtml(std::string_view source, Language language) {
  const auto tokens = highlightTokens(source, language);
  std::string html;
  if (!tokens.empty()) {
    std::size_t position = 0;
    for (const auto& token : tokens) {
      escapeInto(html, source.substr(position, token.begin - position));
      const std::string open = "<span class=\"" + std::string(highlightClass(token.kind)) + "\">";
      for (std::size_t start = token.begin; start < token.end;) {
        auto stop = source.find('\n', start);
        if (stop == npos || stop > token.end) stop = token.end;
        if (stop > start) {
          html += open;
          escapeInto(html, source.substr(start, stop - start));
          html += "</span>";
        }
        if (stop < token.end) html += '\n';
        start = stop + 1;
      }
      position = token.end;
    }
    escapeInto(html, source.substr(position));
    if (html.size() <= source.size() * 8 + 1024) return html;
    html.clear();
  }
  escapeInto(html, source);
  return html;
}

}  // namespace ckgit
