// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cplot/text.hpp"

#include <cstdint>

#include "font_metrics.hpp"

namespace cplot {

namespace {

/// Advance width of one codepoint as a fraction of the font size:
/// the EXACT Adobe Helvetica/Helvetica-Bold metrics for everything
/// WinAnsi covers (Latin incl. Latin-1), a heuristic for the rest.
/// Arial and Liberation Sans are metric-compatible with Helvetica by
/// design, so with the default font stack the layout's reserved
/// space equals the rendered width in every backend.
double char_width_factor(char32_t cp, bool bold) {
    const int millis = detail::helvetica_advance_millis(cp, bold);
    if (millis >= 0) return millis / 1000.0;
    if (cp >= 0x2E80) return 1.0; // CJK and other full-width ranges
    return 0.56;
}

/// Decode one UTF-8 codepoint; advances i.
char32_t next_codepoint(std::string_view s, std::size_t& i) {
    const auto b0 = static_cast<std::uint8_t>(s[i]);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    int extra = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1F;
        extra = 1;
    } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0F;
        extra = 2;
    } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07;
        extra = 3;
    } else {
        ++i;
        return 0xFFFD;
    }
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) {
        cp = (cp << 6) | (static_cast<std::uint8_t>(s[i]) & 0x3F);
    }
    return cp;
}

} // namespace

TextMetrics ApproxTextMeasurer::measure(std::string_view text, const Font& font) const {
    const bool bold = font.weight == FontWeight::Bold;
    double width = 0.0;
    std::size_t i = 0;
    while (i < text.size()) {
        width += char_width_factor(next_codepoint(text, i), bold);
    }
    width *= font.size;

    TextMetrics m;
    m.width = width;
    m.ascent = 0.74 * font.size;
    m.descent = 0.26 * font.size;
    m.height = font.size;
    return m;
}

const TextMeasurer& default_text_measurer() {
    static const ApproxTextMeasurer measurer;
    return measurer;
}

// -- line breaking ---------------------------------------------------------------

namespace {

/// The line-breaking classes this rule distinguishes — UAX #14's, reduced
/// to what the suite's scripts meet. Every code point is in exactly one.
enum class LbClass : unsigned char {
    BK,  ///< mandatory break after (newline, paragraph separator)
    CR,  ///< carriage return: a break after, unless a LF follows
    SP,  ///< space: a break opportunity after the run of spaces
    ZW,  ///< zero-width space: a break opportunity, invisible
    GL,  ///< glue: no break on either side (no-break space, word joiner)
    HY,  ///< hyphen: a break after, when what follows is not a number
    BA,  ///< break after (en dash, soft hyphen)
    OP,  ///< opening punctuation: no break after
    CP,  ///< closing parenthesis: no break before, and none after before a word
    CL,  ///< closing punctuation: no break before; a line may end after it
    IS,  ///< infix separator (`.` `,` `:` `;`): holds inside `3.14`, `e.g.`
    NS,  ///< nonstarter: no break before (small kana, prolonged sound)
    NU,  ///< numeric
    AL,  ///< alphabetic — the word-forming class of Latin, Greek, Cyrillic
    ID,  ///< ideographic: a break on either side (CJK ideographs, kana)
    H2,  ///< Hangul syllable: a break on either side, but see BreakKind::Fallback
};

bool in(char32_t cp, char32_t lo, char32_t hi) { return cp >= lo && cp <= hi; }

LbClass classify(char32_t cp) {
    switch (cp) {
    case 0x000A: case 0x000B: case 0x000C: case 0x0085: case 0x2028: case 0x2029:
        return LbClass::BK;
    case 0x000D:
        return LbClass::CR;
    case 0x0009: case 0x0020: case 0x3000:
        return LbClass::SP;
    case 0x200B:
        return LbClass::ZW;
    case 0x00A0: case 0x2007: case 0x202F: case 0x2060: case 0xFEFF:
        return LbClass::GL;
    case 0x002D: case 0x2010:
        return LbClass::HY;
    case 0x00AD: case 0x2012: case 0x2013: case 0x2014:
        return LbClass::BA;
    // Opening punctuation: ASCII, and the East Asian brackets and quotes.
    case 0x0028: case 0x005B: case 0x007B:
    case 0x3008: case 0x300A: case 0x300C: case 0x300E: case 0x3010: case 0x3014:
    case 0x3016: case 0x3018: case 0x301A: case 0x301D:
    case 0xFF08: case 0xFF3B: case 0xFF5B: case 0xFF5F: case 0xFF62:
        return LbClass::OP;
    // Closing parentheses: no line begins with one, and none ends between
    // one and the word that follows (`(a)b`).
    case 0x0029: case 0x005D: case 0x007D: case 0xFF09: case 0xFF3D: case 0xFF5D:
    case 0xFF60: case 0xFF63:
        return LbClass::CP;
    // Infix separators: a number or an abbreviation is one word.
    case 0x002C: case 0x002E: case 0x003A: case 0x003B:
        return LbClass::IS;
    // Closing punctuation and the marks no line begins with.
    case 0x0021: case 0x003F:
    case 0x3001: case 0x3002: case 0x3009: case 0x300B: case 0x300D: case 0x300F:
    case 0x3011: case 0x3015: case 0x3017: case 0x3019: case 0x301B: case 0x301E:
    case 0x301F:
    case 0xFF01: case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B: case 0xFF1F:
    case 0xFF64: case 0xFF61:
        return LbClass::CL;
    // Nonstarters: the small kana, the prolonged sound mark, the
    // iteration marks — a line never begins with one.
    case 0x3005: case 0x303B: case 0x309D: case 0x309E: case 0x30FC: case 0x30FD:
    case 0x30FE:
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063:
    case 0x3083: case 0x3085: case 0x3087: case 0x308E: case 0x3095: case 0x3096:
    case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9: case 0x30C3:
    case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE: case 0x30F5: case 0x30F6:
    case 0xFF67: case 0xFF68: case 0xFF69: case 0xFF6A: case 0xFF6B: case 0xFF6C:
    case 0xFF6D: case 0xFF6E: case 0xFF6F: case 0xFF70:
        return LbClass::NS;
    default:
        break;
    }
    if (in(cp, '0', '9') || in(cp, 0xFF10, 0xFF19)) return LbClass::NU;
    if (in(cp, 0xAC00, 0xD7A3)) return LbClass::H2;
    // Ideographs and kana: the unified blocks, their extensions, the
    // compatibility ideographs, the kana blocks and the fullwidth forms.
    if (in(cp, 0x3040, 0x30FF) || in(cp, 0x31F0, 0x31FF) || in(cp, 0x3400, 0x4DBF) ||
        in(cp, 0x4E00, 0x9FFF) || in(cp, 0xF900, 0xFAFF) || in(cp, 0x20000, 0x3134F) ||
        in(cp, 0x3100, 0x312F) || in(cp, 0x31A0, 0x31BF) || in(cp, 0x3030, 0x303F) ||
        in(cp, 0xFF21, 0xFF3A) || in(cp, 0xFF41, 0xFF5A) || in(cp, 0xFF66, 0xFF9F) ||
        in(cp, 0x2E80, 0x2FDF) || in(cp, 0x3190, 0x319F) || in(cp, 0x3200, 0x33FF))
        return LbClass::ID;
    return LbClass::AL;
}

}  // namespace

