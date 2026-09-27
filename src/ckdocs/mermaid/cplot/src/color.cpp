// ckplot — colour formatting and the shared CSS colour parser
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cplot/color.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

#include <cworks/format.hpp>
#include <cworks/text.hpp>

namespace cplot {

namespace {

/// Hex digits → value; the caller has validated the character set.
std::uint32_t hex_value(std::string_view hex) {
    std::uint32_t v = 0;
    for (const char c : hex) {
        const int d = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
        v = (v << 4) | static_cast<std::uint32_t>(d);
    }
    return v;
}

/// `rgb(r, g, b)` / `rgba(r, g, b, a)` with channels 0–255 and alpha 0–1.
std::optional<Color> parse_rgb_function(std::string_view s) {
    const bool rgba = s.rfind("rgba", 0) == 0;
    const std::size_t open = s.find('(');
    if (open == std::string_view::npos || s.back() != ')') return std::nullopt;
    std::vector<double> parts;
    std::string current;
    for (std::size_t i = open + 1; i + 1 < s.size(); ++i) {
        if (s[i] == ',') {
            double v = 0.0;
            if (!cworks::parse_double(cworks::trim(current), v)) return std::nullopt;
            parts.push_back(v);
            current.clear();
        } else {
            current.push_back(s[i]);
        }
    }
    double v = 0.0;
    if (!cworks::parse_double(cworks::trim(current), v)) return std::nullopt;
    parts.push_back(v);
    if (parts.size() != (rgba ? 4u : 3u)) return std::nullopt;
    for (std::size_t c = 0; c < 3; ++c)
        if (parts[c] < 0.0 || parts[c] > 255.0) return std::nullopt;
    const double alpha = rgba ? parts[3] : 1.0;
    if (alpha < 0.0 || alpha > 1.0) return std::nullopt;
    const auto channel = [](double value) {
        return static_cast<std::uint32_t>(value + 0.5);
    };
    return Color::rgb((channel(parts[0]) << 16) | (channel(parts[1]) << 8) | channel(parts[2]),
                      alpha);
}

} // namespace

std::string Color::hex() const {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    return buf;
}

std::optional<Color> parse_css_color(std::string_view raw) {
    const std::string s = cworks::trim(std::string(raw));
    if (s.size() >= 2 && s[0] == '#') {
        std::string hex = s.substr(1);
        if (hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            return std::nullopt;
        if (hex.size() == 3 || hex.size() == 4) { // #rgb / #rgba → doubled
            std::string expanded;
            for (const char c : hex) {
                expanded += c;
                expanded += c;
            }
            hex = expanded;
        }
        if (hex.size() == 6) return Color::rgb(hex_value(hex));
        if (hex.size() == 8)
            return Color::rgb(hex_value(std::string_view(hex).substr(0, 6)),
                              static_cast<double>(hex_value(std::string_view(hex).substr(6))) /
                                  255.0);
        return std::nullopt;
    }
    if (s.rfind("rgb", 0) == 0) return parse_rgb_function(s);
    // The CSS Color Level 4 named colours, in full. A parser that knows
    // nine of them turns every other one into a refusal, which is why the
    // suite has exactly one table and it is this one.
    static const std::map<std::string, std::uint32_t> names = {
        {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF},
        {"aquamarine", 0x7FFFD4}, {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC},
        {"bisque", 0xFFE4C4}, {"black", 0x000000}, {"blanchedalmond", 0xFFEBCD},
        {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
        {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00},
        {"chocolate", 0xD2691E}, {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED},
        {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C}, {"cyan", 0x00FFFF},
        {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
        {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9},
        {"darkkhaki", 0xBDB76B}, {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F},
        {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC}, {"darkred", 0x8B0000},
        {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
        {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F},
        {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3}, {"deeppink", 0xFF1493},
        {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
        {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0},
        {"forestgreen", 0x228B22}, {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC},
        {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700}, {"goldenrod", 0xDAA520},
        {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F},
        {"grey", 0x808080}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4},
        {"indianred", 0xCD5C5C}, {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0},
        {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA}, {"lavenderblush", 0xFFF0F5},
        {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6},
        {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF},
        {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
        {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1},
        {"lightsalmon", 0xFFA07A}, {"lightseagreen", 0x20B2AA},
        {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899},
        {"lightslategrey", 0x778899}, {"lightsteelblue", 0xB0C4DE},
        {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
        {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000},
        {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD},
        {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB},
        {"mediumseagreen", 0x3CB371}, {"mediumslateblue", 0x7B68EE},
        {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
        {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970},
        {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1}, {"moccasin", 0xFFE4B5},
        {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6},
        {"olive", 0x808000}, {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500},
        {"orangered", 0xFF4500}, {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA},
        {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE},
        {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9},
        {"peru", 0xCD853F}, {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD},
        {"powderblue", 0xB0E0E6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399},
        {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1},
        {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460},
        {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D},
        {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD},
        {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA},
        {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C},
        {"teal", 0x008080}, {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347},
        {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3},
        {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00},
        {"yellowgreen", 0x9ACD32},
    };
    std::string lower = s;
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const auto it = names.find(lower);
    if (it == names.end()) return std::nullopt;
    return Color::rgb(it->second);
}

} // namespace cplot
