// ckplot — internal shared font metrics
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "font_metrics.hpp"

namespace cplot::detail {

namespace {

// -- Helvetica metrics (Adobe AFM, per mille of the font size) ---------------

/// ASCII 0x20..0x7E for Helvetica (regular/oblique).
constexpr int kHelveticaWidths[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,
    1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,
    333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};

/// ASCII 0x20..0x7E for Helvetica-Bold (bold/bold-oblique).
constexpr int kHelveticaBoldWidths[95] = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611,
    975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556,
    333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
    611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584};

/// Accented Latin-1 letters share the width of their base letter in
/// Helvetica (an Adobe design rule), so the 0xA0..0xFF range reduces
/// to base-letter lookups plus a handful of symbols.
int latin1_width(unsigned char code, bool bold) {
    const auto ascii = [&](char c) {
        return (bold ? kHelveticaBoldWidths : kHelveticaWidths)[c - 0x20];
    };
    switch (code) {
    case 0xA0: return ascii(' ');  // no-break space
    case 0xA1: return ascii('!');  // ¡
    case 0xA2: case 0xA3: case 0xA5: return ascii('$'); // ¢ £ ¥
    case 0xA6: return ascii('|');
    case 0xA7: return 556;         // §
    case 0xA9: case 0xAE: return 737; // © ®
    case 0xAB: case 0xBB: return 556; // « »
    case 0xAC: case 0xB1: case 0xD7: case 0xF7: return 584; // ¬ ± × ÷
    case 0xB0: return 400;         // °
    case 0xB5: return bold ? 611 : 556; // µ
    case 0xB7: return ascii('.');  // ·
    case 0xBF: return 611;         // ¿
    case 0xC6: return 1000;        // Æ
    case 0xD0: return ascii('D');  // Ð
    case 0xDE: return ascii('P');  // Þ
    case 0xDF: return 611;         // ß
    case 0xE6: return 889;         // æ
    case 0xF0: return ascii('o');  // ð
    case 0xFE: return ascii('b');  // þ
    default: break;
    }
    // Accented letters → base letter.
    static constexpr const char* base_upper = "AAAAAA?CEEEEIIII?NOOOOO?OUUUUY";
    static constexpr const char* base_lower = "aaaaaa?ceeeeiiii?nooooo?ouuuuy";
    if (code >= 0xC0 && code <= 0xDD) {
        const char b = base_upper[code - 0xC0];
        if (b != '?') return ascii(b);
    }
    if (code >= 0xE0 && code <= 0xFD) {
        const char b = base_lower[code - 0xE0];
        if (b != '?') return ascii(b);
    }
    if (code == 0xFF) return ascii('y'); // ÿ
    return 556;
}

} // namespace

unsigned char winansi_code(char32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E) return static_cast<unsigned char>(cp);
    if (cp >= 0xA0 && cp <= 0xFF) return static_cast<unsigned char>(cp);
    switch (cp) {
    case 0x20AC: return 0x80; // €
    case 0x2018: return 0x91;
    case 0x2019: return 0x92;
    case 0x201C: return 0x93;
    case 0x201D: return 0x94;
    case 0x2022: return 0x95; // •
    case 0x2013: return 0x96; // –
    case 0x2014: return 0x97; // —
    case 0x2026: return 0x85; // …
    case 0x2030: return 0x89; // ‰
    default: return 0;
    }
}

int winansi_width_millis(unsigned char code, bool bold) {
    if (code >= 0x20 && code <= 0x7E)
        return (bold ? kHelveticaBoldWidths : kHelveticaWidths)[code - 0x20];
    if (code >= 0xA0) return latin1_width(code, bold);
    // 0x80..0x9F WinAnsi extras (€ quotes dashes …) — the common ones.
    switch (code) {
    case 0x80: return 556;                    // €
    case 0x91: case 0x92: return bold ? 278 : 222; // ‘ ’
    case 0x93: case 0x94: return bold ? 500 : 333; // “ ”
    case 0x95: return 350;                    // •
    case 0x96: return 556;                    // –
    case 0x97: return 1000;                   // —
    case 0x85: return 1000;                   // …
    case 0x89: return 1000;                   // ‰
    default: return 556;
    }
}

int helvetica_advance_millis(char32_t cp, bool bold) {
    const unsigned char code = winansi_code(cp);
    if (code == 0) return -1;
    return winansi_width_millis(code, bold);
}

} // namespace cplot::detail
