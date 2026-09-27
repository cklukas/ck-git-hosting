// ckplot — deterministic whole-canvas layouts for structured charts
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <utility>

#include <cworks/app_error.hpp>
#include <cworks/format.hpp>
#include <cworks/trig.hpp>

#include "cplot/text.hpp"
#include "cplot/ticks.hpp"

namespace cplot::detail {
namespace {

Scene base_scene(const Figure& fig, const std::string& chart_title) {
    Scene scene;
    scene.width = fig.width();
    scene.height = fig.height();
    scene.background = fig.current_theme().page_background;
    scene.meta_title = !fig.metadata().title.empty() ? fig.metadata().title : chart_title;
    scene.meta_description = !fig.metadata().description.empty() ? fig.metadata().description
                                                                 : fig.metadata().alt_text;
    scene.meta_extra = fig.metadata().extra;
    if (!fig.compatibility_profile().empty()) {
        scene.meta_generator =
            "cplot-version: " CPLOT_VERSION "; compatibility: " +
            fig.compatibility_profile();
    }
    if (fig.physical_size()) {
        scene.svg_width_attr = fig.physical_size()->first.svg_attribute();
        scene.svg_height_attr = fig.physical_size()->second.svg_attribute();
    }
    // No clip: a structured chart draws on the whole canvas, and the
    // rectangle it would have been cut to is the canvas. The layers are
    // filled by the caller and become the scene's content there.
    return scene;
}

Color chart_color(const Theme& theme, std::size_t index) {
    return theme.palette.empty() ? colors::black : theme.series_color(index);
}

void add_text(std::vector<SceneItem>& items, Point pos, std::string text, Font font,
              Color color, HAlign horizontal = HAlign::Left,
              VAlign vertical = VAlign::Baseline, double rotation = 0.0) {
    items.push_back(TextItem{pos, std::move(text), std::move(font), color, horizontal,
                             vertical, rotation});
}

/// Both boundary curves of a ribbon, each sampled left-to-right (source edge
/// on the left, target edge on the right) with `segments + 1` points. Solid
/// fills join `top` with the reversed `bottom` into one polygon; the gradient
/// mode instead builds one quad per Bézier segment from adjacent points on the
/// two edges, so each quad can carry its own interpolated color.
struct RibbonEdges {
    std::vector<Point> top;
    std::vector<Point> bottom;
};

constexpr int kRibbonSegments = 24;

RibbonEdges ribbon_edges(Point source_top, Point target_top, double height) {
    const double middle = (source_top.x + target_top.x) / 2.0;
    const auto sample_curve = [&](Point p0, Point c0, Point c1, Point p1) {
        std::vector<Point> points;
        points.reserve(static_cast<std::size_t>(kRibbonSegments + 1));
        for (int i = 0; i <= kRibbonSegments; ++i) {
            const double t = static_cast<double>(i) / kRibbonSegments;
            const double u = 1.0 - t;
            const double b0 = u * u * u;
            const double b1 = 3.0 * u * u * t;
            const double b2 = 3.0 * u * t * t;
            const double b3 = t * t * t;
            points.push_back({b0 * p0.x + b1 * c0.x + b2 * c1.x + b3 * p1.x,
                              b0 * p0.y + b1 * c0.y + b2 * c1.y + b3 * p1.y});
        }
        return points;
    };
    const Point source_bottom{source_top.x, source_top.y + height};
    const Point target_bottom{target_top.x, target_top.y + height};
    RibbonEdges edges;
    edges.top =
        sample_curve(source_top, {middle, source_top.y}, {middle, target_top.y}, target_top);
    edges.bottom = sample_curve(source_bottom, {middle, source_bottom.y},
                                {middle, target_bottom.y}, target_bottom);
    return edges;
}

std::vector<Point> ribbon_polygon(Point source_top, Point target_top, double height) {
    const RibbonEdges edges = ribbon_edges(source_top, target_top, height);
    std::vector<Point> points = edges.top;
    points.reserve(edges.top.size() + edges.bottom.size());
    points.insert(points.end(), edges.bottom.rbegin(), edges.bottom.rend());
    return points;
}

struct SankeyModel {
    struct Link {
        int source = 0;
        int target = 0;
        double value = 0.0;
    };
    std::vector<Link> links;
    std::vector<int> rank;
    std::vector<std::vector<int>> columns;
    std::vector<double> flow;
};

SankeyModel validate_sankey(const SankeyChart& chart) {
    if (chart.nodes.empty()) throw Error(cworks::validation_failed("sankey chart has no nodes"));
    if (chart.links.empty()) throw Error(cworks::validation_failed("sankey chart has no links"));
    std::map<std::string, int> ids;
    for (std::size_t i = 0; i < chart.nodes.size(); ++i) {
        const SankeyNode& node = chart.nodes[i];
        if (node.id.empty()) throw Error(cworks::validation_failed(
            "sankey node IDs must not be empty"));
        if (node.label.empty()) throw Error(cworks::validation_failed("sankey node '" + node.id +
            "' has no label"));
        if (!ids.emplace(node.id, static_cast<int>(i)).second)
            throw Error(cworks::validation_failed("duplicate sankey node ID '" + node.id + "'"));
    }

    SankeyModel model;
    std::map<std::pair<int, int>, std::size_t> edge_index;
    for (const SankeyLink& link : chart.links) {
        const auto source = ids.find(link.source);
        const auto target = ids.find(link.target);
        if (source == ids.end()) throw Error(cworks::validation_failed(
            "unknown sankey source node '" + link.source + "'"));
        if (target == ids.end()) throw Error(cworks::validation_failed(
            "unknown sankey target node '" + link.target + "'"));
        if (source->second == target->second)
            throw Error(cworks::validation_failed("sankey links cannot connect a node to itself ('"
                + link.source + "')"));
        if (!std::isfinite(link.value) || link.value <= 0.0)
            throw Error(cworks::validation_failed("sankey link '" + link.source + "' to '" +
                link.target + "' must have a finite positive value"));
        const std::pair<int, int> key{source->second, target->second};
        const auto [it, inserted] = edge_index.emplace(key, model.links.size());
        if (inserted)
            model.links.push_back({key.first, key.second, link.value});
        else
            model.links[it->second].value += link.value;
    }

    const int count = static_cast<int>(chart.nodes.size());
    std::vector<std::vector<int>> outgoing(static_cast<std::size_t>(count));
    std::vector<int> indegree(static_cast<std::size_t>(count), 0);
    for (const SankeyModel::Link& link : model.links) {
        outgoing[static_cast<std::size_t>(link.source)].push_back(link.target);
        ++indegree[static_cast<std::size_t>(link.target)];
    }
    std::vector<int> queue;
    std::vector<bool> pure_source(static_cast<std::size_t>(count), false);
    for (int node = 0; node < count; ++node)
        if (indegree[static_cast<std::size_t>(node)] == 0) {
            pure_source[static_cast<std::size_t>(node)] = true;
            queue.push_back(node);
        }
    model.rank.assign(static_cast<std::size_t>(count), 0);
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const int node = queue[head];
        for (const int target : outgoing[static_cast<std::size_t>(node)]) {
            model.rank[static_cast<std::size_t>(target)] =
                std::max(model.rank[static_cast<std::size_t>(target)],
                         model.rank[static_cast<std::size_t>(node)] + 1);
            if (--indegree[static_cast<std::size_t>(target)] == 0) queue.push_back(target);
        }
    }
    if (queue.size() != chart.nodes.size()) throw Error(cworks::validation_failed(
        "sankey chart links must be acyclic"));

