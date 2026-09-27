// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Typed models for charts whose layout is not Cartesian or polar. These
// models are deliberately independent of config files and frontends: callers
// construct them directly.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>


#include "color.hpp"
#include "colormap.hpp"

namespace cplot {

struct SankeyNode {
    std::string id;    ///< Stable identity used by links; never rendered.
    std::string label; ///< Human-readable node label.
    std::optional<Color> color;
};

struct SankeyLink {
    std::string source;
    std::string target;
    double value = 0.0;
};

/// Horizontal node placement across columns — the d3-sankey/Mermaid
/// alignment family. Justify (the Mermaid default) pulls every terminal
/// node to the last column so all sinks share the right edge; Left keeps
/// each node at its earliest stage (longest path from a source), so nodes
/// of the same stage share one column; Right places each node at its
/// latest stage (counted back from the sinks); Center keeps nodes with
/// inflow at their earliest stage and moves pure sources just left of
/// their first target.
enum class SankeyAlignment { Justify, Left, Right, Center };

/// Vertical ordering of the nodes stacked within a single column. This is
/// orthogonal to SankeyAlignment, which chooses the column: ordering only
/// decides top-to-bottom placement inside a column.
///
/// Auto (the default) runs the d3-sankey/Mermaid relaxation, which reorders
/// each column to minimise ribbon crossings — best for a general flow graph.
/// Descending and Ascending instead pin each column to its node-flow order
/// (largest-on-top / smallest-on-top) and skip the crossing relaxation, so a
/// node that changes rank between stages produces a deliberately crossing
/// ribbon. That value order is the convention for alluvial "share over time"
/// charts, where the crossings are the message.
enum class SankeyOrder { Auto, Ascending, Descending };

/// How each ribbon between two nodes is colored. Mirrors Google Charts'
/// Sankey link.colorMode. Default paints every ribbon one neutral (theme
/// muted) tone so the node columns carry the color; Source and Target paint
/// each ribbon with its source or target node's color; Gradient interpolates
/// the source node color into the target node color along the ribbon (see
/// build_scene in structured_chart_layout.cpp). All modes share the same
/// ribbon translucency so overlapping flows blend, and all compose with
/// SankeyOrder, which only decides geometry.
enum class SankeyLinkColor { Default, Source, Target, Gradient };

struct SankeyChart {
    std::string title;
    std::vector<SankeyNode> nodes;
    std::vector<SankeyLink> links;
    SankeyAlignment alignment = SankeyAlignment::Justify;
    SankeyOrder order = SankeyOrder::Auto;
    SankeyLinkColor link_color = SankeyLinkColor::Default;

    SankeyChart& node(std::string id, std::string label,
                      std::optional<Color> color = std::nullopt);
    SankeyChart& link(std::string source, std::string target, double value);
    std::size_t mark_count() const noexcept { return nodes.size() + links.size(); }
};


struct TreemapNode {
    std::string id;    ///< Stable identity; never rendered.
    std::string label;
    /// Required and positive for leaves. For branches it may be absent, or
    /// equal the sum of the children as an independently supplied check.
    std::optional<double> value;
    /// Optional continuous color measure, independent of `value` (which
    /// drives tile SIZE) — e.g. size = revenue, color = growth %. Set this
    /// on leaves; a branch's is always computed (see TreemapChart::color_scale)
    /// as the size-weighted mean of its children's resolved color values,
    /// so any color_value placed directly on a branch is ignored. A leaf
    /// with no color_value renders with the color scale's `missing` color;
    /// a chart where no node anywhere carries one keeps the original
    /// rotating per-branch palette, unchanged.
    std::optional<double> color_value;
    std::vector<TreemapNode> children;
};

struct TreemapChart {
    std::string title;
    std::vector<TreemapNode> roots;
    /// Maps each node's resolved color_value to a fill color (see
    /// TreemapNode::color_value). Only takes effect once at least one node
    /// in the chart resolves a color measure; configuring this alone, with
    /// no color_value anywhere in the data, has no visible effect and the
    /// chart keeps the original rotating-palette coloring.
    ColorScale color_scale;
    /// Draw a colorbar legend beside the treemap when a color measure is
    /// resolved anywhere in the chart (mirrors HeatmapSeries::colorbar).
    bool colorbar = false;

    std::size_t mark_count() const noexcept;
};


struct AxisEndpoints {
    std::string lower;
    std::string upper;
};

struct QuadrantLabels {
    std::string top_right;
    std::string top_left;
    std::string bottom_left;
    std::string bottom_right;
};

struct QuadrantPoint {
    std::string id;    ///< Stable identity; never rendered.
    std::string label;
    double x = 0.0;    ///< Normalized to [0, 1].
    double y = 0.0;    ///< Normalized to [0, 1].
    std::optional<Color> color;
    std::optional<double> radius;       ///< dot radius in px (default 5)
    std::optional<Color> stroke_color;  ///< dot outline colour
    std::optional<double> stroke_width; ///< dot outline width
};

struct QuadrantChart {
    std::string title;
    AxisEndpoints x_axis;
    AxisEndpoints y_axis;
    QuadrantLabels quadrants;
    std::vector<QuadrantPoint> points;

    QuadrantChart& point(std::string id, std::string label, double x, double y,
                         std::optional<Color> color = std::nullopt);
    std::size_t mark_count() const noexcept { return points.size(); }
};


/// Day-of-week convention for laying out a calendar heatmap's week
/// columns. ISO (Monday-start) is the default — a deliberate difference
/// from Google's calendar chart, which defaults to Sunday-start.
enum class WeekStart { Monday, Sunday };

struct CalendarDay {
    /// Days since 1970-01-01, proleptic Gregorian (see
    /// cworks::days_from_civil) — never a libc time_t, never local time.
    std::int64_t day = 0;
    double value = 0.0;
};

struct CalendarChart {
    std::string title;
    std::vector<CalendarDay> days;
    WeekStart week_start = WeekStart::Monday;
    /// Maps each day's value to a fill color (mirrors HeatmapSeries'
    /// ColorScale). A day with no entry renders with `color_scale.missing`.
    ColorScale color_scale;
    /// Draw a colorbar legend beside the calendar.
    bool colorbar = false;

    CalendarChart& day(std::int64_t days_since_epoch, double value);
    std::size_t mark_count() const noexcept { return days.size(); }
};


/// One colored arc segment of a gauge's scale (e.g. a "green/yellow/red"
/// zone). `from`/`to` are in the gauge's data domain, not normalized.
struct GaugeBand {
    double from = 0.0;
    double to = 0.0;
    Color color;
    std::string label;
};

struct GaugeItem {
    std::string label;
    double value = 0.0;
};

struct GaugeChart {
    std::string title;
    std::vector<GaugeItem> gauges;
    double minimum = 0.0;
    double maximum = 100.0;
    std::vector<GaugeBand> bands;
    /// Explicit tick positions; empty means auto (nice ticks over
    /// [minimum, maximum] — see cplot::detail::linear_ticks).
    std::vector<double> ticks;
    /// Suffix appended to the value readout (e.g. "%", " rpm").
    std::string unit;
    /// printf-style format for the value readout and tick labels (mirrors
    /// HeatmapSeries::value_format); empty uses the default shortest-form
    /// numeric formatting.
    std::string value_format;

    GaugeChart& gauge(std::string label, double value);
    std::size_t mark_count() const noexcept { return gauges.size(); }
};


} // namespace cplot
