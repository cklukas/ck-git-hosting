// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Physical lengths for figure sizes. Layout always works in CSS pixels
// (96 px per inch); physical sizes are preserved for SVG export and
// drive the pixel dimensions of raster output via the figure DPI.
#pragma once

#include <string>

namespace cplot {

struct Length {
    enum class Unit { Px, Mm, Cm, In, Pt };

    double value = 0.0;
    Unit unit = Unit::Px;

    /// Convert to CSS pixels (96 px per inch).
    double to_px() const {
        switch (unit) {
        case Unit::Px: return value;
        case Unit::Mm: return value / 25.4 * 96.0;
        case Unit::Cm: return value / 2.54 * 96.0;
        case Unit::In: return value * 96.0;
        case Unit::Pt: return value / 72.0 * 96.0;
        }
        return value;
    }

    double to_inches() const { return to_px() / 96.0; }

    /// Convert to millimetres directly — no detour through pixels, so a
    /// value already given in mm (or cm/in) round-trips exactly instead of
    /// picking up float noise like 210.00000000000003.
    double to_mm() const {
        switch (unit) {
        case Unit::Px: return value / 96.0 * 25.4;
        case Unit::Mm: return value;
        case Unit::Cm: return value * 10.0;
        case Unit::In: return value * 25.4;
        case Unit::Pt: return value / 72.0 * 25.4;
        }
        return value;
    }

    bool is_physical() const { return unit != Unit::Px; }

    /// SVG attribute form, e.g. "85mm", "6in", "320" (px).
    std::string svg_attribute() const;
};

/// Parse "85mm", "8.5cm", "6in", "12pt", "900px" or "900" (px).
/// Throws cplot::Error on malformed input.
Length parse_length(const std::string& text);

namespace units {
constexpr Length operator""_px(long double v) {
    return {static_cast<double>(v), Length::Unit::Px};
}
constexpr Length operator""_px(unsigned long long v) {
    return {static_cast<double>(v), Length::Unit::Px};
}
constexpr Length operator""_mm(long double v) {
    return {static_cast<double>(v), Length::Unit::Mm};
}
constexpr Length operator""_mm(unsigned long long v) {
    return {static_cast<double>(v), Length::Unit::Mm};
}
constexpr Length operator""_cm(long double v) {
    return {static_cast<double>(v), Length::Unit::Cm};
}
constexpr Length operator""_cm(unsigned long long v) {
    return {static_cast<double>(v), Length::Unit::Cm};
}
constexpr Length operator""_in(long double v) {
    return {static_cast<double>(v), Length::Unit::In};
}
constexpr Length operator""_in(unsigned long long v) {
    return {static_cast<double>(v), Length::Unit::In};
}
constexpr Length operator""_pt(long double v) {
    return {static_cast<double>(v), Length::Unit::Pt};
}
constexpr Length operator""_pt(unsigned long long v) {
    return {static_cast<double>(v), Length::Unit::Pt};
}
} // namespace units

} // namespace cplot