    const int maximum_rank = *std::max_element(model.rank.begin(), model.rank.end());
    if (maximum_rank == 0) throw Error(cworks::validation_failed(
        "sankey chart needs at least two connected stages"));
    switch (chart.alignment) {
    case SankeyAlignment::Justify:
        // Terminal nodes share the right edge even when their path is
        // shorter than another component. Links may span columns; sinks
        // should not stop in an arbitrary interior column merely because
        // their route is short.
        for (int node = 0; node < count; ++node)
            if (outgoing[static_cast<std::size_t>(node)].empty())
                model.rank[static_cast<std::size_t>(node)] = maximum_rank;
        break;
    case SankeyAlignment::Left:
        break; // the longest-path stage is the left alignment
    case SankeyAlignment::Right: {
        // Distance to the farthest sink, walked over the reversed
        // topological order the Kahn queue already provides.
        std::vector<int> height(static_cast<std::size_t>(count), 0);
        for (std::size_t head = queue.size(); head-- > 0;) {
            const int node = queue[head];
            for (const int target : outgoing[static_cast<std::size_t>(node)])
                height[static_cast<std::size_t>(node)] =
                    std::max(height[static_cast<std::size_t>(node)],
                             height[static_cast<std::size_t>(target)] + 1);
        }
        for (int node = 0; node < count; ++node)
            model.rank[static_cast<std::size_t>(node)] =
                maximum_rank - height[static_cast<std::size_t>(node)];
        break;
    }
    case SankeyAlignment::Center:
        // Pure sources sit one column left of their first target; every
        // node that receives flow keeps its longest-path stage.
        for (int node = 0; node < count; ++node) {
            if (pure_source[static_cast<std::size_t>(node)] &&
                !outgoing[static_cast<std::size_t>(node)].empty()) {
                int nearest = maximum_rank;
                for (const int target : outgoing[static_cast<std::size_t>(node)])
                    nearest = std::min(nearest, model.rank[static_cast<std::size_t>(target)]);
                model.rank[static_cast<std::size_t>(node)] = std::max(0, nearest - 1);
            }
        }
        break;
    }
    model.columns.resize(static_cast<std::size_t>(maximum_rank + 1));
    for (int node = 0; node < count; ++node)
        model.columns[static_cast<std::size_t>(model.rank[static_cast<std::size_t>(node)])]
            .push_back(node);

    std::vector<double> incoming(static_cast<std::size_t>(count), 0.0);
    std::vector<double> outgoing_sum(static_cast<std::size_t>(count), 0.0);
    for (const SankeyModel::Link& link : model.links) {
        outgoing_sum[static_cast<std::size_t>(link.source)] += link.value;
        incoming[static_cast<std::size_t>(link.target)] += link.value;
    }
    model.flow.resize(static_cast<std::size_t>(count));
    for (int node = 0; node < count; ++node) {
        model.flow[static_cast<std::size_t>(node)] =
            std::max(incoming[static_cast<std::size_t>(node)],
                     outgoing_sum[static_cast<std::size_t>(node)]);
        if (model.flow[static_cast<std::size_t>(node)] <= 0.0)
            throw Error(cworks::validation_failed("sankey node '" +
                chart.nodes[static_cast<std::size_t>(node)].id + "' is not connected"));
    }
    return model;
}

double validate_treemap_node(const TreemapNode& node, std::set<std::string>& ids,
                             std::map<const TreemapNode*, double>& weights) {
    if (node.id.empty()) throw Error(cworks::validation_failed(
        "treemap node IDs must not be empty"));
    if (node.label.empty()) throw Error(cworks::validation_failed("treemap node '" + node.id +
        "' has no label"));
    if (!ids.insert(node.id).second) throw Error(cworks::validation_failed(
        "duplicate treemap node ID '" + node.id + "'"));
    if (node.color_value && !std::isfinite(*node.color_value))
        throw Error(cworks::validation_failed(
            "treemap node '" + node.label + "' has a non-finite color value"));
    if (node.children.empty()) {
        if (!node.value || !std::isfinite(*node.value) || *node.value <= 0.0)
            throw Error(cworks::validation_failed("treemap leaf '" + node.label +
                "' needs a finite positive value"));
        weights[&node] = *node.value;
        return *node.value;
    }
    double total = 0.0;
    for (const TreemapNode& child : node.children)
        total += validate_treemap_node(child, ids, weights);
    if (node.value) {
        if (!std::isfinite(*node.value) || *node.value <= 0.0)
            throw Error(cworks::validation_failed("treemap branch '" + node.label +
                "' has an invalid value"));
        const double tolerance = std::max(1.0, total) * 1e-9;
        if (std::abs(*node.value - total) > tolerance)
            throw Error(cworks::validation_failed("treemap branch '" + node.label +
                "' value does not equal the sum of its children"));
    }
    weights[&node] = total;
    return total;
}

/// Perceptual luma (ITU-R BT.601 weights, matching the funnel/pie label
/// contrast switches elsewhere in series.cpp), scaled to [0, 1].
double relative_luminance(Color color) {
    return (0.299 * color.r + 0.587 * color.g + 0.114 * color.b) / 255.0;
}

/// Readable label color for text drawn over `fill`: light text on a dark
/// tile, dark text on a light one.
Color contrasting_text_color(Color fill, const Theme& theme) {
    return relative_luminance(fill) < 0.5 ? theme.background : theme.text_color;
}

/// Resolved continuous color measure per node (leaves and branches), the
/// finite [min, max] actually present anywhere in the chart, and whether
/// any node resolved one at all. A branch's resolved value is the
/// SIZE-weighted mean of its children's resolved values (sizes come from
/// `weights`, the same map validate_treemap_node built for layout);
/// children with no resolved color contribute neither a numerator nor a
/// denominator term, so an uncolored sibling cannot pull the mean toward
/// it. A branch whose children are all uncolored has no resolved value
/// either, and falls back to the missing-color rule at render time, same
/// as an uncolored leaf. Any color_value set directly on a branch node is
/// ignored — branch colors are always computed, never authored.
struct TreemapColorModel {
    std::map<const TreemapNode*, double> resolved;
    double data_min = 0.0;
    double data_max = 0.0;
    bool active = false;
};

std::optional<double> resolve_treemap_color_node(const TreemapNode& node,
                                                 const std::map<const TreemapNode*, double>& weights,
                                                 TreemapColorModel& model) {
    std::optional<double> value;
    if (node.children.empty()) {
        value = node.color_value;
    } else {
        double weighted_sum = 0.0;
        double weight_total = 0.0;
        // Child order, not the weights map's pointer-keyed order:
        // summation must not depend on map/hash internals.
        for (const TreemapNode& child : node.children) {
            const std::optional<double> child_value =
                resolve_treemap_color_node(child, weights, model);
            if (!child_value) continue;
            const double child_weight = weights.at(&child);
            weighted_sum += child_weight * *child_value;
            weight_total += child_weight;
        }
        if (weight_total > 0.0) value = weighted_sum / weight_total;
    }
    if (value) {
        model.resolved.emplace(&node, *value);
        if (!model.active) {
            model.data_min = model.data_max = *value;
            model.active = true;
        } else {
            model.data_min = std::min(model.data_min, *value);
            model.data_max = std::max(model.data_max, *value);
        }
    }
    return value;
}

TreemapColorModel resolve_treemap_colors(const TreemapChart& chart,
                                         const std::map<const TreemapNode*, double>& weights) {
    TreemapColorModel model;
    for (const TreemapNode& root : chart.roots) resolve_treemap_color_node(root, weights, model);
    if (model.active && model.data_min == model.data_max) {
        // Pad a degenerate range so the colormap still spans a visible
        // interval (mirrors ColorEncoding::value_range for scatter/bubble
        // color-by-value).
        model.data_min -= 0.5;
        model.data_max += 0.5;
    }
    return model;
}

struct WeightedNode {
    const TreemapNode* node = nullptr;
    double weight = 0.0;
};

void layout_binary(const std::vector<WeightedNode>& nodes, std::size_t first,
                   std::size_t last, RectF rect,
                   std::vector<std::pair<const TreemapNode*, RectF>>& output) {
    if (first >= last) return;
    if (last - first == 1) {
        output.push_back({nodes[first].node, rect});
        return;
    }
    const double total = std::accumulate(
        nodes.begin() + static_cast<std::ptrdiff_t>(first),
        nodes.begin() + static_cast<std::ptrdiff_t>(last), 0.0,
        [](double sum, const WeightedNode& node) { return sum + node.weight; });
    double before = 0.0;
    std::size_t split = first + 1;
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = first + 1; i < last; ++i) {
        before += nodes[i - 1].weight;
        const double difference = std::abs(total / 2.0 - before);
        if (difference < best) {
            best = difference;
            split = i;
        }
    }
    const double first_weight = std::accumulate(
        nodes.begin() + static_cast<std::ptrdiff_t>(first),
        nodes.begin() + static_cast<std::ptrdiff_t>(split), 0.0,
        [](double sum, const WeightedNode& node) { return sum + node.weight; });
    const double fraction = first_weight / total;
    RectF a = rect;
    RectF b = rect;
    if (rect.w >= rect.h) {
        a.w = rect.w * fraction;
        b.x += a.w;
        b.w -= a.w;
    } else {
        a.h = rect.h * fraction;
        b.y += a.h;
        b.h -= a.h;
    }
    layout_binary(nodes, first, split, a, output);
    layout_binary(nodes, split, last, b, output);
}

