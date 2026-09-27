// libcworks — shared utilities for CK Office
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cworks {

/// Strict Unicode scalar UTF-8 validation: rejects overlong sequences,
/// surrogates, truncated sequences, and code points above U+10FFFF.
bool valid_utf8(std::string_view text) noexcept;

/// Suggestion helper for error messages ("did you mean ...?").
/// Returns the entry of `candidates` closest to `needle` by edit
/// distance, or an empty string when nothing is reasonably close
/// (distance greater than max(2, |needle|/3)). Equally close entries
/// are ranked by the length of the prefix they share with `needle`
/// (`sn` suggests `sin` over `ln`), then by their order in
/// `candidates`.
std::string closest_match(const std::string& needle,
                          const std::vector<std::string>& candidates);

/// Trim ASCII whitespace (space, tab, CR) from both ends.
std::string trim(const std::string& text);

/// Number of Unicode code points in UTF-8 `text` (continuation bytes
/// are not counted; malformed sequences count their lead bytes) —
/// the suite's user-facing notion of text length (cwrite's layout,
/// cbase's form length rules).
std::size_t code_points(std::string_view text);

/// Strip control bytes so untrusted text is safe to print to a terminal.
/// Drops every C0 control byte (0x00–0x1F) and DEL (0x7F) — which removes
/// the ESC that introduces every CSI/OSC/DCS escape sequence (window-title
/// spoofing, OSC-52 clipboard writes, cursor moves) as well as CR/BS/BEL/NUL
/// that corrupt a line. Printable ASCII and all UTF-8 multibyte bytes
/// (>= 0x80) pass through unchanged. Note this also removes TAB and newline:
/// it is meant for single-line, terminal-bound cells (e.g. the `--to pretty`
/// table), where an embedded newline or tab would break the layout anyway.
/// Deterministic and locale-free.
std::string sanitize_terminal(std::string_view text);

/// Percent-encode a URL query-component value (RFC 3986). The unreserved
/// set `A-Z a-z 0-9 - . _ ~` passes through; every other byte becomes
/// `%HH` (uppercase hex). A space becomes `%20`, not `+`. Use it for a
/// query-string parameter value; a path-segment value that must keep `/`
/// should not be encoded. Deterministic and locale-free.
std::string percent_encode(std::string_view value);

} // namespace cworks
