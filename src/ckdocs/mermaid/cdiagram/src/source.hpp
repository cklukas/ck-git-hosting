// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Internal: turning raw Mermaid text into the significant lines every
// parser walks. Line endings are normalised, an optional leading YAML
// front-matter block (`---` … `---`) is stripped, `%%` line comments
// and blank lines are dropped, and each remaining line is trimmed and
// tagged with its 1-based source number for error messages.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cdiagram::detail {

/// One significant source line: its trimmed text and 1-based line
/// number in the original source (for diagnostics).
struct Line {
    std::string text;
    std::size_t number = 0;
};

/// Split `source` into significant lines: LF/CRLF normalised, a leading
/// `---`…`---` front-matter block removed, `%%`-comment and blank lines
/// dropped, each remaining line trimmed. `indent` of the original line
/// is preserved separately for parsers that need it (mindmap, etc.).
struct SourceLine {
    std::string text;    ///< trimmed content
    std::size_t number;  ///< 1-based line number
    std::size_t indent;  ///< leading space/tab count before trimming
};

std::vector<SourceLine> significant_lines(std::string_view source);

/// The raw text of a leading YAML front-matter block ("---" … "---"),
/// without the fence lines; empty when the source has no front matter.
/// Mermaid keeps per-diagram configuration here (e.g. the sankey
/// nodeAlignment); builders parse it on demand.
std::string frontmatter(std::string_view source);

/// Remove a single pair of surrounding double quotes if present, else
/// return the input unchanged.
std::string unquote(std::string_view text);

/// Parse a strict decimal number (cworks rules). Returns false when the
/// whole string is not a number.
bool parse_number(std::string_view text, double& out);

/// Split `text` on literal "\n", "<br>", "<br/>" and "<br />" into
/// display lines (Mermaid's label line breaks). Never returns empty:
/// an empty input yields one empty line.
std::vector<std::string> label_lines(std::string_view text);

} // namespace cdiagram::detail
