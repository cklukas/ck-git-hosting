// ckdiagram — shared Mermaid style-directive vocabulary
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The `style` / `classDef` / `class` / `:::` directives share one
// property vocabulary across every diagram type that supports them
// (flowchart, class, state, ER, requirement, block, quadrant). This
// module owns that vocabulary once: the CSS colour parser and the
// comma-separated `key:value` property-list parser. Builders keep their
// own statement grammar and apply the parsed properties to their model.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cplot/color.hpp>
#include <cplot/scene.hpp>

#include "cdiagram/render.hpp"

namespace cdiagram::detail {

/// Parsed style properties (each optional so overlays merge last-wins).
struct StyleProps {
    std::optional<cplot::Color> fill;
    std::optional<cplot::Color> stroke;
    std::optional<cplot::Color> text;  ///< the CSS `color:` property
    std::optional<double> stroke_width;
    std::optional<cplot::DashPattern> dash;  ///< SVG stroke-dasharray
    /// Layer `o` on top of this (last-wins per property).
    void merge(const StyleProps& o) {
        if (o.fill) fill = o.fill;
        if (o.stroke) stroke = o.stroke;
        if (o.text) text = o.text;
        if (o.stroke_width) stroke_width = o.stroke_width;
        if (o.dash) dash = o.dash;
    }
};

/// Parse a comma-separated `key:value` style list (fill / stroke / color /
/// stroke-width / stroke-dasharray). Unparseable colours and unknown keys
/// are reported as diagnostics (never silently dropped) and skipped.
StyleProps parse_style_props(std::string_view text, const RenderOptions& options,
                             std::string_view what, std::size_t line);

/// The classDef registry plus per-element assignments — one resolution
/// model for the `classDef` / `class` / `:::` / `style` directives, shared
/// by the id-keyed builders (class, state, ER, requirement, block; the
/// quadrant chart keeps its own point vocabulary and only shares the
/// colour parser). Resolution order matches Mermaid: the `default` class
/// first, then assigned classes in application order, then the direct
/// `style` override (last wins per property).
class StyleSheet {
public:
    void define(const std::string& name, const StyleProps& props);   ///< classDef
    void assign(const std::string& element, std::string cls);        ///< class / :::
    void style(const std::string& element, const StyleProps& props); ///< style
    StyleProps resolve(const std::string& element) const;

private:
    std::map<std::string, StyleProps> defs_;
    std::map<std::string, std::vector<std::string>> classes_;
    std::map<std::string, StyleProps> direct_;
};

/// Split a comma-separated list, trimming each entry (the id lists of
/// `classDef a,b` / `class x,y name` / `linkStyle 0,2`).
std::vector<std::string> split_commas(std::string_view text);

/// Split a style property list at commas OUTSIDE parentheses, so
/// `fill:rgb(1,2,3),stroke:#000` yields two entries. Entries are trimmed;
/// empties are dropped.
std::vector<std::string> split_style_entries(std::string_view text);

/// Parse one of the three shared styling statements when `line` is one:
///   classDef <name[,name]> k:v,...
///   <assign_keyword> <id[,id]> <class>     (either `class` or `cssClass`)
///   style <id> k:v,...
/// Records the result in `sheet` and returns true; returns false when the
/// line is none of these. Malformed statements are diagnosed and consumed
/// (returns true) — they must never fall through to a builder's node
/// grammar and fabricate elements.
bool parse_style_statement(const std::string& line, StyleSheet& sheet,
                           const RenderOptions& options, std::string_view what,
                           std::size_t number, const char* assign_keyword = "class");

/// Strip a trailing `:::class` annotation from an element token, recording
/// the assignment in `sheet` under the stripped identifier. Repeatable
/// (`Foo:::a:::b`); returns the bare element identifier.
std::string take_class_annotations(std::string token, StyleSheet& sheet);

/// As above, but collects the class names instead of assigning them — for
/// callers whose element key differs from the stripped token (ER aliases:
/// `c[Customer]:::big` styles the entity id `c`).
std::string take_class_annotations(std::string token, std::vector<std::string>& classes);

} // namespace cdiagram::detail
