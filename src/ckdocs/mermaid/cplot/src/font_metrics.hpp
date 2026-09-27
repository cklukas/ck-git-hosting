// ckplot — internal shared font metrics (not installed)
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The exact Adobe AFM advance widths of Helvetica and Helvetica-Bold
// over the WinAnsi character set — cplot's ONE deterministic metric
// system. The layout measurer, the PDF backend, and (via the
// metric-compatible font preference: Arial ≡ Helvetica ≡ Liberation
// Sans by design) the raster backend all agree on these numbers, so
// reserved boxes fit the rendered text exactly, on every machine,
// without consulting font files.
#pragma once

namespace cplot::detail {

/// WinAnsi (cp1252) code for a Unicode codepoint; 0 when unmappable.
unsigned char winansi_code(char32_t cp);

/// Helvetica advance width of a WinAnsi code, in 1/1000 of the font
/// size (Adobe AFM values; bold selects Helvetica-Bold).
int winansi_width_millis(unsigned char code, bool bold);

/// Helvetica advance width of a Unicode codepoint, per mille of the
/// font size; -1 when the codepoint is outside WinAnsi coverage (the
/// caller falls back to its own heuristic).
int helvetica_advance_millis(char32_t cp, bool bold);

} // namespace cplot::detail
