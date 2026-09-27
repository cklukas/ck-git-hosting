// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cplot {

/// RGBA color. Components are in [0, 255], alpha in [0, 1].
struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    double a = 1.0;

    constexpr Color() = default;
    constexpr Color(std::uint8_t red, std::uint8_t green, std::uint8_t blue, double alpha = 1.0)
        : r(red), g(green), b(blue), a(alpha) {}

    /// Construct from 0xRRGGBB.
    static constexpr Color rgb(std::uint32_t hex, double alpha = 1.0) {
        return Color(static_cast<std::uint8_t>((hex >> 16) & 0xFF),
                     static_cast<std::uint8_t>((hex >> 8) & 0xFF),
                     static_cast<std::uint8_t>(hex & 0xFF), alpha);
    }

    constexpr Color with_alpha(double alpha) const { return Color(r, g, b, alpha); }

    constexpr bool operator==(const Color& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }

    /// Hex string "#RRGGBB" (alpha is emitted separately in SVG).
    std::string hex() const;
};

namespace colors {
inline constexpr Color black = Color::rgb(0x000000);
inline constexpr Color white = Color::rgb(0xFFFFFF);
inline constexpr Color transparent = Color(0, 0, 0, 0.0);
inline constexpr Color none = transparent;
} // namespace colors

/// Parse a CSS colour value: `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`,
/// `rgb(r,g,b)`, `rgba(r,g,b,a)`, or any CSS named colour (case
/// insensitive). Surrounding whitespace is ignored.
///
/// This is the suite's ONE colour parser. Every producer that reads a
/// colour out of a document — a chart config, a Mermaid `style`
/// directive, a command line — resolves it here, so `rebeccapurple`
/// means the same thing everywhere and no component has to carry a
/// private, weaker table of its own.
///
/// Returns nullopt (never throws) when the value is not a colour: the
/// caller knows whether that is a refusal or a diagnostic, and only the
/// caller can say which text position it happened at.
std::optional<Color> parse_css_color(std::string_view text);

/// Linearly interpolate between two colors. `t` is clamped to [0, 1]; each RGB
/// channel is mixed and rounded to the nearest integer while alpha blends
/// linearly. Used by the colormap sampler and by the Sankey gradient link
/// mode, both of which need deterministic, backend-independent color mixing.
inline Color lerp(Color a, Color b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    const auto mix = [&](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::round(x + (static_cast<int>(y) - x) * t));
    };
    return Color(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), a.a + (b.a - a.a) * t);
}

} // namespace cplot
