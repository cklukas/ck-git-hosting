// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/yaml_subset.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ckgit/text.hpp"

namespace ckgit {
namespace {

struct RawLine {
  std::size_t indent = 0;       // count of leading spaces
  std::string_view content;     // bytes after the indent (comment not stripped)
  std::size_t number = 0;       // 1-based physical line number
  bool blank = false;           // empty, whitespace-only, or a full-line comment
};

bool isPlainKeyByte(unsigned char character) {
  return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
         (character >= '0' && character <= '9') || character == '.' || character == '_' ||
         character == '-';
}

// Removes a trailing " # comment" that is outside quotes and flow brackets, then
// trims surrounding whitespace. Never applied to block-literal content.
std::string_view stripComment(std::string_view text) {
  bool in_single = false;
  bool in_double = false;
  int depth = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char character = text[index];
    if (in_single) {
      if (character == '\'') in_single = false;
    } else if (in_double) {
      if (character == '\\' && index + 1 < text.size()) {
        ++index;
      } else if (character == '"') {
        in_double = false;
      }
    } else if (character == '\'') {
      in_single = true;
    } else if (character == '"') {
      in_double = true;
    } else if (character == '[' || character == '{') {
      ++depth;
    } else if (character == ']' || character == '}') {
      if (depth > 0) --depth;
    } else if (character == '#' && depth == 0 && (index == 0 || text[index - 1] == ' ')) {
      text = text.substr(0, index);
      break;
    }
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  return text;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

std::string parseScalarToken(const YamlDialect& dialect, std::string_view token, std::size_t line) {
  if (token.empty()) yamlMalformed(dialect, "empty value where a scalar was expected", line);
  if (token.front() == '\'') {
    if (token.size() < 2 || token.back() != '\'') yamlMalformed(dialect, "unterminated single-quoted scalar", line);
    const std::string_view body = token.substr(1, token.size() - 2);
    std::string result;
    for (std::size_t index = 0; index < body.size(); ++index) {
      if (body[index] == '\'') {
        if (index + 1 < body.size() && body[index + 1] == '\'') {
          result.push_back('\'');
          ++index;
        } else {
          yamlMalformed(dialect, "stray quote in single-quoted scalar", line);
        }
      } else {
        result.push_back(body[index]);
      }
    }
    if (result.size() > dialect.bounds.scalar_bytes) yamlTooLarge(dialect, "a scalar exceeds its length limit");
    return result;
  }
  if (token.front() == '"') {
    if (token.size() < 2 || token.back() != '"') yamlMalformed(dialect, "unterminated double-quoted scalar", line);
    const std::string_view body = token.substr(1, token.size() - 2);
    std::string result;
    for (std::size_t index = 0; index < body.size(); ++index) {
      if (body[index] == '\\') {
        if (index + 1 >= body.size()) yamlMalformed(dialect, "dangling escape in double-quoted scalar", line);
        const char next = body[++index];
        switch (next) {
          case '\\': result.push_back('\\'); break;
          case '"': result.push_back('"'); break;
          case 'n': result.push_back('\n'); break;
          case 't': result.push_back('\t'); break;
          default: yamlMalformed(dialect, "unsupported escape in double-quoted scalar", line);
        }
      } else if (body[index] == '"') {
        yamlMalformed(dialect, "stray quote in double-quoted scalar", line);
      } else {
        result.push_back(body[index]);
      }
    }
    if (result.size() > dialect.bounds.scalar_bytes) yamlTooLarge(dialect, "a scalar exceeds its length limit");
    return result;
  }
  // Plain scalar: a flow indicator here means a malformed flow collection, and a
  // control byte means the value was never meant to be plain.
  for (const unsigned char character : token) {
    if (character < 0x20 || character == 0x7f) yamlMalformed(dialect, "control character in a plain value", line);
  }
  if (token.size() > dialect.bounds.scalar_bytes) yamlTooLarge(dialect, "a scalar exceeds its length limit");
  return std::string(token);
}

// Splits a flow collection body on top-level commas, honouring quotes and one
// level of nested brackets (a flow sequence value inside a flow mapping).
std::vector<std::string_view> splitFlow(const YamlDialect& dialect, std::string_view body, std::size_t line) {
  std::vector<std::string_view> parts;
  if (body.find_first_not_of(" \t") == std::string_view::npos) return parts;
  bool in_single = false;
  bool in_double = false;
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t index = 0; index <= body.size(); ++index) {
    const bool at_end = index == body.size();
    const char character = at_end ? '\0' : body[index];
    if (!at_end && in_single) {
      if (character == '\'') in_single = false;
      continue;
    }
    if (!at_end && in_double) {
      if (character == '\\' && index + 1 < body.size()) ++index;
      else if (character == '"') in_double = false;
      continue;
    }
    if (at_end || (character == ',' && depth == 0)) {
      parts.push_back(body.substr(start, index - start));
      start = index + 1;
    } else if (character == '\'') {
      in_single = true;
    } else if (character == '"') {
      in_double = true;
    } else if (character == '[' || character == '{') {
      ++depth;
    } else if (character == ']' || character == '}') {
      if (depth == 0) yamlMalformed(dialect, "unbalanced flow brackets", line);
      --depth;
    }
  }
  if (in_single || in_double || depth != 0) yamlMalformed(dialect, "unterminated flow collection", line);
  return parts;
}

class Parser {
 public:
  Parser(const YamlDialect& dialect, std::vector<RawLine> lines)
      : dialect_(dialect), lines_(std::move(lines)) {}

