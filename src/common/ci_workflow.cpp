// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_workflow.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ckgit/text.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

// A parse tree deliberately deeper than the schema needs is refused before the
// schema is even consulted, so a hostile file cannot exhaust the stack.
constexpr std::size_t kMaximumNestingDepth = 16;

[[noreturn]] void malformed(const std::string& message, std::size_t line) {
  throw std::runtime_error("ci workflow: " + message + " (line " + std::to_string(line) + ")");
}

[[noreturn]] void tooLarge(const std::string& message) {
  throw std::length_error("ci workflow: " + message);
}

// A node of the small YAML subset. A mapping keeps insertion order and rejects
// duplicate keys; a sequence keeps order; a scalar is already unquoted/unescaped.
struct Node {
  enum class Kind { Scalar, Sequence, Mapping };
  Kind kind = Kind::Scalar;
  std::string scalar;
  std::vector<Node> items;
  std::vector<std::pair<std::string, Node>> entries;
  std::size_t line = 0;
};

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

std::string parseScalarToken(std::string_view token, std::size_t line) {
  if (token.empty()) malformed("empty value where a scalar was expected", line);
  if (token.front() == '\'') {
    if (token.size() < 2 || token.back() != '\'') malformed("unterminated single-quoted scalar", line);
    const std::string_view body = token.substr(1, token.size() - 2);
    std::string result;
    for (std::size_t index = 0; index < body.size(); ++index) {
      if (body[index] == '\'') {
        if (index + 1 < body.size() && body[index + 1] == '\'') {
          result.push_back('\'');
          ++index;
        } else {
          malformed("stray quote in single-quoted scalar", line);
        }
      } else {
        result.push_back(body[index]);
      }
    }
    if (result.size() > kMaximumCiScalarBytes) tooLarge("a scalar exceeds its length limit");
    return result;
  }
  if (token.front() == '"') {
    if (token.size() < 2 || token.back() != '"') malformed("unterminated double-quoted scalar", line);
    const std::string_view body = token.substr(1, token.size() - 2);
    std::string result;
    for (std::size_t index = 0; index < body.size(); ++index) {
      if (body[index] == '\\') {
        if (index + 1 >= body.size()) malformed("dangling escape in double-quoted scalar", line);
        const char next = body[++index];
        switch (next) {
          case '\\': result.push_back('\\'); break;
          case '"': result.push_back('"'); break;
          case 'n': result.push_back('\n'); break;
          case 't': result.push_back('\t'); break;
          default: malformed("unsupported escape in double-quoted scalar", line);
        }
      } else if (body[index] == '"') {
        malformed("stray quote in double-quoted scalar", line);
      } else {
        result.push_back(body[index]);
      }
    }
    if (result.size() > kMaximumCiScalarBytes) tooLarge("a scalar exceeds its length limit");
    return result;
  }
  // Plain scalar: a flow indicator here means a malformed flow collection, and a
  // control byte means the value was never meant to be plain.
  for (const unsigned char character : token) {
    if (character < 0x20 || character == 0x7f) malformed("control character in a plain value", line);
  }
  if (token.size() > kMaximumCiScalarBytes) tooLarge("a scalar exceeds its length limit");
  return std::string(token);
}

// Splits a flow collection body on top-level commas, honouring quotes and one
// level of nested brackets (a flow sequence value inside a flow mapping).
std::vector<std::string_view> splitFlow(std::string_view body, std::size_t line) {
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
      if (depth == 0) malformed("unbalanced flow brackets", line);
      --depth;
    }
  }
  if (in_single || in_double || depth != 0) malformed("unterminated flow collection", line);
  return parts;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

class Parser {
 public:
  explicit Parser(std::vector<RawLine> lines) : lines_(std::move(lines)) {}

  Node parseDocument() {
    skipBlanks();
    if (pos_ >= lines_.size()) malformed("the workflow file is empty", 1);
    if (lines_[pos_].indent != 0) malformed("the top level must not be indented", lines_[pos_].number);
    Node root = parseBlockMapping(0, /*depth=*/0);
    skipBlanks();
    if (pos_ < lines_.size()) malformed("unexpected content after the workflow", lines_[pos_].number);
    return root;
  }

 private:
  void skipBlanks() {
    while (pos_ < lines_.size() && lines_[pos_].blank) ++pos_;
  }