std::vector<std::pair<const TreemapNode*, RectF>> layout_children(
    const std::vector<TreemapNode>& children, RectF rect,
    const std::map<const TreemapNode*, double>& weights) {
    std::vector<WeightedNode> ordered;
    ordered.reserve(children.size());
    for (const TreemapNode& child : children) ordered.push_back({&child, weights.at(&child)});
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const WeightedNode& a, const WeightedNode& b) {
                         return a.weight > b.weight;
                     });
    std::vector<std::pair<const TreemapNode*, RectF>> result;
    layout_binary(ordered, 0, ordered.size(), rect, result);
    return result;
}

void draw_treemap_node(const TreemapNode& node, RectF rect, std::size_t color_index,
                       const Theme& theme, const TextMeasurer& text,
                       const std::map<const TreemapNode*, double>& weights,
                       const ColorScale& color_scale, const TreemapColorModel& color_model,
                       PanelLayers& panel, bool diversify_children = false) {
    constexpr double gap = 2.0;
    if (rect.w <= gap * 2.0 || rect.h <= gap * 2.0) return;
    rect.x += gap;
    rect.y += gap;
    rect.w -= gap * 2.0;
    rect.h -= gap * 2.0;

    // Purely additive: with no color measure resolved anywhere in the
    // chart this reproduces the original rotating-palette color exactly,
    // byte for byte. Once a measure is in use, unresolved nodes (a leaf
    // with no color_value, or a branch whose children are all uncolored)
    // fall back to the scale's `missing` color instead of the palette.
    Color color;
    if (color_model.active) {
        const auto resolved = color_model.resolved.find(&node);
        color = resolved != color_model.resolved.end()
                    ? color_scale.color(resolved->second, color_model.data_min,
                                        color_model.data_max)
                    : color_scale.missing;
    } else {
        color = chart_color(theme, color_index);
    }
    const Font label_font = theme.base_font();

    if (node.children.empty()) {
        ShapeStyle tile;
        tile.fill = color.with_alpha(0.72);
        tile.stroke = theme.background;
        tile.stroke_width = 1.0;
        panel.marks.push_back(RectItem{rect, tile});
        // Label contrast only matters once tiles carry data-driven colors
        // (a diverging colormap spans both very light and very dark
        // fills); the legacy palette path keeps the fixed theme color.
        const Color label_color =
            color_model.active ? contrasting_text_color(color, theme) : theme.text_color;
        const double available = rect.w - 10.0;
        if (rect.h >= label_font.size * 1.8 && available > 0.0 &&
            text.measure(node.label, label_font).width <= available) {
            add_text(panel.over, {rect.x + rect.w / 2.0, rect.y + rect.h / 2.0},
                     node.label, label_font, label_color, HAlign::Center,
                     VAlign::Middle);
        }
        return;
    }

    ShapeStyle boundary;
    boundary.stroke = color.with_alpha(0.8);
    boundary.stroke_width = 1.0;
    panel.over.push_back(RectItem{rect, boundary});
    double header = 0.0;
    if (rect.h >= 42.0 && text.measure(node.label, label_font).width <= rect.w - 10.0) {
        header = label_font.size * 1.55;
        add_text(panel.over, {rect.x + 5.0, rect.y + 3.0}, node.label, label_font,
                 theme.text_color, HAlign::Left, VAlign::Top);
    }
    RectF content{rect.x, rect.y + header, rect.w, rect.h - header};
    for (const auto& [child, child_rect] : layout_children(node.children, content, weights)) {
        const auto position = std::find_if(node.children.begin(), node.children.end(),
                                           [&](const TreemapNode& candidate) {
                                               return &candidate == child;
                                           });
        const std::size_t child_index =
            static_cast<std::size_t>(position - node.children.begin());
        draw_treemap_node(*child, child_rect,
                          diversify_children ? child_index : color_index, theme, text,
                          weights, color_scale, color_model, panel);
    }
}

void validate_quadrant(const QuadrantChart& chart) {
    if (chart.points.empty()) throw Error(cworks::validation_failed(
        "quadrant chart has no points"));
    std::set<std::string> ids;
    for (const QuadrantPoint& point : chart.points) {
        if (point.id.empty()) throw Error(cworks::validation_failed(
            "quadrant point IDs must not be empty"));
        if (!ids.insert(point.id).second)
            throw Error(cworks::validation_failed("duplicate quadrant point ID '" + point.id +
                "'"));
        if (point.label.empty()) throw Error(cworks::validation_failed("quadrant point '" + point.id
            + "' has no label"));
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0.0 ||
            point.x > 1.0 || point.y < 0.0 || point.y > 1.0)
            throw Error(cworks::validation_failed("quadrant point '" + point.label +
                "' must have x and y in the inclusive range [0, 1]"));
    }
}

// -- calendar heatmap ---------------------------------------------------

/// Days since the Unix epoch → ISO weekday index (0 = Monday .. 6 =
/// Sunday). 1970-01-01 (day 0) was a Thursday (ISO index 3); anchoring
/// every other day to that fixed point keeps this independent of libc
/// time and locale, per the suite's determinism rule.
int iso_weekday(std::int64_t day) {
    std::int64_t m = (day + 3) % 7;
    if (m < 0) m += 7;
    return static_cast<int>(m);
}

/// Row index (0..6, top to bottom) within a week band for the chart's
/// configured week_start.
int calendar_weekday(std::int64_t day, WeekStart week_start) {
    const int iso = iso_weekday(day);
    return week_start == WeekStart::Sunday ? (iso + 1) % 7 : iso;
}

/// Fixed English weekday initials, ISO (Monday-first) order, rotated to
/// the configured week_start — never locale-dependent, per the suite's
/// no-locale rule.
const char* weekday_letter(int row, WeekStart week_start) {
    static constexpr const char* kLetters[7] = {"M", "T", "W", "T", "F", "S", "S"};
    const int iso = week_start == WeekStart::Sunday ? (row + 6) % 7 : row;
    return kLetters[iso];
}