  YamlNode parseDocument() {
    skipBlanks();
    if (pos_ >= lines_.size()) malformed(std::string(dialect_.document) + " file is empty", 1);
    if (lines_[pos_].indent != 0) malformed("the top level must not be indented", lines_[pos_].number);
    YamlNode root = parseBlockMapping(0, /*depth=*/0);
    skipBlanks();
    if (pos_ < lines_.size()) {
      malformed("unexpected content after " + std::string(dialect_.document), lines_[pos_].number);
    }
    return root;
  }

 private:
  [[noreturn]] void malformed(const std::string& message, std::size_t line) const {
    yamlMalformed(dialect_, message, line);
  }

  [[noreturn]] void tooLarge(const std::string& message) const { yamlTooLarge(dialect_, message); }

  [[noreturn]] void tooDeep() const { tooLarge(std::string(dialect_.document) + " is nested too deeply"); }

  void skipBlanks() {
    while (pos_ < lines_.size() && lines_[pos_].blank) ++pos_;
  }

  YamlNode parseValue(std::string_view text, std::size_t line, std::size_t owner_indent, std::size_t depth) {
    const std::string_view value = stripComment(text);
    if (value == "|") return parseBlockLiteral(owner_indent, line);
    if (!value.empty() && value.front() == '[') return parseFlowSequence(value, line);
    if (!value.empty() && value.front() == '{') return parseFlowMapping(value, line, depth);
    YamlNode node;
    node.kind = YamlNode::Kind::Scalar;
    node.line = line;
    node.scalar = parseScalarToken(dialect_, value, line);
    return node;
  }

  YamlNode parseFlowSequence(std::string_view text, std::size_t line) {
    if (text.size() < 2 || text.back() != ']') malformed("unterminated flow sequence", line);
    YamlNode node;
    node.kind = YamlNode::Kind::Sequence;
    node.line = line;
    for (std::string_view part : splitFlow(dialect_, text.substr(1, text.size() - 2), line)) {
      if (node.items.size() >= dialect_.bounds.flow_sequence_items) tooLarge("a sequence has too many items");
      YamlNode item;
      item.kind = YamlNode::Kind::Scalar;
      item.line = line;
      item.scalar = parseScalarToken(dialect_, trim(part), line);
      node.items.push_back(std::move(item));
    }
    return node;
  }