std::vector<LineBreak> line_breaks(std::u32string_view text) {
    std::vector<LineBreak> out;
    const std::size_t n = text.size();
    if (n < 2) return out;
    std::vector<LbClass> cls(n);
    for (std::size_t i = 0; i < n; ++i) cls[i] = classify(text[i]);

    for (std::size_t i = 1; i < n; ++i) {
        const LbClass after = cls[i];
        // The class before the position, looking past the spaces that end
        // the line: `word  次` breaks after the spaces, and what decides
        // whether it may is the word, not the space.
        std::size_t j = i;
        while (j > 0 && cls[j - 1] == LbClass::SP) --j;
        const bool spaces_before = j < i;
        const LbClass before = cls[j == 0 ? 0 : j - 1];

        // LB4–LB6: a break is mandatory after a line terminator; a CR LF
        // pair is one terminator, so nothing breaks between them.
        if (cls[i - 1] == LbClass::BK || (cls[i - 1] == LbClass::CR && after != LbClass::BK)) {
            out.push_back({i, BreakKind::Mandatory});
            continue;
        }
        if (cls[i - 1] == LbClass::CR) continue;
        // LB7: no break before a space or a zero-width space; the
        // opportunity is after the run.
        if (after == LbClass::SP || after == LbClass::ZW) continue;
        // LB8: a zero-width space breaks after it, whatever follows.
        if (cls[i - 1] == LbClass::ZW) {
            out.push_back({i, BreakKind::Allowed});
            continue;
        }
        // LB12: glue on either side holds.
        if (after == LbClass::GL || (!spaces_before && before == LbClass::GL)) continue;
        // LB13: no break before closing punctuation or a nonstarter.
        if (after == LbClass::CL || after == LbClass::CP || after == LbClass::NS ||
            after == LbClass::IS)
            continue;
        // LB14: no break after opening punctuation, spaces or not.
        if (before == LbClass::OP) continue;
        // LB18: a break after spaces, for everything that reached here.
        if (spaces_before) {
            out.push_back({i, BreakKind::Allowed});
            continue;
        }
        // LB21: a hyphen or a dash breaks after it — except a hyphen
        // before a number, which is a minus sign.
        if (before == LbClass::BA || (before == LbClass::HY && after != LbClass::NU)) {
            out.push_back({i, BreakKind::Allowed});
            continue;
        }
        if (before == LbClass::HY) continue;
        // No break before a hyphen or a dash: they end a line, they do not
        // begin one.
        if (after == LbClass::HY || after == LbClass::BA) continue;
        // A sentence mark ends a line freely (`文。「章」` may break after
        // the 。); a closing parenthesis holds to the word after it (LB30)
        // and breaks only before an ideograph or another bracket.
        if (before == LbClass::CL) {
            out.push_back({i, BreakKind::Allowed});
            continue;
        }
        if (before == LbClass::CP || before == LbClass::IS) {
            if (after == LbClass::AL || after == LbClass::NU) continue;
            out.push_back({i, BreakKind::Allowed});
            continue;
        }
        // Ideographs and kana break freely with one another and with a
        // word or a number on either side (LB30 lets an ideograph stand
        // next to a Latin word across a line end).
        const bool before_ideo = before == LbClass::ID;
        const bool after_ideo = after == LbClass::ID;
        if (before_ideo || after_ideo) {
            if (before == LbClass::OP) continue;
            out.push_back({i, BreakKind::Allowed});
            continue;
        }
        // Hangul: a syllable boundary is a last resort inside a word
        // (BreakKind::Fallback); next to a Latin word or a number it is an
        // ordinary opportunity, as an ideograph's is.
        const bool before_hangul = before == LbClass::H2;
        const bool after_hangul = after == LbClass::H2;
        if (before_hangul && after_hangul) {
            out.push_back({i, BreakKind::Fallback});
            continue;
        }
        if (before_hangul || after_hangul) {
            out.push_back({i, BreakKind::Allowed});
            continue;
        }
        // Letters, numbers and the closing marks between them hold
        // together: `3.14`, `e.g.`, `a)b` — no break inside a word.
    }
    return out;
}

} // namespace cplot