  Node parseValue(std::string_view text, std::size_t line, std::size_t owner_indent, std::size_t depth) {
    const std::string_view value = stripComment(text);
    if (value == "|") return parseBlockLiteral(owner_indent, line);
    if (!value.empty() && value.front() == '[') return parseFlowSequence(value, line);
    if (!value.empty() && value.front() == '{') return parseFlowMapping(value, line, depth);
    Node node;
    node.kind = Node::Kind::Scalar;
    node.line = line;
    node.scalar = parseScalarToken(value, line);
    return node;
  }

  Node parseFlowSequence(std::string_view text, std::size_t line) {
    if (text.size() < 2 || text.back() != ']') malformed("unterminated flow sequence", line);
    Node node;
    node.kind = Node::Kind::Sequence;
    node.line = line;
    for (std::string_view part : splitFlow(text.substr(1, text.size() - 2), line)) {
      if (node.items.size() >= kMaximumCiArgvItems) tooLarge("a sequence has too many items");
      Node item;
      item.kind = Node::Kind::Scalar;
      item.line = line;
      item.scalar = parseScalarToken(trim(part), line);
      node.items.push_back(std::move(item));
    }
    return node;
  }

  Node parseFlowMapping(std::string_view text, std::size_t line, std::size_t depth) {
    if (depth + 1 > kMaximumNestingDepth) tooLarge("the workflow is nested too deeply");
    if (text.size() < 2 || text.back() != '}') malformed("unterminated flow mapping", line);
    Node node;
    node.kind = Node::Kind::Mapping;
    node.line = line;
    for (std::string_view part : splitFlow(text.substr(1, text.size() - 2), line)) {
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
  Node parseBlockLiteral(std::size_t owner_indent, std::size_t line) {
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
      if (text.size() > kMaximumCiScriptBytes) tooLarge("a block scalar exceeds its length limit");
    }
    while (text.size() >= 2 && text[text.size() - 1] == '\n' && text[text.size() - 2] == '\n') {
      text.pop_back();
    }
    Node node;
    node.kind = Node::Kind::Scalar;
    node.line = line;
    node.scalar = std::move(text);
    return node;
  }

  Node parseBlockSequence(std::size_t indent, std::size_t depth) {
    if (depth + 1 > kMaximumNestingDepth) tooLarge("the workflow is nested too deeply");
    Node node;
    node.kind = Node::Kind::Sequence;
    node.line = lines_[pos_].number;
    while (true) {
      skipBlanks();
      if (pos_ >= lines_.size() || lines_[pos_].indent != indent || !startsWithDash(lines_[pos_])) break;
      if (node.items.size() >= kMaximumCiJobs && node.items.size() >= kMaximumCiStepsPerJob) {
        tooLarge("a sequence has too many items");
      }
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

  Node parseBlockNode(std::size_t min_indent, std::size_t depth) {
    skipBlanks();
    if (pos_ >= lines_.size() || lines_[pos_].indent < min_indent) {
      malformed("a value is missing where the file indents further", pos_ < lines_.size() ? lines_[pos_].number : 0);
    }
    const std::size_t indent = lines_[pos_].indent;
    if (startsWithDash(lines_[pos_])) return parseBlockSequence(indent, depth);
    return parseBlockMapping(indent, depth);
  }

  Node parseBlockMapping(std::size_t indent, std::size_t depth) {
    if (depth + 1 > kMaximumNestingDepth) tooLarge("the workflow is nested too deeply");
    Node node;
    node.kind = Node::Kind::Mapping;
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
  Node parseSeededMapping(std::size_t indent, std::string_view seed, std::size_t seed_line, std::size_t depth) {
    if (depth + 1 > kMaximumNestingDepth) tooLarge("the workflow is nested too deeply");
    ++pos_;  // consume the dash line
    Node node;
    node.kind = Node::Kind::Mapping;
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

  void readMappingEntry(Node& node, std::string_view content, std::size_t line, std::size_t indent,
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

  void addEntry(Node& node, std::string_view key, Node value, std::size_t line) {
    if (key.empty() || key.size() > kMaximumCiKeyBytes) malformed("a key is empty or too long", line);
    for (const unsigned char character : key) {
      if (!isPlainKeyByte(character)) malformed("a key has an unsupported character", line);
    }
    const std::string key_string(key);
    for (const auto& entry : node.entries) {
      if (entry.first == key_string) malformed("duplicate key '" + key_string + "'", line);
    }
    if (node.entries.size() >= kMaximumCiEnvEntries + 8) tooLarge("a mapping has too many entries");
    node.entries.emplace_back(key_string, std::move(value));
  }

  std::vector<RawLine> lines_;
  std::size_t pos_ = 0;
};

// ---- schema interpretation -------------------------------------------------

const Node& requireKind(const Node& node, Node::Kind kind, const char* what) {
  if (node.kind != kind) malformed(std::string("expected ") + what, node.line);
  return node;
}

const Node* findEntry(const Node& mapping, std::string_view key) {
  for (const auto& entry : mapping.entries) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

void rejectUnknownKeys(const Node& mapping, std::initializer_list<std::string_view> allowed) {
  for (const auto& entry : mapping.entries) {
    if (std::find(allowed.begin(), allowed.end(), entry.first) == allowed.end()) {
      malformed("unknown key '" + entry.first + "'", mapping.line);
    }
  }
}

bool isValidEnvName(std::string_view name) {
  if (name.empty() || name.size() > kMaximumCiKeyBytes) return false;
  const unsigned char first = name.front();
  const bool first_ok = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_';
  if (!first_ok) return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_';
  });
}

bool isValidCiName(std::string_view name) {
  return !name.empty() && name.size() <= kMaximumCiNameBytes &&
         std::all_of(name.begin(), name.end(), [](unsigned char character) {
           return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                  character == '-';
         });
}

CiEnv interpretEnv(const Node& node) {
  requireKind(node, Node::Kind::Mapping, "env to be a mapping of name: value");
  if (node.entries.size() > kMaximumCiEnvEntries) tooLarge("env has too many entries");
  CiEnv env;
  for (const auto& entry : node.entries) {
    if (!isValidEnvName(entry.first)) malformed("invalid environment name '" + entry.first + "'", node.line);
    const Node& value = requireKind(entry.second, Node::Kind::Scalar, "an env value to be a scalar");
    env.emplace_back(entry.first, value.scalar);
  }
  return env;
}

CiStep interpretStep(const Node& node) {
  requireKind(node, Node::Kind::Mapping, "a step to be a mapping");
  rejectUnknownKeys(node, {"name", "run", "script"});
  CiStep step;
  if (const Node* name = findEntry(node, "name")) {
    step.name = requireKind(*name, Node::Kind::Scalar, "a step name to be a scalar").scalar;
    if (step.name.empty() || step.name.size() > kMaximumCiNameBytes) {
      malformed("a step name is empty or too long", node.line);
    }
  }
  const Node* run = findEntry(node, "run");
  const Node* script = findEntry(node, "script");
  if ((run != nullptr) == (script != nullptr)) {
    malformed("a step needs exactly one of 'run' or 'script'", node.line);
  }
  if (script != nullptr) {
    step.script = requireKind(*script, Node::Kind::Scalar, "script to be text").scalar;
    if (step.script.empty()) malformed("a script is empty", node.line);
    if (step.script.size() > kMaximumCiScriptBytes) tooLarge("a script exceeds its length limit");
    return step;
  }
  if (run->kind == Node::Kind::Scalar) {
    step.script = run->scalar;  // scalar run: is a shell command line
    if (step.script.empty()) malformed("a run command is empty", node.line);
    return step;
  }
  requireKind(*run, Node::Kind::Sequence, "run to be a command line or an argv list");
  if (run->items.empty()) malformed("a run argv list is empty", node.line);
  if (run->items.size() > kMaximumCiArgvItems) tooLarge("a run argv list is too long");
  for (const Node& item : run->items) {
    const Node& argument = requireKind(item, Node::Kind::Scalar, "each argv item to be a scalar");
    if (argument.scalar.empty()) malformed("an argv item is empty", node.line);
    step.argv.push_back(argument.scalar);
  }
  return step;
}

CiJob interpretJob(const Node& node) {
  requireKind(node, Node::Kind::Mapping, "each job to be a mapping");
  rejectUnknownKeys(node, {"name", "env", "steps"});
  CiJob job;
  const Node* name = findEntry(node, "name");
  if (name == nullptr) malformed("a job is missing 'name'", node.line);
  job.name = requireKind(*name, Node::Kind::Scalar, "a job name to be a scalar").scalar;
  if (!isValidCiName(job.name)) malformed("invalid job name '" + job.name + "'", node.line);
  if (const Node* env = findEntry(node, "env")) job.env = interpretEnv(*env);
  const Node* steps = findEntry(node, "steps");
  if (steps == nullptr) malformed("job '" + job.name + "' has no steps", node.line);
  requireKind(*steps, Node::Kind::Sequence, "steps to be a list");
  if (steps->items.empty()) malformed("job '" + job.name + "' has no steps", node.line);
  if (steps->items.size() > kMaximumCiStepsPerJob) tooLarge("a job has too many steps");
  for (const Node& step : steps->items) job.steps.push_back(interpretStep(step));
  return job;
}

CiWorkflow interpret(const Node& root) {
  requireKind(root, Node::Kind::Mapping, "the workflow to be a mapping");
  rejectUnknownKeys(root, {"version", "on", "env", "jobs"});

  const Node* version = findEntry(root, "version");
  if (version == nullptr) malformed("the workflow is missing 'version'", root.line);
  const std::string& version_text = requireKind(*version, Node::Kind::Scalar, "version to be a number").scalar;
  int version_value = 0;
  const auto [end, error] = std::from_chars(version_text.data(), version_text.data() + version_text.size(), version_value);
  if (error != std::errc{} || end != version_text.data() + version_text.size() || version_value != 1) {
    malformed("version must be 1", root.line);
  }

  CiWorkflow workflow;
  workflow.version = 1;

  if (const Node* on = findEntry(root, "on")) {
    requireKind(*on, Node::Kind::Mapping, "'on' to be a mapping");
    rejectUnknownKeys(*on, {"branches"});
    if (const Node* branches = findEntry(*on, "branches")) {
      requireKind(*branches, Node::Kind::Sequence, "branches to be a list");
      if (branches->items.size() > kMaximumCiBranches) tooLarge("too many branches");
      for (const Node& branch : branches->items) {
        const std::string& value = requireKind(branch, Node::Kind::Scalar, "each branch to be a scalar").scalar;
        if (!isValidBranchName(value)) malformed("invalid branch name '" + value + "'", on->line);
        workflow.branches.push_back(value);
      }
    }
  }

  if (const Node* env = findEntry(root, "env")) workflow.env = interpretEnv(*env);

  const Node* jobs = findEntry(root, "jobs");
  if (jobs == nullptr) malformed("the workflow is missing 'jobs'", root.line);
  requireKind(*jobs, Node::Kind::Sequence, "jobs to be a list");
  if (jobs->items.empty()) malformed("the workflow has no jobs", root.line);
  if (jobs->items.size() > kMaximumCiJobs) tooLarge("too many jobs");
  std::set<std::string> job_names;
  for (const Node& job_node : jobs->items) {
    CiJob job = interpretJob(job_node);
    if (!job_names.insert(job.name).second) malformed("duplicate job name '" + job.name + "'", job_node.line);
    workflow.jobs.push_back(std::move(job));
  }
  return workflow;
}

std::vector<RawLine> splitLines(std::string_view content) {
  std::vector<RawLine> lines;
  std::size_t start = 0;
  std::size_t number = 0;
  while (start <= content.size()) {
    std::size_t newline = content.find('\n', start);
    const bool last = newline == std::string_view::npos;
    if (last) newline = content.size();
    ++number;
    if (lines.size() >= kMaximumCiLines) tooLarge("the workflow has too many lines");
    std::string_view physical = content.substr(start, newline - start);
    if (physical.size() > kMaximumCiLineBytes) tooLarge("a line exceeds its length limit");
    if (!physical.empty() && physical.back() == '\r') {
      throw std::runtime_error("ci workflow: carriage returns are not allowed (use LF line endings)");
    }
    std::size_t indent = 0;
    while (indent < physical.size() && physical[indent] == ' ') ++indent;
    if (indent < physical.size() && physical[indent] == '\t') {
      throw std::runtime_error("ci workflow: tabs may not be used for indentation (line " +
                               std::to_string(number) + ")");
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

CiWorkflow parseCiWorkflow(std::string_view content) {
  if (content.size() > kMaximumCiWorkflowBytes) tooLarge("the workflow file exceeds its size limit");
  if (!isValidUtf8(content)) throw std::runtime_error("ci workflow: the file is not valid UTF-8");
  Parser parser(splitLines(content));
  return interpret(parser.parseDocument());
}

}  // namespace ckgit