  YamlNode parseFlowMapping(std::string_view text, std::size_t line, std::size_t depth) {
    if (depth + 1 > dialect_.bounds.nesting_depth) tooDeep();
    if (text.size() < 2 || text.back() != '}') malformed("unterminated flow mapping", line);
    YamlNode node;
    node.kind = YamlNode::Kind::Mapping;
    node.line = line;
    for (std::string_view part : splitFlow(dialect_, text.substr(1, text.size() - 2), line)) {
      const std::string_view entry = trim(part);
      const std::size_t colon = findKeyColon(entry);
      if (colon == std::string_view::npos) malformed("flow mapping entry is not key: value", line);
      const std::string_view key = trim(entry.substr(0, colon));
      const std::string_view rest = trim(entry.substr(colon + 1));
      addEntry(node, key, parseValue(rest, line, /*owner_indent=*/0, depth + 1), line);
    }
    return node;
  }

  // Reads a `|` block scalar: every following line indented deeper than the key
  // that introduced it, dedented by the first content line's indent.
  YamlNode parseBlockLiteral(std::size_t owner_indent, std::size_t line) {
    std::vector<std::size_t> body;
    std::size_t base = std::string::npos;
    while (pos_ < lines_.size()) {
      const RawLine& raw = lines_[pos_];
      if (!raw.blank && raw.indent <= owner_indent) break;
      if (!raw.blank && base == std::string::npos) base = raw.indent;
      body.push_back(pos_);
      ++pos_;
    }
    if (base == std::string::npos) malformed("block scalar '|' has no content", line);
    std::string text;
    for (const std::size_t index : body) {
      const RawLine& raw = lines_[index];
      if (raw.blank) {
        text.push_back('\n');
        continue;
      }
      if (raw.indent < base) malformed("block scalar line is under-indented", raw.number);
      // The raw content excludes the indent already; re-add the indentation
      // beyond the block's base so nested shell structure is preserved.
      text.append(raw.indent - base, ' ');
      text.append(raw.content);
      text.push_back('\n');
      if (text.size() > dialect_.bounds.block_scalar_bytes) tooLarge("a block scalar exceeds its length limit");
    }
    while (text.size() >= 2 && text[text.size() - 1] == '\n' && text[text.size() - 2] == '\n') {
      text.pop_back();
    }
    YamlNode node;
    node.kind = YamlNode::Kind::Scalar;
    node.line = line;
    node.scalar = std::move(text);
    return node;
  }

  YamlNode parseBlockSequence(std::size_t indent, std::size_t depth) {
    if (depth + 1 > dialect_.bounds.nesting_depth) tooDeep();
    YamlNode node;
    node.kind = YamlNode::Kind::Sequence;
    node.line = lines_[pos_].number;
    while (true) {
      skipBlanks();
      if (pos_ >= lines_.size() || lines_[pos_].indent != indent || !startsWithDash(lines_[pos_])) break;
      if (node.items.size() >= dialect_.bounds.block_sequence_items) tooLarge("a sequence has too many items");
      const RawLine line = lines_[pos_];
      const std::string_view after = line.content.substr(1);  // drop '-'
      if (after.empty() || trim(after).empty()) {
        // The item is a block on the following, more-indented lines.
        ++pos_;
        node.items.push_back(parseBlockNode(indent + 1, depth + 1));
        continue;
      }
      if (after.front() != ' ') malformed("a sequence dash needs a space before its item", line.number);
      const std::size_t lead = after.find_first_not_of(' ');
      const std::size_t item_col = line.indent + 1 + lead;
      const std::string_view item = after.substr(lead);
      if (findKeyColon(stripComment(item)) != std::string_view::npos) {
        node.items.push_back(parseSeededMapping(item_col, item, line.number, depth + 1));
      } else {
        ++pos_;
        node.items.push_back(parseValue(item, line.number, line.indent, depth + 1));
      }
    }
    if (node.items.empty()) malformed("a block sequence has no items", node.line);
    return node;
  }