/// Fixed English month abbreviations (month is 1..12) — never
/// locale-dependent.
const char* month_abbrev(int month) {
    static constexpr const char* kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return kMonths[month - 1];
}

/// One value per calendar day, plus the finite range those values span.
/// `values` holds exactly one entry per day in `chart.days` — never a
/// silent sum or overwrite when two days coincide (see validate_calendar).
struct CalendarModel {
    std::map<std::int64_t, double> values;
    double data_min = 0.0;
    double data_max = 0.0;
};

CalendarModel validate_calendar(const CalendarChart& chart) {
    if (chart.days.empty()) throw Error(cworks::validation_failed("calendar chart has no data"));
    CalendarModel model;
    for (const CalendarDay& entry : chart.days) {
        if (!std::isfinite(entry.value)) throw Error(cworks::validation_failed("calendar chart has a non-finite value"));
        const auto [it, inserted] = model.values.emplace(entry.day, entry.value);
        if (!inserted)
            throw Error(cworks::validation_failed("calendar chart has more than one value for " +
                                                  cworks::format_datetime(entry.day * 86400LL * 1'000'000LL) +
                                                  " (duplicate dates are a validation error, never silently summed or "
                                                  "overwritten)"));
    }
    model.data_min = model.data_max = model.values.begin()->second;
    for (const auto& [day, value] : model.values) {
        model.data_min = std::min(model.data_min, value);
        model.data_max = std::max(model.data_max, value);
    }
    if (model.data_min == model.data_max) {
        // Pad a degenerate range so the colormap still spans a visible
        // interval (mirrors ColorEncoding::value_range for scatter/bubble
        // color-by-value elsewhere in series.cpp).
        model.data_min -= 0.5;
        model.data_max += 0.5;
    }
    return model;
}

/// One calendar-year row band: the ISO week grid covering Jan 1 – Dec 31
/// of `year`, anchored at `grid_start` (the first day of the week
/// containing Jan 1, which may fall in the previous year).
struct YearBand {
    int year = 0;
    std::int64_t jan1 = 0;
    std::int64_t dec31 = 0;
    std::int64_t grid_start = 0;
    int week_count = 0;
};

/// One band per calendar year actually present in the data, oldest
/// first — std::set already keeps `years` in ascending order.
std::vector<YearBand> build_year_bands(const CalendarModel& model, WeekStart week_start) {
    std::set<int> years;
    for (const auto& [day, value] : model.values) {
        int y = 0, m = 0, d = 0;
        cworks::civil_from_days(day, y, m, d);
        years.insert(y);
    }
    std::vector<YearBand> bands;
    for (int year : years) {
        YearBand band;
        band.year = year;
        band.jan1 = cworks::days_from_civil(year, 1, 1);
        band.dec31 = cworks::days_from_civil(year, 12, 31);
        band.grid_start = band.jan1 - calendar_weekday(band.jan1, week_start);
        band.week_count = static_cast<int>((band.dec31 - band.grid_start) / 7) + 1;
        bands.push_back(band);
    }
    return bands;
}

// -- gauge ---------------------------------------------------------------

void validate_gauge(const GaugeChart& chart) {
    if (chart.gauges.empty()) throw Error(cworks::validation_failed("gauge chart has no gauges"));
    if (!std::isfinite(chart.minimum) || !std::isfinite(chart.maximum) ||
        chart.minimum >= chart.maximum)
        throw Error(cworks::validation_failed("gauge chart's min must be finite and less than max"));
    for (const GaugeItem& item : chart.gauges) {
        if (item.label.empty()) throw Error(cworks::validation_failed("gauge chart has a gauge with no label"));
        if (!std::isfinite(item.value))
            throw Error(cworks::validation_failed("gauge '" + item.label + "' has a non-finite value"));
    }
    for (const GaugeBand& band : chart.bands) {
        if (!std::isfinite(band.from) || !std::isfinite(band.to) || band.from > band.to)
            throw Error(cworks::validation_failed("gauge band must have a finite 'from' not greater than 'to'"));
    }
    for (double tick : chart.ticks)
        if (!std::isfinite(tick)) throw Error(cworks::validation_failed("gauge tick values must be finite"));
}

// relative_luminance / contrasting_text_color are defined earlier in this
// file (treemap's label-contrast switch); the gauge value label reuses
// them directly rather than duplicating the formula.

/// Point at `radius` px from `center`, at `angle_deg` measured clockwise
/// from 12 o'clock — the scene's SectorItem convention (see scene.hpp).
/// Deterministic trigonometry, for the reason given on on_circle there.
Point polar_point(Point center, double radius, double angle_deg) {
    double sine = 0.0;
    double cosine = 0.0;
    cworks::sincos_deg(angle_deg, sine, cosine);
    return {center.x + radius * sine, center.y - radius * cosine};
}

/// Draws one gauge dial into `cell`: a thin background track over
/// [minimum, maximum], any configured bands, tick marks, a needle
/// clamped to the domain, and the value/label readout below. Never a
/// 3D/glossy widget — a flat arc in theme colors, matching the pie chart's
/// SectorItem usage.
void draw_gauge(const GaugeChart& chart, const GaugeItem& item, const RectF& cell,
                const Theme& theme, const TickSet& ticks, PanelLayers& panel) {
    constexpr double kStartAngle = -90.0; // 9 o'clock
    constexpr double kEndAngle = 90.0;    // 3 o'clock, sweeping through 12 o'clock
    const Font label_font = theme.base_font();
    const Font value_font =
        theme.base_font().with_size(theme.base_font().size * 1.35).with_weight(FontWeight::Bold);
    const Font tick_font = theme.tick_font();

    constexpr double kMargin = 10.0;
    const double text_block = label_font.size * 1.3 + value_font.size * 1.3 + 8.0;
    double radius = std::min(cell.w / 2.0 - kMargin, cell.h - 2.0 * kMargin - text_block);
    radius = std::min(radius, 90.0); // a calm, flat dial — never oversized
    if (radius < 30.0) throw Error(cworks::validation_failed("figure is too small for the gauge chart"));

    const Point center{cell.x + cell.w / 2.0, cell.y + kMargin + radius};
    const double band_thickness = std::clamp(radius * 0.22, 8.0, 20.0);
    const double radius_inner = radius - band_thickness;

    const auto angle_for = [&](double value) {
        const double clamped = std::clamp(value, chart.minimum, chart.maximum);
        const double fraction = (clamped - chart.minimum) / (chart.maximum - chart.minimum);
        return kStartAngle + (kEndAngle - kStartAngle) * fraction;
    };

    // Background track: the full [minimum, maximum] sweep in a muted tone,
    // so the scale reads even where no band covers a value.
    SectorItem track;
    track.center = center;
    track.radius_inner = radius_inner;
    track.radius_outer = radius;
    track.start_angle = kStartAngle;
    track.end_angle = kEndAngle;
    track.style.fill = theme.axis_color.with_alpha(0.12);
    panel.marks.push_back(track);

    for (const GaugeBand& band : chart.bands) {
        const double from = std::clamp(band.from, chart.minimum, chart.maximum);
        const double to = std::clamp(band.to, chart.minimum, chart.maximum);
        if (!(to > from)) continue; // clamped away: the band lies outside the scale
        SectorItem sector;
        sector.center = center;
        sector.radius_inner = radius_inner;
        sector.radius_outer = radius;
        sector.start_angle = angle_for(from);
        sector.end_angle = angle_for(to);
        sector.style.fill = band.color;
        panel.marks.push_back(sector);
        if (!band.label.empty()) {
            const Point pos = polar_point(center, (radius_inner + radius) / 2.0,
                                          (sector.start_angle + sector.end_angle) / 2.0);
            add_text(panel.over, pos, band.label, tick_font,
                     contrasting_text_color(band.color, theme), HAlign::Center, VAlign::Middle);
        }
    }

    for (const Tick& tick : ticks.ticks) {
        const double angle = angle_for(tick.value);
        ShapeStyle tick_style;
        tick_style.stroke = theme.axis_color;
        tick_style.stroke_width = 1.0;
        panel.over.push_back(LineItem{polar_point(center, radius + 2.0, angle),
                                      polar_point(center, radius + 8.0, angle), tick_style});
        const HAlign align =
            angle < -15.0 ? HAlign::Right : (angle > 15.0 ? HAlign::Left : HAlign::Center);
        add_text(panel.over, polar_point(center, radius + 12.0, angle), tick.label, tick_font,
                 theme.muted_text_color, align, VAlign::Middle);
    }

    // Needle: a thin radial line clamped to [minimum, maximum], plus a
    // hub — a flat indicator, never a 3D dashboard needle.
    const double needle_angle = angle_for(item.value);
    ShapeStyle needle_style;
    needle_style.stroke = theme.text_color;
    needle_style.stroke_width = 2.0;
    needle_style.cap = LineCap::Round;
    needle_style.join = LineJoin::Round;
    panel.marks.push_back(
        LineItem{center, polar_point(center, radius_inner - 4.0, needle_angle), needle_style});
    ShapeStyle hub_style;
    hub_style.fill = theme.text_color;
    panel.marks.push_back(CircleItem{center, 3.5, hub_style});

    // The value readout always shows the true, unclamped number — only
    // the needle and arc position clamp to the domain.
    const std::string value_text =
        (chart.value_format.empty() ? format_tick_value(item.value, 0.0)
                                    : format_with(chart.value_format, item.value)) +
        chart.unit;
    add_text(panel.over, {center.x, center.y + 10.0}, value_text, value_font, theme.text_color,
             HAlign::Center, VAlign::Top);
    add_text(panel.over, {center.x, center.y + 10.0 + value_font.size * 1.3}, item.label,
             label_font, theme.muted_text_color, HAlign::Center, VAlign::Top);
}

} // namespace

