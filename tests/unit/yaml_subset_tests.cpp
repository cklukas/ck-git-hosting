// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/yaml_subset.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

#include "ckgit/ci_workflow.hpp"

namespace {

using ckgit::YamlNode;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("yaml subset: " + message);
}

YamlNode parse(const std::string& text) {
  return ckgit::parseYamlSubset(text, ckgit::kCiWorkflowYamlDialect);
}

// The message a rejection carries, or "" when the text was accepted; a
// length_error (an exceeded bound) is marked as such.
std::string rejection(const std::string& text, const ckgit::YamlDialect& dialect = ckgit::kCiWorkflowYamlDialect) {
  try {
    ckgit::parseYamlSubset(text, dialect);
    return {};
  } catch (const std::length_error& error) {
    return std::string("length_error: ") + error.what();
  } catch (const std::runtime_error& error) {
    return error.what();
  }
}

bool contains(std::string_view text, std::string_view part) {
  return text.find(part) != std::string_view::npos;
}

const YamlNode& entry(const YamlNode& mapping, std::string_view key) {
  const YamlNode* found = ckgit::yamlFindEntry(mapping, key);
  require(found != nullptr, "missing entry '" + std::string(key) + "'");
  return *found;
}

void testStructures() {
  const YamlNode root = parse(
      "# a full-line comment\n"
      "name: build   # a trailing comment\n"
      "url: http://example.test/x:y\n"
      "nested:\n"
      "  inner: 1\n"
      "  list:\n"
      "    - a\n"
      "    - b\n"
      "items:\n"
      "  - name: first\n"
      "    extra: x\n"
      "  - name: second\n"
      "  -\n"
      "    name: third\n"
      "flow: { a: 1, b: [x, y], c: 'q' }\n"
      "seq: [one, two]\n"
      "empty_seq: []\n");
  require(root.kind == YamlNode::Kind::Mapping && root.line == 2, "the document is a mapping starting at its first entry");
  require(entry(root, "name").scalar == "build", "a trailing comment is stripped from a plain scalar");
  require(entry(root, "url").scalar == "http://example.test/x:y", "a colon inside a value is not a key separator");
  const YamlNode& nested = entry(root, "nested");
  require(nested.kind == YamlNode::Kind::Mapping && entry(nested, "inner").scalar == "1", "a block mapping nests");
  const YamlNode& list = entry(nested, "list");
  require(list.kind == YamlNode::Kind::Sequence && list.items.size() == 2 && list.items[1].scalar == "b",
          "a block sequence of scalars");
  const YamlNode& items = entry(root, "items");
  require(items.items.size() == 3, "three sequence items, one of them a block on the next line");
  require(entry(items.items[0], "name").scalar == "first" && entry(items.items[0], "extra").scalar == "x",
          "a dash-seeded mapping continues on the aligned lines");
  require(entry(items.items[2], "name").scalar == "third", "a bare dash starts a block item");
  require(items.items[0].line == 10 && items.items[1].line == 12, "sequence mappings record their lines");
  const YamlNode& flow = entry(root, "flow");
  require(flow.kind == YamlNode::Kind::Mapping && entry(flow, "a").scalar == "1", "a flow mapping");
  require(entry(flow, "b").kind == YamlNode::Kind::Sequence && entry(flow, "b").items[1].scalar == "y",
          "one level of flow nesting: a sequence inside a mapping");
  require(entry(flow, "c").scalar == "q", "a quoted scalar inside a flow mapping");
  require(entry(root, "seq").items.size() == 2 && entry(root, "empty_seq").items.empty(),
          "flow sequences, including an empty one");
}

void testScalars() {
  const YamlNode root = parse(
      "plain: hello world\n"
      "single: 'it''s # not a comment'\n"
      "double: \"tab\\there \\\"quoted\\\" back\\\\slash\\nnext\"\n"
      "block: |\n"
      "  first line\n"
      "\n"
      "    indented more\n"
      "  last\n"
      "\n"
      "after: done\n");
  require(entry(root, "plain").scalar == "hello world", "a plain scalar keeps interior spaces");
  require(entry(root, "single").scalar == "it's # not a comment", "'' escapes a quote; # inside quotes is text");
  require(entry(root, "double").scalar == "tab\there \"quoted\" back\\slash\nnext", "the four double-quote escapes");
  require(entry(root, "block").scalar == "first line\n\n  indented more\nlast\n",
          "a block literal keeps relative indentation and one trailing newline");
  require(entry(root, "after").scalar == "done", "parsing resumes after a block literal");
  require(contains(rejection("a: 'open\n"), "unterminated single-quoted scalar"), "unterminated single quote");
  require(contains(rejection("a: \"x\\q\"\n"), "unsupported escape"), "an escape outside the four");
  require(contains(rejection("a: \"x\\\n"), "unterminated double-quoted scalar"), "unterminated double quote");
  require(contains(rejection("a: |\nb: 1\n"), "block scalar '|' has no content"), "an empty block literal");
}

