// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "theme.hpp"

namespace cplot {

struct TextMetrics {
    double width = 0.0;
    double height = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
};

/// Measures rendered text size. The layout engine depends only on this
/// interface; an accurate FreeType-backed implementation can be added
/// later without touching layout code.
class TextMeasurer {
public:
    virtual ~TextMeasurer() = default;
    virtual TextMetrics measure(std::string_view text, const Font& font) const = 0;
};

/// Deterministic measurement from compiled-in metrics: the exact
/// Adobe Helvetica/Helvetica-Bold advance widths for everything
/// WinAnsi covers (Latin incl. Latin-1), a per-class approximation
/// beyond that. Arial and Liberation Sans share Helvetica's widths
/// by design, so with the default font stack the reserved space
/// equals the rendered width in every backend — independent of the
/// fonts installed on the machine.
class ApproxTextMeasurer : public TextMeasurer {
public:
    TextMetrics measure(std::string_view text, const Font& font) const override;
};

const TextMeasurer& default_text_measurer();

// -- line breaking ---------------------------------------------------------------

/// How firmly a line may end at a position.
enum class BreakKind {
    /// The text itself ends the line here (a newline).
    Mandatory,
    /// A line may end here: after a space or a hyphen, between ideographs
    /// or kana, between an ideograph and a Latin word.
    Allowed,
    /// A line may end here only when no Allowed opportunity fits: between
    /// the syllables of a Korean word, which is written with spaces and
    /// broken at them by preference, and split inside a word only when a
    /// word alone is wider than the line.
    Fallback,
};

/// One place a line may end: before the code point at `position`, so the
/// line holds [line start, position) and the next line starts there. Spaces
/// before an opportunity belong to the line that ends; they are what the
/// line's measured width may leave out.
struct LineBreak {
    std::size_t position = 0;
    BreakKind kind = BreakKind::Allowed;

    friend bool operator==(const LineBreak&, const LineBreak&) = default;
};

/// The line-break opportunities of `text` (code points), ascending, the
/// end of the text excluded. The rules are the Unicode line-breaking
/// algorithm's (UAX #14) reduced to the classes this suite's scripts
/// meet — Latin, Greek and Cyrillic words, numbers, hyphens, the East
/// Asian ideographs, kana with its small forms and prolonged-sound mark,
/// the CJK opening and closing punctuation, Hangul — with one deliberate
/// departure named in BreakKind::Fallback. Stated once here so a chart
/// label, a typeset paragraph and a terminal line all break the same way.
std::vector<LineBreak> line_breaks(std::u32string_view text);

} // namespace cplot