Scene build_scene(const Figure& fig, const SankeyChart& chart) {
    SankeyModel model = validate_sankey(chart);

    // A value order pins each column largest-on-top (or smallest-on-top) so a
    // node that changes rank between stages crosses on purpose — the alluvial
    // convention. Sorting up front means the centred packing below already
    // reflects the order, and the crossing-minimisation relaxation is skipped
    // so it cannot undo it. Ties keep node (insertion) order for determinism.
    if (chart.order != SankeyOrder::Auto) {
        const bool descending = chart.order == SankeyOrder::Descending;
        for (std::vector<int>& column : model.columns)
            std::stable_sort(column.begin(), column.end(), [&](int a, int b) {
                const double fa = model.flow[static_cast<std::size_t>(a)];
                const double fb = model.flow[static_cast<std::size_t>(b)];
                return descending ? fa > fb : fa < fb;
            });
    }

    Scene scene = base_scene(fig, chart.title);
    PanelLayers panel;
    const Theme& theme = fig.current_theme();
    const Font font = theme.base_font();
    const Font title_font = theme.title_font();

    const double padding = std::max(16.0, theme.padding);
    const double title_height = chart.title.empty() ? 0.0 : title_font.size * 2.0;
    const double top = padding + title_height;
    const double available_height = fig.height() - top - padding;
    const double node_width = 14.0;
    const double node_gap = 12.0;
    const double left = padding;
    const double right = fig.width() - padding - node_width;
    if (available_height <= 40.0 || right - left <= 80.0)
        throw Error(cworks::validation_failed("figure is too small for the sankey chart"));

    // A crowded column shrinks the gap instead of failing, matching Mermaid's
    // padding clamp; the clamped gap feeds the scale, packing, and slots below.
    std::size_t widest_column = 1;
    for (const std::vector<int>& column : model.columns)
        widest_column = std::max(widest_column, column.size());
    const double gap =
        widest_column > 1
            ? std::min(node_gap, available_height / static_cast<double>(widest_column - 1))
            : node_gap;

    double scale = std::numeric_limits<double>::infinity();
    for (const std::vector<int>& column : model.columns) {
        if (column.empty()) continue;
        const double total = std::accumulate(
            column.begin(), column.end(), 0.0,
            [&](double sum, int node) { return sum + model.flow[static_cast<std::size_t>(node)]; });
        const double gaps = gap * static_cast<double>(column.size() - 1);
        scale = std::min(scale, (available_height - gaps) / total);
    }

    const std::size_t node_count = chart.nodes.size();
    std::vector<double> x(node_count), y(node_count), height(node_count);
    const double column_step = (right - left) / static_cast<double>(model.columns.size() - 1);
    for (std::size_t rank = 0; rank < model.columns.size(); ++rank) {
        const std::vector<int>& column = model.columns[rank];
        if (column.empty()) continue;
        double column_height = gap * static_cast<double>(column.size() - 1);
        for (const int node : column)
            column_height += model.flow[static_cast<std::size_t>(node)] * scale;
        double cursor = top + (available_height - column_height) / 2.0;
        for (const int node : column) {
            const std::size_t index = static_cast<std::size_t>(node);
            x[index] = left + static_cast<double>(rank) * column_step;
            y[index] = cursor;
            height[index] = model.flow[index] * scale;
            cursor += height[index] + gap;
        }
    }

    std::vector<std::vector<std::size_t>> incoming(node_count), outgoing(node_count);
    for (std::size_t i = 0; i < model.links.size(); ++i) {
        const SankeyModel::Link& link = model.links[i];
        outgoing[static_cast<std::size_t>(link.source)].push_back(i);
        incoming[static_cast<std::size_t>(link.target)].push_back(i);
    }

    // Ribbons attach to a node in slots stacked from its top, ordered by the
    // partner node's position so ribbons sharing a node never cross each other.
    const auto ordered_links = [&](const std::vector<std::size_t>& links, bool by_target) {
        std::vector<std::size_t> order = links;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            const int partner_a = by_target ? model.links[a].target : model.links[a].source;
            const int partner_b = by_target ? model.links[b].target : model.links[b].source;
            return y[static_cast<std::size_t>(partner_a)] < y[static_cast<std::size_t>(partner_b)];
        });
        return order;
    };
    const auto slot_offset = [&](const std::vector<std::size_t>& order, std::size_t link) {
        double offset = 0.0;
        for (const std::size_t candidate : order) {
            if (candidate == link) break;
            offset += model.links[candidate].value * scale;
        }
        return offset;
    };

    // Relaxation in the d3-sankey/Mermaid layout model: every node is pulled
    // toward the value-weighted mean of the positions that would let each of
    // its ribbons run straight (partner slot minus own slot offset), with
    // column-spanning ribbons weighted by their span. After each column the
    // nodes are reordered by position and re-packed to restore the gap — that
    // reordering is what untangles ribbon crossings between columns.
    const auto relax_node = [&](int node, bool by_incoming, double alpha) {
        const std::size_t index = static_cast<std::size_t>(node);
        const std::vector<std::size_t>& attached = by_incoming ? incoming[index] : outgoing[index];
        if (attached.empty()) return;
        const std::vector<std::size_t> own = ordered_links(attached, !by_incoming);
        double sum = 0.0;
        double weight = 0.0;
        for (const std::size_t link : attached) {
            const std::size_t partner = static_cast<std::size_t>(
                by_incoming ? model.links[link].source : model.links[link].target);
            const std::vector<std::size_t>& partner_links =
                by_incoming ? outgoing[partner] : incoming[partner];
            const double partner_slot =
                y[partner] + slot_offset(ordered_links(partner_links, by_incoming), link);
            const double span = static_cast<double>(
                model.rank[static_cast<std::size_t>(model.links[link].target)] -
                model.rank[static_cast<std::size_t>(model.links[link].source)]);
            const double link_weight = model.links[link].value * span;
            sum += (partner_slot - slot_offset(own, link)) * link_weight;
            weight += link_weight;
        }
        y[index] += (sum / weight - y[index]) * alpha;
    };
    const auto settle_column = [&](std::vector<int>& column) {
        std::stable_sort(column.begin(), column.end(), [&](int a, int b) {
            return y[static_cast<std::size_t>(a)] < y[static_cast<std::size_t>(b)];
        });
        double floor_y = top;
        for (const int node : column) {
            const std::size_t index = static_cast<std::size_t>(node);
            y[index] = std::max(y[index], floor_y);
            floor_y = y[index] + height[index] + gap;
        }
        double ceiling = top + available_height;
        for (auto it = column.rbegin(); it != column.rend(); ++it) {
            const std::size_t index = static_cast<std::size_t>(*it);
            y[index] = std::min(y[index], ceiling - height[index]);
            ceiling = y[index] - gap;
        }
    };
    // Crossing-minimisation relaxation reorders columns by position, so it runs
    // only in Auto mode; a value order is authoritative and must not be undone.
    // The centred packing above already gives sorted columns their final slots.
    if (chart.order == SankeyOrder::Auto) {
        double alpha = 1.0;
        for (int iteration = 0; iteration < 6; ++iteration) {
            for (std::size_t rank = model.columns.size() - 1; rank-- > 0;) {
                for (const int node : model.columns[rank]) relax_node(node, false, alpha);
                settle_column(model.columns[rank]);
            }
            for (std::size_t rank = 1; rank < model.columns.size(); ++rank) {
                for (const int node : model.columns[rank]) relax_node(node, true, alpha);
                settle_column(model.columns[rank]);
            }
            alpha *= 0.99;
        }
    }

    std::vector<double> source_y(model.links.size()), target_y(model.links.size());
    for (std::size_t node = 0; node < node_count; ++node) {
        double cursor = y[node];
        for (const std::size_t link : ordered_links(outgoing[node], true)) {
            source_y[link] = cursor;
            cursor += model.links[link].value * scale;
        }
        cursor = y[node];
        for (const std::size_t link : ordered_links(incoming[node], false)) {
            target_y[link] = cursor;
            cursor += model.links[link].value * scale;
        }
    }

    // All ribbon modes share one translucency so overlapping flows blend; the
    // node columns stay opaque. `default` paints a single neutral tone so the
    // columns carry the color, `source`/`target` take one endpoint's node
    // color, and `gradient` walks the source color into the target color along
    // the ribbon by filling one quad per Bézier segment.
    constexpr double kRibbonAlpha = 0.38;
    for (std::size_t i = 0; i < model.links.size(); ++i) {
        const SankeyModel::Link& link = model.links[i];
        const std::size_t source_index = static_cast<std::size_t>(link.source);
        const std::size_t target_index = static_cast<std::size_t>(link.target);
        const Color source_color =
            chart.nodes[source_index].color.value_or(chart_color(theme, source_index));
        const Color target_color =
            chart.nodes[target_index].color.value_or(chart_color(theme, target_index));
        const Point source_top{x[source_index] + node_width, source_y[i]};
        const Point target_top{x[target_index], target_y[i]};
        const double ribbon_height = link.value * scale;
        if (chart.link_color == SankeyLinkColor::Gradient) {
            const RibbonEdges edges = ribbon_edges(source_top, target_top, ribbon_height);
            const std::size_t segments = edges.top.size() - 1;
            for (std::size_t s = 0; s < segments; ++s) {
                ShapeStyle style;
                style.fill = cplot::lerp(source_color, target_color,
                                         (static_cast<double>(s) + 0.5) /
                                             static_cast<double>(segments))
                                 .with_alpha(kRibbonAlpha);
                panel.marks.push_back(PolygonItem{
                    {edges.top[s], edges.top[s + 1], edges.bottom[s + 1], edges.bottom[s]}, style});
            }
        } else {
            ShapeStyle style;
            switch (chart.link_color) {
            case SankeyLinkColor::Source: style.fill = source_color.with_alpha(kRibbonAlpha); break;
            case SankeyLinkColor::Target: style.fill = target_color.with_alpha(kRibbonAlpha); break;
            case SankeyLinkColor::Default:
            case SankeyLinkColor::Gradient:
                style.fill = theme.muted_text_color.with_alpha(kRibbonAlpha);
                break;
            }
            panel.marks.push_back(
                PolygonItem{ribbon_polygon(source_top, target_top, ribbon_height), style});
        }
    }
    for (std::size_t node = 0; node < node_count; ++node) {
        ShapeStyle style;
        style.fill = chart.nodes[node].color.value_or(chart_color(theme, node));
        panel.marks.push_back(RectItem{{x[node], y[node], node_width, height[node]}, style});
    }

    // Labels: measured boxes, decluttered vertically (a label must stay
    // beside its node horizontally), then drawn over a translucent
    // backdrop so ribbons behind a label dim instead of clashing with it.
    const TextMeasurer& measurer = default_text_measurer();
    struct NodeLabel {
        Point pos;
        HAlign align = HAlign::Left;
        double w = 0.0;
        double h = 0.0;
        std::size_t node = 0;
    };
    std::vector<NodeLabel> labels(node_count);
    for (std::size_t node = 0; node < node_count; ++node) {
        const bool sink = static_cast<std::size_t>(model.rank[node]) + 1 == model.columns.size();
        const TextMetrics metrics = measurer.measure(chart.nodes[node].label, font);
        labels[node] = {{sink ? x[node] - 6.0 : x[node] + node_width + 6.0,
                         y[node] + height[node] / 2.0},
                        sink ? HAlign::Right : HAlign::Left,
                        metrics.width,
                        metrics.height,
                        node};
    }
    const auto label_left = [](const NodeLabel& label) {
        return label.align == HAlign::Right ? label.pos.x - label.w : label.pos.x;
    };
    constexpr double kLabelPad = 2.0;
    for (int iteration = 0; iteration < 30; ++iteration) {
        bool moved = false;
        for (std::size_t i = 0; i < labels.size(); ++i)
            for (std::size_t j = i + 1; j < labels.size(); ++j) {
                NodeLabel& a = labels[i];
                NodeLabel& b = labels[j];
                const double overlap_x = std::min(label_left(a) + a.w, label_left(b) + b.w) -
                                         std::max(label_left(a), label_left(b));
                const double overlap_y = (a.h + b.h) / 2.0 + kLabelPad -
                                         std::abs(a.pos.y - b.pos.y);
                if (overlap_x <= 0.0 || overlap_y <= 0.0) continue;
                const double shift = overlap_y / 2.0 + 0.25;
                if (a.pos.y <= b.pos.y) {
                    a.pos.y -= shift;
                    b.pos.y += shift;
                } else {
                    a.pos.y += shift;
                    b.pos.y -= shift;
                }
                moved = true;
            }
        if (!moved) break;
    }
    ShapeStyle halo;
    halo.fill = theme.background.with_alpha(0.7);
    for (const NodeLabel& label : labels) {
        const double clamped =
            std::clamp(label.pos.y, label.h / 2.0, fig.height() - label.h / 2.0);
        TextItem item{{label.pos.x, clamped},
                      chart.nodes[label.node].label,
                      font,
                      theme.text_color,
                      label.align,
                      VAlign::Middle,
                      0.0};
        panel.over.push_back(text_halo(item, measurer, kLabelPad, halo));
        panel.over.push_back(std::move(item));
    }
    if (!chart.title.empty())
        add_text(panel.over, {fig.width() / 2.0, padding}, chart.title, title_font,
                 theme.text_color, HAlign::Center, VAlign::Top);
    scene.root.add(std::move(panel).build());
    return scene;
}