void testRejections() {
  require(contains(rejection(""), "ci workflow: the workflow file is empty (line 1)"),
          "the dialect names the file in the empty-document message");
  require(contains(rejection("  a: 1\n"), "the top level must not be indented"), "an indented top level");
  require(contains(rejection("a: 1\n\tb: 2\n"), "tabs may not be used for indentation (line 2)"),
          "tabs for indentation, with the line the reference document quotes");
  require(contains(rejection("a: 1\r\n"), "carriage returns are not allowed"), "CRLF line endings");
  require(contains(rejection("a: 1\na: 2\n"), "duplicate key 'a' (line 2)"), "a duplicate key");
  require(contains(rejection("a b: 1\n"), "a key has an unsupported character"), "a key with a space");
  require(contains(rejection("a: [1, 2\n"), "unterminated flow sequence"), "an unterminated flow sequence");
  require(contains(rejection("a: {x: 1\n"), "unterminated flow mapping"), "an unterminated flow mapping");
  require(contains(rejection("a: {x 1}\n"), "flow mapping entry is not key: value"), "a flow entry without a colon");
  require(contains(rejection("a: [x]]\n"), "unbalanced flow brackets"), "a closing bracket too many inside a flow");
  require(entry(parse("a: ]\n"), "a").scalar == "]", "a bare ']' outside any flow collection is a plain scalar");
  require(contains(rejection("a:\n"), "a value is missing where the file indents further"), "a key with nothing after it");
  require(contains(rejection("just text\n"), "expected a key: value entry"), "a line that is not an entry");
  require(entry(parse("-x: 1\n"), "-x").scalar == "1", "a dash without a space is a key character, not a sequence item");
  require(contains(rejection("-x\n"), "expected a key: value entry"), "a dash without a space or a colon is not an entry");
  require(contains(rejection("a: 1\n- b\n"), "unexpected content after the workflow"),
          "a top-level sequence after the mapping");
  require(contains(rejection(std::string("a: \x01\n")), "control character in a plain value"), "a control byte");
  require(contains(rejection("a: \"\xff\"\n"), "the file is not valid UTF-8"), "invalid UTF-8 is refused first");
  require(contains(rejection("a: 1\n  b: 2\n"), "unexpected content after the workflow"),
          "a deeper-indented line after a completed scalar entry");
}

void testHelpers() {
  const YamlNode root = parse("a: 1\nb: [x]\n");
  require(ckgit::yamlFindEntry(root, "zzz") == nullptr, "a missing key finds nothing");
  bool threw = false;
  try {
    ckgit::yamlRequireKind(ckgit::kCiWorkflowYamlDialect, entry(root, "a"), YamlNode::Kind::Sequence, "a list");
  } catch (const std::runtime_error& error) {
    threw = contains(error.what(), "ci workflow: expected a list (line 1)");
  }
  require(threw, "requireKind reports the expectation and the node's line");
  threw = false;
  try {
    ckgit::yamlRejectUnknownKeys(ckgit::kCiWorkflowYamlDialect, root, {"a"});
  } catch (const std::runtime_error& error) {
    threw = contains(error.what(), "unknown key 'b'");
  }
  require(threw, "unknown keys are named");
  ckgit::yamlRejectUnknownKeys(ckgit::kCiWorkflowYamlDialect, root, {"a", "b", "c"});
  const ckgit::YamlDialect docs{ckgit::kCiWorkflowYamlBounds, "ckdocs config", "the config"};
  bool named = false;
  try {
    ckgit::parseYamlSubset("", docs);
  } catch (const std::runtime_error& error) {
    named = std::string(error.what()) == "ckdocs config: the config file is empty (line 1)";
  }
  require(named, "another dialect names its own file in every message");
}

