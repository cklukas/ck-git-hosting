// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Internal: the non-fatal diagnostics channel. The suite charter forbids
// silently coercing or dropping input, so a parser that meets a construct
// it recognises but does not render (an interaction directive, a diagram-
// level control it flattens) must SAY SO rather than skip in silence.
//
// A cdiagram::Error (an exception) is still the right tool for input that
// cannot be laid out at all; these helpers are for the recoverable case —
// the diagram renders, but one line was ignored, and `check()` (the
// surface `ckwrite check` calls) reports it. render() only records when a
// sink is attached (RenderOptions::diagnostics), so the normal render
// path pays nothing and stays allocation-free.
//
// cworks::Diagnostic carries only {severity, message}, so the source line
// is a string convention — "<type> (line N): <what>" — exactly matching
// the shape the throwing paths already use, so a message reads the same
// whether it arrived as an error or a diagnostic.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cworks/limits.hpp>
#include <cworks/text.hpp>

#include "cdiagram/render.hpp"

namespace cdiagram::detail {

/// "<type> (line N): <what>" — the suite's line-anchored message shape.
inline std::string at_line(std::string_view type, std::size_t line, std::string_view what) {
    return std::string(type) + " (line " + std::to_string(line) + "): " +
           std::string(what);
}

/// Append a diagnostic to the render sink, if one is attached. A no-op on
/// the normal render path (options.diagnostics == nullptr).
inline void diagnose(const RenderOptions& options, cworks::Diagnostic::Severity severity,
                     std::string message) {
    if (options.diagnostics)
        options.diagnostics->push_back({severity, std::move(message)});
}

/// Record that `keyword` on `line` is recognised Mermaid but not rendered
/// by this engine. `note` explains what happened ("interactions are not
/// rendered"); the diagram still lays out without it. Warning severity —
/// the output is usable but incomplete.
inline void diagnose_unsupported(const RenderOptions& options, std::string_view type,
                                 std::size_t line, std::string_view keyword,
                                 std::string_view note) {
    if (!options.diagnostics) return;
    std::string what = "'";
    what += keyword;
    what += "' is not supported and was ignored";
    if (!note.empty()) {
        what += " (";
        what += note;
        what += ")";
    }
    diagnose(options, cworks::Diagnostic::Severity::Warning, at_line(type, line, what));
}

/// Record a line the parser could not classify at all. Offers a
/// did-you-mean when `token` is a near-miss of a known keyword. Warning
/// severity: the line was skipped, the rest of the diagram still renders.
inline void diagnose_unrecognized(const RenderOptions& options, std::string_view type,
                                  std::size_t line, const std::string& token,
                                  const std::vector<std::string>& known) {
    if (!options.diagnostics) return;
    std::string what = "unrecognized statement '";
    what += token;
    what += "'";
    const std::string suggestion = cworks::closest_match(token, known);
    if (!suggestion.empty()) {
        what += " — did you mean '";
        what += suggestion;
        what += "'?";
    }
    what += " (ignored)";
    diagnose(options, cworks::Diagnostic::Severity::Warning, at_line(type, line, what));
}

} // namespace cdiagram::detail