Scene build_scene(const Figure& fig, const TreemapChart& chart) {
    if (chart.roots.empty()) throw Error(cworks::validation_failed("treemap chart has no roots"));
    std::set<std::string> ids;
    std::map<const TreemapNode*, double> weights;
    for (const TreemapNode& root : chart.roots) (void)validate_treemap_node(root, ids, weights);
    const TreemapColorModel color_model = resolve_treemap_colors(chart, weights);
    const bool show_colorbar = chart.colorbar && color_model.active;

    Scene scene = base_scene(fig, chart.title);
    PanelLayers panel;
    const Theme& theme = fig.current_theme();
    const TextMeasurer& text = default_text_measurer();
    const Font title_font = theme.title_font();
    const double padding = std::max(12.0, theme.padding);
    const double title_height = chart.title.empty() ? 0.0 : title_font.size * 2.0;
    RectF content{padding, padding + title_height, fig.width() - 2.0 * padding,
                  fig.height() - 2.0 * padding - title_height};
    if (show_colorbar) content.w -= 56.0; // matches the axes colorbar reservation
    if (content.w <= 40.0 || content.h <= 40.0)
        throw Error(cworks::validation_failed("figure is too small for the treemap chart"));
    for (const auto& [root, rect] : layout_children(chart.roots, content, weights)) {
        const auto position = std::find_if(chart.roots.begin(), chart.roots.end(),
                                           [&](const TreemapNode& candidate) {
                                               return &candidate == root;
                                           });
        const std::size_t color = static_cast<std::size_t>(position - chart.roots.begin());
        draw_treemap_node(*root, rect, color, theme, text, weights, chart.color_scale,
                          color_model, panel,
                          chart.roots.size() == 1 && !root->children.empty());
    }
    if (show_colorbar)
        append_colorbar_scale(panel.over, chart.color_scale, color_model.data_min,
                              color_model.data_max, content, theme);
    if (!chart.title.empty())
        add_text(panel.over, {fig.width() / 2.0, padding}, chart.title, title_font,
                 theme.text_color, HAlign::Center, VAlign::Top);
    scene.root.add(std::move(panel).build());
    return scene;
}

