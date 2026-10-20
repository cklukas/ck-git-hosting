// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/highlight.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ckgit {
namespace {

using Tokens = std::vector<HighlightToken>;
constexpr auto npos = std::string_view::npos;
// Embedded languages (a fence in Markdown, a script in HTML, a recipe in a
// Dockerfile) nest at most this deep; deeper text stays plain.
constexpr int kMaximumNesting = 4;

unsigned char u(char byte) { return static_cast<unsigned char>(byte); }
bool identStart(char byte) { return std::isalpha(u(byte)) != 0 || byte == '_' || u(byte) >= 0x80; }
bool identChar(char byte) { return identStart(byte) || std::isdigit(u(byte)) != 0; }
bool digit(char byte) { return std::isdigit(u(byte)) != 0; }
bool upper(char byte) { return byte >= 'A' && byte <= 'Z'; }
bool space(char byte) { return byte == ' ' || byte == '\t'; }
// Horizontal white space, including the carriage return of CRLF text.
bool blank(char byte) { return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\f' || byte == '\v'; }
bool has(std::string_view set, char byte) { return byte != '\0' && set.find(byte) != npos; }
bool startsAt(std::string_view s, std::size_t i, std::string_view prefix) {
  return !prefix.empty() && i <= s.size() && s.size() - i >= prefix.size() && s.compare(i, prefix.size(), prefix) == 0;
}
// The length of the UTF-8 sequence a lead byte starts (1 for anything else),
// so a character literal never ends inside a sequence.
std::size_t utf8Length(char byte) {
  const auto b = u(byte);
  if (b >= 0xf0 && b <= 0xf7) return 4;
  if (b >= 0xe0) return b <= 0xef ? 3 : 1;
  if (b >= 0xc0) return 2;
  return 1;
}

void add(Tokens& out, std::size_t begin, std::size_t end, TokenKind kind) {
  if (end > begin) out.push_back({begin, end, kind});
}

std::size_t lineEnd(std::string_view s, std::size_t i) {
  const auto newline = s.find('\n', i);
  return newline == npos ? s.size() : newline;
}

std::size_t identEnd(std::string_view s, std::size_t i) {
  while (i < s.size() && identChar(s[i])) ++i;
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

// A character literal starting at the quote s[i], bounded to a few bytes so a
// stray apostrophe never makes the scan run on: its end, or npos.
std::size_t shortQuoteEnd(std::string_view s, std::size_t i) {
  const auto limit = std::min(s.size(), i + 16);
  for (std::size_t j = i + 1; j < limit; ++j) {
    if (s[j] == '\n') return npos;
    if (s[j] == '\\') {
      ++j;
      continue;
    }
    if (s[j] == '\'') return j + 1;
  }
  return npos;
}

// A character literal 'x', '\n', '\u{1F600}' or 'é' at the quote s[i]: its
// end, or npos when the apostrophe starts none (a Rust lifetime, an OCaml
// type variable, an Ada attribute).
std::size_t charLiteralEnd(std::string_view s, std::size_t i, char escape) {
  const auto n = s.size();
  std::size_t j = i + 1;
  if (j >= n || s[j] == '\n' || s[j] == '\'') return npos;
  if (escape != '\0' && s[j] == escape) {
    for (std::size_t k = j + 2; k < n && k < i + 14; ++k) {
      if (s[k] == '\n') return npos;
      if (s[k] == '\'') return k + 1;
    }
    return npos;
  }
  j += utf8Length(s[j]);
  return j < n && s[j] == '\'' ? j + 1 : npos;
}

// A number starting at the digit s[i]: digits, letters (hex, suffixes,
// exponents), '.', '_' and C++ digit separators, plus a sign right after an
// exponent letter.
std::size_t numberEnd(std::string_view s, std::size_t i) {
  const bool hex = i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X');
  std::size_t j = i;
  while (j < s.size()) {
    const char byte = s[j];
    if (std::isalnum(u(byte)) != 0 || byte == '.' || byte == '_' || byte == '\'') {
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

// The same for the other languages, where a '.' belongs to the number only
// before a digit (so `5.times` and `0..n` keep their method and range) and an
// apostrophe only where it separates digits or sizes a Verilog literal.
std::size_t genericNumberEnd(std::string_view s, std::size_t i, bool apostrophes) {
  const bool hex = i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X');
  std::size_t j = i;
  while (j < s.size()) {
    const char byte = s[j];
    if (std::isalnum(u(byte)) != 0 || byte == '_' || (byte == '\'' && apostrophes)) {
      ++j;
    } else if (byte == '.' && !hex && j + 1 < s.size() && digit(s[j + 1])) {
      ++j;
    } else if ((byte == '+' || byte == '-') && !hex && j > i && (s[j - 1] == 'e' || s[j - 1] == 'E') &&
               j + 1 < s.size() && digit(s[j + 1])) {
      ++j;
    } else {
      break;
    }
  }
  return j;
}

template <std::size_t N>
bool listed(std::string_view word, const std::string_view (&words)[N]) {
  return std::find(std::begin(words), std::end(words), word) != std::end(words);
}

bool listedIn(std::span<const std::string_view> words, std::string_view word) {
  return std::find(words.begin(), words.end(), word) != words.end();
}

std::string lower(std::string_view value) {
  std::string result(value);
  for (char& byte : result) byte = static_cast<char>(std::tolower(u(byte)));
  return result;
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(u(x)) == std::tolower(u(y)); });
}

// Lower-cases a short word into `buffer`; a longer one is no keyword anyway.
std::string_view lowerInto(std::string_view word, std::array<char, 64>& buffer) {
  if (word.size() > buffer.size()) return {};
  for (std::size_t k = 0; k < word.size(); ++k) buffer[k] = static_cast<char>(std::tolower(u(word[k])));
  return {buffer.data(), word.size()};
}

template <typename F>
void eachWord(std::string_view list, F&& f) {
  std::size_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && list[i] == ' ') ++i;
    std::size_t j = i;
    while (j < list.size() && list[j] != ' ') ++j;
    if (j > i) f(list.substr(i, j - i));
    i = j;
  }
}

// A sorted, space-separated word list from the language tables.
class WordSet {
 public:
  WordSet() = default;
  explicit WordSet(std::string_view list) {
    eachWord(list, [this](std::string_view word) { words_.push_back(word); });
    std::sort(words_.begin(), words_.end());
  }
  bool contains(std::string_view word) const {
    return !word.empty() && std::binary_search(words_.begin(), words_.end(), word);
  }

 private:
  std::vector<std::string_view> words_;
};

// Remembers its last forward search for a byte set: the first match at or
// after any position inside the stretch already searched is that same match.
// Repeated lookaheads from nearby positions ("is there a closing `}` on this
// line?") therefore cost nothing extra, which keeps every tokenizer linear
// even on hostile input such as a line of 100 000 unclosed `${`.
class Seeker {
 public:
  Seeker(std::string_view text, std::string_view set) : text_(text), set_(set) {}
  std::size_t next(std::size_t from) {
    if (searched_ && from >= from_ && from <= at_) return at_;
    searched_ = true;
    from_ = from;
    at_ = from >= text_.size() ? text_.size() : text_.find_first_of(set_, from);
    if (at_ == npos) at_ = text_.size();
    return at_;
  }

 private:
  std::string_view text_;
  std::string_view set_;
  bool searched_ = false;
  std::size_t from_ = 0;
  std::size_t at_ = 0;
};

void tokenize(Language language, std::string_view s, Tokens& out, int depth);

// Tokens of the embedded text [begin, end) in another language, shifted to
// their place in `s`.
void tokenizeEmbedded(Language language, std::string_view s, std::size_t begin, std::size_t end, Tokens& out,
                      int depth) {
  if (language == Language::None || depth >= kMaximumNesting || end <= begin) return;
  Tokens inner;
  tokenize(language, s.substr(begin, end - begin), inner, depth + 1);
  for (const auto& token : inner) out.push_back({token.begin + begin, token.end + begin, token.kind});
}

// ---- C, C++ and Objective-C -------------------------------------------------

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

// C23, including the spellings C11 introduced with a leading underscore.
constexpr std::string_view kCKeywords[] = {
    "alignas", "alignof", "auto", "bool", "break", "case", "char", "const", "constexpr", "continue", "default",
    "do", "double", "else", "enum", "extern", "false", "float", "for", "goto", "if", "inline", "int", "long",
    "nullptr", "register", "restrict", "return", "short", "signed", "sizeof", "static", "static_assert", "struct",
    "switch", "thread_local", "true", "typedef", "typeof", "typeof_unqual", "union", "unsigned", "void",
    "volatile", "while", "_Alignas", "_Alignof", "_Atomic", "_BitInt", "_Bool", "_Complex", "_Decimal32",
    "_Decimal64", "_Decimal128", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local",
};

constexpr std::string_view kObjectiveCKeywords[] = {
    "id", "self", "super", "nil", "Nil", "YES", "NO", "instancetype", "in", "out", "inout", "bycopy", "byref",
    "oneway", "__block", "__weak", "__strong", "__unsafe_unretained", "__autoreleasing", "_Nonnull",
    "_Nullable", "nonnull", "nullable",
};

// Standard library type names that are no keywords.
constexpr std::string_view kCTypes[] = {
    "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t", "intmax_t", "uintmax_t", "int8_t", "int16_t",
    "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "FILE", "va_list", "max_align_t",
    "nullptr_t", "wint_t", "off_t", "time_t", "clock_t", "char16_t", "char32_t", "wchar_t",
};

constexpr std::string_view kCppTypes[] = {
    "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t", "intmax_t", "uintmax_t", "int8_t", "int16_t",
    "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "FILE", "va_list", "max_align_t",
    "nullptr_t", "string", "string_view", "wstring", "u8string", "vector", "array", "map", "multimap", "set",
    "multiset", "unordered_map", "unordered_set", "deque", "forward_list", "optional", "variant", "span", "pair",
    "tuple", "unique_ptr", "shared_ptr", "weak_ptr", "function", "byte", "ostream", "istream", "iostream",
    "ostringstream", "istringstream", "stringstream", "ifstream", "ofstream", "fstream", "exception",
    "runtime_error", "logic_error", "invalid_argument", "out_of_range", "error_code", "mutex", "thread",
    "atomic", "initializer_list",
};

constexpr std::string_view kObjectiveCTypes[] = {
    "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t", "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "FILE", "BOOL", "SEL", "IMP", "Class", "NSInteger",
    "NSUInteger", "CGFloat", "NSString", "NSArray", "NSDictionary", "NSNumber", "NSObject", "NSError",
};

constexpr std::string_view kCppStringPrefixes[] = {"L", "u", "U", "u8", "R", "LR", "uR", "UR", "u8R"};

struct CFamily {
  std::span<const std::string_view> keywords;
  std::span<const std::string_view> extra_keywords;
  std::span<const std::string_view> types;
  bool objective_c = false;
};

constexpr CFamily kCppFamily{kCppKeywords, {}, kCppTypes, false};
constexpr CFamily kCFamily{kCKeywords, {}, kCTypes, false};
constexpr CFamily kObjectiveCFamily{kCKeywords, kObjectiveCKeywords, kObjectiveCTypes, true};

void tokenizeCFamily(std::string_view s, Tokens& out, const CFamily& family) {
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
      const auto end = shortQuoteEnd(s, i);
      if (end != npos) {
        add(out, i, end, TokenKind::String);
        i = end;
      } else {
        ++i;
      }
      continue;
    }
    if (byte == '@' && family.objective_c && i + 1 < s.size()) {
      // @"literal" strings and @interface / @end / @property directives.
      if (s[i + 1] == '"') {
        const auto end = quotedEnd(s, i + 1, '"', true, true);
        add(out, i, end, TokenKind::String);
        i = end;
        continue;
      }
      if (identStart(s[i + 1])) {
        const auto end = identEnd(s, i + 1);
        add(out, i, end, TokenKind::Keyword);
        i = end;
        continue;
      }
    }
    if (identStart(byte)) {
      const auto j = identEnd(s, i);
      const auto word = s.substr(i, j - i);
      if (j < s.size() && (s[j] == '"' || s[j] == '\'') && listed(word, kCppStringPrefixes)) {
        if (word.back() == 'R' && s[j] == '"') {
          // The delimiter is at most 16 characters, so the search for its
          // '(' is bounded too.
          const auto open = s.substr(0, std::min(s.size(), j + 18)).find('(', j + 1);
          if (open != npos && open - j - 1 <= 16) {
            const std::string closer = ")" + std::string(s.substr(j + 1, open - j - 1)) + "\"";
            auto end = s.find(closer, open + 1);
            end = end == npos ? s.size() : end + closer.size();
            add(out, i, end, TokenKind::String);
            i = end;
            continue;
          }
        } else if (s[j] == '"') {
          const auto end = quotedEnd(s, j, '"', true, true);
          add(out, i, end, TokenKind::String);
          i = end;
          continue;
        } else if (const auto end = shortQuoteEnd(s, j); end != npos) {
          add(out, i, end, TokenKind::String);
          i = end;
          continue;
        }
      }
      if (listedIn(family.keywords, word) || listedIn(family.extra_keywords, word)) {
        add(out, i, j, TokenKind::Keyword);
      } else if (listedIn(family.types, word)) {
        add(out, i, j, TokenKind::Type);
      } else if (j < s.size() && s[j] == '(') {
        add(out, i, j, TokenKind::Function);
      }
      i = j;
      continue;
    }
    if (digit(byte)) {
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
      while (j < s.size() && (identChar(s[j]) || s[j] == '.')) ++j;
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
    if (identStart(byte)) {
      const auto j = identEnd(s, i);
      const auto word = s.substr(i, j - i);
      if (j < s.size() && (s[j] == '"' || s[j] == '\'') && word.size() <= 2 && listed(lower(word), kPythonStringPrefixes)) {
        i = pythonString(s, i, j, out);
        continue;
      }
      if (listed(word, kPythonKeywords)) add(out, i, j, TokenKind::Keyword);
      else if (j < s.size() && s[j] == '(') add(out, i, j, TokenKind::Function);
      i = j;
      continue;
    }
    if (digit(byte)) {
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

// A make variable reference at s[i] == '$' before `e`: $(NAME), ${NAME},
// $(NAME:a=b), $@ and the other automatic variables, and $$ (a dollar for
// the shell, with the shell variable name after it). A function call such
// as $(shell …) marks `$(shell` and leaves its arguments to the caller.
std::size_t makeVariable(std::string_view s, std::size_t i, std::size_t e, Tokens& out) {
  if (i + 1 >= e) return i + 1;
  const char next = s[i + 1];
  if (next == '$') {
    const auto end = identEnd(s.substr(0, e), i + 2);
    add(out, i, end, TokenKind::Variable);
    return end;
  }
  if (next == '(' || next == '{') {
    const char close = next == '(' ? ')' : '}';
    std::size_t j = i + 2;
    while (j < e && (identChar(s[j]) || has(".-/@<^*%?+", s[j]))) ++j;
    if (j < e && s[j] == close) {
      add(out, i, j + 1, TokenKind::Variable);
      return j + 1;
    }
    if (j < e && s[j] == ':') {
      std::size_t k = j;
      while (k < e && s[k] != close && s[k] != '$' && s[k] != '\n') ++k;
      if (k < e && s[k] == close) {
        add(out, i, k + 1, TokenKind::Variable);
        return k + 1;
      }
    }
    add(out, i, j, TokenKind::Variable);
    return j;
  }
  if (identChar(next) || has("@<^?*+|%", next)) {
    add(out, i, i + 2, TokenKind::Variable);
    return i + 2;
  }
  return i + 1;
}

// `make` marks a Makefile recipe: $(VAR), ${VAR} and $@ are make's, $$name is
// the shell's, and there are no here-documents.
void tokenizeShell(std::string_view s, Tokens& out, bool make = false) {
  std::string heredoc;  // a pending here-document delimiter
  bool heredoc_strip = false;
  Seeker brace(s, "}\n");
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
    if (byte == '$' && make) {
      i = makeVariable(s, i, s.size(), out);
      continue;
    }
    if (byte == '$') {
      if (i + 1 < s.size()) {
        const char next = s[i + 1];
        if (next == '{') {
          const auto close = brace.next(i + 2);
          const auto end = close < s.size() && s[close] == '}' ? close + 1 : close;
          add(out, i, end, TokenKind::Variable);
          i = end;
          continue;
        }
        if (identStart(next)) {
          const auto end = identEnd(s, i + 1);
          add(out, i, end, TokenKind::Variable);
          i = end;
          continue;
        }
        if (digit(next) || next == '@' || next == '*' || next == '#' || next == '?' || next == '$' || next == '!' ||
            next == '-') {
          add(out, i, i + 2, TokenKind::Variable);
          i += 2;
          continue;
        }
      }
      ++i;
      continue;
    }
    if (byte == '<' && !make && s.substr(i, 2) == "<<" && s.substr(i, 3) != "<<<") {
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
      while (k < s.size() && (identChar(s[k]) || s[k] == '-')) ++k;
      if (k > j && (quote == 0 || (k < s.size() && s[k] == quote))) {
        heredoc = std::string(s.substr(j, k - j));
        heredoc_strip = strip;
        i = quote == 0 ? k : k + 1;
        continue;
      }
      i += 2;
      continue;
    }
    if (identStart(byte)) {
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
                       [](char byte) { return std::isalnum(u(byte)) != 0; });
  }
  bool digits = false;
  while (i < value.size() && digit(value[i])) {
    ++i;
    digits = true;
  }
  if (i < value.size() && value[i] == '.') {
    ++i;
    while (i < value.size() && digit(value[i])) {
      ++i;
      digits = true;
    }
  }
  if (!digits) return false;
  if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
    ++i;
    if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
    bool exponent = false;
    while (i < value.size() && digit(value[i])) {
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
// Anchors, tags and quoted runs are consumed in a loop rather than by
// recursion, so a line of ten thousand `& ` cannot exhaust the stack.
void yamlValue(std::string_view s, std::size_t p, std::size_t end, std::size_t owner_indent, Tokens& out,
               std::size_t& block_indent) {
  while (true) {
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
      p = j;
      continue;
    }
    if (byte == '|' || byte == '>') {
      std::size_t j = p + 1;
      while (j < end && (s[j] == '+' || s[j] == '-' || digit(s[j]))) ++j;
      if (j == end || space(s[j])) {
        add(out, p, j, TokenKind::Keyword);
        block_indent = owner_indent;
        p = j;
        continue;
      }
    }
    if (byte == '"' || byte == '\'') {
      const auto quote_end = quotedEnd(s.substr(0, end), p, byte, byte == '"', true);
      add(out, p, quote_end, TokenKind::String);
      p = quote_end;
      continue;
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
    return;
  }
}

void tokenizeYaml(std::string_view s, Tokens& out) {
  std::size_t block_indent = npos;  // set while inside a `|` or `>` scalar
  for (std::size_t i = 0; i < s.size();) {
    const auto end = lineEnd(s, i);
    std::size_t indent = 0;
    while (i + indent < end && s[i + indent] == ' ') ++indent;
    bool blank_line = true;
    for (std::size_t k = i; k < end && blank_line; ++k) blank_line = space(s[k]);
    if (block_indent != npos) {
      if (blank_line || indent > block_indent) {
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
    if (digit(byte) || (byte == '-' && i + 1 < s.size() && digit(s[i + 1]))) {
      std::size_t j = i + 1;
      while (j < s.size() && (digit(s[j]) || s[j] == '.' || s[j] == 'e' || s[j] == 'E' || s[j] == '+' || s[j] == '-')) {
        ++j;
      }
      add(out, i, j, TokenKind::Number);
      i = j;
      continue;
    }
    if (identStart(byte)) {
      const auto j = identEnd(s, i);
      const auto word = s.substr(i, j - i);
      if (word == "true" || word == "false" || word == "null") add(out, i, j, TokenKind::Keyword);
      i = j;
      continue;
    }
    ++i;
  }
}

// ---- The table-driven tokenizer --------------------------------------------
//
// Most languages differ only in which comment markers, string forms and word
// lists they have, so one tokenizer reads a Syntax description per language.
// Word lists are space-separated (lower case for case-insensitive languages);
// marker lists too. The tokenizer looks at each byte once, apart from short
// lookaheads that are either bounded or remembered (see Seeker), and from
// scans that consume what they cover.

// An explicit string or comment form beyond the plain quote characters:
// triple quotes, verbatim and raw strings, pragmas. An empty `close` ends the
// run at the end of its line.
struct Delimited {
  std::string_view open;
  std::string_view close;
  TokenKind kind = TokenKind::String;
  bool escapes = true;
  bool interpolates = false;
  bool multiline = true;
};

// How an apostrophe that is no plain quote reads: C-like 'x' character
// literals, or those plus Rust lifetimes and ML type variables ('a).
enum class Chars : unsigned char { None, C, Lifetime };
// What a capitalised identifier is by language rule: a type or constructor
// (Haskell, OCaml, Elixir aliases, Ruby constants) or a variable (Erlang).
enum class Capitals : unsigned char { Plain, Types, Variables };
// Whether the first word of each line (assembly) or statement (nginx) is a
// keyword whatever it is.
enum class FirstWord : unsigned char { None, Line, Statement };

struct Syntax {
  std::string_view line_comments{};     // markers; a leading ^ allows one only at a statement start
  std::string_view block_comments{};    // open/close pairs
  std::string_view nested_comments{};   // open/close pairs that nest
  std::span<const Delimited> delimited{};
  std::string_view quotes{};            // single-line strings with escapes
  std::string_view multiline_quotes{};  // strings that may span lines, with escapes
  std::string_view raw_quotes{};        // strings that may span lines, without escapes
  std::string_view string_prefixes{};   // identifiers that may prefix a string (r"…"); "*" for any
  char escape = '\\';
  Chars chars = Chars::None;
  bool transpose = false;             // an apostrophe after an operand is an operator (MATLAB, Julia)
  std::string_view char_prefix{};       // a one-character literal after this ($a, ?a, \a, #\a)
  std::string_view keywords{};
  std::string_view types{};
  std::string_view literals{};          // constants such as true or nil, shown as keywords
  std::string_view variables{};         // predefined variables
  bool case_insensitive = false;
  std::string_view comment_words{};     // words that start a comment (REM)
  std::string_view end_markers{};       // a line starting with one ends the code (__END__)
  std::string_view definers{};          // the next identifier is a function name (def, fn)
  std::string_view type_definers{};     // the next identifier is a type name (class, struct)
  std::string_view ident_start_extra{};
  std::string_view ident_extra{};
  std::string_view sigils{};            // prefixes of variable names ($x, @x)
  std::string_view annotations{};       // prefixes of annotations, attributes and built-ins (@x)
  char line_annotation = '\0';        // the same, only at a line start (-module, %define)
  std::string_view markers{};           // literal directives such as <?php
  bool hash_preprocessor = false;     // # at a line start begins a directive line
  bool hash_attributes = false;       // #[…] and #![…] attributes
  std::string_view interpolation{};   // opener of code inside interpolating strings: ${, #{, \(
  std::string_view interpolating_quotes{};
  std::string_view string_sigils{};     // variable prefixes inside interpolating strings
  bool prefix_interpolates = false;   // only prefixed strings interpolate (Scala s"…")
  bool functions = true;              // an identifier before ( is a function
  bool regex = false;                 // /…/ where an operand is expected is a regular expression
  Capitals capitals = Capitals::Plain;
  bool colon_symbols = false;         // :name is a symbol or keyword
  bool key_colons = false;            // name: followed by space is a key
  bool assign_keys = false;           // name = … is a key (configuration languages)
  bool lisp = false;                  // the identifier after ( is the function
  FirstWord first_word = FirstWord::None;
  bool labels = false;                // name: at a line start is a label
  bool registers = false;             // numbered machine registers are variables
  bool heredocs = false;              // <<ID, <<~ID, <<-ID and <<<ID here-documents
  bool perl_quotes = false;           // q{…}, qw(…), m/…/, s/…/…/, tr/…/…/
  bool ruby_percent = false;          // %w[…], %q(…), %r{…}
  bool elixir_sigils = false;         // ~r/…/, ~s(…), ~S"""…"""
  bool rust_strings = false;          // r#"…"#, b"…", b'x'
  bool long_brackets = false;         // Lua and CMake [[…]] / [==[…]==], also after a comment marker
  std::string_view line_block_open{}; // a column-0 line block comment (=begin, or = for POD)
  std::string_view line_block_close{};
  bool macro_bang = false;            // name!( is a macro
  bool hyphen_functions = false;      // Verb-Noun is a command
  bool digit_quote = false;           // apostrophes inside numbers (8'hFF)
};

// The word lists of a Syntax, split and sorted once.
struct Words {
  explicit Words(const Syntax& x)
      : prefixes(x.string_prefixes), keywords(x.keywords), types(x.types), literals(x.literals),
        variables(x.variables), comment_words(x.comment_words), end_markers(x.end_markers), definers(x.definers),
        type_definers(x.type_definers) {
    eachWord(x.line_comments, [this](std::string_view word) { line_comments.push_back(word); });
    eachWord(x.markers, [this](std::string_view word) { markers.push_back(word); });
    pairs(x.block_comments, blocks);
    pairs(x.nested_comments, nested);
  }
  static void pairs(std::string_view list, std::vector<std::pair<std::string_view, std::string_view>>& into) {
    std::vector<std::string_view> words;
    eachWord(list, [&words](std::string_view word) { words.push_back(word); });
    for (std::size_t k = 0; k + 1 < words.size(); k += 2) into.emplace_back(words[k], words[k + 1]);
  }

  WordSet prefixes;
  WordSet keywords;
  WordSet types;
  WordSet literals;
  WordSet variables;
  WordSet comment_words;
  WordSet end_markers;
  WordSet definers;
  WordSet type_definers;
  std::vector<std::string_view> line_comments;
  std::vector<std::string_view> markers;
  std::vector<std::pair<std::string_view, std::string_view>> blocks;
  std::vector<std::pair<std::string_view, std::string_view>> nested;
};

// Numbered registers of the common architectures: r0–r31, x0–x30, w0–w30,
// v/q/d/s/h/b0–31, f0–31, xmm/ymm/zmm0–31 and r8d/r8w/r8b.
bool machineRegister(std::string_view word) {
  if (word.size() < 2 || word.size() > 5) return false;
  auto rest = word;
  if (rest.starts_with("xmm") || rest.starts_with("ymm") || rest.starts_with("zmm")) rest.remove_prefix(3);
  else if (has("rxwvqdshbf", rest.front())) rest.remove_prefix(1);
  else return false;
  std::size_t digits = 0;
  while (digits < rest.size() && digit(rest[digits])) ++digits;
  if (digits == 0 || digits > 2) return false;
  rest.remove_prefix(digits);
  return rest.empty() || (word.front() == 'r' && rest.size() == 1 && has("dwb", rest.front()));
}

bool hyphenated(std::string_view word) {
  for (std::size_t k = 1; k + 1 < word.size(); ++k) {
    if (word[k] == '-' && std::isalpha(u(word[k - 1])) != 0 && std::isalpha(u(word[k + 1])) != 0) return true;
  }
  return false;
}

char closerOf(char open) {
  switch (open) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '<': return '>';
    default: return open;
  }
}

class Generic {
 public:
  Generic(const Syntax& syntax, const Words& words, std::string_view s, Tokens& out)
      : x_(syntax), w_(words), s_(s), n_(s.size()), out_(out), newline_(s, "\n"), brace_(s, "}\n") {}

  void run() {
    std::size_t i = 0;
    // A shebang line; `#![…]` is a Rust inner attribute instead.
    if (s_.starts_with("#!") && !s_.starts_with("#![")) {
      i = newline_.next(0);
      emit(0, i, TokenKind::Comment);
    }
    while (i < n_) i = step(i);
  }

 private:
  struct Form {
    std::string_view close;  // empty: the string ends at the line end
    bool multiline = false;
    bool escapes = true;
    bool interpolates = false;
  };
  // An interpolation in progress: the string to resume and the bracket depth
  // at which its closing bracket appears.
  struct Frame {
    Form form;
    int depth = 0;
  };
  enum class Pending { None, Function, Type };
  static constexpr std::size_t kMaximumFrames = 16;

  void emit(std::size_t begin, std::size_t end, TokenKind kind) { add(out_, begin, end, kind); }
  char at(std::size_t i) const { return i < n_ ? s_[i] : '\0'; }

  std::size_t step(std::size_t i) {
    const char c = s_[i];
    if (c == '\n') return newline(i);
    if (blank(c)) return i + 1;
    const bool at_line = line_start_;
    const bool at_statement = statement_start_;
    const bool first = first_word_;
    const bool after_paren = after_paren_;
    const auto pending = pending_;
    line_start_ = statement_start_ = first_word_ = after_paren_ = false;
    pending_ = Pending::None;
    if (const auto end = structure(i, c, at_line, at_statement); end != npos) return end;
    if (identStart(c) || has(x_.ident_start_extra, c)) return word(i, at_line, first, after_paren, pending);
    if (digit(c)) {
      const auto end = genericNumberEnd(s_, i, x_.digit_quote);
      emit(i, end, TokenKind::Number);
      expect_operand_ = false;
      return end;
    }
    return punctuation(i, c);
  }

  std::size_t newline(std::size_t i) {
    line_start_ = statement_start_ = true;
    expect_operand_ = true;
    pending_ = Pending::None;
    if (x_.first_word != FirstWord::None) first_word_ = true;
    if (heredoc_.empty()) return i + 1;
    // A here-document body runs to the line that starts with its delimiter
    // (indented for <<~ and <<-, followed by ; for PHP).
    for (std::size_t j = i + 1; j < n_;) {
      const auto end = newline_.next(j);
      auto line = s_.substr(j, end - j);
      while (!line.empty() && blank(line.front())) line.remove_prefix(1);
      if (line.starts_with(heredoc_) && (line.size() == heredoc_.size() || !identChar(line[heredoc_.size()]))) {
        emit(i + 1, j, TokenKind::String);
        heredoc_.clear();
        return end;
      }
      j = end + 1;
    }
    emit(i + 1, n_, TokenKind::String);
    heredoc_.clear();
    return n_;
  }

  // Comments, strings and the other constructs that start with punctuation;
  // npos when none starts at i.
  std::size_t structure(std::size_t i, char c, bool at_line, bool at_statement) {
    const bool column0 = i == 0 || s_[i - 1] == '\n';
    if (column0 && !x_.line_block_open.empty() && lineBlockAt(i)) return lineBlock(i);
    for (const auto marker : w_.markers) {
      if (startsAt(s_, i, marker)) {
        emit(i, i + marker.size(), TokenKind::Preprocessor);
        expect_operand_ = true;
        return i + marker.size();
      }
    }
    if (x_.hash_attributes && c == '#' && (startsAt(s_, i, "#[") || startsAt(s_, i, "#!["))) return attribute(i);
    for (const auto& form : x_.delimited) {
      if (!startsAt(s_, i, form.open)) continue;
      if (form.kind == TokenKind::String) {
        return stringRun(i, i + form.open.size(), {form.close, form.multiline, form.escapes, form.interpolates});
      }
      return closed(i, i + form.open.size(), form.close, form.kind);
    }
    if (at_line && x_.hash_preprocessor && c == '#') return toLineEnd(i, TokenKind::Preprocessor);
    if (x_.long_brackets) {
      for (auto marker : w_.line_comments) {
        if (marker.front() == '^') marker.remove_prefix(1);
        if (startsAt(s_, i, marker) && longBracketLevel(i + marker.size()) != npos) {
          return longBracket(i, i + marker.size(), TokenKind::Comment);
        }
      }
      if (c == '[' && longBracketLevel(i) != npos) return longBracket(i, i, TokenKind::String);
    }
    for (const auto& [open, close] : w_.nested) {
      if (startsAt(s_, i, open)) return nestedComment(i, open, close);
    }
    for (const auto& [open, close] : w_.blocks) {
      if (startsAt(s_, i, open)) return closed(i, i + open.size(), close, TokenKind::Comment);
    }
    for (auto marker : w_.line_comments) {
      if (marker.front() == '^') {
        if (!at_statement) continue;
        marker.remove_prefix(1);
      }
      if (startsAt(s_, i, marker)) return toLineEnd(i, TokenKind::Comment);
    }
    if (c == '\'' && x_.transpose && !expect_operand_) return i + 1;
    if (c == '\'' && x_.chars != Chars::None) return apostrophe(i);
    if (Form form; quoteAt(i, form)) return stringRun(i, i + 1, form);
    if (!x_.char_prefix.empty() && startsAt(s_, i, x_.char_prefix)) {
      const auto end = prefixedCharEnd(i);
      emit(i, end, TokenKind::String);
      expect_operand_ = false;
      return end;
    }
    if (has(x_.sigils, c)) {
      if (const auto end = sigilEnd(i); end > i) {
        emit(i, end, TokenKind::Variable);
        expect_operand_ = false;
        return end;
      }
    }
    if (at_line && x_.line_annotation != '\0' && c == x_.line_annotation && identStart(at(i + 1))) return annotation(i);
    if (has(x_.annotations, c) && identStart(at(i + 1))) return annotation(i);
    if (x_.colon_symbols && c == ':') {
      if (const auto end = symbol(i); end != npos) return end;
    }
    if (x_.heredocs && c == '<' && heredoc_.empty() && startsAt(s_, i, "<<")) {
      if (const auto end = heredocStart(i); end != npos) return end;
    }
    if (x_.ruby_percent && c == '%' && expect_operand_) {
      if (const auto end = percentLiteral(i); end != npos) return end;
    }
    if (x_.elixir_sigils && c == '~') {
      if (const auto end = sigilLiteral(i); end != npos) return end;
    }
    if (x_.regex && c == '/' && expect_operand_ && i >= regex_blocked_until_) {
      if (const auto end = regexLiteral(i); end != npos) return end;
    }
    return npos;
  }

  std::size_t word(std::size_t i, bool at_line, bool first, bool after_paren, Pending pending) {
    auto end = wordEnd(i);
    const auto text = s_.substr(i, end - i);
    std::array<char, 64> buffer{};
    const auto key = x_.case_insensitive ? lowerInto(text, buffer) : text;
    if (end < n_) {
      if (x_.rust_strings) {
        if (const auto stop = rustString(i, end, text); stop != npos) return stop;
      }
      if (!x_.string_prefixes.empty() && (x_.string_prefixes == "*" || w_.prefixes.contains(key))) {
        Form form;
        std::size_t open = 0;
        if (stringFormAt(end, form, open)) {
          // r"…", br"…" and raw"…" take backslashes literally.
          if (text.size() <= 3 && text.find_first_of("rR") != npos) form.escapes = false;
          form.interpolates = x_.prefix_interpolates;
          return stringRun(i, end + open, form);
        }
      }
      if (x_.perl_quotes) {
        if (const auto stop = perlQuote(i, end, text); stop != npos) return stop;
      }
    }
    const bool column0 = i == 0 || s_[i - 1] == '\n';
    if (column0 && w_.end_markers.contains(text)) {
      emit(i, n_, TokenKind::Comment);
      return n_;
    }
    if (w_.comment_words.contains(key)) return toLineEnd(i, TokenKind::Comment);
    const char next = at(end);
    const bool keyword = w_.keywords.contains(key);
    const bool literal = w_.literals.contains(key);
    auto kind = TokenKind::Comment;
    bool classified = true;
    if (x_.lisp && (digit(text.front()) || (text.size() > 1 && has("+-", text.front()) && digit(text[1])))) {
      kind = TokenKind::Number;
    } else if (pending != Pending::None && !keyword && !literal) {
      kind = pending == Pending::Function ? TokenKind::Function : TokenKind::Type;
    } else if (x_.key_colons && next == ':' && (end + 1 >= n_ || blank(s_[end + 1]) || s_[end + 1] == '\n')) {
      kind = TokenKind::Property;
    } else if (x_.labels && at_line && next == ':' && at(end + 1) != ':') {
      kind = TokenKind::Function;
      ++end;  // the colon belongs to the label, and the mnemonic after it is still first
      first_word_ = true;
    } else if (x_.macro_bang && next == '!' && has("([{", at(end + 1))) {
      kind = TokenKind::Preprocessor;
      ++end;
    } else if (x_.assign_keys && assignmentFollows(end)) {
      kind = TokenKind::Property;
    } else if (keyword || literal) {
      kind = TokenKind::Keyword;
    } else if (w_.types.contains(key)) {
      kind = TokenKind::Type;
    } else if (w_.variables.contains(key) || (x_.registers && machineRegister(key))) {
      kind = TokenKind::Variable;
    } else if (first && x_.first_word != FirstWord::None) {
      kind = TokenKind::Keyword;
    } else if (x_.capitals == Capitals::Types && upper(text.front())) {
      kind = TokenKind::Type;
    } else if (x_.capitals == Capitals::Variables && (upper(text.front()) || text.front() == '_')) {
      kind = TokenKind::Variable;
    } else if (x_.lisp && after_paren) {
      kind = TokenKind::Function;
    } else if (x_.functions && next == '(') {
      kind = TokenKind::Function;
    } else if (x_.hyphen_functions && hyphenated(text)) {
      kind = TokenKind::Function;
    } else if (x_.hyphen_functions && text.size() > 1 && text.front() == '-' && std::isalpha(u(text[1])) != 0) {
      kind = TokenKind::Property;  // a parameter name such as -Path
    } else {
      classified = false;
    }
    if (classified) emit(i, end, kind);
    if (w_.definers.contains(key)) pending_ = Pending::Function;
    else if (w_.type_definers.contains(key)) pending_ = Pending::Type;
    // After a keyword such as `return` an operand follows, so a `/` there
    // starts a regular expression; after a name or literal it divides.
    expect_operand_ = keyword && !literal;
    return end;
  }

  std::size_t punctuation(std::size_t i, char c) {
    const bool statements = x_.first_word == FirstWord::Statement;
    switch (c) {
      case '(':
      case '[':
      case '{':
        ++depth_;
        expect_operand_ = true;
        after_paren_ = c == '(';
        if (c == '{' && statements) first_word_ = true;
        return i + 1;
      case ')':
      case ']':
      case '}':
        if (!frames_.empty() && frames_.back().depth == depth_) {
          // The bracket that closes an interpolation: the string resumes.
          const auto frame = frames_.back();
          frames_.pop_back();
          emit(i, i + 1, TokenKind::Variable);
          return stringRun(i + 1, i + 1, frame.form);
        }
        if (depth_ > 0) --depth_;
        // After a block a new statement, possibly a /regex/, may follow
        // (an awk pattern); after ) or ] a `/` divides.
        expect_operand_ = c == '}';
        if (c == '}' && statements) first_word_ = true;
        return i + 1;
      case ';':
        statement_start_ = true;
        if (statements) first_word_ = true;
        expect_operand_ = true;
        return i + 1;
      default:
        expect_operand_ = true;
        return i + 1;
    }
  }

  std::size_t wordEnd(std::size_t i) const {
    auto j = i + 1;
    while (j < n_ && (identChar(s_[j]) || has(x_.ident_extra, s_[j]) || has(x_.ident_start_extra, s_[j]))) ++j;
    return j;
  }

  std::size_t toLineEnd(std::size_t i, TokenKind kind) {
    const auto end = newline_.next(i);
    emit(i, end, kind);
    return end;
  }

  // A run from `begin` whose body starts at `body` and ends after `close`
  // (or at the line end when `close` is empty, or at the input end).
  std::size_t closed(std::size_t begin, std::size_t body, std::string_view close, TokenKind kind) {
    std::size_t end = n_;
    if (close.empty()) end = newline_.next(body);
    else if (const auto found = s_.find(close, body); found != npos) end = found + close.size();
    emit(begin, end, kind);
    return end;
  }

  std::size_t nestedComment(std::size_t i, std::string_view open, std::string_view close) {
    std::size_t depth = 0;
    std::size_t j = i;
    while (j < n_) {
      if (startsAt(s_, j, open)) {
        ++depth;
        j += open.size();
      } else if (startsAt(s_, j, close)) {
        j += close.size();
        if (--depth == 0) break;
      } else {
        ++j;
      }
    }
    emit(i, j, TokenKind::Comment);
    return j;
  }

  bool lineBlockAt(std::size_t i) const {
    const auto open = x_.line_block_open;
    if (open == "=") return s_[i] == '=' && std::isalpha(u(at(i + 1))) != 0;
    return startsAt(s_, i, open) && !identChar(at(i + open.size()));
  }

  // From an opening line to the end of the line starting with the closer.
  std::size_t lineBlock(std::size_t i) {
    std::size_t j = newline_.next(i);
    while (j < n_) {
      if (startsAt(s_, j + 1, x_.line_block_close)) {
        j = newline_.next(j + 1);
        break;
      }
      j = newline_.next(j + 1);
    }
    emit(i, j, TokenKind::Comment);
    return j;
  }

  std::size_t attribute(std::size_t i) {
    std::size_t j = i + (s_[i + 1] == '!' ? 3 : 2);
    int depth = 1;
    while (j < n_ && s_[j] != '\n') {
      const char c = s_[j++];
      if (c == '[') ++depth;
      else if (c == ']' && --depth == 0) break;
    }
    emit(i, j, TokenKind::Preprocessor);
    return j;
  }

  // The level of a long bracket [[ or [==[ at i, or npos. Levels are capped so
  // the closer stays short and searching for it stays linear.
  std::size_t longBracketLevel(std::size_t i) const {
    if (at(i) != '[') return npos;
    std::size_t j = i + 1;
    while (j < n_ && s_[j] == '=' && j - i <= 32) ++j;
    return at(j) == '[' ? j - i - 1 : npos;
  }

  std::size_t longBracket(std::size_t begin, std::size_t i, TokenKind kind) {
    const auto level = longBracketLevel(i);
    const std::string closer = "]" + std::string(level, '=') + "]";
    const auto found = s_.find(closer, i + level + 2);
    const auto end = found == npos ? n_ : found + closer.size();
    emit(begin, end, kind);
    if (kind == TokenKind::String) expect_operand_ = false;
    return end;
  }

  bool quoteAt(std::size_t i, Form& form) const {
    const char c = s_[i];
    const bool interpolates = has(x_.interpolating_quotes, c);
    if (has(x_.quotes, c)) form = {s_.substr(i, 1), false, true, interpolates};
    else if (has(x_.multiline_quotes, c)) form = {s_.substr(i, 1), true, true, interpolates};
    else if (has(x_.raw_quotes, c)) form = {s_.substr(i, 1), true, false, interpolates};
    else return false;
    return true;
  }

  bool stringFormAt(std::size_t i, Form& form, std::size_t& open) const {
    for (const auto& d : x_.delimited) {
      if (d.kind == TokenKind::String && startsAt(s_, i, d.open)) {
        form = {d.close, d.multiline, d.escapes, d.interpolates};
        open = d.open.size();
        return true;
      }
    }
    open = 1;
    return quoteAt(i, form);
  }

  // The string from `begin` whose body starts at j. Inside an interpolating
  // string, `$name` style variables become their own tokens, and an
  // interpolation opener (`${`) ends the string token: the code after it is
  // tokenized normally until its closing bracket resumes the string (see
  // punctuation). At most kMaximumFrames interpolations nest; deeper ones
  // stay part of the string.
  std::size_t stringRun(std::size_t begin, std::size_t j, const Form& form) {
    expect_operand_ = false;
    while (j < n_) {
      const char c = s_[j];
      if (form.interpolates) {
        if (startsAt(s_, j, x_.interpolation)) {
          if (x_.interpolation == "{" && at(j + 1) == '{') {  // {{ is a literal brace
            j += 2;
            continue;
          }
          if (frames_.size() < kMaximumFrames) {
            emit(begin, j, TokenKind::String);
            const auto open_end = j + x_.interpolation.size();
            emit(j, open_end, TokenKind::Variable);
            frames_.push_back({form, depth_});
            expect_operand_ = true;
            return open_end;
          }
        }
        if (has(x_.string_sigils, c)) {
          if (const auto end = sigilEnd(j); end > j) {
            emit(begin, j, TokenKind::String);
            emit(j, end, TokenKind::Variable);
            begin = j = end;
            continue;
          }
        }
      }
      if (form.escapes && x_.escape != '\0' && c == x_.escape && j + 1 < n_) {
        j += 2;
        continue;
      }
      if (form.close.empty()) {
        if (c == '\n') break;
      } else if (startsAt(s_, j, form.close)) {
        j += form.close.size();
        emit(begin, j, TokenKind::String);
        return j;
      }
      if (c == '\n' && !form.multiline) break;
      ++j;
    }
    emit(begin, j, TokenKind::String);
    return j;
  }

  // A variable after a sigil at i: $name, $1, ${…} on one line; i when none.
  std::size_t sigilEnd(std::size_t i) {
    const char next = at(i + 1);
    if (next == '{') {
      const auto close = brace_.next(i + 2);
      return close < n_ && s_[close] == '}' ? close + 1 : i;
    }
    if (identStart(next) || digit(next)) return identEnd(s_, i + 2);
    return i;
  }

  std::size_t apostrophe(std::size_t i) {
    if (const auto end = charLiteralEnd(s_, i, x_.escape); end != npos) {
      emit(i, end, TokenKind::String);
      expect_operand_ = false;
      return end;
    }
    if (x_.chars == Chars::Lifetime && identStart(at(i + 1))) {
      const auto end = identEnd(s_, i + 1);
      emit(i, end, TokenKind::Type);
      expect_operand_ = false;
      return end;
    }
    expect_operand_ = true;
    return i + 1;
  }

  std::size_t prefixedCharEnd(std::size_t i) const {
    auto j = i + x_.char_prefix.size();
    if (j >= n_ || s_[j] == '\n') return j;
    const bool backslash_prefix = x_.char_prefix.back() == '\\';
    if (s_[j] == '\\' && !backslash_prefix) j = std::min(n_, j + 2);
    else j = std::min(n_, j + utf8Length(s_[j]));
    // Named characters: \newline, #\space.
    if (backslash_prefix) {
      while (j < n_ && std::isalpha(u(s_[j])) != 0) ++j;
    }
    return j;
  }

  std::size_t annotation(std::size_t i) {
    auto end = i + 1;
    while (end < n_ && (identChar(s_[end]) || (s_[end] == '.' && identStart(at(end + 1))))) ++end;
    emit(i, end, TokenKind::Preprocessor);
    expect_operand_ = false;
    return end;
  }

  std::size_t symbol(std::size_t i) {
    auto j = i + 1;
    if (at(j) == ':') ++j;
    if (!(identStart(at(j)) || has(x_.ident_start_extra, at(j)))) return npos;
    if (i > 0 && (identChar(s_[i - 1]) || has(":)]}\"'", s_[i - 1]))) return npos;
    const auto end = wordEnd(j);
    emit(i, end, TokenKind::Property);
    expect_operand_ = false;
    return end;
  }

  bool assignmentFollows(std::size_t j) const {
    while (j < n_ && blank(s_[j])) ++j;
    return at(j) == '=' && !has("=>~", at(j + 1));
  }

  std::size_t heredocStart(std::size_t i) {
    auto j = i + 2;
    if (at(j) == '<') ++j;
    if (at(j) == '~' || at(j) == '-') ++j;
    char quote = '\0';
    if (at(j) == '\'' || at(j) == '"') quote = s_[j++];
    auto k = j;
    while (k < n_ && (std::isalnum(u(s_[k])) != 0 || s_[k] == '_')) ++k;
    const auto id = s_.substr(j, k - j);
    // Upper-case delimiters only, so `a <<b` stays a shift.
    if (id.empty() || !(upper(id.front()) || id.front() == '_')) return npos;
    if (quote != '\0') {
      if (at(k) != quote) return npos;
      ++k;
    } else if (!std::all_of(id.begin(), id.end(), [](char b) { return upper(b) || digit(b) || b == '_'; })) {
      return npos;
    }
    heredoc_ = std::string(id);
    return k;
  }

  // From an opening delimiter at j to just past its closer; brackets nest.
  std::size_t delimitedEnd(std::size_t j) const {
    const char open = s_[j];
    const char close = closerOf(open);
    int depth = 1;
    for (std::size_t k = j + 1; k < n_; ++k) {
      const char c = s_[k];
      if (c == '\\') {
        ++k;
        continue;
      }
      if (close != open && c == open) ++depth;
      else if (c == close && --depth == 0) return k + 1;
    }
    return n_;
  }

  std::size_t perlQuote(std::size_t i, std::size_t j, std::string_view text) {
    static constexpr std::string_view kOperators[] = {"q", "qq", "qw", "qr", "m", "s", "tr", "y"};
    if (!listed(text, kOperators)) return npos;
    const char d = s_[j];
    if (std::ispunct(u(d)) == 0 || has(",;)]}>=:-", d)) return npos;
    if (i >= 2 && s_[i - 1] == '>' && s_[i - 2] == '-') return npos;  // a method ->s(…)
    auto end = delimitedEnd(j);
    if (text == "s" || text == "tr" || text == "y") {
      if (closerOf(d) != d) {
        while (end < n_ && (blank(s_[end]) || s_[end] == '\n')) ++end;
        if (end < n_ && closerOf(s_[end]) != s_[end]) end = delimitedEnd(end);
      } else if (end - 1 > j && s_[end - 1] == d) {
        end = delimitedEnd(end - 1);
      }
    }
    while (end < n_ && std::isalpha(u(s_[end])) != 0) ++end;
    emit(i, end, TokenKind::String);
    expect_operand_ = false;
    return end;
  }

  std::size_t percentLiteral(std::size_t i) {
    auto j = i + 1;
    if (has("qQwWiIrsx", at(j)) && has("([{<|!/^~", at(j + 1))) ++j;
    if (!has("([{<|!/^~", at(j))) return npos;
    auto end = delimitedEnd(j);
    while (end < n_ && std::isalpha(u(s_[end])) != 0) ++end;
    emit(i, end, TokenKind::String);
    expect_operand_ = false;
    return end;
  }

  std::size_t sigilLiteral(std::size_t i) {
    auto j = i + 1;
    if (std::islower(u(at(j))) != 0) {
      ++j;
    } else {
      while (j < n_ && upper(s_[j])) ++j;
    }
    if (j == i + 1 || !has("/|\"'([{<", at(j))) return npos;
    std::size_t end = n_;
    const char d = s_[j];
    if ((d == '"' || d == '\'') && at(j + 1) == d && at(j + 2) == d) {
      const auto found = s_.find(s_.substr(j, 3), j + 3);
      end = found == npos ? n_ : found + 3;
    } else {
      end = delimitedEnd(j);
    }
    while (end < n_ && std::isalpha(u(s_[end])) != 0) ++end;
    emit(i, end, TokenKind::String);
    expect_operand_ = false;
    return end;
  }

  // A /…/ literal on one line; when it never closes, no other one starts
  // before the line end, so a line of slashes is scanned once.
  std::size_t regexLiteral(std::size_t i) {
    const char next = at(i + 1);
    if (i + 1 >= n_ || next == '\n' || next == '/' || next == '*') return npos;
    bool in_class = false;
    std::size_t j = i + 1;
    for (; j < n_; ++j) {
      const char c = s_[j];
      if (c == '\n') break;
      if (c == '\\') {
        if (at(j + 1) == '\n' || j + 1 >= n_) {
          ++j;
          break;
        }
        ++j;
        continue;
      }
      if (c == '[') in_class = true;
      else if (c == ']') in_class = false;
      else if (c == '/' && !in_class) break;
    }
    if (j < n_ && s_[j] == '/') {
      ++j;
      while (j < n_ && std::isalpha(u(s_[j])) != 0) ++j;
      emit(i, j, TokenKind::String);
      expect_operand_ = false;
      return j;
    }
    regex_blocked_until_ = j;
    return npos;
  }

  // Rust r"…", r#"…"#, br"…", b"…", c"…" and b'x'; npos for anything else.
  std::size_t rustString(std::size_t i, std::size_t j, std::string_view text) {
    if ((text == "r" || text == "br" || text == "cr") && (s_[j] == '"' || s_[j] == '#')) {
      std::size_t k = j;
      while (k < n_ && s_[k] == '#' && k - j < 255) ++k;
      if (at(k) == '"') {
        const std::string closer = "\"" + std::string(k - j, '#');
        const auto found = s_.find(closer, k + 1);
        const auto end = found == npos ? n_ : found + closer.size();
        emit(i, end, TokenKind::String);
        expect_operand_ = false;
        return end;
      }
      if (text == "r" && k == j + 1 && identStart(at(k))) {  // a raw identifier r#type
        expect_operand_ = false;
        return identEnd(s_, k);
      }
      return npos;
    }
    if (text == "b" && s_[j] == '\'') {
      if (const auto end = charLiteralEnd(s_, j, '\\'); end != npos) {
        emit(i, end, TokenKind::String);
        expect_operand_ = false;
        return end;
      }
    }
    if ((text == "b" || text == "c") && s_[j] == '"') return stringRun(i, j + 1, {s_.substr(j, 1), true, true, false});
    return npos;
  }

  const Syntax& x_;
  const Words& w_;
  std::string_view s_;
  std::size_t n_;
  Tokens& out_;
  Seeker newline_;
  Seeker brace_;
  bool line_start_ = true;
  bool statement_start_ = true;
  bool first_word_ = true;
  bool expect_operand_ = true;  // a `/` here starts a regular expression
  bool after_paren_ = false;
  Pending pending_ = Pending::None;
  int depth_ = 0;
  std::vector<Frame> frames_;
  std::string heredoc_;
  std::size_t regex_blocked_until_ = 0;
};

// ---- Language descriptions -------------------------------------------------
//
// Each list below is the language's reserved words and the standard names a
// reader expects to stand out; the field order follows struct Syntax, which
// designated initializers require.

constexpr Delimited kTripleDouble[] = {{"\"\"\"", "\"\"\""}};
constexpr Delimited kRawTripleInterpolated[] = {{"\"\"\"", "\"\"\"", TokenKind::String, false, true}};
constexpr Delimited kRawTriple[] = {{"\"\"\"", "\"\"\"", TokenKind::String, false}};

constexpr Delimited kCSharpStrings[] = {
    {"$@\"", "\"", TokenKind::String, false, true},
    {"@$\"", "\"", TokenKind::String, false, true},
    {"$\"\"\"", "\"\"\"", TokenKind::String, false, true},
    {"\"\"\"", "\"\"\"", TokenKind::String, false},
    {"$\"", "\"", TokenKind::String, true, true, false},
    {"@\"", "\"", TokenKind::String, false},
};

constexpr Syntax kCSharp{
    .line_comments = "//",
    .block_comments = "/* */",
    .delimited = kCSharpStrings,
    .quotes = "\"",
    .chars = Chars::C,
    .keywords = "abstract add alias and as ascending async await base break by case catch checked class const "
                "continue default delegate descending do dynamic else enum equals event explicit extern file "
                "finally fixed for foreach from get global goto group if implicit in init interface internal into "
                "is join let lock managed nameof namespace new not notnull on operator or orderby out override "
                "params partial private protected public readonly record ref remove required return scoped sealed "
                "select set sizeof stackalloc static struct switch this throw try typeof unchecked unmanaged "
                "unsafe using value var virtual volatile when where while with yield",
    .types = "bool byte char decimal double float int long nint nuint object sbyte short string uint ulong ushort "
             "void String Object Task List Dictionary IEnumerable Span",
    .literals = "true false null",
    .type_definers = "class struct interface enum record",
    .hash_preprocessor = true,
    .interpolation = "{",
};

constexpr Syntax kJava{
    .line_comments = "//",
    .block_comments = "/* */",
    .delimited = kTripleDouble,
    .quotes = "\"",
    .chars = Chars::C,
    .keywords = "abstract assert break case catch class const continue default do else enum exports extends "
                "final finally for goto if implements import instanceof interface module native new non-sealed "
                "open opens package permits private protected provides public record requires return sealed "
                "static strictfp super switch synchronized this throw throws to transient transitive try uses var "
                "volatile when while with yield",
    .types = "boolean byte char double float int long short void String Object Integer Long Double Float "
             "Boolean Character Byte Short Void Number List Map Set Optional",
    .literals = "true false null",
    .type_definers = "class interface enum record",
    .annotations = "@",
};

constexpr Syntax kKotlin{
    .line_comments = "//",
    .nested_comments = "/* */",
    .delimited = kRawTripleInterpolated,
    .quotes = "\"",
    .chars = Chars::C,
    .keywords = "abstract actual annotation as break by catch class companion const constructor continue "
                "crossinline data delegate do dynamic else enum expect external field file final finally for fun "
                "get if import in infix init inline inner interface internal is lateinit noinline object open "
                "operator out override package param private property protected public receiver reified return "
                "sealed set setparam super suspend tailrec this throw try typealias typeof val value var vararg "
                "when where while",
    .types = "Any Unit Nothing Int Long Short Byte Double Float Boolean Char String Array List MutableList Map "
             "MutableMap Set MutableSet UInt ULong UByte UShort Sequence Pair Triple IntArray ByteArray",
    .literals = "true false null",
    .definers = "fun",
    .type_definers = "class interface object typealias",
    .annotations = "@",
    .interpolation = "${",
    .interpolating_quotes = "\"",
    .string_sigils = "$",
};

constexpr Syntax kScala{
    .line_comments = "//",
    .nested_comments = "/* */",
    .delimited = kRawTriple,
    .quotes = "\"",
    .string_prefixes = "*",
    .chars = Chars::C,
    .keywords = "abstract case catch class def derives do else enum export extends extension final finally for "
                "forSome given if implicit import infix inline lazy match new object opaque open override package "
                "private protected return sealed super then this throw trait transparent try type using val var "
                "while with yield",
    .types = "Int Long Short Byte Double Float Boolean Char String Unit Any AnyRef AnyVal Nothing Null Option "
             "List Seq Map Set Vector Array Either Future",
    .literals = "true false null None Some Nil",
    .definers = "def",
    .type_definers = "class trait object type enum",
    .annotations = "@",
    .interpolation = "${",
    .string_sigils = "$",
    .prefix_interpolates = true,
};

constexpr Delimited kGroovyStrings[] = {
    {"'''", "'''"},
    {"\"\"\"", "\"\"\"", TokenKind::String, true, true},
};

constexpr Syntax kGroovy{
    .line_comments = "//",
    .block_comments = "/* */",
    .delimited = kGroovyStrings,
    .quotes = "'\"",
    .keywords = "abstract as assert break case catch class const continue def default do else enum extends final "
                "finally for goto if implements import in instanceof interface native new package private "
                "protected public return static super switch synchronized this throw throws trait transient try "
                "var volatile while",
    .types = "boolean byte char double float int long short void String Object Integer List Map",
    .literals = "true false null",
    .type_definers = "class interface trait enum",
    .annotations = "@",
    .interpolation = "${",
    .interpolating_quotes = "\"",
    .string_sigils = "$",
    .regex = true,
};

constexpr Delimited kDartStrings[] = {
    {"'''", "'''", TokenKind::String, true, true},
    {"\"\"\"", "\"\"\"", TokenKind::String, true, true},
};

constexpr Syntax kDart{
    .line_comments = "//",
    .nested_comments = "/* */",
    .delimited = kDartStrings,
    .quotes = "'\"",
    .string_prefixes = "r",
    .keywords = "abstract as assert async await base break case catch class const continue covariant default "
                "deferred do dynamic else enum export extends extension external factory final finally for get "
                "hide if implements import in interface is late library mixin new on operator part required "
                "rethrow return sealed set show static super switch sync this throw try typedef var void when "
                "while with yield",
    .types = "int double num bool String List Map Set Object Never Null Future Stream Iterable Record Function",
    .literals = "true false null",
    .type_definers = "class mixin enum extension typedef",
    .annotations = "@",
    .interpolation = "${",
    .interpolating_quotes = "'\"",
    .string_sigils = "$",
};

constexpr Delimited kSwiftStrings[] = {
    {"#\"\"\"", "\"\"\"#", TokenKind::String, false},
    {"#\"", "\"#", TokenKind::String, false, false, false},
    {"\"\"\"", "\"\"\"", TokenKind::String, true, true},
};

constexpr Syntax kSwift{
    .line_comments = "//",
    .nested_comments = "/* */",
    .delimited = kSwiftStrings,
    .quotes = "\"",
    .keywords = "actor any as associatedtype async await borrowing break case catch class consume consuming "
                "continue convenience default defer deinit didSet do dynamic each else enum extension "
                "fallthrough fileprivate final for func get guard if import in indirect init inout internal is "
                "isolated lazy let macro mutating nonisolated nonmutating open operator optional override package "
                "precedencegroup private protocol public repeat required rethrows return self Self set some "
                "static struct subscript super switch throw throws try typealias unowned var weak where while "
                "willSet",
    .types = "Int Int8 Int16 Int32 Int64 UInt UInt8 UInt16 UInt32 UInt64 Float Double Bool String Character "
             "Array Dictionary Set Optional Void Any AnyObject Never Result Error Data URL Date",
    .literals = "true false nil",
    .definers = "func",
    .type_definers = "class struct protocol enum extension actor typealias",
    .annotations = "@#",
    .hash_preprocessor = true,
    .interpolation = "\\(",
    .interpolating_quotes = "\"",
};

constexpr Syntax kGo{
    .line_comments = "//",
    .block_comments = "/* */",
    .quotes = "\"",
    .raw_quotes = "`",
    .chars = Chars::C,
    .keywords = "break case chan const continue default defer else fallthrough for func go goto if import "
                "interface map package range return select struct switch type var",
    .types = "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint "
             "uint8 uint16 uint32 uint64 uintptr any comparable",
    .literals = "true false nil iota",
    .definers = "func",
    .type_definers = "type",
};

constexpr Syntax kRust{
    .line_comments = "//",
    .nested_comments = "/* */",
    .multiline_quotes = "\"",
    .chars = Chars::Lifetime,
    .keywords = "abstract as async await become box break const continue crate do dyn else enum extern final fn "
                "for gen if impl in let loop macro macro_rules match mod move mut override priv pub ref return "
                "self Self static struct super trait try type typeof union unsafe unsized use virtual where while "
                "yield",
    .types = "bool char str u8 u16 u32 u64 u128 usize i8 i16 i32 i64 i128 isize f32 f64 String Vec Option "
             "Result Box Rc Arc Cell RefCell HashMap HashSet BTreeMap BTreeSet VecDeque Cow Path PathBuf",
    .literals = "true false Some None Ok Err",
    .definers = "fn",
    .type_definers = "struct enum trait type union",
    .hash_attributes = true,
    .rust_strings = true,
    .macro_bang = true,
};

constexpr Delimited kZigStrings[] = {{"\\\\", "", TokenKind::String, false, false, false}};

constexpr Syntax kZig{
    .line_comments = "//",
    .delimited = kZigStrings,
    .quotes = "\"",
    .chars = Chars::C,
    .keywords = "addrspace align allowzero and anyframe anytype asm break callconv catch comptime const continue "
                "defer else enum errdefer error export extern fn for if inline linksection noalias noinline "
                "nosuspend opaque or orelse packed pub resume return struct suspend switch test threadlocal try "
                "union unreachable usingnamespace var volatile while",
    .types = "i8 i16 i32 i64 i128 isize u1 u8 u16 u32 u64 u128 usize f16 f32 f64 f80 f128 bool void noreturn "
             "type anyerror anyopaque comptime_int comptime_float c_int c_uint c_long c_ulong c_char",
    .literals = "true false null undefined",
    .definers = "fn",
    .annotations = "@",
};

constexpr Syntax kD{
    .line_comments = "//",
    .block_comments = "/* */",
    .nested_comments = "/+ +/",
    .quotes = "\"",
    .raw_quotes = "`",
    .string_prefixes = "r x",
    .chars = Chars::C,
    .keywords = "abstract alias align asm assert auto body break case cast catch class const continue debug "
                "default delegate deprecated do else enum export extern final finally for foreach "
                "foreach_reverse function goto if immutable import in inout interface invariant is lazy mixin "
                "module new nothrow out override package pragma private protected public pure ref return scope "
                "shared static struct super switch synchronized template this throw try typeid typeof union "
                "unittest version while with __gshared __traits",
    .types = "bool byte ubyte short ushort int uint long ulong cent ucent float double real char wchar dchar "
             "void string wstring dstring size_t ptrdiff_t",
    .literals = "true false null",
    .type_definers = "class struct interface enum union template",
    .annotations = "@",
};

constexpr std::string_view kJavaScriptKeywords =
    "as async await break case catch class const continue debugger default delete do else export extends "
    "finally for from function get if import in instanceof let new of return set static super switch this "
    "throw try typeof var void while with yield";

constexpr Syntax kJavaScript{
    .line_comments = "//",
    .block_comments = "/* */",
    .quotes = "'\"",
    .multiline_quotes = "`",
    .keywords = kJavaScriptKeywords,
    .types = "Array Boolean Date Error Map Number Object Promise RegExp Set String Symbol BigInt WeakMap WeakSet",
    .literals = "true false null undefined NaN Infinity",
    .definers = "function",
    .type_definers = "class",
    .ident_start_extra = "$",
    .sigils = "#",
    .annotations = "@",
    .interpolation = "${",
    .interpolating_quotes = "`",
    .regex = true,
};

constexpr Syntax kTypeScript{
    .line_comments = "//",
    .block_comments = "/* */",
    .quotes = "'\"",
    .multiline_quotes = "`",
    .keywords = "abstract accessor as asserts async await break case catch class const continue debugger declare "
                "default delete do else enum export extends finally for from function get global if implements "
                "import in infer instanceof interface is keyof let module namespace new of out override private "
                "protected public readonly return satisfies set static super switch this throw try type typeof "
                "unique var void while with yield",
    .types = "any bigint boolean never number object string symbol unknown Array Boolean Date Error Map Number "
             "Object Promise Record Partial Readonly RegExp Set String Symbol",
    .literals = "true false null undefined NaN Infinity",
    .definers = "function",
    .type_definers = "class interface type enum namespace",
    .ident_start_extra = "$",
    .sigils = "#",
    .annotations = "@",
    .interpolation = "${",
    .interpolating_quotes = "`",
    .regex = true,
};

constexpr Syntax kPhp{
    .line_comments = "// #",
    .block_comments = "/* */",
    .multiline_quotes = "'\"`",
    .keywords = "abstract and array as break callable case catch class clone const continue declare default do "
                "echo else elseif empty enddeclare endfor endforeach endif endswitch endwhile enum eval exit "
                "extends final finally fn for foreach from function global goto if implements include "
                "include_once instanceof insteadof interface isset list match namespace new or print private "
                "protected public readonly require require_once return static switch throw trait try unset use "
                "var while xor yield",
    .types = "int float bool string object mixed void iterable never self parent",
    .literals = "true false null __class__ __dir__ __file__ __function__ __line__ __method__ __namespace__ "
                "__trait__",
    .case_insensitive = true,
    .definers = "function",
    .type_definers = "class interface trait enum",
    .sigils = "$",
    .markers = "<?php <?= ?>",
    .hash_attributes = true,
    .interpolating_quotes = "\"`",
    .string_sigils = "$",
    .heredocs = true,
};

constexpr Syntax kPerl{
    .line_comments = "#",
    .multiline_quotes = "'\"`",
    .keywords = "and bless chomp chop close cmp defined delete die do each else elsif eq eval exists exit for "
                "foreach ge goto grep gt if join keys last le local lt map my ne next no not open or our pop "
                "print printf push redo ref require return reverse say scalar shift sort splice split sprintf "
                "state sub undef unless unshift until use values wantarray warn while xor BEGIN END "
                "__PACKAGE__ __FILE__ __LINE__",
    .end_markers = "__END__ __DATA__",
    .definers = "sub",
    .type_definers = "package",
    .sigils = "$@%",
    .interpolating_quotes = "\"`",
    .string_sigils = "$@",
    .regex = true,
    .heredocs = true,
    .perl_quotes = true,
    .line_block_open = "=",
    .line_block_close = "=cut",
};

constexpr Syntax kRuby{
    .line_comments = "#",
    .multiline_quotes = "\"'`",
    .keywords = "BEGIN END alias and attr_accessor attr_reader attr_writer begin break case class def defined? "
                "do else elsif end ensure extend for if in include loop module next not or prepend private "
                "protected public raise redo require require_relative rescue retry return self super then undef "
                "unless until when while yield __method__ __FILE__ __LINE__ __dir__",
    .literals = "true false nil",
    .end_markers = "__END__",
    .definers = "def",
    .type_definers = "class module",
    .ident_extra = "?!",
    .sigils = "@$",
    .interpolation = "#{",
    .interpolating_quotes = "\"`",
    .regex = true,
    .capitals = Capitals::Types,
    .colon_symbols = true,
    .key_colons = true,
    .heredocs = true,
    .ruby_percent = true,
    .line_block_open = "=begin",
    .line_block_close = "=end",
};

constexpr Syntax kLua{
    .line_comments = "--",
    .quotes = "'\"",
    .keywords = "and break do else elseif end for function goto if in local not or repeat return then until "
                "while",
    .literals = "true false nil",
    .variables = "self",
    .definers = "function",
    .long_brackets = true,
};

constexpr Syntax kR{
    .line_comments = "#",
    .multiline_quotes = "\"'",
    .keywords = "if else repeat while function for in next break return switch library require",
    .literals = "TRUE FALSE NULL NA NA_integer_ NA_real_ NA_character_ NA_complex_ Inf NaN",
    .ident_start_extra = ".",
    .ident_extra = ".",
};

constexpr Delimited kJuliaStrings[] = {{"\"\"\"", "\"\"\"", TokenKind::String, true, true}};

constexpr Syntax kJulia{
    .line_comments = "#",
    .nested_comments = "#= =#",
    .delimited = kJuliaStrings,
    .multiline_quotes = "\"",
    .raw_quotes = "`",
    .string_prefixes = "*",
    .chars = Chars::C,
    .transpose = true,
    .keywords = "abstract baremodule begin break catch const continue do else elseif end export finally for "
                "function global if import in isa let local macro module mutable outer primitive public quote "
                "return struct try type using where while",
    .types = "Int Int8 Int16 Int32 Int64 Int128 UInt UInt8 UInt16 UInt32 UInt64 UInt128 Float16 Float32 Float64 "
             "Bool Char String Symbol Any Nothing Missing Vector Matrix Array Dict Set Tuple NamedTuple Function "
             "Number Real Integer AbstractFloat AbstractString AbstractArray AbstractVector AbstractMatrix "
             "Complex Rational BigInt BigFloat",
    .literals = "true false nothing missing Inf NaN",
    .definers = "function macro",
    .type_definers = "struct",
    .ident_extra = "!",
    .annotations = "@",
    .interpolation = "$(",
    .interpolating_quotes = "\"",
    .string_sigils = "$",
    .colon_symbols = true,
};

constexpr Delimited kNimStrings[] = {
    {"{.", ".}", TokenKind::Preprocessor},
    {"\"\"\"", "\"\"\"", TokenKind::String, false},
};

constexpr Syntax kNim{
    .line_comments = "#",
    .nested_comments = "#[ ]#",
    .delimited = kNimStrings,
    .quotes = "\"",
    .string_prefixes = "*",
    .chars = Chars::C,
    .keywords = "addr and as asm bind block break case cast concept const continue converter defer discard "
                "distinct div do elif else end enum except export finally for from func if import in include "
                "interface is isnot iterator let macro method mixin mod not notin object of or out proc ptr raise "
                "ref return shl shr static template try tuple type using var when while xor yield",
    .types = "int int8 int16 int32 int64 uint uint8 uint16 uint32 uint64 float float32 float64 bool char string "
             "cstring pointer seq array set openArray varargs void auto untyped typed Natural Positive",
    .literals = "true false nil",
    .definers = "proc func method iterator template macro converter",
};

constexpr Syntax kCrystal{
    .line_comments = "#",
    .multiline_quotes = "\"",
    .chars = Chars::C,
    .keywords = "abstract alias annotation as asm begin break case class def do else elsif end ensure enum "
                "extend for fun if in include instance_sizeof lib macro module next of out pointerof private "
                "protected require rescue return select self sizeof struct super then type typeof uninitialized "
                "union unless until verbatim when while with yield",
    .types = "Int8 Int16 Int32 Int64 UInt8 UInt16 UInt32 UInt64 Float32 Float64 String Char Bool Symbol Array "
             "Hash Nil Tuple NamedTuple Proc Pointer Slice",
    .literals = "true false nil",
    .definers = "def macro fun",
    .type_definers = "class module struct enum lib annotation",
    .ident_extra = "?!",
    .sigils = "@$",
    .interpolation = "#{",
    .interpolating_quotes = "\"",
    .regex = true,
    .capitals = Capitals::Types,
    .colon_symbols = true,
    .key_colons = true,
    .heredocs = true,
    .ruby_percent = true,
};

constexpr Delimited kElixirStrings[] = {
    {"\"\"\"", "\"\"\"", TokenKind::String, true, true},
    {"'''", "'''", TokenKind::String, true, true},
};

constexpr Syntax kElixir{
    .line_comments = "#",
    .delimited = kElixirStrings,
    .multiline_quotes = "\"'",
    .char_prefix = "?",
    .keywords = "after alias and case catch cond def defdelegate defexception defguard defimpl defmacro "
                "defmacrop defmodule defoverridable defp defprotocol defstruct do else end fn for if import in "
                "not or quote raise receive require rescue reraise super try unless unquote unquote_splicing use "
                "when with",
    .literals = "true false nil",
    .definers = "def defp defmacro defmacrop defguard defdelegate",
    .ident_extra = "?!",
    .annotations = "@",
    .interpolation = "#{",
    .interpolating_quotes = "\"'",
    .capitals = Capitals::Types,
    .colon_symbols = true,
    .key_colons = true,
    .elixir_sigils = true,
};

constexpr Syntax kErlang{
    .line_comments = "%",
    .quotes = "'",
    .multiline_quotes = "\"",
    .char_prefix = "$",
    .keywords = "after and andalso band begin bnot bor bsl bsr bxor case catch cond div else end fun if let maybe "
                "not of or orelse receive rem try when xor",
    .literals = "true false undefined",
    .annotations = "?",
    .line_annotation = '-',
    .capitals = Capitals::Variables,
};

constexpr Delimited kHaskellPragmas[] = {{"{-#", "#-}", TokenKind::Preprocessor}};

constexpr Syntax kHaskell{
    .line_comments = "--",
    .nested_comments = "{- -}",
    .delimited = kHaskellPragmas,
    .quotes = "\"",
    .chars = Chars::C,
    .keywords = "as case class data default deriving do else family forall foreign hiding if import in infix "
                "infixl infixr instance let mdo module newtype of pattern proc qualified rec then type where",
    .ident_extra = "'",
    .capitals = Capitals::Types,
};

constexpr Syntax kElm{
    .line_comments = "--",
    .nested_comments = "{- -}",
    .delimited = kTripleDouble,
    .quotes = "\"",
    .chars = Chars::C,
    .keywords = "alias as case effect else exposing if import in let module of port then type where",
    .ident_extra = "'",
    .capitals = Capitals::Types,
};

constexpr Delimited kOCamlStrings[] = {{"{|", "|}", TokenKind::String, false}};

constexpr Syntax kOCaml{
    .nested_comments = "(* *)",
    .delimited = kOCamlStrings,
    .multiline_quotes = "\"",
    .chars = Chars::Lifetime,
    .keywords = "and as assert asr begin class constraint do done downto else end exception external for fun "
                "function functor if in include inherit initializer land lazy let lor lsl lsr lxor match method "
                "mod module mutable new nonrec object of open or private rec sig struct then to try type val "
                "virtual when while with",
    .types = "int float bool char string unit list array option ref bytes exn int32 int64 nativeint format",
    .literals = "true false",
    .type_definers = "type",
    .ident_extra = "'",
    .capitals = Capitals::Types,
};

constexpr Delimited kFSharpStrings[] = {
    {"$\"\"\"", "\"\"\"", TokenKind::String, false, true},
    {"\"\"\"", "\"\"\"", TokenKind::String, false},
    {"$@\"", "\"", TokenKind::String, false, true},
    {"@$\"", "\"", TokenKind::String, false, true},
    {"@\"", "\"", TokenKind::String, false},
    {"$\"", "\"", TokenKind::String, true, true},
};

constexpr Syntax kFSharp{
    .line_comments = "//",
    .nested_comments = "(* *)",
    .delimited = kFSharpStrings,
    .multiline_quotes = "\"",
    .chars = Chars::Lifetime,
    .keywords = "abstract and as assert base begin class default delegate do done downcast downto elif else end "
                "exception extern finally fixed for fun function global if in inherit inline interface internal "
                "lazy let match member module mutable namespace new not of open or override private public rec "
                "return select sig static struct then to try type upcast use val void when while with yield",
    .types = "int int8 int16 int32 int64 uint8 uint16 uint32 uint64 byte sbyte float float32 double decimal bool "
             "char string unit obj list array seq option voption exn bigint nativeint Map Set Result Async Task",
    .literals = "true false null",
    .type_definers = "type",
    .ident_extra = "'",
    .hash_preprocessor = true,
    .interpolation = "{",
};

// Lisp symbols may contain almost any punctuation.
constexpr std::string_view kLispSymbolChars = "!$%&*+-./<=>?^_~";

constexpr Syntax kCommonLisp{
    .line_comments = ";",
    .nested_comments = "#| |#",
    .multiline_quotes = "\"",
    .char_prefix = "#\\",
    .keywords = "and apply block case cond declaim declare decf defclass defconstant defgeneric defmacro "
                "defmethod defpackage defparameter defstruct defun defvar destructuring-bind do do* dolist "
                "dotimes ecase error etypecase eval-when flet format funcall function handler-bind handler-case "
                "if ignore-errors in-package incf labels lambda let let* loop macrolet multiple-value-bind not or "
                "pop prog1 prog2 progn provide push quote require return return-from setf setq signal the "
                "typecase unless unwind-protect values when with-open-file with-slots",
    .literals = "t nil",
    .definers = "defun defmacro defgeneric defmethod",
    .type_definers = "defclass defstruct",
    .ident_start_extra = kLispSymbolChars,
    .ident_extra = kLispSymbolChars,
    .functions = false,
    .colon_symbols = true,
    .lisp = true,
};

constexpr Syntax kScheme{
    .line_comments = ";",
    .nested_comments = "#| |#",
    .multiline_quotes = "\"",
    .char_prefix = "#\\",
    .keywords = "and begin call-with-current-continuation call/cc case cond define define-record-type "
                "define-syntax define-values delay do dynamic-wind else export force guard if import lambda let "
                "let* let-values letrec letrec* library match module or parameterize provide quasiquote quote "
                "require set! struct syntax-case syntax-rules unless unquote when #lang",
    .literals = "#t #f #true #false",
    .ident_start_extra = "!$%&*+-./<=>?^_~#",
    .ident_extra = kLispSymbolChars,
    .functions = false,
    .lisp = true,
};

constexpr Delimited kClojureStrings[] = {{"#\"", "\"", TokenKind::String, true, false, true}};

constexpr Syntax kClojure{
    .line_comments = ";",
    .delimited = kClojureStrings,
    .multiline_quotes = "\"",
    .char_prefix = "\\",
    .keywords = "-> ->> and as-> binding case catch cond cond-> cond->> condp def defmacro defmethod defmulti defn "
                "defn- defonce defprotocol defrecord deftype do doseq dotimes doto finally fn for if if-let if-not "
                "import let letfn loop new ns or quote recur require set! some-> some->> throw try use var when "
                "when-let when-not when-some",
    .literals = "true false nil",
    .definers = "defn defn- defmacro defmulti defmethod",
    .type_definers = "defprotocol defrecord deftype",
    .ident_start_extra = kLispSymbolChars,
    .ident_extra = "!$%&*+-./<=>?^_~'#",
    .functions = false,
    .colon_symbols = true,
    .lisp = true,
};

constexpr Delimited kSqlStrings[] = {{"$$", "$$", TokenKind::String, false}};

constexpr Syntax kSql{
    .line_comments = "--",
    .block_comments = "/* */",
    .delimited = kSqlStrings,
    .multiline_quotes = "'\"`",
    .escape = '\0',
    .keywords = "add all alter analyze and any as asc attach auto_increment begin between both by cascade case "
                "cast catch charset check close collate column commit conflict constraint create cross cube "
                "current_date current_time current_timestamp cursor database declare default delete desc detach "
                "distinct do drop else end engine escape except exec execute exists explain extension fetch "
                "filter following for foreign from full function go grant group grouping having identity if "
                "ilike in index inner insert intersect into is join key language lateral leading left like limit "
                "loop materialized merge matched natural not nothing of offset on open or order outer output over "
                "owner partition pragma preceding primary print procedure range recursive references replace "
                "return returning returns revoke right role rollback rollup row rows schema select sequence set "
                "some table temporary then to top trailing transaction trigger truncate try unbounded union "
                "unique update use user using vacuum values view when where while window with within",
    .types = "bigint bigserial binary bit blob bool boolean bytea char character date datetime datetime2 decimal "
             "double float image int integer interval json jsonb money nchar ntext numeric nvarchar precision "
             "real serial smallint smallserial text time timestamp timestamptz tinyint uniqueidentifier uuid "
             "varbinary varchar varying xml year",
    .literals = "true false null unknown",
    .case_insensitive = true,
    .sigils = "@$",
};

constexpr Syntax kVisualBasic{
    .line_comments = "'",
    .quotes = "\"",
    .escape = '\0',
    .keywords = "addhandler addressof alias and andalso as async await byref byval call case catch cbool cbyte "
                "cchar cdate cdbl cdec cint class clng cobj const continue csbyte cshort csng cstr ctype cuint "
                "culng cushort declare default delegate dim directcast do each else elseif end endif enum erase "
                "error event exit finally for friend function get gettype global gosub goto handles if implements "
                "imports in inherits interface is isnot iterator let lib like loop me mod module mustinherit "
                "mustoverride mybase myclass namespace narrowing new next not of on operator option optional or "
                "orelse overloads overridable overrides paramarray partial private property protected public "
                "raiseevent readonly redim removehandler resume return select set shadows shared static step "
                "stop structure sub synclock then throw to try trycast typeof using when while widening with "
                "withevents writeonly xor yield",
    .types = "boolean byte char date decimal double integer long object sbyte short single string uinteger ulong "
             "ushort variant",
    .literals = "true false nothing",
    .case_insensitive = true,
    .comment_words = "rem",
    .definers = "sub function property",
    .type_definers = "class structure interface enum module",
    .hash_preprocessor = true,
};

constexpr Delimited kPowerShellStrings[] = {
    {"@\"", "\n\"@", TokenKind::String, true, true},
    {"@'", "\n'@", TokenKind::String, false},
};

constexpr Syntax kPowerShell{
    .line_comments = "#",
    .block_comments = "<# #>",
    .delimited = kPowerShellStrings,
    .multiline_quotes = "\"",
    .raw_quotes = "'",
    .escape = '`',
    .keywords = "begin break catch class clean continue data define do dynamicparam else elseif end enum exit "
                "filter finally for foreach from function hidden if in inlinescript param parallel process "
                "return sequence static switch throw trap try until using var while workflow -and -as -band "
                "-bnot -bor -bxor -ceq -clike -cmatch -cne -contains -creplace -eq -f -ge -gt -ieq -in -is "
                "-isnot -join -le -like -lt -match -ne -not -notcontains -notin -notlike -notmatch -or -replace "
                "-split -xor",
    .types = "string int long bool double float decimal char byte array hashtable object void datetime regex "
             "xml scriptblock pscustomobject psobject",
    .case_insensitive = true,
    .definers = "function filter",
    .type_definers = "class enum",
    .ident_start_extra = "-",
    .ident_extra = "-",
    .sigils = "$@",
    .interpolation = "$(",
    .interpolating_quotes = "\"",
    .string_sigils = "$",
    .hyphen_functions = true,
};

constexpr Syntax kFortran{
    .line_comments = "!",
    .quotes = "'\"",
    .escape = '\0',
    .keywords = "allocatable allocate assign associate asynchronous backspace bind block call case class close "
                "codimension common concurrent contains contiguous continue critical cycle data deallocate "
                "default deferred dimension do elemental else elseif elsewhere end enddo endfunction endif "
                "endmodule endprogram endselect endsubroutine endtype entry enum enumerator equivalence error "
                "exit extends external final flush forall format function generic go goto if implicit import "
                "include inout inquire intent interface intrinsic in kind len module namelist none "
                "non_overridable nopass nullify only open operator optional out parameter pass pause pointer "
                "print private procedure program protected public pure read recursive result return rewind save "
                "select sequence stop submodule subroutine target then to type use value volatile wait where "
                "while write",
    .types = "integer real double precision complex character logical",
    .case_insensitive = true,
    .definers = "subroutine function",
    .type_definers = "module program",
    .hash_preprocessor = true,
};

constexpr Delimited kPascalDirectives[] = {{"{$", "}", TokenKind::Preprocessor}};

constexpr Syntax kPascal{
    .line_comments = "//",
    .block_comments = "{ } (* *)",
    .delimited = kPascalDirectives,
    .quotes = "'",
    .escape = '\0',
    .keywords = "absolute abstract and array as asm assembler begin case cdecl class const constructor default "
                "destructor div do downto else end except exports external file finalization finally for "
                "forward function generic goto helper if implementation in inherited initialization inline "
                "interface is label library mod nil not object of on operator or out overload override packed "
                "private procedure program property protected public published raise read record reintroduce "
                "repeat resourcestring set shl shr specialize stdcall strict then threadvar to try type unit "
                "until uses var virtual while with write xor",
    .types = "integer cardinal shortint smallint longint int64 byte word longword qword real single double "
             "extended currency boolean char widechar ansichar string ansistring widestring unicodestring pchar "
             "pointer variant tobject",
    .literals = "true false nil self result",
    .case_insensitive = true,
    .definers = "procedure function constructor destructor",
};

constexpr Syntax kAda{
    .line_comments = "--",
    .quotes = "\"",
    .escape = '\0',
    .chars = Chars::C,
    .keywords = "abort abs abstract accept access aliased all and array at begin body case constant declare "
                "delay delta digits do else elsif end entry exception exit for function generic goto if in "
                "interface is limited loop mod new not null of or others out overriding package parallel pragma "
                "private procedure protected raise range record rem renames requeue return reverse select "
                "separate some subtype synchronized tagged task terminate then type until use when while with "
                "xor",
    .types = "integer natural positive float long_float boolean character string wide_string duration "
             "long_integer short_integer",
    .literals = "true false",
    .case_insensitive = true,
    .definers = "procedure function",
};

constexpr Syntax kMatlab{
    .line_comments = "%",
    .block_comments = "%{ %}",
    .quotes = "\"'",
    .escape = '\0',
    .transpose = true,
    .keywords = "arguments break case catch classdef continue else elseif end enumeration events for function "
                "global if methods otherwise parfor persistent properties return spmd switch try while",
    .types = "double single int8 int16 int32 int64 uint8 uint16 uint32 uint64 logical char string cell struct",
    .literals = "true false pi inf nan eps",
};

constexpr Syntax kVerilog{
    .line_comments = "//",
    .block_comments = "/* */",
    .quotes = "\"",
    .keywords = "always always_comb always_ff always_latch and assert assign assume automatic begin bit break "
                "buf byte case casex casez class clocking const constraint continue cover covergroup coverpoint "
                "default defparam disable do else end endcase endclass endclocking endfunction endgenerate "
                "endgroup endinterface endmodule endpackage endprimitive endprogram endproperty endsequence "
                "endspecify endtask enum event export extends extern final for force foreach forever fork "
                "function generate genvar if iff import initial inout input inside int integer interface join "
                "join_any join_none localparam logic longint modport module nand negedge new nor not null or "
                "output package packed parameter posedge primitive priority program property pure rand randc "
                "real realtime ref reg release repeat return sequence shortint signed specify static string "
                "struct super supply0 supply1 task this time tri typedef union unique unsigned virtual void wait "
                "wand while wire with wor xnor xor",
    .definers = "module function task",
    .sigils = "$",
    .annotations = "`",
    .digit_quote = true,
};

constexpr Syntax kVhdl{
    .line_comments = "--",
    .block_comments = "/* */",
    .quotes = "\"",
    .string_prefixes = "x b o",
    .escape = '\0',
    .chars = Chars::C,
    .keywords = "abs access after alias all and architecture array assert attribute begin block body buffer bus "
                "case component configuration constant context disconnect downto else elsif end entity exit "
                "file for force function generate generic group guarded if impure in inertial inout is label "
                "library linkage literal loop map mod nand new next nor not null of on open or others out "
                "package parameter port postponed procedure process pure range record register reject release "
                "rem report return rol ror select severity shared signal sla sll sra srl subtype then to "
                "transport type unaffected units until use variable wait when while with xnor xor",
    .types = "bit bit_vector boolean character integer natural positive real string time std_logic "
             "std_logic_vector std_ulogic std_ulogic_vector signed unsigned",
    .literals = "true false",
    .case_insensitive = true,
    .definers = "function procedure",
    .type_definers = "entity",
};

constexpr Syntax kSolidity{
    .line_comments = "//",
    .block_comments = "/* */",
    .quotes = "\"'",
    .string_prefixes = "hex unicode",
    .keywords = "abstract anonymous as assembly break calldata catch constant constructor continue contract "
                "delete do else emit enum error event external fallback for function if immutable import indexed "
                "interface internal is library mapping memory modifier new override payable pragma private "
                "public pure receive return returns revert storage struct transient try type unchecked using "
                "view virtual while",
    .types = "address bool string bytes byte int uint int8 int16 int32 int64 int128 int256 uint8 uint16 uint32 "
             "uint64 uint128 uint160 uint256 bytes1 bytes2 bytes4 bytes8 bytes16 bytes20 bytes32 fixed ufixed",
    .literals = "true false wei gwei ether seconds minutes hours days weeks this super msg block tx",
    .definers = "function modifier event error",
    .type_definers = "contract interface library struct enum",
};

constexpr Delimited kGraphQLStrings[] = {{"\"\"\"", "\"\"\"", TokenKind::String, false}};

constexpr Syntax kGraphQL{
    .line_comments = "#",
    .delimited = kGraphQLStrings,
    .quotes = "\"",
    .keywords = "query mutation subscription fragment on type interface union enum input scalar schema extend "
                "directive implements repeatable",
    .types = "Int Float String Boolean ID",
    .literals = "true false null",
    .definers = "query mutation subscription fragment",
    .type_definers = "type interface union enum input scalar",
    .sigils = "$",
    .annotations = "@",
    .key_colons = true,
};

constexpr Syntax kProtobuf{
    .line_comments = "//",
    .block_comments = "/* */",
    .quotes = "\"'",
    .keywords = "edition enum extend extensions import map max message oneof option optional package public "
                "repeated required reserved returns rpc service stream syntax to weak",
    .types = "double float int32 int64 uint32 uint64 sint32 sint64 fixed32 fixed64 sfixed32 sfixed64 bool "
             "string bytes",
    .literals = "true false",
    .definers = "rpc",
    .type_definers = "message enum service",
};

constexpr Syntax kThrift{
    .line_comments = "// #",
    .block_comments = "/* */",
    .quotes = "\"'",
    .keywords = "const cpp_include enum exception extends include namespace oneway optional required service "
                "struct throws typedef union",
    .types = "bool byte i8 i16 i32 i64 double string binary list set map uuid void",
    .literals = "true false",
    .type_definers = "struct enum service exception union",
};

constexpr Syntax kHcl{
    .line_comments = "# //",
    .block_comments = "/* */",
    .quotes = "\"",
    .keywords = "backend check content data dynamic else endfor endif for for_each if import in lifecycle locals "
                "module moved output provider removed required_providers resource terraform variable",
    .literals = "true false null",
    .interpolation = "${",
    .interpolating_quotes = "\"",
    .assign_keys = true,
    .heredocs = true,
};

constexpr Delimited kNixStrings[] = {{"''", "''", TokenKind::String, false, true}};

constexpr Syntax kNix{
    .line_comments = "#",
    .block_comments = "/* */",
    .delimited = kNixStrings,
    .multiline_quotes = "\"",
    .keywords = "assert else if import in inherit let or rec then with",
    .literals = "true false null",
    .variables = "builtins",
    .ident_extra = "-'",
    .interpolation = "${",
    .interpolating_quotes = "\"",
    .assign_keys = true,
};

constexpr Syntax kAwk{
    .line_comments = "#",
    .quotes = "\"",
    .keywords = "BEGIN BEGINFILE END ENDFILE break continue delete do else exit for func function getline if in "
                "next nextfile print printf return while",
    .variables = "ARGC ARGV CONVFMT ENVIRON FILENAME FNR FS NF NR OFMT OFS ORS RLENGTH RS RSTART SUBSEP",
    .definers = "function func",
    .sigils = "$",
    .regex = true,
};

constexpr Syntax kTcl{
    .line_comments = "^#",
    .multiline_quotes = "\"",
    .keywords = "after append apply array break catch chan clock close concat continue coroutine dict else "
                "elseif encoding error eval exec expr file finally for foreach format gets global if incr info "
                "interp join lappend lassign lindex linsert list llength lmap lrange lreplace lsearch lset lsort "
                "namespace on open package proc puts regexp regsub rename require return set source split string "
                "subst switch tailcall then throw trap try unset uplevel upvar variable vwait while yield",
    .definers = "proc",
    .sigils = "$",
    .interpolation = "[",
    .interpolating_quotes = "\"",
    .string_sigils = "$",
};

constexpr Syntax kCMake{
    .line_comments = "#",
    .multiline_quotes = "\"",
    .keywords = "if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro "
                "return break continue block endblock and or not command defined equal exists greater in_list "
                "is_absolute is_directory less matches path_equal policy strequal strgreater strless target test "
                "version_equal version_greater version_less",
    .literals = "on off yes no y n true false ignore notfound",
    .case_insensitive = true,
    .sigils = "$",
    .interpolating_quotes = "\"",
    .string_sigils = "$",
    .long_brackets = true,
};

constexpr Syntax kNginx{
    .line_comments = "#",
    .quotes = "'\"",
    .literals = "on off",
    .sigils = "$",
    .functions = false,
    .first_word = FirstWord::Statement,
};

constexpr Syntax kAssembly{
    .line_comments = "; // ^#",
    .block_comments = "/* */",
    .quotes = "\"'",
    .variables = "rax rbx rcx rdx rsi rdi rbp rsp rip eax ebx ecx edx esi edi ebp esp eip ax bx cx dx si di bp "
                 "sp ip al bl cl dl ah bh ch dh sil dil bpl spl cs ds es fs gs ss lr pc fp xzr wzr eflags rflags",
    .case_insensitive = true,
    .sigils = "%",
    .annotations = ".",
    .line_annotation = '%',
    .functions = false,
    .first_word = FirstWord::Line,
    .labels = true,
    .registers = true,
};

// ---- HTML and XML ----------------------------------------------------------

// Names of tags and attributes: letters, digits and the punctuation XML names
// and template attributes (v-on:click, @click, :href, x.y) use.
bool markupNameChar(char c) { return identChar(c) || c == '-' || c == ':' || c == '.'; }
bool markupSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// An entity &amp; &#38; &#x26; at s[i]: its end, or i.
std::size_t entityEnd(std::string_view s, std::size_t i) {
  std::size_t j = i + 1;
  const auto limit = std::min(s.size(), i + 34);
  if (j < limit && s[j] == '#') {
    ++j;
    if (j < limit && (s[j] == 'x' || s[j] == 'X')) ++j;
  }
  const auto name = j;
  while (j < limit && std::isalnum(u(s[j])) != 0) ++j;
  return j > name && j < limit && s[j] == ';' ? j + 1 : i;
}

// The `</name` that ends a script or style body, matched case-insensitively.
std::size_t closingTag(std::string_view s, std::size_t from, std::string_view name) {
  for (auto at = s.find("</", from); at != npos; at = s.find("</", at + 2)) {
    const auto tag = at + 2;
    if (s.size() - tag >= name.size() && equalsIgnoringCase(s.substr(tag, name.size()), name) &&
        (tag + name.size() == s.size() || !markupNameChar(s[tag + name.size()]))) {
      return at;
    }
  }
  return s.size();
}

// The language of a <script> or <style> body from its type and lang attributes.
Language embeddedLanguage(std::string_view tag, std::string_view type, std::string_view lang) {
  const auto t = lower(type);
  const auto l = lower(lang);
  if (equalsIgnoringCase(tag, "style")) {
    if (l == "scss" || l == "sass") return Language::Scss;
    if (l == "less") return Language::Less;
    return Language::Css;
  }
  if (l == "ts" || l == "tsx" || l == "typescript" || t.find("typescript") != npos) return Language::TypeScript;
  if (t.find("json") != npos) return Language::Json;
  if (t.empty() || t == "module" || t.find("javascript") != npos || t.find("ecmascript") != npos || t.find("jsx") != npos) {
    return Language::JavaScript;
  }
  return Language::None;  // templates and other data blocks stay plain
}

// Tags are keywords, attribute names properties and their values strings;
// comments, doctypes and processing instructions, CDATA and entities get
// their own kinds. In HTML a <script> or <style> body is highlighted as the
// language it holds.
void tokenizeMarkup(std::string_view s, Tokens& out, bool html, int depth) {
  const auto n = s.size();
  std::size_t i = 0;
  const auto run = [&](std::size_t begin, std::size_t body, std::string_view close, TokenKind kind) {
    const auto found = s.find(close, body);
    const auto end = found == npos ? n : found + close.size();
    add(out, begin, end, kind);
    return end;
  };
  while (i < n) {
    const char c = s[i];
    if (c == '&') {
      const auto end = entityEnd(s, i);
      add(out, i, end, TokenKind::Variable);
      i = std::max(end, i + 1);
      continue;
    }
    if (c != '<') {
      ++i;
      continue;
    }
    if (startsAt(s, i, "<!--")) {
      i = run(i, i + 4, "-->", TokenKind::Comment);
      continue;
    }
    if (startsAt(s, i, "<![CDATA[")) {
      i = run(i, i + 9, "]]>", TokenKind::String);
      continue;
    }
    if (startsAt(s, i, "<?")) {
      i = run(i, i + 2, "?>", TokenKind::Preprocessor);
      continue;
    }
    if (startsAt(s, i, "<!")) {
      i = run(i, i + 2, ">", TokenKind::Preprocessor);
      continue;
    }
    std::size_t j = i + 1;
    const bool closing = j < n && s[j] == '/';
    if (closing) ++j;
    if (j >= n || !(std::isalpha(u(s[j])) != 0 || s[j] == '_' || u(s[j]) >= 0x80)) {
      ++i;
      continue;
    }
    std::size_t k = j;
    while (k < n && markupNameChar(s[k])) ++k;
    add(out, j, k, TokenKind::Keyword);
    const auto name = s.substr(j, k - j);
    std::string_view type;
    std::string_view lang;
    bool ended = false;
    bool self_closing = false;
    std::size_t p = k;
    while (p < n) {
      const char b = s[p];
      if (b == '>') {
        ++p;
        ended = true;
        break;
      }
      if (b == '/' && p + 1 < n && s[p + 1] == '>') {
        p += 2;
        ended = self_closing = true;
        break;
      }
      if (b == '<') break;  // the tag never closed; a new one starts here
      if (markupSpace(b) || b == '/' || b == '=') {
        ++p;
        continue;
      }
      if (b == '"' || b == '\'') {
        const auto end = quotedEnd(s, p, b, false, false);
        add(out, p, end, TokenKind::String);
        p = end;
        continue;
      }
      const auto attribute = p;
      while (p < n && !markupSpace(s[p]) && !has("=>/\"'<", s[p])) ++p;
      add(out, attribute, p, TokenKind::Property);
      const auto attribute_name = s.substr(attribute, p - attribute);
      std::size_t q = p;
      while (q < n && markupSpace(s[q])) ++q;
      if (q >= n || s[q] != '=') continue;
      ++q;
      while (q < n && markupSpace(s[q])) ++q;
      std::size_t value_end = q;
      std::string_view value;
      if (q < n && (s[q] == '"' || s[q] == '\'')) {
        value_end = quotedEnd(s, q, s[q], false, false);
        value = s.substr(q + 1, value_end - q - 1);
        if (!value.empty() && value.back() == s[q]) value.remove_suffix(1);
      } else {
        while (value_end < n && !markupSpace(s[value_end]) && s[value_end] != '>') ++value_end;
        value = s.substr(q, value_end - q);
      }
      add(out, q, value_end, TokenKind::String);
      if (equalsIgnoringCase(attribute_name, "type")) type = value;
      if (equalsIgnoringCase(attribute_name, "lang")) lang = value;
      p = value_end;
    }
    i = p;
    if (html && !closing && ended && !self_closing &&
        (equalsIgnoringCase(name, "script") || equalsIgnoringCase(name, "style"))) {
      const auto close = closingTag(s, p, name);
      tokenizeEmbedded(embeddedLanguage(name, type, lang), s, p, close, out, depth);
      i = close;
    }
  }
}

// ---- CSS, SCSS and Less ----------------------------------------------------

enum class Sheet { Css, Scss, Less };

// Less writes variables as @name, so only these at-rules are keywords there.
constexpr std::string_view kLessAtRules[] = {
    "charset", "container", "counter-style", "document", "font-face", "font-feature-values", "import",
    "keyframes", "layer", "media", "namespace", "page", "plugin", "property", "scope", "starting-style",
    "supports", "viewport", "-webkit-keyframes", "-moz-keyframes",
};

bool cssNameChar(char c) { return identChar(c) || c == '-'; }

// Selectors (tags as keywords, .class and #id as types, :pseudo as
// annotations), property names, values with numbers and units, hex colours,
// !important, functions, at-rules and the preprocessors' variables.
void tokenizeSheet(std::string_view s, Tokens& out, Sheet sheet) {
  const auto n = s.size();
  bool value = false;  // inside a declaration's value
  // Whether `name:` declares a property or starts a selector such as
  // `a:hover {`: the first of ; { } after the colon decides.
  Seeker terminator(s, ";{}");
  Seeker brace(s, "}\n");
  for (std::size_t i = 0; i < n;) {
    const char c = s[i];
    const char next = i + 1 < n ? s[i + 1] : '\0';
    if (c == '/' && next == '*') {
      const auto found = s.find("*/", i + 2);
      const auto end = found == npos ? n : found + 2;
      add(out, i, end, TokenKind::Comment);
      i = end;
      continue;
    }
    if (c == '/' && next == '/' && sheet != Sheet::Css) {
      const auto end = lineEnd(s, i);
      add(out, i, end, TokenKind::Comment);
      i = end;
      continue;
    }
    if (c == '"' || c == '\'') {
      const auto end = quotedEnd(s, i, c, true, true);
      add(out, i, end, TokenKind::String);
      i = end;
      continue;
    }
    if (c == '@' && (identStart(next) || next == '-')) {
      std::size_t j = i + 1;
      while (j < n && cssNameChar(s[j])) ++j;
      const auto name = s.substr(i + 1, j - i - 1);
      const bool variable = sheet == Sheet::Less && !listed(lower(name), kLessAtRules);
      add(out, i, j, variable ? TokenKind::Variable : TokenKind::Keyword);
      i = j;
      continue;
    }
    if (c == '$' && sheet == Sheet::Scss && (identStart(next) || next == '-')) {
      std::size_t j = i + 1;
      while (j < n && cssNameChar(s[j])) ++j;
      add(out, i, j, TokenKind::Variable);
      i = j;
      continue;
    }
    if (c == '#' && next == '{' && sheet != Sheet::Css) {
      const auto close = brace.next(i + 2);
      const auto end = close < n && s[close] == '}' ? close + 1 : close;
      add(out, i, end, TokenKind::Variable);
      i = end;
      continue;
    }
    if (c == '#' && cssNameChar(next)) {
      std::size_t j = i + 1;
      while (j < n && cssNameChar(s[j])) ++j;
      const auto digits = j - i - 1;
      const bool hex = std::all_of(s.begin() + static_cast<std::ptrdiff_t>(i) + 1, s.begin() + static_cast<std::ptrdiff_t>(j),
                                   [](char b) { return std::isxdigit(u(b)) != 0; });
      if (value) {
        if (hex && (digits == 3 || digits == 4 || digits == 6 || digits == 8)) add(out, i, j, TokenKind::Number);
      } else {
        add(out, i, j, TokenKind::Type);
      }
      i = j;
      continue;
    }
    if (!value && c == '.' && (identStart(next) || next == '-')) {
      std::size_t j = i + 1;
      while (j < n && cssNameChar(s[j])) ++j;
      add(out, i, j, TokenKind::Type);
      i = j;
      continue;
    }
    if (digit(c) || (c == '.' && digit(next)) ||
        (value && (c == '-' || c == '+') && (digit(next) || (next == '.' && i + 2 < n && digit(s[i + 2]))))) {
      std::size_t j = i + 1;
      while (j < n && (digit(s[j]) || (s[j] == '.' && j + 1 < n && digit(s[j + 1])))) ++j;
      if (j < n && s[j] == '%') ++j;
      else while (j < n && std::isalpha(u(s[j])) != 0) ++j;
      add(out, i, j, TokenKind::Number);
      i = j;
      continue;
    }
    if (c == '!' && value && std::isalpha(u(next)) != 0) {
      std::size_t j = i + 1;
      while (j < n && std::isalpha(u(s[j])) != 0) ++j;
      add(out, i, j, TokenKind::Keyword);
      i = j;
      continue;
    }
    if (c == ':' && !value) {
      std::size_t j = i + 1;
      if (j < n && s[j] == ':') ++j;
      const auto name = j;
      while (j < n && cssNameChar(s[j])) ++j;
      if (j > name) add(out, i, j, TokenKind::Preprocessor);
      i = std::max(j, i + 1);
      continue;
    }
    if (identStart(c) || (c == '-' && (identStart(next) || next == '-'))) {
      std::size_t j = i + 1;
      while (j < n && cssNameChar(s[j])) ++j;
      const auto word = s.substr(i, j - i);
      if (value) {
        if (j < n && s[j] == '(') {
          if (equalsIgnoringCase(word, "url") && j + 1 < n && s[j + 1] != '"' && s[j + 1] != '\'') {
            // An unquoted url(…) is one string, so `//` inside it is no comment.
            std::size_t k = j + 1;
            while (k < n && s[k] != ')' && s[k] != '\n') ++k;
            add(out, i, j, TokenKind::Function);
            add(out, j + 1, k, TokenKind::String);
            i = k;
            continue;
          }
          add(out, i, j, TokenKind::Function);
        } else if (word.starts_with("--")) {
          add(out, i, j, TokenKind::Variable);
        }
        i = j;
        continue;
      }
      std::size_t k = j;
      while (k < n && blank(s[k])) ++k;
      if (k < n && s[k] == ':') {
        // A `#{` interpolation in the value is no block.
        const auto stop = terminator.next(k + 1);
        if (stop >= n || s[stop] != '{' || s[stop - 1] == '#') {
          add(out, i, j, TokenKind::Property);
          value = true;
          i = k + 1;
          continue;
        }
      }
      add(out, i, j, j < n && s[j] == '(' ? TokenKind::Function : TokenKind::Keyword);
      i = j;
      continue;
    }
    if (c == '{' || c == '}' || c == ';') value = false;
    ++i;
  }
}

// ---- Markdown --------------------------------------------------------------

struct MarkdownFence {
  char marker = '\0';
  std::size_t length = 0;
  Language language = Language::None;
};

bool markdownFenceOpen(std::string_view line, MarkdownFence& fence) {
  if (line.empty() || (line.front() != '`' && line.front() != '~')) return false;
  std::size_t count = 0;
  while (count < line.size() && line[count] == line.front()) ++count;
  if (count < 3) return false;
  auto info = line.substr(count);
  if (line.front() == '`' && info.find('`') != npos) return false;
  while (!info.empty() && blank(info.front())) info.remove_prefix(1);
  info = info.substr(0, info.find_first_of(" \t\r{,"));
  fence = {line.front(), count, languageForName(info)};
  return true;
}

bool markdownFenceClose(std::string_view line, const MarkdownFence& fence) {
  std::size_t count = 0;
  while (count < line.size() && line[count] == fence.marker) ++count;
  if (count < fence.length) return false;
  return std::all_of(line.begin() + static_cast<std::ptrdiff_t>(count), line.end(), blank);
}

// Lookaheads for the inline syntax, each remembered (see Seeker) so that a
// line full of unmatched `*`, `[` or `<` is still read in linear time.
struct MarkdownSeekers {
  explicit MarkdownSeekers(std::string_view s)
      : ticks(s, "`\n"), stars(s, "*\n"), underscores(s, "_\n"), brackets(s, "]\n"), parens(s, ")\n"),
        angles(s, ">\n"), spaces(s, " \t\n"), colons(s, ":\n"), ats(s, "@\n") {}
  Seeker ticks, stars, underscores, brackets, parens, angles, spaces, colons, ats;
};

// Code spans and links are strings, link texts properties, strong emphasis
// keywords and emphasis variables, inline HTML annotations.
void markdownInline(std::string_view s, std::size_t q, std::size_t e, Tokens& out, MarkdownSeekers& seek) {
  while (q < e) {
    const char c = s[q];
    if (c == '\\' && q + 1 < e && std::ispunct(u(s[q + 1])) != 0) {
      q += 2;
      continue;
    }
    if (c == '`') {
      std::size_t r = q;
      while (r < e && s[r] == '`') ++r;
      const auto close = seek.ticks.next(r);
      if (close < e) {
        auto stop = close;
        while (stop < e && s[stop] == '`') ++stop;
        add(out, q, stop, TokenKind::String);
        q = stop;
      } else {
        q = r;
      }
      continue;
    }
    if (c == '*' || c == '_') {
      std::size_t r = q;
      while (r < e && s[r] == c) ++r;
      const bool intraword = c == '_' && q > 0 && std::isalnum(u(s[q - 1])) != 0;
      if (intraword || r >= e || blank(s[r])) {
        q = r;
        continue;
      }
      const auto close = (c == '*' ? seek.stars : seek.underscores).next(r);
      if (close < e && !blank(s[close - 1])) {
        auto stop = close;
        while (stop < e && s[stop] == c) ++stop;
        add(out, q, stop, r - q >= 2 ? TokenKind::Keyword : TokenKind::Variable);
        q = stop;
      } else {
        q = r;
      }
      continue;
    }
    if (c == '[' || (c == '!' && q + 1 < e && s[q + 1] == '[')) {
      const auto open = c == '!' ? q + 1 : q;
      const auto close = seek.brackets.next(open + 1);
      if (close < e && close + 1 < e) {
        if (s[close + 1] == '(') {
          const auto paren = seek.parens.next(close + 2);
          if (paren < e) {
            add(out, q, close + 1, TokenKind::Property);
            add(out, close + 1, paren + 1, TokenKind::String);
            q = paren + 1;
            continue;
          }
        } else if (s[close + 1] == '[') {
          add(out, q, close + 1, TokenKind::Property);
          q = close + 1;
          continue;
        }
      }
      q = open + 1;
      continue;
    }
    if (c == '<') {
      const auto close = seek.angles.next(q + 1);
      if (close < e && close > q + 1) {
        const char first = s[q + 1];
        const bool unbroken = seek.spaces.next(q + 1) > close;
        if (unbroken && std::isalpha(u(first)) != 0 && (seek.colons.next(q + 1) < close || seek.ats.next(q + 1) < close)) {
          add(out, q, close + 1, TokenKind::String);
          q = close + 1;
          continue;
        }
        if (std::isalpha(u(first)) != 0 || first == '/' || first == '!') {
          add(out, q, close + 1, TokenKind::Preprocessor);
          q = close + 1;
          continue;
        }
      }
      ++q;
      continue;
    }
    if (c == 'h' && (q == 0 || std::isalnum(u(s[q - 1])) == 0) &&
        (startsAt(s, q, "https://") || startsAt(s, q, "http://"))) {
      std::size_t stop = q;
      while (stop < e && !blank(s[stop]) && s[stop] != '<' && s[stop] != ')') ++stop;
      add(out, q, stop, TokenKind::String);
      q = stop;
      continue;
    }
    if (c == '&') {
      const auto end = std::min(entityEnd(s, q), e);
      add(out, q, end, TokenKind::Variable);
      q = std::max(end, q + 1);
      continue;
    }
    ++q;
  }
}

// Headings, rules and setext underlines are keywords; list and quote markers
// too; fences are annotations and their bodies are highlighted in the
// language the info string names; front matter is YAML.
void tokenizeMarkdown(std::string_view s, Tokens& out, int depth) {
  const auto n = s.size();
  std::size_t i = 0;
  if (s.starts_with("---\n") || s.starts_with("---\r\n")) {
    const auto body = lineEnd(s, 0) + 1;
    for (std::size_t j = body; j < n;) {
      const auto end = lineEnd(s, j);
      auto line = s.substr(j, end - j);
      if (line.ends_with('\r')) line.remove_suffix(1);
      if (line == "---" || line == "...") {
        add(out, 0, 3, TokenKind::Keyword);
        tokenizeEmbedded(Language::Yaml, s, body, j, out, depth);
        add(out, j, j + 3, TokenKind::Keyword);
        i = end + 1;
        break;
      }
      j = end + 1;
    }
  }
  MarkdownSeekers seek(s);
  MarkdownFence fence;
  bool in_fence = false;
  std::size_t fence_body = 0;
  bool paragraph = false;  // the previous line was paragraph text
  while (i < n) {
    const auto end = lineEnd(s, i);
    auto line = s.substr(i, end - i);
    if (line.ends_with('\r')) line.remove_suffix(1);
    const auto line_end = i + line.size();
    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') ++indent;
    const auto text = line.substr(indent);
    const auto p = i + indent;
    if (in_fence) {
      if (indent <= 3 && markdownFenceClose(text, fence)) {
        tokenizeEmbedded(fence.language, s, fence_body, i, out, depth);
        add(out, p, line_end, TokenKind::Preprocessor);
        in_fence = false;
      }
      i = end + 1;
      continue;
    }
    if (std::all_of(text.begin(), text.end(), blank)) {
      paragraph = false;
      i = end + 1;
      continue;
    }
    if (indent >= 4 && !paragraph) {  // an indented code block
      i = end + 1;
      continue;
    }
    if (markdownFenceOpen(text, fence)) {
      add(out, p, line_end, TokenKind::Preprocessor);
      in_fence = true;
      fence_body = std::min(n, end + 1);
      paragraph = false;
      i = end + 1;
      continue;
    }
    if (text.front() == '#') {
      std::size_t hashes = 0;
      while (hashes < text.size() && text[hashes] == '#') ++hashes;
      if (hashes <= 6 && (hashes == text.size() || blank(text[hashes]))) {
        add(out, p, line_end, TokenKind::Keyword);
        paragraph = false;
        i = end + 1;
        continue;
      }
    }
    if (has("=-*_", text.front())) {
      const char mark = text.front();
      std::size_t count = 0;
      bool rule = true;
      for (const char b : text) {
        if (b == mark) ++count;
        else if (!blank(b)) rule = false;
      }
      const bool underline = paragraph && (mark == '=' || (mark == '-' && text.find_first_of(" \t") == npos));
      if (rule && (underline || (count >= 3 && mark != '='))) {
        add(out, p, line_end, TokenKind::Keyword);
        paragraph = false;
        i = end + 1;
        continue;
      }
    }
    if (text.starts_with("<!--")) {
      const auto found = s.find("-->", p + 4);
      const auto stop = found == npos ? n : found + 3;
      add(out, p, stop, TokenKind::Comment);
      paragraph = false;
      i = stop;
      continue;
    }
    std::size_t q = p;
    while (q < line_end && s[q] == '>') {
      add(out, q, q + 1, TokenKind::Keyword);
      ++q;
      while (q < line_end && blank(s[q])) ++q;
    }
    if (q < line_end && has("-*+", s[q]) && (q + 1 == line_end || blank(s[q + 1]))) {
      add(out, q, q + 1, TokenKind::Keyword);
      ++q;
    } else {
      std::size_t d = q;
      while (d < line_end && d - q < 9 && digit(s[d])) ++d;
      if (d > q && d < line_end && (s[d] == '.' || s[d] == ')') && (d + 1 == line_end || blank(s[d + 1]))) {
        add(out, q, d + 1, TokenKind::Keyword);
        q = d + 1;
      }
    }
    if (q < line_end && s[q] == '[') {
      // A link reference definition: [label]: url "title".
      const auto close = seek.brackets.next(q + 1);
      if (close + 1 < line_end && s[close + 1] == ':') {
        add(out, q, close + 1, TokenKind::Property);
        std::size_t url = close + 2;
        while (url < line_end && blank(s[url])) ++url;
        std::size_t stop = url;
        while (stop < line_end && !blank(s[stop])) ++stop;
        add(out, url, stop, TokenKind::String);
        q = stop;
      }
    }
    markdownInline(s, q, line_end, out, seek);
    paragraph = true;
    i = end + 1;
  }
  if (in_fence) tokenizeEmbedded(fence.language, s, fence_body, n, out, depth);
}

// ---- INI, properties and TOML ----------------------------------------------

// A value: a quoted string, a boolean, a number, or text with ${var},
// $(var), %(var)s and $VAR references; then an inline ; or # comment.
void iniValue(std::string_view s, std::size_t p, std::size_t e, Tokens& out) {
  while (p < e && blank(s[p])) ++p;
  if (p >= e) return;
  if (s[p] == ';' || s[p] == '#') {
    add(out, p, e, TokenKind::Comment);
    return;
  }
  if (s[p] == '"' || s[p] == '\'') {
    const auto end = quotedEnd(s.substr(0, e), p, s[p], s[p] == '"', true);
    add(out, p, end, TokenKind::String);
    p = end;
    while (p < e && blank(s[p])) ++p;
    if (p < e && (s[p] == ';' || s[p] == '#')) add(out, p, e, TokenKind::Comment);
    return;
  }
  std::size_t stop = e;
  for (std::size_t k = p + 1; k < e; ++k) {
    if ((s[k] == ';' || s[k] == '#') && blank(s[k - 1])) {
      stop = k;
      break;
    }
  }
  auto value_end = stop;
  while (value_end > p && blank(s[value_end - 1])) --value_end;
  const auto value = s.substr(p, value_end - p);
  if (yamlWord(value) || equalsIgnoringCase(value, "none")) {
    add(out, p, value_end, TokenKind::Keyword);
  } else if (yamlNumber(value)) {
    add(out, p, value_end, TokenKind::Number);
  } else {
    for (std::size_t k = p; k < value_end;) {
      const char c = s[k];
      if ((c == '$' || c == '%') && k + 1 < value_end && (s[k + 1] == '{' || s[k + 1] == '(')) {
        const char close = s[k + 1] == '{' ? '}' : ')';
        const auto found = s.substr(0, value_end).find(close, k + 2);
        if (found != npos) {
          auto stop_at = found + 1;
          if (c == '%' && stop_at < value_end && std::isalpha(u(s[stop_at])) != 0) ++stop_at;  // %(name)s
          add(out, k, stop_at, TokenKind::Variable);
          k = stop_at;
          continue;
        }
        k = value_end;  // no closer anywhere further on this line
        continue;
      }
      if (c == '$' && k + 1 < value_end && identStart(s[k + 1])) {
        const auto stop_at = identEnd(s.substr(0, value_end), k + 1);
        add(out, k, stop_at, TokenKind::Variable);
        k = stop_at;
        continue;
      }
      ++k;
    }
  }
  if (stop < e) add(out, stop, e, TokenKind::Comment);
}

void tokenizeIni(std::string_view s, Tokens& out) {
  for (std::size_t i = 0; i < s.size();) {
    const auto end = lineEnd(s, i);
    std::size_t p = i;
    while (p < end && blank(s[p])) ++p;
    if (p < end) {
      const char c = s[p];
      if (c == ';' || c == '#' || (c == '!' && p == i)) {
        add(out, p, end, TokenKind::Comment);
      } else if (c == '[') {
        const auto close = s.substr(0, end).find(']', p);
        const auto stop = close == npos ? end : close + 1;
        add(out, p, stop, TokenKind::Keyword);
        iniValue(s, stop, end, out);
      } else {
        const auto separator = s.substr(0, end).find_first_of("=:", p);
        if (separator != npos) {
          auto key_end = separator;
          while (key_end > p && blank(s[key_end - 1])) --key_end;
          add(out, p, key_end, TokenKind::Property);
          iniValue(s, separator + 1, end, out);
        }
      }
    }
    i = end + 1;
  }
}

// Tables are keywords, keys properties (also inside inline tables), strings
// including the multi-line forms, numbers and dates, booleans.
void tokenizeToml(std::string_view s, Tokens& out) {
  const auto n = s.size();
  std::vector<char> open;     // the brackets of the arrays and inline tables we are in
  std::size_t overflow = 0;   // nesting beyond what `open` keeps
  bool key = true;            // a key may start here
  bool line_start = true;
  for (std::size_t i = 0; i < n;) {
    const char c = s[i];
    if (c == '\n') {
      if (open.empty()) key = true;
      line_start = true;
      ++i;
      continue;
    }
    if (blank(c)) {
      ++i;
      continue;
    }
    const bool first = line_start;
    line_start = false;
    if (c == '#') {
      const auto end = lineEnd(s, i);
      add(out, i, end, TokenKind::Comment);
      i = end;
      continue;
    }
    if (c == '[' && first && open.empty() && overflow == 0) {
      const auto end = lineEnd(s, i);
      const bool array = i + 1 < n && s[i + 1] == '[';
      const auto close = s.substr(0, end).find(array ? "]]" : "]", i);
      const auto stop = close == npos ? end : close + (array ? 2 : 1);
      add(out, i, stop, TokenKind::Keyword);
      i = stop;
      continue;
    }
    if (c == '"' || c == '\'') {
      std::size_t end = n;
      if (startsAt(s, i, c == '"' ? "\"\"\"" : "'''")) {
        const auto close = s.find(s.substr(i, 3), i + 3);
        end = close == npos ? n : close + 3;
        while (end < n && s[end] == c && end - close < 5) ++end;  // """a"""" ends with the quote in the text
      } else {
        end = quotedEnd(s, i, c, c == '"', true);
      }
      add(out, i, end, key ? TokenKind::Property : TokenKind::String);
      i = end;
      continue;
    }
    if (c == '=') {
      key = false;
      ++i;
      continue;
    }
    if (c == '[' || c == '{') {
      if (open.size() < 64) open.push_back(c);
      else ++overflow;
      key = c == '{';
      ++i;
      continue;
    }
    if (c == ']' || c == '}') {
      if (overflow > 0) --overflow;
      else if (!open.empty()) open.pop_back();
      key = false;
      ++i;
      continue;
    }
    if (c == ',') {
      key = !open.empty() && open.back() == '{';
      ++i;
      continue;
    }
    if (std::isalnum(u(c)) != 0 || c == '_' || c == '-' || c == '+') {
      std::size_t j = i;
      if (key) {
        while (j < n && (std::isalnum(u(s[j])) != 0 || s[j] == '_' || s[j] == '-')) ++j;
        add(out, i, std::max(j, i + 1), TokenKind::Property);
      } else {
        while (j < n && (std::isalnum(u(s[j])) != 0 || has("_-+:.", s[j]))) ++j;
        const auto word = s.substr(i, j - i);
        if (word == "true" || word == "false") add(out, i, j, TokenKind::Keyword);
        else if (digit(c) || ((c == '+' || c == '-') && j > i + 1) || word.ends_with("inf") || word.ends_with("nan")) {
          add(out, i, j, TokenKind::Number);
        }
      }
      i = std::max(j, i + 1);
      continue;
    }
    ++i;
  }
}

// ---- Diff ------------------------------------------------------------------

bool diffCount(std::string_view text, std::size_t& at, long& count) {
  std::size_t j = at;
  long start = 0;
  while (j < text.size() && digit(text[j]) && j - at < 12) start = start * 10 + (text[j++] - '0');
  if (j == at) return false;
  count = 1;
  if (j < text.size() && text[j] == ',') {
    const auto k = ++j;
    count = 0;
    while (j < text.size() && digit(text[j]) && j - k < 12) count = count * 10 + (text[j++] - '0');
    if (j == k) return false;
  }
  at = j;
  return true;
}

// Unified and context diffs: headers are keywords, hunk headers annotations,
// added and removed lines whole tokens of their own. Inside a hunk the line
// counts of its header decide where it ends, so a removed line reading
// "-- a" is not mistaken for a header.
void tokenizeDiff(std::string_view s, Tokens& out) {
  static constexpr std::string_view kHeaders[] = {
      "diff ", "index ", "Index: ", "--- ", "+++ ", "*** ", "new file mode", "deleted file mode", "old mode",
      "new mode", "similarity index", "dissimilarity index", "rename from", "rename to", "copy from", "copy to",
      "Binary files", "===",
  };
  const auto n = s.size();
  bool in_hunk = false;
  bool counted = false;
  long old_left = 0;
  long new_left = 0;
  for (std::size_t i = 0; i < n;) {
    const auto end = lineEnd(s, i);
    const auto line = s.substr(i, end - i);
    const auto next = end < n ? s.substr(end + 1, std::min<std::size_t>(4, n - end - 1)) : std::string_view();
    if (in_hunk) {
      const char c = line.empty() ? ' ' : line.front();
      const bool header = !counted && line.starts_with("--- ") && next.starts_with("+++ ");
      if (!header && (c == '+' || c == '-' || c == ' ' || c == '\\' || c == '\r')) {
        if (c == '+') add(out, i, end, TokenKind::Inserted);
        else if (c == '-') add(out, i, end, TokenKind::Deleted);
        else if (c == '\\') add(out, i, end, TokenKind::Comment);
        if (c != '\\') {
          if (c != '+') --old_left;
          if (c != '-') --new_left;
        }
        if (counted && old_left <= 0 && new_left <= 0) in_hunk = false;
        i = end + 1;
        continue;
      }
      in_hunk = false;
    }
    if (line.starts_with("@@")) {
      const auto close = line.find("@@", 2);
      add(out, i, close == npos ? end : i + close + 2, TokenKind::Preprocessor);
      // "@@ -a,b +c,d @@": the old and new line counts of the hunk.
      std::size_t at = 4;
      in_hunk = true;
      counted = line.size() > 4 && line[2] == ' ' && line[3] == '-' && diffCount(line, at, old_left) &&
                line.substr(at, 2) == " +";
      if (counted) {
        at += 2;
        counted = diffCount(line, at, new_left);
      }
      if (counted && old_left <= 0 && new_left <= 0) in_hunk = false;
    } else if (std::any_of(std::begin(kHeaders), std::end(kHeaders), [&](std::string_view h) { return line.starts_with(h); })) {
      add(out, i, end, TokenKind::Keyword);
    } else if (line.starts_with("+")) {
      add(out, i, end, TokenKind::Inserted);
    } else if (line.starts_with("-")) {
      add(out, i, end, TokenKind::Deleted);
    } else if (line.starts_with("\\")) {
      add(out, i, end, TokenKind::Comment);
    } else if (line.starts_with("!")) {
      add(out, i, end, TokenKind::Preprocessor);
    }
    i = end + 1;
  }
}

// ---- Dockerfile ------------------------------------------------------------

bool trailingBackslash(std::string_view line) {
  while (!line.empty() && blank(line.back())) line.remove_suffix(1);
  return !line.empty() && line.back() == '\\';
}

// Arguments of an instruction: strings, $VAR and ${VAR}, --flags, AS, numbers.
void dockerArguments(std::string_view s, std::size_t p, std::size_t e, Tokens& out) {
  while (p < e) {
    const char c = s[p];
    if (c == '"' || c == '\'') {
      const auto end = quotedEnd(s.substr(0, e), p, c, c == '"', true);
      add(out, p, end, TokenKind::String);
      p = end;
      continue;
    }
    if (c == '$' && p + 1 < e && (s[p + 1] == '{' || identStart(s[p + 1]))) {
      std::size_t end = p + 1;
      if (s[end] == '{') {
        const auto close = s.substr(0, e).find('}', end);
        end = close == npos ? e : close + 1;
      } else {
        end = identEnd(s.substr(0, e), end);
      }
      add(out, p, end, TokenKind::Variable);
      p = end;
      continue;
    }
    const bool word_start = p == 0 || blank(s[p - 1]) || s[p - 1] == '\n';
    if (c == '-' && word_start && p + 1 < e && s[p + 1] == '-') {
      std::size_t end = p + 2;
      while (end < e && (std::isalnum(u(s[end])) != 0 || s[end] == '-' || s[end] == '_')) ++end;
      add(out, p, end, TokenKind::Property);
      p = end;
      continue;
    }
    if (identStart(c)) {
      const auto end = identEnd(s.substr(0, e), p);
      if (word_start && equalsIgnoringCase(s.substr(p, end - p), "as")) add(out, p, end, TokenKind::Keyword);
      p = end;
      continue;
    }
    if (digit(c) && word_start) {
      std::size_t end = p;
      while (end < e && digit(s[end])) ++end;
      add(out, p, end, TokenKind::Number);
      p = end;
      continue;
    }
    ++p;
  }
}

// Instructions are keywords; RUN, CMD, ENTRYPOINT and SHELL hold shell code
// (across continuation lines), the others arguments.
void tokenizeDockerfile(std::string_view s, Tokens& out, int depth) {
  static constexpr std::string_view kInstructions[] = {
      "add", "arg", "cmd", "copy", "entrypoint", "env", "expose", "from", "healthcheck", "label", "maintainer",
      "onbuild", "run", "shell", "stopsignal", "user", "volume", "workdir",
  };
  static constexpr std::string_view kShellInstructions[] = {"run", "cmd", "entrypoint", "shell"};
  const auto n = s.size();
  bool continued = false;
  for (std::size_t i = 0; i < n;) {
    const auto end = lineEnd(s, i);
    const auto line = s.substr(i, end - i);
    std::size_t p = i;
    while (p < end && blank(s[p])) ++p;
    if (p < end && s[p] == '#') {
      add(out, p, end, TokenKind::Comment);
      i = end + 1;
      continue;
    }
    if (continued) {
      dockerArguments(s, p, end, out);
      continued = trailingBackslash(line);
      i = end + 1;
      continue;
    }
    std::size_t q = p;
    while (q < end && std::isalpha(u(s[q])) != 0) ++q;
    auto instruction = lower(s.substr(p, q - p));
    if (!listed(instruction, kInstructions)) {
      dockerArguments(s, p, end, out);
      continued = trailingBackslash(line);
      i = end + 1;
      continue;
    }
    add(out, p, q, TokenKind::Keyword);
    if (instruction == "onbuild") {
      std::size_t r = q;
      while (r < end && blank(s[r])) ++r;
      std::size_t t = r;
      while (t < end && std::isalpha(u(s[t])) != 0) ++t;
      const auto inner = lower(s.substr(r, t - r));
      if (listed(inner, kInstructions)) {
        add(out, r, t, TokenKind::Keyword);
        instruction = inner;
        q = t;
      }
    }
    if (listed(instruction, kShellInstructions)) {
      std::size_t stop = end;
      auto current = line;
      while (trailingBackslash(current) && stop < n) {
        const auto next_end = lineEnd(s, stop + 1);
        current = s.substr(stop + 1, next_end - stop - 1);
        stop = next_end;
      }
      tokenizeEmbedded(Language::Shell, s, q, stop, out, depth);
      continued = false;
      i = stop + 1;
      continue;
    }
    dockerArguments(s, q, end, out);
    continued = trailingBackslash(line);
    i = end + 1;
  }
}

// ---- Makefile --------------------------------------------------------------

constexpr std::string_view kMakeDirectives[] = {
    "define", "else", "endef", "endif", "export", "ifdef", "ifeq", "ifndef", "ifneq", "include", "-include",
    "sinclude", "override", "private", "undefine", "unexport", "vpath",
};

// Variable references and comments; with `targets`, the words are rule
// targets (functions; special targets such as .PHONY are keywords).
void makeText(std::string_view s, std::size_t p, std::size_t e, Tokens& out, bool targets) {
  while (p < e) {
    const char c = s[p];
    if (c == '$') {
      p = makeVariable(s, p, e, out);
      continue;
    }
    if (c == '#' && (p == 0 || s[p - 1] != '\\')) {
      add(out, p, e, TokenKind::Comment);
      return;
    }
    if (targets && !blank(c) && c != '\n' && c != '\\') {
      std::size_t q = p + 1;  // the first byte may be an escaped `#`
      while (q < e && !blank(s[q]) && s[q] != '$' && s[q] != '#' && s[q] != '\\' && s[q] != '\n') ++q;
      const bool special = c == '.' && q > p + 1 && upper(s[p + 1]);
      add(out, p, q, special ? TokenKind::Keyword : TokenKind::Function);
      p = q;
      continue;
    }
    ++p;
  }
}

// A line that is no recipe and no directive: the first ':' or '=' outside
// $(…) makes it an assignment (=, :=, ::=, ?=, +=, !=) or a rule. True for a
// rule.
bool makeStatement(std::string_view s, std::size_t p, std::size_t e, Tokens& out) {
  int depth = 0;
  for (std::size_t k = p; k < e; ++k) {
    const char c = s[k];
    if (c == '$' && k + 1 < e && (s[k + 1] == '(' || s[k + 1] == '{')) {
      ++depth;
      ++k;
      continue;
    }
    if (depth > 0) {
      if (c == '(' || c == '{') ++depth;
      else if (c == ')' || c == '}') --depth;
      continue;
    }
    if (c == '#') break;
    if (c == '=') {
      std::size_t op = k;
      while (op > p && has(":?+!", s[op - 1]) && k - op < 3) --op;
      auto name_end = op;
      while (name_end > p && blank(s[name_end - 1])) --name_end;
      if (s.substr(p, name_end - p).find('$') == npos) add(out, p, name_end, TokenKind::Variable);
      else makeText(s, p, name_end, out, false);
      makeText(s, k + 1, e, out, false);
      return false;
    }
    if (c == ':') {
      if (k + 1 < e && (s[k + 1] == '=' || (s[k + 1] == ':' && k + 2 < e && s[k + 2] == '='))) continue;
      makeText(s, p, k, out, true);
      auto rest = k + 1;
      if (rest < e && s[rest] == ':') ++rest;
      makeText(s, rest, e, out, false);
      return true;
    }
  }
  makeText(s, p, e, out, false);
  return false;
}

void tokenizeMakefile(std::string_view s, Tokens& out) {
  const auto n = s.size();
  bool in_define = false;
  bool in_rule = false;  // a rule was read, so tab-indented lines are its recipe
  for (std::size_t i = 0; i < n;) {
    // The logical line: physical lines joined by trailing backslashes.
    auto stop = lineEnd(s, i);
    while (stop < n && trailingBackslash(s.substr(i, stop - i))) stop = lineEnd(s, stop + 1);
    if (s[i] == '\t' && in_rule && !in_define) {
      std::size_t p = i + 1;
      while (p < stop && has("@-+", s[p])) ++p;
      add(out, i + 1, p, TokenKind::Preprocessor);
      Tokens recipe;
      tokenizeShell(s.substr(p, stop - p), recipe, true);
      for (const auto& token : recipe) out.push_back({token.begin + p, token.end + p, token.kind});
      i = stop + 1;
      continue;
    }
    std::size_t p = i;
    while (p < stop && blank(s[p])) ++p;
    if (p == stop) {
      i = stop + 1;
      continue;
    }
    if (s[p] == '#') {
      add(out, p, stop, TokenKind::Comment);
      i = stop + 1;
      continue;
    }
    std::size_t q = p;
    while (q < stop && (std::isalpha(u(s[q])) != 0 || s[q] == '-' || s[q] == '_')) ++q;
    const auto word = s.substr(p, q - p);
    const bool whole = q == stop || blank(s[q]) || s[q] == '(';
    if (in_define) {
      if (word == "endef" && whole) {
        add(out, p, q, TokenKind::Keyword);
        in_define = false;
      } else {
        makeText(s, p, stop, out, false);
      }
      i = stop + 1;
      continue;
    }
    if (whole && listed(word, kMakeDirectives)) {
      add(out, p, q, TokenKind::Keyword);
      if (word == "define" || word == "undefine") {
        std::size_t r = q;
        while (r < stop && blank(s[r])) ++r;
        std::size_t t = r;
        while (t < stop && !blank(s[t]) && s[t] != '\n') ++t;
        add(out, r, t, TokenKind::Variable);
        if (word == "define") in_define = true;
      } else if (word == "export" || word == "unexport" || word == "override" || word == "private") {
        makeStatement(s, q, stop, out);
        in_rule = false;
      } else {
        makeText(s, q, stop, out, false);
      }
      i = stop + 1;
      continue;
    }
    in_rule = makeStatement(s, p, stop, out);
    i = stop + 1;
  }
}

// ---- LaTeX -----------------------------------------------------------------

// Commands are keywords, environment names types, math and verbatim text
// strings, % comments comments, macro parameters (#1) variables.
void tokenizeLatex(std::string_view s, Tokens& out) {
  static constexpr std::string_view kVerbatim[] = {"verbatim", "verbatim*", "Verbatim", "lstlisting", "minted", "comment"};
  const auto n = s.size();
  std::size_t math_blocked_until = 0;  // no inline math starts before here (an earlier $ never closed)
  for (std::size_t i = 0; i < n;) {
    const char c = s[i];
    if (c == '%') {
      const auto end = lineEnd(s, i);
      add(out, i, end, TokenKind::Comment);
      i = end;
      continue;
    }
    if (c == '\\') {
      if (i + 1 >= n) break;
      const char next = s[i + 1];
      if (std::isalpha(u(next)) != 0 || next == '@') {
        std::size_t j = i + 1;
        while (j < n && (std::isalpha(u(s[j])) != 0 || s[j] == '@')) ++j;
        if (j < n && s[j] == '*') ++j;
        const auto command = s.substr(i + 1, j - i - 1);
        add(out, i, j, TokenKind::Keyword);
        if ((command == "begin" || command == "end") && j < n && s[j] == '{') {
          const auto close = s.find_first_of("}\n", j + 1);
          if (close != npos && s[close] == '}') {
            const auto name = s.substr(j + 1, close - j - 1);
            add(out, j + 1, close, TokenKind::Type);
            i = close + 1;
            if (command == "begin" && listed(name, kVerbatim)) {
              const std::string closer = "\\end{" + std::string(name) + "}";
              const auto found = s.find(closer, i);
              const auto stop = found == npos ? n : found;
              add(out, i, stop, name == "comment" ? TokenKind::Comment : TokenKind::String);
              i = stop;
            }
            continue;
          }
        }
        if ((command == "verb" || command == "verb*") && j < n && s[j] != '\n' && !blank(s[j])) {
          const char delimiter = s[j];
          std::size_t k = j + 1;
          while (k < n && s[k] != delimiter && s[k] != '\n') ++k;
          const auto stop = k < n && s[k] == delimiter ? k + 1 : k;
          add(out, j, stop, TokenKind::String);
          i = stop;
          continue;
        }
        i = j;
        continue;
      }
      if (next == '(' || next == '[') {
        const std::string_view closer = next == '(' ? "\\)" : "\\]";
        const auto found = s.find(closer, i + 2);
        const auto stop = found == npos ? n : found + 2;
        add(out, i, stop, TokenKind::String);
        i = stop;
        continue;
      }
      i += 2;  // an escaped character such as \% or \\ .
      continue;
    }
    if (c == '$') {
      if (i + 1 < n && s[i + 1] == '$') {
        const auto found = s.find("$$", i + 2);
        const auto stop = found == npos ? n : found + 2;
        add(out, i, stop, TokenKind::String);
        i = stop;
        continue;
      }
      if (i >= math_blocked_until) {
        // Inline math ends at the next unescaped $ within the paragraph.
        std::size_t j = i + 1;
        bool closed = false;
        for (; j < n; ++j) {
          if (s[j] == '\\') {
            ++j;
            continue;
          }
          if (s[j] == '$') {
            closed = true;
            break;
          }
          if (s[j] == '\n' && j + 1 < n && s[j + 1] == '\n') break;
        }
        if (closed) {
          add(out, i, j + 1, TokenKind::String);
          i = j + 1;
          continue;
        }
        math_blocked_until = j;
      }
      ++i;
      continue;
    }
    if (c == '#' && i + 1 < n && digit(s[i + 1])) {
      add(out, i, i + 2, TokenKind::Variable);
      i += 2;
      continue;
    }
    if (std::isalpha(u(c)) != 0) {
      while (i < n && std::isalnum(u(s[i])) != 0) ++i;
      continue;
    }
    if (digit(c)) {
      const auto end = genericNumberEnd(s, i, false);
      add(out, i, end, TokenKind::Number);
      i = end;
      continue;
    }
    ++i;
  }
}

// ---- Windows batch files ---------------------------------------------------

// REM and :: comments, :labels as functions, %VAR%, %1, %%i, %~dp0 and
// !VAR! as variables, the built-in commands as keywords (any case).
void tokenizeBatch(std::string_view s, Tokens& out) {
  static const WordSet keywords(
      "aux call cd chdir choice cls color con copy defined del dir disabledelayedexpansion do echo else "
      "enabledelayedexpansion enableextensions endlocal equ erase errorlevel exist exit find findstr for geq goto "
      "gtr if in leq lss md mkdir move neq not nul off on pause popd prn pushd rd ren rename rmdir set setlocal "
      "shift start timeout title type where");
  const auto n = s.size();
  Seeker percent(s, "%\n");
  Seeker bang(s, "!\n");
  for (std::size_t i = 0; i < n;) {
    const auto end = lineEnd(s, i);
    std::size_t p = i;
    while (p < end && (blank(s[p]) || s[p] == '@')) {
      if (s[p] == '@') add(out, p, p + 1, TokenKind::Preprocessor);
      ++p;
    }
    if (startsAt(s, p, "::")) {
      add(out, p, end, TokenKind::Comment);
      i = end + 1;
      continue;
    }
    if (p < end && s[p] == ':' && p + 1 < end && identStart(s[p + 1])) {
      add(out, p, identEnd(s.substr(0, end), p + 1), TokenKind::Function);
      i = end + 1;
      continue;
    }
    for (std::size_t q = p; q < end;) {
      const char c = s[q];
      if (c == '^') {
        q += 2;
        continue;
      }
      if (c == '"') {
        const auto stop = quotedEnd(s.substr(0, end), q, '"', false, true);
        add(out, q, stop, TokenKind::String);
        q = stop;
        continue;
      }
      if (c == '%' && q + 1 < end) {
        const char next = s[q + 1];
        std::size_t stop = q;
        if (next == '%' && q + 2 < end) {
          stop = q + 2;
          if (s[stop] == '~') {
            while (stop + 1 < end && std::isalpha(u(s[stop + 1])) != 0) ++stop;
          }
          stop = std::min(end, stop + 1);
        } else if (digit(next) || next == '*') {
          stop = q + 2;
        } else if (next == '~') {
          stop = q + 2;
          while (stop < end && std::isalpha(u(s[stop])) != 0) ++stop;
          if (stop < end && (digit(s[stop]) || s[stop] == '*')) ++stop;  // %~dp0
        } else {
          const auto close = percent.next(q + 1);
          if (close < end && close > q + 1) stop = close + 1;
        }
        if (stop > q) {
          add(out, q, stop, TokenKind::Variable);
          q = stop;
          continue;
        }
      }
      if (c == '!') {
        const auto close = bang.next(q + 1);
        if (close < end && close > q + 1 &&
            std::all_of(s.begin() + static_cast<std::ptrdiff_t>(q) + 1, s.begin() + static_cast<std::ptrdiff_t>(close),
                        [](char b) { return identChar(b) || b == ':' || b == '~' || b == ','; })) {
          add(out, q, close + 1, TokenKind::Variable);
          q = close + 1;
          continue;
        }
      }
      if (identStart(c)) {
        const auto stop = identEnd(s.substr(0, end), q);
        const auto word = lower(s.substr(q, stop - q));
        if (word == "rem" && (stop == end || blank(s[stop]))) {
          add(out, q, end, TokenKind::Comment);
          break;
        }
        if (keywords.contains(word)) add(out, q, stop, TokenKind::Keyword);
        q = stop;
        continue;
      }
      if (digit(c) && (q == 0 || !identChar(s[q - 1]))) {
        std::size_t stop = q;
        while (stop < end && digit(s[stop])) ++stop;
        if (stop == end || !identChar(s[stop])) add(out, q, stop, TokenKind::Number);
        q = stop;
        continue;
      }
      ++q;
    }
    i = end + 1;
  }
}

// ---- HTTP messages ---------------------------------------------------------

// Request and status lines, header names as properties, and a body in the
// language its Content-Type names. Several messages may follow each other,
// separated by ### lines as in request collections; # and // lines are
// comments.
void tokenizeHttp(std::string_view s, Tokens& out, int depth) {
  const auto n = s.size();
  std::size_t i = 0;
  while (i < n) {
    while (i < n) {
      const auto end = lineEnd(s, i);
      auto line = s.substr(i, end - i);
      while (!line.empty() && blank(line.back())) line.remove_suffix(1);
      if (line.empty()) {
        i = end + 1;
        continue;
      }
      if (line.starts_with("#") || line.starts_with("//")) {
        add(out, i, i + line.size(), TokenKind::Comment);
        i = end + 1;
        continue;
      }
      break;
    }
    if (i >= n) break;
    const auto end = lineEnd(s, i);
    std::size_t p = i;
    const auto word = [&] {
      const auto begin = p;
      while (p < end && !blank(s[p])) ++p;
      const auto stop = p;
      while (p < end && blank(s[p])) ++p;
      return std::pair{begin, stop};
    };
    if (startsAt(s, i, "HTTP/")) {
      const auto [version, version_end] = word();
      add(out, version, version_end, TokenKind::Keyword);
      const auto [status, status_end] = word();
      if (status_end > status && digit(s[status])) add(out, status, status_end, TokenKind::Number);
    } else {
      const auto [method, method_end] = word();
      add(out, method, method_end, TokenKind::Keyword);
      const auto [target, target_end] = word();
      add(out, target, target_end, TokenKind::String);
      const auto [version, version_end] = word();
      if (startsAt(s, version, "HTTP/")) add(out, version, version_end, TokenKind::Keyword);
    }
    i = end + 1;
    auto body = Language::None;
    while (i < n) {
      const auto line_end = lineEnd(s, i);
      auto line = s.substr(i, line_end - i);
      while (!line.empty() && blank(line.back())) line.remove_suffix(1);
      if (line.empty()) {
        i = line_end + 1;
        break;
      }
      const auto colon = line.find(':');
      if (colon != npos && colon > 0) {
        add(out, i, i + colon, TokenKind::Property);
        if (equalsIgnoringCase(line.substr(0, colon), "content-type")) {
          const auto type = lower(line.substr(colon + 1));
          if (type.find("json") != npos) body = Language::Json;
          else if (type.find("html") != npos) body = Language::Html;
          else if (type.find("xml") != npos) body = Language::Xml;
          else if (type.find("graphql") != npos) body = Language::GraphQL;
          else if (type.find("javascript") != npos) body = Language::JavaScript;
          else if (type.find("css") != npos) body = Language::Css;
          else if (type.find("yaml") != npos) body = Language::Yaml;
        }
      }
      i = line_end + 1;
    }
    const auto body_begin = std::min(i, n);
    std::size_t body_end = body_begin;
    while (body_end < n && !startsAt(s, body_end, "###")) body_end = std::min(n, lineEnd(s, body_end) + 1);
    tokenizeEmbedded(body, s, body_begin, body_end, out, depth);
    i = body_end;
  }
}

// ---- The language table ----------------------------------------------------

// One row per language, in enumerator order: display name, accepted names
// (fence info strings, lower case; the first is canonical), file extensions,
// exact file names, file name prefixes ("Dockerfile.dev"), shebang
// interpreters, and the syntax for the table-driven tokenizer (null for a
// dedicated tokenizer). Lookups, highlightLanguages() and the tokenizer
// dispatch all read this one table.
struct LanguageInfo {
  Language language;
  std::string_view name;
  std::string_view aliases;
  std::string_view extensions;
  std::string_view files;
  std::string_view file_prefixes;
  std::string_view interpreters;
  const Syntax* syntax;
};

constexpr LanguageInfo kLanguages[] = {
    {Language::Cpp, "C++", "cpp c++ cxx cc hpp hh hxx h ino cuda cu", "cpp cxx cc c++ hpp hh hxx h inc ino cu cuh ipp tpp",
     "", "", "", nullptr},
    {Language::Python, "Python", "python py python3 py3 python2 starlark bzl", "py pyi pyw bzl", "BUILD.bazel WORKSPACE SConstruct SConscript",
     "", "python", nullptr},
    {Language::Shell, "Shell", "bash sh shell zsh ksh dash ash console shell-session shellsession terminal shell-script shellscript",
     "sh bash zsh ksh dash ash bats ebuild command", "PKGBUILD", "", "sh bash zsh dash ksh ash mksh", nullptr},
    {Language::Yaml, "YAML", "yaml yml", "yaml yml", "", "", "", nullptr},
    {Language::Json, "JSON", "json jsonc json5 jsonl ndjson geojson webmanifest", "json jsonc json5 jsonl ndjson geojson webmanifest ipynb",
     "flake.lock composer.lock Pipfile.lock", "", "", nullptr},
    {Language::C, "C", "c", "c", "", "", "", nullptr},
    {Language::ObjectiveC, "Objective-C", "objective-c objectivec objc obj-c objc++ objective-c++", "m mm", "", "", "", nullptr},
    {Language::CSharp, "C#", "csharp c# cs csx cake", "cs csx cake", "", "", "", &kCSharp},
    {Language::Java, "Java", "java jsh", "java jsh", "", "", "", &kJava},
    {Language::Kotlin, "Kotlin", "kotlin kt kts", "kt kts", "", "", "", &kKotlin},
    {Language::Scala, "Scala", "scala sc sbt", "scala sc sbt", "", "", "scala", &kScala},
    {Language::Groovy, "Groovy", "groovy gradle gvy jenkinsfile", "groovy gradle gvy gy gsh", "Jenkinsfile", "", "groovy", &kGroovy},
    {Language::Dart, "Dart", "dart", "dart", "", "", "dart", &kDart},
    {Language::Swift, "Swift", "swift", "swift", "", "", "swift", &kSwift},
    {Language::Go, "Go", "go golang", "go", "", "", "", &kGo},
    {Language::Rust, "Rust", "rust rs", "rs", "", "", "", &kRust},
    {Language::Zig, "Zig", "zig", "zig zon", "", "", "", &kZig},
    {Language::D, "D", "d dlang", "d di", "", "", "", &kD},
    {Language::JavaScript, "JavaScript", "javascript js jsx mjs cjs node", "js jsx mjs cjs", "", "", "node nodejs deno bun",
     &kJavaScript},
    {Language::TypeScript, "TypeScript", "typescript ts tsx mts cts", "ts tsx mts cts", "", "", "ts-node", &kTypeScript},
    {Language::Php, "PHP", "php php3 php4 php5 php7 php8 phtml", "php phtml php3 php4 php5 php7 php8 phps", "", "", "php", &kPhp},
    {Language::Perl, "Perl", "perl pl pm perl5", "pl pm pod t psgi", "", "", "perl", &kPerl},
    {Language::Ruby, "Ruby", "ruby rb jruby rake gemspec", "rb rake gemspec ru podspec rbs",
     "Gemfile Rakefile Podfile Vagrantfile Brewfile Guardfile Capfile Fastfile", "", "ruby jruby truffleruby", &kRuby},
    {Language::Lua, "Lua", "lua luau", "lua luau rockspec", "", "", "lua luajit", &kLua},
    {Language::R, "R", "r rlang rscript splus", "r", "", "", "rscript", &kR},
    {Language::Julia, "Julia", "julia jl", "jl", "", "", "julia", &kJulia},
    {Language::Nim, "Nim", "nim nims nimble", "nim nims nimble", "", "", "", &kNim},
    {Language::Crystal, "Crystal", "crystal cr", "cr", "", "", "crystal", &kCrystal},
    {Language::Elixir, "Elixir", "elixir ex exs", "ex exs", "", "", "elixir", &kElixir},
    {Language::Erlang, "Erlang", "erlang erl hrl escript", "erl hrl escript", "rebar.config", "", "escript", &kErlang},
    {Language::Haskell, "Haskell", "haskell hs", "hs hs-boot", "", "", "runghc runhaskell", &kHaskell},
    {Language::Elm, "Elm", "elm", "elm", "", "", "", &kElm},
    {Language::OCaml, "OCaml", "ocaml ml mli", "ml mli mll mly", "", "", "ocaml", &kOCaml},
    {Language::FSharp, "F#", "fsharp f# fs fsx fsi", "fs fsx fsi", "", "", "", &kFSharp},
    {Language::CommonLisp, "Common Lisp", "lisp common-lisp commonlisp cl elisp emacs-lisp el", "lisp lsp cl el asd", "", "",
     "sbcl clisp", &kCommonLisp},
    {Language::Scheme, "Scheme", "scheme scm racket rkt guile", "scm ss rkt sld sls", "", "", "guile racket csi", &kScheme},
    {Language::Clojure, "Clojure", "clojure clj cljs cljc edn", "clj cljs cljc edn", "", "", "bb clojure", &kClojure},
    {Language::Sql, "SQL", "sql mysql mariadb postgresql postgres psql pgsql sqlite sqlite3 plsql tsql t-sql mssql",
     "sql ddl dml pgsql plsql", "", "", "", &kSql},
    {Language::VisualBasic, "Visual Basic", "vb vbnet vb.net visual-basic visualbasic vba vbs vbscript", "vb vbs bas vba", "",
     "", "", &kVisualBasic},
    {Language::PowerShell, "PowerShell", "powershell ps1 pwsh ps psm1 psd1 posh", "ps1 psm1 psd1", "", "", "pwsh powershell",
     &kPowerShell},
    {Language::Batch, "Batch", "batch bat cmd dosbatch winbatch", "bat cmd", "", "", "", nullptr},
    {Language::Fortran, "Fortran", "fortran f90 f95 f03 f08 f77 f for", "f f90 f95 f03 f08 f77 for ftn fpp", "", "", "",
     &kFortran},
    {Language::Pascal, "Pascal", "pascal delphi objectpascal freepascal pas dpr lpr", "pas pp dpr lpr dpk", "", "", "",
     &kPascal},
    {Language::Ada, "Ada", "ada adb ads", "adb ads ada", "", "", "", &kAda},
    {Language::Matlab, "MATLAB", "matlab octave", "", "", "", "octave", &kMatlab},
    {Language::Verilog, "Verilog", "verilog v systemverilog sv svh vh", "v vh sv svh", "", "", "", &kVerilog},
    {Language::Vhdl, "VHDL", "vhdl vhd", "vhd vhdl", "", "", "", &kVhdl},
    {Language::Solidity, "Solidity", "solidity sol", "sol", "", "", "", &kSolidity},
    {Language::GraphQL, "GraphQL", "graphql gql graphqls", "graphql gql graphqls", "", "", "", &kGraphQL},
    {Language::Protobuf, "Protocol Buffers", "protobuf proto proto3", "proto", "", "", "", &kProtobuf},
    {Language::Thrift, "Thrift", "thrift", "thrift", "", "", "", &kThrift},
    {Language::Hcl, "HCL", "hcl terraform tf tfvars", "hcl tf tfvars", "", "", "", &kHcl},
    {Language::Nix, "Nix", "nix", "nix", "", "", "", &kNix},
    {Language::Awk, "Awk", "awk gawk mawk nawk", "awk", "", "", "awk gawk mawk nawk", &kAwk},
    {Language::Tcl, "Tcl", "tcl tk", "tcl tk", "", "", "tclsh wish expect", &kTcl},
    {Language::CMake, "CMake", "cmake", "cmake", "CMakeLists.txt", "", "", &kCMake},
    {Language::Nginx, "Nginx", "nginx nginxconf", "", "nginx.conf", "", "", &kNginx},
    {Language::Assembly, "Assembly", "asm assembly nasm gas s x86asm armasm masm yasm", "asm s nasm", "", "", "",
     &kAssembly},
    {Language::Html, "HTML", "html htm xhtml svg vue svelte", "html htm xhtml shtml svg vue svelte", "", "", "", nullptr},
    {Language::Xml, "XML", "xml xsl xslt xsd rss atom plist wsdl xaml",
     "xml xsl xslt xsd rss atom plist wsdl xaml csproj fsproj vbproj vcxproj props targets nuspec resx storyboard xib "
     "kml gpx",
     "", "", "", nullptr},
    {Language::Css, "CSS", "css", "css", "", "", "", nullptr},
    {Language::Scss, "SCSS", "scss sass", "scss sass", "", "", "", nullptr},
    {Language::Less, "Less", "less", "less", "", "", "", nullptr},
    {Language::Markdown, "Markdown", "markdown md mkd mdown mdx", "md markdown mkd mkdn mdown mdx", "", "", "", nullptr},
    {Language::Ini, "INI", "ini cfg conf properties dosini editorconfig gitconfig desktop systemd",
     "ini cfg conf properties desktop service socket timer mount inf", "", "", "", nullptr},
    {Language::Toml, "TOML", "toml", "toml", "Cargo.lock poetry.lock Pipfile uv.lock", "", "", nullptr},
    {Language::Diff, "Diff", "diff patch udiff", "diff patch rej", "", "", "", nullptr},
    {Language::Dockerfile, "Dockerfile", "dockerfile docker containerfile", "dockerfile containerfile",
     "Dockerfile Containerfile", "Dockerfile. Containerfile.", "", nullptr},
    {Language::Makefile, "Makefile", "makefile make mk gnumakefile bsdmake", "mk mak make",
     "Makefile GNUmakefile BSDmakefile makefile", "Makefile.", "make gmake", nullptr},
    {Language::Latex, "LaTeX", "latex tex sty cls", "tex sty cls ltx dtx ins", "", "", "", nullptr},
    {Language::Http, "HTTP", "http https rest", "http rest", "", "", "", nullptr},
};

const LanguageInfo* infoFor(Language language) {
  for (const auto& info : kLanguages) {
    if (info.language == language) return &info;
  }
  return nullptr;
}

bool containsWord(std::string_view list, std::string_view word) {
  bool found = false;
  eachWord(list, [&](std::string_view candidate) { found = found || candidate == word; });
  return found;
}

const Words& wordsFor(const Syntax& syntax) {
  static const std::vector<std::pair<const Syntax*, Words>> all = [] {
    std::vector<std::pair<const Syntax*, Words>> compiled;
    for (const auto& info : kLanguages) {
      if (info.syntax != nullptr) compiled.emplace_back(info.syntax, Words(*info.syntax));
    }
    return compiled;
  }();
  for (const auto& [owner, words] : all) {
    if (owner == &syntax) return words;
  }
  static const Words none{Syntax{}};
  return none;
}

void tokenize(Language language, std::string_view s, Tokens& out, int depth) {
  switch (language) {
    case Language::None: return;
    case Language::Cpp: tokenizeCFamily(s, out, kCppFamily); return;
    case Language::C: tokenizeCFamily(s, out, kCFamily); return;
    case Language::ObjectiveC: tokenizeCFamily(s, out, kObjectiveCFamily); return;
    case Language::Python: tokenizePython(s, out); return;
    case Language::Shell: tokenizeShell(s, out); return;
    case Language::Yaml: tokenizeYaml(s, out); return;
    case Language::Json: tokenizeJson(s, out); return;
    case Language::Html: tokenizeMarkup(s, out, true, depth); return;
    case Language::Xml: tokenizeMarkup(s, out, false, depth); return;
    case Language::Css: tokenizeSheet(s, out, Sheet::Css); return;
    case Language::Scss: tokenizeSheet(s, out, Sheet::Scss); return;
    case Language::Less: tokenizeSheet(s, out, Sheet::Less); return;
    case Language::Markdown: tokenizeMarkdown(s, out, depth); return;
    case Language::Ini: tokenizeIni(s, out); return;
    case Language::Toml: tokenizeToml(s, out); return;
    case Language::Diff: tokenizeDiff(s, out); return;
    case Language::Dockerfile: tokenizeDockerfile(s, out, depth); return;
    case Language::Makefile: tokenizeMakefile(s, out); return;
    case Language::Latex: tokenizeLatex(s, out); return;
    case Language::Batch: tokenizeBatch(s, out); return;
    case Language::Http: tokenizeHttp(s, out, depth); return;
    default: break;
  }
  if (const auto* info = infoFor(language); info != nullptr && info->syntax != nullptr) {
    Generic(*info->syntax, wordsFor(*info->syntax), s, out).run();
  }
}

bool continuationByte(char byte) { return (u(byte) & 0xc0) == 0x80; }

// The guarantees of highlightTokens, enforced once more at the end: tokens in
// order, non-empty, inside the text, and never starting or ending inside a
// UTF-8 sequence (a boundary on a continuation byte moves forward).
Tokens sanitized(const Tokens& tokens, std::string_view s) {
  Tokens clean;
  clean.reserve(tokens.size());
  std::size_t last = 0;
  for (auto token : tokens) {
    token.end = std::min(token.end, s.size());
    while (token.begin < token.end && continuationByte(s[token.begin])) ++token.begin;
    while (token.end < s.size() && continuationByte(s[token.end])) ++token.end;
    token.begin = std::max(token.begin, last);
    if (token.end <= token.begin) continue;
    clean.push_back(token);
    last = token.end;
  }
  return clean;
}

void escapeInto(std::string& out, std::string_view value) {
  for (const char byte : value) {
    switch (byte) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:
        if ((u(byte) < 0x20 && byte != '\n' && byte != '\t') || u(byte) == 0x7f) out += "\xef\xbf\xbd";
        else out += byte;
    }
  }
}

std::vector<std::string_view> splitWords(std::string_view list) {
  std::vector<std::string_view> words;
  eachWord(list, [&words](std::string_view word) { words.push_back(word); });
  return words;
}

// The interpreter a shebang line runs: the program's base name, or after
// `env` (skipping its options such as -S) the first operand's.
std::string shebangInterpreter(std::string_view source) {
  auto line = source.substr(2, lineEnd(source, 0) - 2);
  const auto next = [&line]() {
    while (!line.empty() && blank(line.front())) line.remove_prefix(1);
    std::size_t k = 0;
    while (k < line.size() && !blank(line[k])) ++k;
    const auto word = line.substr(0, k);
    line.remove_prefix(k);
    return word;
  };
  const auto base = [](std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == npos ? path : path.substr(slash + 1);
  };
  auto program = base(next());
  if (program == "env") {
    do program = next();
    while (program.starts_with("-"));
    program = base(program);
  }
  return lower(program);
}

}  // namespace

Language languageForName(std::string_view name) {
  if (name.empty() || name.size() > 32) return Language::None;
  const auto lowered = lower(name);
  for (const auto& info : kLanguages) {
    if (containsWord(info.aliases, lowered)) return info.language;
  }
  return Language::None;
}

Language languageForPath(std::string_view path) {
  const auto slash = path.rfind('/');
  const auto name = slash == npos ? path : path.substr(slash + 1);
  if (name.empty()) return Language::None;
  for (const auto& info : kLanguages) {
    bool match = false;
    eachWord(info.files, [&](std::string_view file) { match = match || equalsIgnoringCase(file, name); });
    if (match) return info.language;
  }
  const auto dot = name.rfind('.');
  if (dot != npos && dot != 0) {
    const auto extension = lower(name.substr(dot + 1));
    for (const auto& info : kLanguages) {
      if (containsWord(info.extensions, extension)) return info.language;
    }
  }
  for (const auto& info : kLanguages) {
    bool match = false;
    eachWord(info.file_prefixes, [&](std::string_view prefix) {
      match = match || (name.size() > prefix.size() && equalsIgnoringCase(name.substr(0, prefix.size()), prefix));
    });
    if (match) return info.language;
  }
  return Language::None;
}

Language languageForShebang(std::string_view source) {
  if (!source.starts_with("#!")) return Language::None;
  const auto interpreter = shebangInterpreter(source);
  if (interpreter.empty()) return Language::None;
  for (const auto& info : kLanguages) {
    bool match = false;
    // An exact name, or one with a version suffix: python3.12, perl5, lua5.4.
    eachWord(info.interpreters, [&](std::string_view candidate) {
      match = match || (std::string_view(interpreter).starts_with(candidate) &&
                        std::all_of(interpreter.begin() + static_cast<std::ptrdiff_t>(candidate.size()), interpreter.end(),
                                    [](char b) { return digit(b) || b == '.'; }));
    });
    if (match) return info.language;
  }
  return Language::None;
}

Language detectLanguage(std::string_view path, std::string_view source) {
  const auto by_path = languageForPath(path);
  return by_path != Language::None ? by_path : languageForShebang(source);
}

std::vector<HighlightLanguage> highlightLanguages() {
  std::vector<HighlightLanguage> languages;
  for (const auto& info : kLanguages) {
    languages.push_back({info.language, info.name, splitWords(info.aliases), splitWords(info.extensions), splitWords(info.files)});
  }
  return languages;
}

std::string_view languageName(Language language) {
  const auto* info = infoFor(language);
  return info == nullptr ? std::string_view() : info->name;
}

std::vector<HighlightToken> highlightTokens(std::string_view source, Language language) {
  Tokens out;
  if (source.size() > kMaximumHighlightBytes) return out;
  tokenize(language, source, out, 0);
  return sanitized(out, source);
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
    case TokenKind::Type: return "hl-t";
    case TokenKind::Function: return "hl-f";
    case TokenKind::Inserted: return "hl-i";
    case TokenKind::Deleted: return "hl-d";
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
      // Each search stops at the token's end, so many tokens on one long line
      // do not each scan to that line's end.
      const auto run = source.substr(0, token.end);
      for (std::size_t start = token.begin; start < token.end;) {
        auto stop = run.find('\n', start);
        if (stop == npos) stop = token.end;
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