// The workflow dialect must keep applying exactly the limits ci_workflow.hpp
// documents; the parser's own bound checks are exercised below with a dialect
// small enough that each bound can be hit on its own.
static_assert(ckgit::kCiWorkflowYamlBounds.document_bytes == ckgit::kMaximumCiWorkflowBytes);
static_assert(ckgit::kCiWorkflowYamlBounds.line_bytes == ckgit::kMaximumCiLineBytes);
static_assert(ckgit::kCiWorkflowYamlBounds.lines == ckgit::kMaximumCiLines);
static_assert(ckgit::kCiWorkflowYamlBounds.key_bytes == ckgit::kMaximumCiKeyBytes);
static_assert(ckgit::kCiWorkflowYamlBounds.scalar_bytes == ckgit::kMaximumCiScalarBytes);
static_assert(ckgit::kCiWorkflowYamlBounds.block_scalar_bytes == ckgit::kMaximumCiScriptBytes);
static_assert(ckgit::kCiWorkflowYamlBounds.flow_sequence_items == ckgit::kMaximumCiArgvItems);
static_assert(ckgit::kCiWorkflowYamlBounds.block_sequence_items == ckgit::kMaximumCiStepsPerJob);
static_assert(ckgit::kCiWorkflowYamlBounds.block_sequence_items >= ckgit::kMaximumCiJobs);
static_assert(ckgit::kCiWorkflowYamlBounds.mapping_entries == ckgit::kMaximumCiEnvEntries + 8);
static_assert(ckgit::kCiWorkflowYamlBounds.nesting_depth == 16);
static_assert(ckgit::kCiWorkflowYamlDialect.prefix == "ci workflow");
static_assert(ckgit::kCiWorkflowYamlDialect.document == "the workflow");

constexpr ckgit::YamlBounds kTinyBounds{
    /*document_bytes=*/300,
    /*line_bytes=*/50,
    /*lines=*/20,
    /*key_bytes=*/8,
    /*scalar_bytes=*/20,
    /*block_scalar_bytes=*/40,
    /*flow_sequence_items=*/3,
    /*block_sequence_items=*/4,
    /*mapping_entries=*/5,
    /*nesting_depth=*/3,
};
constexpr ckgit::YamlDialect kTiny{kTinyBounds, "tiny", "the fixture"};

std::string tinyRejection(const std::string& text) { return rejection(text, kTiny); }

bool tinyTooLarge(const std::string& text, std::string_view message) {
  const std::string result = tinyRejection(text);
  return result.rfind("length_error: tiny: ", 0) == 0 && contains(result, message);
}

// One entry followed by comment lines, padded to exactly `size` bytes without
// any line exceeding `line_bytes` (the newline included).
std::string paddedDocument(std::size_t size, std::size_t line_bytes) {
  std::string text = "a: 1\n";
  while (text.size() < size) {
    const std::size_t room = size - text.size();
    const std::size_t width = room < line_bytes ? room : line_bytes;
    if (width < 2) {
      text += "\n";
    } else {
      text += "#" + std::string(width - 2, 'x') + "\n";
    }
  }
  return text;
}

