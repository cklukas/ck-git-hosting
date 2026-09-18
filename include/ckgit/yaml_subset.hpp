// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ckgit {

// The strict, bounded YAML subset shared by every configuration file this
// product reads from a repository (`.ckgit/ci.yml`, `ckdocs.yml`). It is
// deliberately small: block and flow mappings and sequences (one level of flow
// nesting), plain / single-quoted / double-quoted scalars with the escapes
// `\\ \" \n \t` only, `|` block literals, `#` comments, spaces-only
// indentation, LF line endings, UTF-8. No anchors, aliases, tags, multiple
// documents, or merge keys. Anything outside the subset is rejected rather
// than guessed; a documented bound that is exceeded is reported as such.

struct YamlEntry;

// A node of the parsed tree. A mapping keeps insertion order and never holds a
// duplicate key; a sequence keeps order; a scalar is already unquoted and
// unescaped. `line` is the 1-based physical line the node started on, for
// error messages that point a reader at the right place.
struct YamlNode {
  enum class Kind { Scalar, Sequence, Mapping };
  Kind kind = Kind::Scalar;
  std::string scalar;
  std::vector<YamlNode> items;
  std::vector<YamlEntry> entries;
  std::size_t line = 0;
};

// A mapping's one key/value pair. A plain aggregate, deliberately not
// std::pair<std::string, YamlNode>: clang (unlike gcc) eagerly evaluates
// std::pair's conditionally-explicit converting constructor against YamlNode
// while YamlNode is still being defined (entries is its own member), and
// rejects it as incomplete. Defining YamlEntry only after YamlNode is
// complete sidesteps that entirely.
struct YamlEntry {
  std::string key;
  YamlNode value;
};

// Every limit the parser enforces. Exceeding one throws std::length_error;
// any other malformed input throws std::runtime_error. Each file format picks
// its own numbers (see kCiWorkflowYamlDialect in ci_workflow.hpp).
struct YamlBounds {
  std::size_t document_bytes;       // the whole file
  std::size_t line_bytes;           // one physical line
  std::size_t lines;                // physical lines per file
  std::size_t key_bytes;            // one mapping key
  std::size_t scalar_bytes;         // one plain or quoted scalar
  std::size_t block_scalar_bytes;   // one `|` block literal
  std::size_t flow_sequence_items;  // items in `[a, b]`
  std::size_t block_sequence_items; // items in a `- a` / `- b` block
  std::size_t mapping_entries;      // entries in one mapping
  std::size_t nesting_depth;        // mappings/sequences nested inside each other
};

// What a file format calls itself in error messages: "<prefix>: <message>",
// and "<document> is nested too deeply", so a reader of the message knows
// which file is meant.
struct YamlDialect {
  YamlBounds bounds;
  std::string_view prefix;    // e.g. "ci workflow"
  std::string_view document;  // e.g. "the workflow"
};

// Parses one complete document, whose top level must be a mapping. Applies the
// document byte bound and the UTF-8 check before anything else.
YamlNode parseYamlSubset(std::string_view content, const YamlDialect& dialect);

// Helpers for interpreting a parsed tree against a schema, with the same error
// wording every format uses.
[[noreturn]] void yamlMalformed(const YamlDialect& dialect, const std::string& message, std::size_t line);
[[noreturn]] void yamlTooLarge(const YamlDialect& dialect, const std::string& message);
const YamlNode& yamlRequireKind(const YamlDialect& dialect, const YamlNode& node, YamlNode::Kind kind,
                                const char* what);
const YamlNode* yamlFindEntry(const YamlNode& mapping, std::string_view key);
void yamlRejectUnknownKeys(const YamlDialect& dialect, const YamlNode& mapping,
                           std::initializer_list<std::string_view> allowed);

}  // namespace ckgit