Scene build_scene(const Figure& fig, const QuadrantChart& chart) {
    validate_quadrant(chart);
    Scene scene = base_scene(fig, chart.title);
    PanelLayers panel;
    const Theme& theme = fig.current_theme();
    const Font font = theme.base_font();
    const Font title_font = theme.title_font();
    const Font quadrant_font = theme.base_font();
    const double padding = std::max(16.0, theme.padding);
    const double title_height = chart.title.empty() ? 0.0 : title_font.size * 2.0;
    const double left = padding + 58.0;
    const double top = padding + title_height;
    const double bottom = padding + font.size * 2.2;
    const double right = padding + 16.0;
    const double size = std::min(fig.width() - left - right, fig.height() - top - bottom);
    if (size <= 100.0) throw Error(cworks::validation_failed(
        "figure is too small for the quadrant chart"));
    const double x0 = left;
    const double y0 = top;
    const double x1 = x0 + size;
    const double y1 = y0 + size;
    const double middle_x = (x0 + x1) / 2.0;
    const double middle_y = (y0 + y1) / 2.0;
    const RectF cells[4] = {{middle_x, y0, size / 2.0, size / 2.0},
                            {x0, y0, size / 2.0, size / 2.0},
                            {x0, middle_y, size / 2.0, size / 2.0},
                            {middle_x, middle_y, size / 2.0, size / 2.0}};
    const std::string labels[4] = {chart.quadrants.top_right, chart.quadrants.top_left,
                                   chart.quadrants.bottom_left,
                                   chart.quadrants.bottom_right};
    for (std::size_t quadrant = 0; quadrant < 4; ++quadrant) {
        ShapeStyle fill;
        fill.fill = chart_color(theme, quadrant).with_alpha(0.10);
        panel.under.push_back(RectItem{cells[quadrant], fill});
        if (!labels[quadrant].empty())
            add_text(panel.over,
                     {cells[quadrant].x + cells[quadrant].w / 2.0,
                      cells[quadrant].y + 6.0},
                     labels[quadrant], quadrant_font, theme.muted_text_color,
                     HAlign::Center, VAlign::Top);
    }
    ShapeStyle frame;
    frame.stroke = theme.axis_color;
    frame.stroke_width = std::max(1.0, theme.axis_stroke_width);
    panel.over.push_back(RectItem{{x0, y0, size, size}, frame});
    ShapeStyle midline = frame;
    midline.stroke_width = 1.0;
    panel.over.push_back(LineItem{{middle_x, y0}, {middle_x, y1}, midline});
    panel.over.push_back(LineItem{{x0, middle_y}, {x1, middle_y}, midline});

    if (!chart.x_axis.lower.empty())
        add_text(panel.over, {x0, y1 + 8.0}, chart.x_axis.lower, font,
                 theme.text_color, HAlign::Left, VAlign::Top);
    if (!chart.x_axis.upper.empty())
        add_text(panel.over, {x1, y1 + 8.0}, chart.x_axis.upper, font,
                 theme.text_color, HAlign::Right, VAlign::Top);
    if (!chart.y_axis.lower.empty())
        add_text(panel.over, {x0 - 16.0, y0 + size * 0.75}, chart.y_axis.lower,
                 font, theme.text_color, HAlign::Center, VAlign::Middle, -90.0);
    if (!chart.y_axis.upper.empty())
        add_text(panel.over, {x0 - 16.0, y0 + size * 0.25}, chart.y_axis.upper,
                 font, theme.text_color, HAlign::Center, VAlign::Middle, -90.0);

    for (std::size_t i = 0; i < chart.points.size(); ++i) {
        const QuadrantPoint& point = chart.points[i];
        const Point position{x0 + point.x * size, y1 - point.y * size};
        ShapeStyle dot;
        dot.fill = point.color.value_or(chart_color(theme, i));
        if (point.stroke_color || point.stroke_width) {
            dot.stroke = point.stroke_color.value_or(theme.axis_color);
            dot.stroke_width = point.stroke_width.value_or(1.0);
        }
        const double radius = point.radius.value_or(5.0);
        panel.marks.push_back(CircleItem{position, radius, dot});
        add_text(panel.over, {position.x + radius + 3.0, position.y}, point.label, font,
                 theme.text_color, HAlign::Left, VAlign::Middle);
    }
    if (!chart.title.empty())
        add_text(panel.over, {fig.width() / 2.0, padding}, chart.title, title_font,
                 theme.text_color, HAlign::Center, VAlign::Top);
    scene.root.add(std::move(panel).build());
    return scene;
}