  YamlNode parseBlockNode(std::size_t min_indent, std::size_t depth) {
    skipBlanks();
    if (pos_ >= lines_.size() || lines_[pos_].indent < min_indent) {
      malformed("a value is missing where the file indents further", pos_ < lines_.size() ? lines_[pos_].number : 0);
    }
    const std::size_t indent = lines_[pos_].indent;
    if (startsWithDash(lines_[pos_])) return parseBlockSequence(indent, depth);
    return parseBlockMapping(indent, depth);
  }

  YamlNode parseBlockMapping(std::size_t indent, std::size_t depth) {
    if (depth + 1 > dialect_.bounds.nesting_depth) tooDeep();
    YamlNode node;
    node.kind = YamlNode::Kind::Mapping;
    node.line = lines_[pos_].number;
    while (true) {
      skipBlanks();
      if (pos_ >= lines_.size() || lines_[pos_].indent != indent || startsWithDash(lines_[pos_])) break;
      const RawLine line = lines_[pos_];
      ++pos_;
      readMappingEntry(node, line.content, line.number, indent, depth);
    }
    if (node.entries.empty()) malformed("a block mapping has no entries", node.line);
    return node;
  }

  // A mapping whose first entry is the text after a sequence dash, with later
  // sibling entries aligned at the dash's content column.
  YamlNode parseSeededMapping(std::size_t indent, std::string_view seed, std::size_t seed_line, std::size_t depth) {
    if (depth + 1 > dialect_.bounds.nesting_depth) tooDeep();
    ++pos_;  // consume the dash line
    YamlNode node;
    node.kind = YamlNode::Kind::Mapping;
    node.line = seed_line;
    readMappingEntry(node, seed, seed_line, indent, depth);
    while (true) {
      skipBlanks();
      if (pos_ >= lines_.size() || lines_[pos_].indent != indent || startsWithDash(lines_[pos_])) break;
      const RawLine line = lines_[pos_];
      ++pos_;
      readMappingEntry(node, line.content, line.number, indent, depth);
    }
    return node;
  }

  void readMappingEntry(YamlNode& node, std::string_view content, std::size_t line, std::size_t indent,
                        std::size_t depth) {
    const std::string_view stripped_for_colon = stripComment(content);
    const std::size_t colon = findKeyColon(stripped_for_colon);
    if (colon == std::string_view::npos) malformed("expected a key: value entry", line);
    const std::string_view key = trim(content.substr(0, colon));
    const std::string_view rest = trim(content.substr(colon + 1));
    if (rest.empty()) {
      addEntry(node, key, parseBlockNode(indent + 1, depth + 1), line);
    } else {
      addEntry(node, key, parseValue(rest, line, indent, depth), line);
    }
  }

  static bool startsWithDash(const RawLine& line) {
    return !line.content.empty() && line.content.front() == '-' &&
           (line.content.size() == 1 || line.content[1] == ' ');
  }

  // The first ':' that ends a key: either at end of the (comment-stripped) text
  // or followed by a space. A ':' inside a URL or value is not a separator.
  static std::size_t findKeyColon(std::string_view text) {
    bool in_single = false;
    bool in_double = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
      const char character = text[index];
      if (in_single) {
        if (character == '\'') in_single = false;
      } else if (in_double) {
        if (character == '\\' && index + 1 < text.size()) ++index;
        else if (character == '"') in_double = false;
      } else if (character == '\'') {
        in_single = true;
      } else if (character == '"') {
        in_double = true;
      } else if (character == ':' && (index + 1 == text.size() || text[index + 1] == ' ')) {
        return index;
      }
    }
    return std::string_view::npos;
  }

