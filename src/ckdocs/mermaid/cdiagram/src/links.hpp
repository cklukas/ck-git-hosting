// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Internal: parsing of hyperlink directives, shared by every diagram type.
//
// The suite treats element links as a uniform, cross-diagram feature: any
// element in any diagram may carry ONE link. Mermaid's own link syntaxes are a
// subset we keep working —
//   * flowchart:  click <ref> "url" ["tooltip"] [_target]
//                 click <ref> href "url" ["tooltip"] [_target]
//   * class:      link <ref> "url" ["tooltip"]  /  callback <ref> "name" ["tt"]
//                 click <ref> href "url" ["tooltip"]
//   * sequence:   link <actor>: <label> @ <url>
//                 links <actor>: {"label": "url", ...}   (first pair; no menus)
// — and the universal `click <ref> ...` form is offered to every other diagram
// type so links compose everywhere.
//
// A parser fills a LinkTable (ref -> link) while reading statements, then at
// draw time looks each element's ref up and calls Canvas::add_link with the
// element's rectangle. JavaScript callback forms carry no URL and are reported
// as unsupported (there is no runtime in a static SVG/PDF), never linked.
#pragma once

#include <map>
#include <string>
#include <string_view>

namespace cdiagram::detail {

/// One parsed link directive: the URL plus optional tooltip and target. A
/// callback form (a JS function, not a URL) sets `callback` and leaves `href`
/// empty — such directives are recognised but not exportable.
struct LinkDirective {
    std::string href;
    std::string title;
    std::string target;
    bool callback = false;
};

/// Element ref -> link, filled during parse and consulted at draw time.
using LinkTable = std::map<std::string, LinkDirective>;

/// True when `line` is a `click …` interaction statement.
bool is_click_statement(std::string_view line);

/// Parse `click <ref> …` (the universal link form). On success sets `ref` to the
/// element identifier and `out` to the link (or a callback marker) and returns
/// true. Returns false when the line is not a well-formed click statement.
bool parse_click_statement(std::string_view line, std::string& ref, LinkDirective& out);

/// Parse a class-diagram `link <ref> "url" ["tt"]` or `callback <ref> "name"
/// ["tt"]` statement. Returns false for any other line (including `link …@…`,
/// which is sequence syntax).
bool parse_class_link_statement(std::string_view line, std::string& ref, LinkDirective& out);

/// Parse a sequence-diagram `link <actor>: <label> @ <url>` or
/// `links <actor>: {"label":"url", …}` statement (the first pair only — the
/// suite assigns at most one link per element and renders no popup menu).
bool parse_sequence_link_statement(std::string_view line, std::string& ref, LinkDirective& out);

} // namespace cdiagram::detail