Scene build_scene(const Figure& fig, const CalendarChart& chart) {
    const CalendarModel model = validate_calendar(chart);
    const std::vector<YearBand> bands = build_year_bands(model, chart.week_start);

    Scene scene = base_scene(fig, chart.title);
    PanelLayers panel;
    const Theme& theme = fig.current_theme();
    const TextMeasurer& text = default_text_measurer();
    const Font title_font = theme.title_font();
    const Font small_font = theme.tick_font();
    const double padding = std::max(16.0, theme.padding);
    const double title_height = chart.title.empty() ? 0.0 : title_font.size * 2.0;
    const double top = padding + title_height;

    double year_label_w = 0.0;
    for (const YearBand& band : bands)
        year_label_w =
            std::max(year_label_w, text.measure(std::to_string(band.year), small_font).width);
    const double weekday_label_w = small_font.size * 1.6;
    const double left_grid = padding + year_label_w + 6.0 + weekday_label_w;

    std::size_t weeks_max = 1;
    for (const YearBand& band : bands)
        weeks_max = std::max(weeks_max, static_cast<std::size_t>(band.week_count));

    const double right_reserve = chart.colorbar ? 56.0 : 0.0;
    const double available_width = fig.width() - left_grid - padding - right_reserve;

    const double month_label_h = small_font.size * 1.4;
    constexpr double kBandGap = 10.0;
    const double band_count = static_cast<double>(bands.size());
    const double available_height =
        fig.height() - top - padding - band_count * month_label_h - (band_count - 1.0) * kBandGap;

    const double cell_w = available_width / static_cast<double>(weeks_max);
    const double cell_h = available_height / (band_count * 7.0);
    double cell_size = std::min({cell_w, cell_h, 22.0});
    if (cell_size < 6.0) throw Error(cworks::validation_failed("figure is too small for the calendar chart"));

    // Center the bands vertically when the figure offers more height than
    // the width-bound cell_size actually needs (a wide-and-short year
    // strip in a tall figure should not hug the top with dead space below).
    const double content_height =
        band_count * (month_label_h + 7.0 * cell_size) + (band_count - 1.0) * kBandGap;
    const double content_top =
        top + std::max(0.0, (fig.height() - top - padding - content_height) / 2.0);
    double cursor_y = content_top;
    for (const YearBand& band : bands) {
        const double grid_top = cursor_y + month_label_h;
        add_text(panel.over, {padding + year_label_w, grid_top + 3.5 * cell_size},
                 std::to_string(band.year), small_font, theme.muted_text_color, HAlign::Right,
                 VAlign::Middle);
        for (int row = 0; row < 7; ++row)
            add_text(panel.over,
                     {left_grid - 6.0, grid_top + row * cell_size + cell_size / 2.0},
                     weekday_letter(row, chart.week_start), small_font, theme.muted_text_color,
                     HAlign::Right, VAlign::Middle);

        for (int month = 1; month <= 12; ++month) {
            const std::int64_t month_start = cworks::days_from_civil(band.year, month, 1);
            const std::int64_t next_month_first =
                month < 12 ? cworks::days_from_civil(band.year, month + 1, 1)
                          : cworks::days_from_civil(band.year + 1, 1, 1);
            const std::int64_t month_end = next_month_first - 1;
            const auto w0 = static_cast<double>((month_start - band.grid_start) / 7);
            const double d0 = static_cast<double>(calendar_weekday(month_start, chart.week_start));
            const auto w1 = static_cast<double>((month_end - band.grid_start) / 7);
            const double d1 = static_cast<double>(calendar_weekday(month_end, chart.week_start));

            add_text(panel.over, {left_grid + w0 * cell_size, cursor_y}, month_abbrev(month),
                     small_font, theme.muted_text_color, HAlign::Left, VAlign::Top);

            // A stepped outline that traces exactly the cells belonging to
            // this month within the week grid (the classic d3 calendar-view
            // "month path"): out along the first week's row, down the left
            // edge, along the last week's row, and back up the right edge.
            ShapeStyle outline;
            outline.stroke = theme.axis_color.with_alpha(0.35);
            outline.stroke_width = 1.0;
            std::vector<Point> path = {
                {left_grid + (w0 + 1.0) * cell_size, grid_top + d0 * cell_size},
                {left_grid + w0 * cell_size, grid_top + d0 * cell_size},
                {left_grid + w0 * cell_size, grid_top + 7.0 * cell_size},
                {left_grid + w1 * cell_size, grid_top + 7.0 * cell_size},
                {left_grid + w1 * cell_size, grid_top + (d1 + 1.0) * cell_size},
                {left_grid + (w1 + 1.0) * cell_size, grid_top + (d1 + 1.0) * cell_size},
                {left_grid + (w1 + 1.0) * cell_size, grid_top},
                {left_grid + (w0 + 1.0) * cell_size, grid_top},
            };
            panel.over.push_back(PolygonItem{std::move(path), outline});
        }

        for (std::int64_t day = band.jan1; day <= band.dec31; ++day) {
            const auto col = static_cast<double>((day - band.grid_start) / 7);
            const double row = static_cast<double>(calendar_weekday(day, chart.week_start));
            const double x = left_grid + col * cell_size;
            const double y = grid_top + row * cell_size;
            const auto found = model.values.find(day);
            const Color fill = found != model.values.end()
                                   ? chart.color_scale.color(found->second, model.data_min,
                                                             model.data_max)
                                   : chart.color_scale.missing;
            ShapeStyle cell_style;
            cell_style.fill = fill;
            constexpr double kCellGap = 1.5;
            panel.marks.push_back(
                RectItem{{x + kCellGap, y + kCellGap, cell_size - 2.0 * kCellGap,
                         cell_size - 2.0 * kCellGap},
                        cell_style});
        }

        cursor_y = grid_top + 7.0 * cell_size + kBandGap;
    }

    if (chart.colorbar) {
        const RectF grid_extent{left_grid, content_top, static_cast<double>(weeks_max) * cell_size,
                                cursor_y - kBandGap - content_top};
        append_colorbar_scale(panel.over, chart.color_scale, model.data_min, model.data_max,
                              grid_extent, theme);
    }

    if (!chart.title.empty())
        add_text(panel.over, {fig.width() / 2.0, padding}, chart.title, title_font,
                 theme.text_color, HAlign::Center, VAlign::Top);
    scene.root.add(std::move(panel).build());
    return scene;
}

Scene build_scene(const Figure& fig, const GaugeChart& chart) {
    validate_gauge(chart);
    Scene scene = base_scene(fig, chart.title);
    PanelLayers panel;
    const Theme& theme = fig.current_theme();
    const Font title_font = theme.title_font();
    const double padding = std::max(16.0, theme.padding);
    const double title_height = chart.title.empty() ? 0.0 : title_font.size * 2.0;
    const double top = padding + title_height;
    const RectF area{padding, top, fig.width() - 2.0 * padding, fig.height() - top - padding};
    if (area.w <= 60.0 || area.h <= 60.0)
        throw Error(cworks::validation_failed("figure is too small for the gauge chart"));

    const std::size_t count = chart.gauges.size();
    const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = static_cast<int>(
        std::ceil(static_cast<double>(count) / static_cast<double>(cols)));
    const double cell_w = area.w / static_cast<double>(cols);
    const double cell_h = area.h / static_cast<double>(rows);

    TickSet ticks;
    if (!chart.ticks.empty()) {
        for (double value : chart.ticks) {
            Tick tick;
            tick.value = value;
            tick.label = chart.value_format.empty() ? format_tick_value(value, 0.0)
                                                     : format_with(chart.value_format, value);
            ticks.ticks.push_back(std::move(tick));
        }
    } else {
        ticks = linear_ticks(chart.minimum, chart.maximum, 5, chart.value_format, false);
    }

    for (std::size_t i = 0; i < count; ++i) {
        const int row = static_cast<int>(i) / cols;
        const int col = static_cast<int>(i) % cols;
        const RectF cell{area.x + col * cell_w, area.y + row * cell_h, cell_w, cell_h};
        draw_gauge(chart, chart.gauges[i], cell, theme, ticks, panel);
    }

    if (!chart.title.empty())
        add_text(panel.over, {fig.width() / 2.0, padding}, chart.title, title_font,
                 theme.text_color, HAlign::Center, VAlign::Top);
    scene.root.add(std::move(panel).build());
    return scene;
}

} // namespace cplot::detail