  void addEntry(YamlNode& node, std::string_view key, YamlNode value, std::size_t line) {
    if (key.empty() || key.size() > dialect_.bounds.key_bytes) malformed("a key is empty or too long", line);
    for (const unsigned char character : key) {
      if (!isPlainKeyByte(character)) malformed("a key has an unsupported character", line);
    }
    const std::string key_string(key);
    for (const auto& entry : node.entries) {
      if (entry.first == key_string) malformed("duplicate key '" + key_string + "'", line);
    }
    if (node.entries.size() >= dialect_.bounds.mapping_entries) tooLarge("a mapping has too many entries");
    node.entries.emplace_back(key_string, std::move(value));
  }

  const YamlDialect& dialect_;
  std::vector<RawLine> lines_;
  std::size_t pos_ = 0;
};

std::vector<RawLine> splitLines(const YamlDialect& dialect, std::string_view content) {
  std::vector<RawLine> lines;
  std::size_t start = 0;
  std::size_t number = 0;
  while (start <= content.size()) {
    std::size_t newline = content.find('\n', start);
    const bool last = newline == std::string_view::npos;
    if (last) newline = content.size();
    ++number;
    if (lines.size() >= dialect.bounds.lines) {
      yamlTooLarge(dialect, std::string(dialect.document) + " has too many lines");
    }
    std::string_view physical = content.substr(start, newline - start);
    if (physical.size() > dialect.bounds.line_bytes) yamlTooLarge(dialect, "a line exceeds its length limit");
    if (!physical.empty() && physical.back() == '\r') {
      throw std::runtime_error(std::string(dialect.prefix) + ": carriage returns are not allowed (use LF line endings)");
    }
    std::size_t indent = 0;
    while (indent < physical.size() && physical[indent] == ' ') ++indent;
    if (indent < physical.size() && physical[indent] == '\t') {
      yamlMalformed(dialect, "tabs may not be used for indentation", number);
    }
    RawLine line;
    line.number = number;
    line.indent = indent;
    line.content = physical.substr(indent);
    line.blank = line.content.empty() || line.content.front() == '#';
    lines.push_back(line);
    if (last) break;
    start = newline + 1;
  }
  return lines;
}

}  // namespace

void yamlMalformed(const YamlDialect& dialect, const std::string& message, std::size_t line) {
  throw std::runtime_error(std::string(dialect.prefix) + ": " + message + " (line " + std::to_string(line) + ")");
}

void yamlTooLarge(const YamlDialect& dialect, const std::string& message) {
  throw std::length_error(std::string(dialect.prefix) + ": " + message);
}

const YamlNode& yamlRequireKind(const YamlDialect& dialect, const YamlNode& node, YamlNode::Kind kind,
                                const char* what) {
  if (node.kind != kind) yamlMalformed(dialect, std::string("expected ") + what, node.line);
  return node;
}

const YamlNode* yamlFindEntry(const YamlNode& mapping, std::string_view key) {
  for (const auto& entry : mapping.entries) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

void yamlRejectUnknownKeys(const YamlDialect& dialect, const YamlNode& mapping,
                           std::initializer_list<std::string_view> allowed) {
  for (const auto& entry : mapping.entries) {
    if (std::find(allowed.begin(), allowed.end(), entry.first) == allowed.end()) {
      yamlMalformed(dialect, "unknown key '" + entry.first + "'", mapping.line);
    }
  }
}

YamlNode parseYamlSubset(std::string_view content, const YamlDialect& dialect) {
  if (content.size() > dialect.bounds.document_bytes) {
    yamlTooLarge(dialect, std::string(dialect.document) + " file exceeds its size limit");
  }
  if (!isValidUtf8(content)) {
    throw std::runtime_error(std::string(dialect.prefix) + ": the file is not valid UTF-8");
  }
  Parser parser(dialect, splitLines(dialect, content));
  return parser.parseDocument();
}

}  // namespace ckgit