void testBounds() {
  const ckgit::YamlBounds& bounds = kTinyBounds;
  // Document bytes, checked before anything else is looked at.
  require(tinyRejection(paddedDocument(bounds.document_bytes, bounds.line_bytes)).empty(),
          "a document at its byte limit");
  require(tinyTooLarge(paddedDocument(bounds.document_bytes + 1, bounds.line_bytes),
                       "the fixture file exceeds its size limit"),
          "one byte over the document limit");
  // Line bytes: a comment line, so no other bound is involved.
  require(tinyRejection("a: 1\n#" + std::string(bounds.line_bytes - 1, 'x') + "\n").empty(), "a line at its limit");
  require(tinyTooLarge("a: 1\n#" + std::string(bounds.line_bytes, 'x') + "\n", "a line exceeds its length limit"),
          "one byte over the line limit");
  // Line count: physical lines, blank ones included, plus the empty remainder
  // after a final newline.
  require(tinyRejection("a: 1\n" + std::string(bounds.lines - 2, '\n')).empty(), "lines at the limit");
  require(tinyTooLarge("a: 1\n" + std::string(bounds.lines - 1, '\n'), "the fixture has too many lines"),
          "one line over the line limit");
  // Key bytes (a malformed-input error, not a size error).
  require(tinyRejection(std::string(bounds.key_bytes, 'k') + ": 1\n").empty(), "a key at its limit");
  require(contains(tinyRejection(std::string(bounds.key_bytes + 1, 'k') + ": 1\n"), "a key is empty or too long"),
          "a key over its limit");
  // Scalar bytes, for each of the three scalar styles.
  require(tinyRejection("a: " + std::string(bounds.scalar_bytes, 'x') + "\n").empty(), "a plain scalar at its limit");
  require(tinyTooLarge("a: " + std::string(bounds.scalar_bytes + 1, 'x') + "\n", "a scalar exceeds its length limit"),
          "a plain scalar over its limit");
  require(tinyTooLarge("a: '" + std::string(bounds.scalar_bytes + 1, 'x') + "'\n", "a scalar exceeds its length limit"),
          "a single-quoted scalar over its limit");
  require(tinyTooLarge("a: \"" + std::string(bounds.scalar_bytes + 1, 'x') + "\"\n", "a scalar exceeds its length limit"),
          "a double-quoted scalar over its limit");
  require(tinyRejection("a: [" + std::string(bounds.scalar_bytes, 'x') + "]\n").empty(), "a flow item at its limit");
  require(tinyTooLarge("a: [" + std::string(bounds.scalar_bytes + 1, 'x') + "]\n", "a scalar exceeds its length limit"),
          "a flow item over its limit");
  // Block scalar bytes: the newline counts, so a 39-byte line fills 40 exactly.
  require(tinyRejection("a: |\n  " + std::string(bounds.block_scalar_bytes - 1, 'y') + "\n").empty(),
          "a block literal at its limit");
  require(tinyTooLarge("a: |\n  " + std::string(bounds.block_scalar_bytes, 'y') + "\n",
                       "a block scalar exceeds its length limit"),
          "a block literal over its limit");
  // Flow sequence items.
  std::string flow = "a: [";
  for (std::size_t index = 0; index < bounds.flow_sequence_items; ++index) flow += index ? ", x" : "x";
  require(tinyRejection(flow + "]\n").empty(), "a flow sequence at its limit");
  require(tinyTooLarge(flow + ", x]\n", "a sequence has too many items"), "a flow sequence over its limit");
  // Block sequence items.
  std::string block_seq = "a:\n";
  for (std::size_t index = 0; index < bounds.block_sequence_items; ++index) block_seq += "  - x\n";
  require(tinyRejection(block_seq).empty(), "a block sequence at its limit");
  require(tinyTooLarge(block_seq + "  - x\n", "a sequence has too many items"), "a block sequence over its limit");
  // Mapping entries, block and flow.
  std::string mapping;
  std::string flow_mapping = "m: {";
  for (std::size_t index = 0; index < bounds.mapping_entries; ++index) {
    mapping += "k" + std::to_string(index) + ": 1\n";
    flow_mapping += (index ? ", k" : "k") + std::to_string(index) + ": 1";
  }
  require(tinyRejection(mapping).empty(), "a block mapping at its limit");
  require(tinyTooLarge(mapping + "extra: 1\n", "a mapping has too many entries"), "a block mapping over its limit");
  require(tinyRejection(flow_mapping + "}\n").empty(), "a flow mapping at its limit");
  require(tinyTooLarge(flow_mapping + ", extra: 1}\n", "a mapping has too many entries"),
          "a flow mapping over its limit");
  // Nesting depth: the top-level mapping is level 1, so `nesting_depth`
  // mappings inside each other are allowed and one more is not.
  std::string deep;
  for (std::size_t level = 0; level < bounds.nesting_depth; ++level) deep += std::string(level * 2, ' ') + "n:\n";
  deep += std::string(bounds.nesting_depth * 2, ' ') + "leaf: 1\n";
  require(tinyTooLarge(deep, "the fixture is nested too deeply"), "one mapping past the nesting limit");
  std::string ok_deep;
  for (std::size_t level = 0; level + 1 < bounds.nesting_depth; ++level) ok_deep += std::string(level * 2, ' ') + "n:\n";
  ok_deep += std::string((bounds.nesting_depth - 1) * 2, ' ') + "leaf: 1\n";
  require(tinyRejection(ok_deep).empty(), "nesting at the limit is accepted");
  // A sequence nests too: a mapping inside a sequence inside a mapping inside
  // the root is four levels.
  require(tinyTooLarge("a:\n  b:\n    - c: 1\n", "the fixture is nested too deeply"), "a sequence counts as a level");
  require(tinyRejection("a:\n  - c: 1\n").empty(), "a sequence within the limit");
  // A flow mapping value is counted at its owner's level (the parser has always
  // done so); a flow mapping inside a flow mapping is one level deeper.
  require(tinyRejection("a:\n  b:\n    c: {d: 1}\n").empty(), "a flow mapping value sits at its owner's level");
  require(tinyTooLarge("a:\n  b:\n    c: {d: {e: 1}}\n", "the fixture is nested too deeply"),
          "a flow mapping inside a flow mapping is one level deeper");
}

}  // namespace

void testYamlSubset() {
  testStructures();
  testScalars();
  testRejections();
  testHelpers();
  testBounds();
}
