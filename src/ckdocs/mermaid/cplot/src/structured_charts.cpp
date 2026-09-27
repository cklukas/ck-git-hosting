// ckplot — typed structured-chart models and table adapters
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "cplot/structured_charts.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include <cworks/app_error.hpp>

#include "cplot/figure.hpp"

namespace cplot {
namespace {
std::size_t tree_count(const TreemapNode& node) {
    std::size_t count = 1;
    for (const TreemapNode& child : node.children) count += tree_count(child);
    return count;
}

} // namespace
SankeyChart& SankeyChart::node(std::string id, std::string label,
                               std::optional<Color> color) {
    nodes.push_back({std::move(id), std::move(label), color});
    return *this;
}

SankeyChart& SankeyChart::link(std::string source, std::string target, double value) {
    links.push_back({std::move(source), std::move(target), value});
    return *this;
}

std::size_t TreemapChart::mark_count() const noexcept {
    std::size_t count = 0;
    for (const TreemapNode& root : roots) count += tree_count(root);
    return count;
}

CalendarChart& CalendarChart::day(std::int64_t days_since_epoch, double value) {
    days.push_back({days_since_epoch, value});
    return *this;
}

GaugeChart& GaugeChart::gauge(std::string label, double value) {
    gauges.push_back({std::move(label), value});
    return *this;
}

QuadrantChart& QuadrantChart::point(std::string id, std::string label, double x, double y,
                                    std::optional<Color> color) {
    QuadrantPoint point;
    point.id = std::move(id);
    point.label = std::move(label);
    point.x = x;
    point.y = y;
    point.color = color;
    points.push_back(std::move(point));
    return *this;
}

} // namespace cplot
