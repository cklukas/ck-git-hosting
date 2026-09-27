// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>

#include "color.hpp"

namespace cplot {

enum class Colormap {
    Viridis,
    Cividis,
    Magma,
    Plasma,
    Blues,
    Reds,
    Greys,
    RedBlue,
};

Colormap colormap_from_name(const std::string& name);
std::string colormap_name(Colormap map);
Color sample_colormap(Colormap map, double t);

struct ColorScale {
    Colormap colormap = Colormap::Viridis;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<double> midpoint;
    Color missing = Color::rgb(0xE6E6E6);
    bool reverse = false;

    Color color(double value, double data_min, double data_max) const;
};

} // namespace cplot
