// libcworks — shared utilities for CK Office
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The suite's YAML-subset reader/writer (originated in cplot, shared
// here). Supports the subset CK Office documents need: nested
// maps and
// lists via indentation, "- " list items (scalar or map), block lists
// at or below their mapping key's indentation (both standard styles),
// inline flow lists [a, b, c], quoted scalars, and # comments. Not
// supported: anchors, multi-line scalars, multiple documents, tabs
// for indentation. Errors carry file:line locations.
#pragma once

#include <functional>

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace cworks {

struct YamlNode {
    enum class Type { Null, Scalar, Map, List };

    /// One key/value entry of a map node. A dedicated struct (instead of
    /// std::pair) because pair requires complete types at instantiation,
    /// which clang rejects for this recursive node type. Members are named
    /// first/second so structured bindings and pair-style access work.
    struct Entry;

    Type type = Type::Null;
    std::string scalar;
    bool quoted = false;
    int line = 0;
    /// Exact half-open UTF-8 range of this node's written value. Nodes made
    /// programmatically have no source range. The parser retains this small
    /// bit of provenance so an embedding grammar can make a semantic edit
    /// without searching user text for a matching value.
    std::size_t source_begin = std::numeric_limits<std::size_t>::max();
    std::size_t source_end = std::numeric_limits<std::size_t>::max();
    std::vector<Entry> map;
    std::vector<YamlNode> list;

    bool is_null() const { return type == Type::Null; }
    bool is_scalar() const { return type == Type::Scalar; }
    bool is_map() const { return type == Type::Map; }
    bool is_list() const { return type == Type::List; }
    bool has_source_range() const {
        return source_begin != std::numeric_limits<std::size_t>::max() &&
               source_end != std::numeric_limits<std::size_t>::max() &&
               source_end >= source_begin;
    }

    const YamlNode* find(const std::string& key) const;
    YamlNode& insert(const std::string& key);

    std::string as_string() const { return scalar; }
    double as_double(bool* ok = nullptr) const;
    bool as_bool(bool* ok = nullptr) const;

    static YamlNode make_scalar(std::string value);
};

struct YamlNode::Entry {
    std::string first;
    YamlNode second;

    Entry() = default;
    Entry(std::string key, YamlNode value)
        : first(std::move(key)), second(std::move(value)) {}
};

inline const YamlNode* YamlNode::find(const std::string& key) const {
    for (const auto& [k, v] : map)
        if (k == key) return &v;
    return nullptr;
}

inline YamlNode& YamlNode::insert(const std::string& key) {
    for (auto& [k, v] : map)
        if (k == key) return v;
    map.emplace_back(key, YamlNode{});
    type = Type::Map;
    return map.back().second;
}

inline YamlNode YamlNode::make_scalar(std::string value) {
    YamlNode n;
    n.type = Type::Scalar;
    n.scalar = std::move(value);
    return n;
}

/// Parse YAML text. Throws cworks::Error with "file:line" on failure.
/// Optional synchronous checkpoints permit a caller to abort parsing by
/// throwing. Exceptions propagate unchanged; no partial tree is returned.
YamlNode parse_yaml(const std::string& text, const std::string& filename = "<config>",
                    const std::function<void()>& checkpoint = {});

/// Serialize a node back to YAML (canonical form; parse ∘ dump ∘ parse
/// is the identity for nodes produced by parse_yaml).
std::string dump_yaml(const YamlNode& node, int indent = 0);

/// One-line flow serialization (`{k: v, list: [a, b]}`) — for edit
/// fields and log lines where a node must fit on a single line.
/// parse_yaml reads the result back (flow maps and lists).
std::string dump_yaml_flow(const YamlNode& node);

/// Set a value at a dotted path like "axes.title" or "series.0.label".
/// Creates intermediate maps as needed; numeric segments index lists.
void set_path(YamlNode& root, const std::string& path, const std::string& value);

} // namespace cworks
