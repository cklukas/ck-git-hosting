// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cplot/colormap.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <cworks/app_error.hpp>

#include "cplot/figure.hpp"

namespace cplot {

namespace {

struct Stop {
    double t;
    Color color;
};

template <std::size_t N>
Color sample_stops(const std::array<Stop, N>& stops, double t) {
    t = std::clamp(t, 0.0, 1.0);
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].t) {
            const double span = stops[i].t - stops[i - 1].t;
            const double local = span > 0.0 ? (t - stops[i - 1].t) / span : 0.0;
            return lerp(stops[i - 1].color, stops[i].color, local);
        }
    }
    return stops.back().color;
}

} // namespace

Colormap colormap_from_name(const std::string& name) {
    if (name == "viridis") return Colormap::Viridis;
    if (name == "cividis") return Colormap::Cividis;
    if (name == "magma") return Colormap::Magma;
    if (name == "plasma") return Colormap::Plasma;
    if (name == "blues") return Colormap::Blues;
    if (name == "reds") return Colormap::Reds;
    if (name == "greys" || name == "grays") return Colormap::Greys;
    if (name == "red_blue" || name == "red-blue" || name == "rdbu") return Colormap::RedBlue;
    cworks::AppError e = cworks::object_not_found("colormap", name);
    e.summary = "unknown colormap: " + name;
    throw Error(std::move(e));
}

std::string colormap_name(Colormap map) {
    switch (map) {
    case Colormap::Viridis: return "viridis";
    case Colormap::Cividis: return "cividis";
    case Colormap::Magma: return "magma";
    case Colormap::Plasma: return "plasma";
    case Colormap::Blues: return "blues";
    case Colormap::Reds: return "reds";
    case Colormap::Greys: return "greys";
    case Colormap::RedBlue: return "red_blue";
    }
    return "viridis";
}

Color sample_colormap(Colormap map, double t) {
    static constexpr std::array<Stop, 5> viridis{{
        {0.00, Color::rgb(0x440154)},
        {0.25, Color::rgb(0x3B528B)},
        {0.50, Color::rgb(0x21918C)},
        {0.75, Color::rgb(0x5EC962)},
        {1.00, Color::rgb(0xFDE725)},
    }};
    static constexpr std::array<Stop, 5> cividis{{
        {0.00, Color::rgb(0x00224E)},
        {0.25, Color::rgb(0x31446B)},
        {0.50, Color::rgb(0x666870)},
        {0.75, Color::rgb(0xA3905B)},
        {1.00, Color::rgb(0xFDE737)},
    }};
    static constexpr std::array<Stop, 5> magma{{
        {0.00, Color::rgb(0x000004)},
        {0.25, Color::rgb(0x3B0F70)},
        {0.50, Color::rgb(0x8C2981)},
        {0.75, Color::rgb(0xDE4968)},
        {1.00, Color::rgb(0xFCFDBF)},
    }};
    static constexpr std::array<Stop, 5> plasma{{
        {0.00, Color::rgb(0x0D0887)},
        {0.25, Color::rgb(0x7E03A8)},
        {0.50, Color::rgb(0xCC4778)},
        {0.75, Color::rgb(0xF89540)},
        {1.00, Color::rgb(0xF0F921)},
    }};
    static constexpr std::array<Stop, 4> blues{{
        {0.00, Color::rgb(0xF7FBFF)},
        {0.35, Color::rgb(0xC6DBEF)},
        {0.70, Color::rgb(0x4292C6)},
        {1.00, Color::rgb(0x084594)},
    }};
    static constexpr std::array<Stop, 4> reds{{
        {0.00, Color::rgb(0xFFF5F0)},
        {0.35, Color::rgb(0xFCBBA1)},
        {0.70, Color::rgb(0xFB6A4A)},
        {1.00, Color::rgb(0xA50F15)},
    }};
    static constexpr std::array<Stop, 4> greys{{
        {0.00, Color::rgb(0xFFFFFF)},
        {0.35, Color::rgb(0xCCCCCC)},
        {0.70, Color::rgb(0x737373)},
        {1.00, Color::rgb(0x111111)},
    }};
    static constexpr std::array<Stop, 3> red_blue{{
        {0.00, Color::rgb(0xB2182B)},
        {0.50, Color::rgb(0xF7F7F7)},
        {1.00, Color::rgb(0x2166AC)},
    }};

    switch (map) {
    case Colormap::Viridis: return sample_stops(viridis, t);
    case Colormap::Cividis: return sample_stops(cividis, t);
    case Colormap::Magma: return sample_stops(magma, t);
    case Colormap::Plasma: return sample_stops(plasma, t);
    case Colormap::Blues: return sample_stops(blues, t);
    case Colormap::Reds: return sample_stops(reds, t);
    case Colormap::Greys: return sample_stops(greys, t);
    case Colormap::RedBlue: return sample_stops(red_blue, t);
    }
    return sample_stops(viridis, t);
}

Color ColorScale::color(double value, double data_min, double data_max) const {
    if (!std::isfinite(value)) return missing;
    const double lo = minimum.value_or(data_min);
    const double hi = maximum.value_or(data_max);
    double t = 0.5;
    if (midpoint && lo < *midpoint && *midpoint < hi) {
        if (value <= *midpoint) {
            t = 0.5 * (value - lo) / (*midpoint - lo);
        } else {
            t = 0.5 + 0.5 * (value - *midpoint) / (hi - *midpoint);
        }
    } else {
        const double span = hi - lo;
        t = span != 0.0 ? (value - lo) / span : 0.5;
    }
    t = std::clamp(t, 0.0, 1.0);
    if (reverse) t = 1.0 - t;
    return sample_colormap(colormap, t);
}

} // namespace cplot
