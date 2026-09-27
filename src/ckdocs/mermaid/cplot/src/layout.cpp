// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Layout engine: resolves scales and ticks, measures text, computes the
// plot area, and assembles the backend-independent scene. Deterministic:
// the same figure always produces the same scene. Supports subplot grids,
// a secondary y axis, and stacked bars.
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

#include <cworks/app_error.hpp>
#include <cworks/trig.hpp>

#include "cplot/text.hpp"
#include "decade.hpp"
#include "internal.hpp"

namespace cplot::detail {

namespace {

struct Range {
    double lo = 0.0, hi = 1.0;
    bool valid = false;
    void include(double v) {
        if (!std::isfinite(v)) return;
        if (!valid) {
            lo = hi = v;
            valid = true;
            return;
        }
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
};

struct ResolvedAxes {
    Scale x;
    Scale y;
    Scale y2;
    Scale x2;
    TickSet x_ticks;
    TickSet y_ticks;
    TickSet y2_ticks;
    TickSet x2_ticks;
    bool has_y2 = false;
    bool has_x2 = false;
};

/// The domains a shared axis forces onto a panel (Figure::x_extent /
/// y_extent): fully built scales whose pixel ranges are set per panel; ticks
/// are still generated from each panel's own axis settings.
struct SharedDomains {
    std::optional<Scale> x;
    std::optional<Scale> y;
};

/// Panel framing: the margins between cell and plot area, plus the
/// x-label rotation decision (which feeds the bottom margin).
struct PanelFrame {
    double left = 0.0;
    double right = 0.0;
    double top = 0.0;
    double bottom = 0.0;
    bool rotate_x_labels = false;
};

/// Per-panel build options for subplot grids with shared axes.
struct PanelOptions {
    const SharedDomains* shared = nullptr;
    const PanelFrame* frame = nullptr; ///< externally aligned margins
    bool suppress_x_text = false; ///< shared x: tick/axis labels only on the bottom row
    bool suppress_y_text = false; ///< shared y: tick/axis labels only on the left column
};

Scale make_numeric(const Axis& axis, const Range& data) {
    double lo = data.valid ? data.lo : 0.0;
    double hi = data.valid ? data.hi : 1.0;
    if (axis.explicit_range()) {
        lo = axis.explicit_range()->first;
        hi = axis.explicit_range()->second;
    }
    switch (axis.scale_kind()) {
    case ScaleKind::Log10: {
        if (!(lo > 0)) lo = std::max(hi * 1e-6, 1e-12);
        if (!axis.explicit_range()) {
            // Snap the domain onto whole decades. Which decade a bound falls
            // in is a discrete decision — it fixes how many decades of ticks
            // the axis carries — so it is decided by comparison against the
            // decade boundaries, not by flooring a logarithm (decade.hpp).
            lo = decade_value(decade_floor(lo));
            hi = decade_value(decade_ceil(hi));
            if (lo == hi) hi = lo * 10.0;
        }
        return Scale::log10(lo, hi);
    }
    case ScaleKind::Log2:
    case ScaleKind::Ln: {
        // Unlike log10, non-positive data on a log2/ln axis is an error
        // (never silently clamped — a charter rule).
        if (!(lo > 0))
            throw Error(cworks::validation_failed(
                "log axis requires positive values; the data minimum is " + std::to_string(lo) +
                " (use a symlog axis for signed data)"));
        const double base = axis.scale_kind() == ScaleKind::Log2 ? 2.0 : kEuler;
        if (!axis.explicit_range()) {
            lo = decade_value(decade_floor(lo, base), base);
            hi = decade_value(decade_ceil(hi, base), base);
            if (lo == hi) hi = lo * base;
        }
        return axis.scale_kind() == ScaleKind::Log2 ? Scale::log2(lo, hi) : Scale::ln(lo, hi);
    }
    case ScaleKind::Symlog: {
        // Symlog admits negatives and zero; do NOT clamp lo > 0.
        if (!axis.explicit_range()) {
            double span = hi - lo;
            if (span <= 0) span = 1.0;
            lo -= span * 0.02;
            hi += span * 0.02;
        }
        return Scale::symlog(lo, hi, axis.symlog_linthresh());
    }
    case ScaleKind::DateTime: {
        if (!axis.explicit_range()) {
            double span = hi - lo;
            if (span <= 0) span = 3600.0;
            lo -= span * 0.02;
            hi += span * 0.02;
        }
        return Scale::datetime(lo, hi);
    }
    case ScaleKind::Custom: {
        if (!axis.explicit_range()) {
            const NiceRange nice = nice_range(lo, hi, axis.tick_count_target());
            lo = nice.lo;
            hi = nice.hi;
        }
        return Scale::transform(axis.custom_scale_name(), lo, hi);
    }
    default: {
        if (!axis.explicit_range()) {
            const NiceRange nice = nice_range(lo, hi, axis.tick_count_target());
            // The ticks stay on the nice bounds; only the frame (the pixel
            // mapping domain) is widened, so data/error extremes that reach a
            // nice bound are not drawn flush on the frame — where the series
            // clip shaves marks that sit on the boundary (e.g. an error-bar
            // cap). The frame is only ever extended outward (a chart with slack
            // is unchanged) and never crosses zero, so bar/area baselines stay
            // anchored at 0. Ticks are generated over the nice bounds via
            // set_tick_domain, so tick count, step, and labels are unchanged.
            double frame_lo = nice.lo, frame_hi = nice.hi;
            if (data.valid) {
                const double pad = (nice.hi - nice.lo) * 0.03;
                frame_lo = std::min(nice.lo, data.lo - pad);
                frame_hi = std::max(nice.hi, data.hi + pad);
                if (nice.lo >= 0.0 && frame_lo < 0.0) frame_lo = 0.0;
                if (nice.hi <= 0.0 && frame_hi > 0.0) frame_hi = 0.0;
            }
            Scale s = Scale::linear(frame_lo, frame_hi);
            s.set_tick_domain(nice.lo, nice.hi);
            return s;
        }
        return Scale::linear(lo, hi);
    }
    }
}

TickSet make_ticks(const Axis& axis, const Scale& scale) {
    TickSet set;
    if (!axis.fixed_ticks().empty()) {
        for (double v : axis.fixed_ticks()) {
            const std::string& f = axis.tick_format_text();
            std::string label;
            if (scale.kind() == ScaleKind::DateTime) {
                label = format_datetime(v, f.empty() ? "%Y-%m-%d" : f);
            } else {
                label = f.empty() ? format_tick_value(v, 0.0) : format_with(f, v);
            }
            set.ticks.push_back({v, std::move(label), false});
        }
    } else {
        set = scale.ticks(axis.tick_count_target(), axis.tick_format_text(),
                          axis.minor_ticks_enabled());
    }
    if (axis.absolute_labels_enabled()) {
        // Drop the sign from the finished label rather than re-formatting
        // |value|: the text stays byte-identical to its positive twin under
        // every format path (default, printf, "%g" fallbacks).
        for (auto& t : set.ticks) {
            if (t.value < 0.0 && !t.label.empty() && t.label.front() == '-')
                t.label.erase(0, 1);
        }
    }
    return set;
}

/// Coordinate system shared by all visible series of a panel.
/// Mixing incompatible systems is rejected with a clear error.
const char* coordinate_system_name(CoordinateSystem cs) {
    switch (cs) {
    case CoordinateSystem::PartToWhole: return "pie/doughnut";
    case CoordinateSystem::Radar: return "radar";
    case CoordinateSystem::Polar: return "polar";
    default: return "cartesian";
    }
}

CoordinateSystem panel_coordinate_system(const Axes& axes) {
    CoordinateSystem cs = CoordinateSystem::Cartesian;
    bool any = false;
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible()) continue;
        const CoordinateSystem c = s->coordinate_system();
        if (!any) {
            cs = c;
            any = true;
        } else if (c != cs) {
            throw Error(cworks::validation_failed(std::string("cannot mix ") +
                coordinate_system_name(cs) + " and " + coordinate_system_name(c) +
                " series in one axes panel"));
        }
    }
    if (cs != CoordinateSystem::Cartesian && !axes.annotations().empty()) {
        throw Error(cworks::validation_failed(std::string("annotations are not supported on ") +
            coordinate_system_name(cs) + " axes"));
    }
    return cs;
}

/// Validate stacked area series in a panel: consistent mode, identical
/// finite x coordinates. Returns the stacked area series in order.
std::vector<const AreaSeries*> stacked_area_series(const Axes& axes) {
    std::vector<const AreaSeries*> stacked;
    bool any_percent = false, any_plain = false;
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible()) continue;
        const auto* area = dynamic_cast<const AreaSeries*>(s.get());
        if (!area || !area->is_stacked()) continue;
        (area->is_percent_stacked() ? any_percent : any_plain) = true;
        for (double v : area->x_data()) {
            if (!std::isfinite(v))
                throw Error(cworks::validation_failed(
                    "stacked area series requires finite x values"));
        }
        for (double v : area->y_data()) {
            if (!std::isfinite(v))
                throw Error(cworks::validation_failed(
                    "stacked area series requires finite y values "
                    "(use missing: zero or clean the data)"));
        }
        if (!stacked.empty() && area->x_data() != stacked.front()->x_data()) {
            throw Error(cworks::validation_failed(
                "stacked area series must share identical x coordinates"));
        }
        stacked.push_back(area);
    }
    if (any_percent && any_plain) {
        throw Error(cworks::validation_failed(
            "cannot mix stacked and 100% stacked area series in one axes"));
    }
    if (any_percent) {
        for (const AreaSeries* area : stacked) {
            for (double v : area->y_data()) {
                if (v < 0.0)
                    throw Error(cworks::validation_failed(
                        "100% stacked area requires non-negative values"));
            }
        }
    }
    return stacked;
}

/// Per-point totals across stacked area series (for percent normalization).
std::vector<double> stacked_area_totals(const std::vector<const AreaSeries*>& stacked) {
    std::vector<double> totals;
    for (const AreaSeries* area : stacked) {
        const auto& ys = area->y_data();
        if (totals.size() < ys.size()) totals.resize(ys.size(), 0.0);
        for (std::size_t i = 0; i < ys.size(); ++i) totals[i] += ys[i];
    }
    return totals;
}

/// Validate stacked bar series (consistent percent mode) and return
/// per-category totals for 100% stacks. Returns true when percent mode.
bool stacked_bar_percent_mode(const Axes& axes, std::vector<double>& totals) {
    bool any_percent = false, any_plain = false;
    totals.clear();
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible()) continue;
        const auto* bar = dynamic_cast<const BarSeries*>(s.get());
        if (!bar || !bar->is_stacked()) continue;
        (bar->is_percent_stacked() ? any_percent : any_plain) = true;
        const auto& vals = bar->values();
        if (totals.size() < vals.size()) totals.resize(vals.size(), 0.0);
        for (std::size_t i = 0; i < vals.size(); ++i) {
            if (std::isfinite(vals[i])) totals[i] += vals[i];
        }
    }
    if (any_percent && any_plain) {
        throw Error(cworks::validation_failed(
            "cannot mix stacked and 100% stacked bar series in one axes"));
    }
    if (any_percent) {
        for (const auto& s : axes.series_list()) {
            if (!s->is_visible()) continue;
            const auto* bar = dynamic_cast<const BarSeries*>(s.get());
            if (!bar || !bar->is_percent_stacked()) continue;
            for (double v : bar->values()) {
                if (std::isfinite(v) && v < 0.0)
                    throw Error(cworks::validation_failed(
                        "100% stacked bars require non-negative values"));
            }
        }
    }
    return any_percent;
}

/// Decide scale kinds and ranges from axis config plus series data.
/// Also computes stacked-bar totals for correct value-axis ranges.
ResolvedAxes resolve_scales(const Axes& axes, const SharedDomains* shared = nullptr) {
    const auto& series = axes.series_list();

    bool cat_x = false, cat_y = false;
    bool has_xy_category_series = false;
    std::vector<std::string> x_categories;
    std::vector<std::string> y_categories;
    const auto merge_categories = [](std::vector<std::string>& dst,
                                     const std::vector<std::string>& src) {
        for (const auto& value : src) {
            if (std::find(dst.begin(), dst.end(), value) == dst.end()) dst.push_back(value);
        }
    };
    for (const auto& s : series) {
        if (!s->is_visible()) continue;
        // A category-x series bound to x2 is incoherent: x2 is always a
        // numeric scale, and its categories would fold into the primary x.
        if (s->uses_x2() && s->wants_category_x()) {
            throw Error(cworks::validation_failed(
                "axis: x2 requires a numeric-x series; this series uses a category x axis"));
        }
        if (s->wants_category_x()) {
            cat_x = true;
            merge_categories(x_categories, s->x_category_names());
        }
        if (s->wants_category_y()) {
            cat_y = true;
            merge_categories(y_categories, s->y_category_names());
        }
        if (s->wants_category_x() && s->wants_category_y()) has_xy_category_series = true;
    }
    if (cat_x && cat_y && !has_xy_category_series) {
        throw Error(cworks::validation_failed(
            "cannot mix vertical and horizontal bar series in one axes"));
    }

    Range x_range, y_range, y2_range, x2_range;
    bool any_y2 = false;
    bool any_x2 = false;

    // Stacked area series: the y range covers cumulative totals, not the
    // individual series values.
    const std::vector<const AreaSeries*> stacked_areas = stacked_area_series(axes);
    if (!stacked_areas.empty()) {
        const bool percent = stacked_areas.front()->is_percent_stacked();
        for (double v : stacked_areas.front()->x_data()) x_range.include(v);
        y_range.include(0.0);
        if (percent) {
            y_range.include(100.0);
        } else {
            std::vector<double> pos, neg;
            for (const AreaSeries* area : stacked_areas) {
                const auto& ys = area->y_data();
                if (pos.size() < ys.size()) {
                    pos.resize(ys.size(), 0.0);
                    neg.resize(ys.size(), 0.0);
                }
                for (std::size_t i = 0; i < ys.size(); ++i) {
                    if (ys[i] >= 0.0)
                        pos[i] += ys[i];
                    else
                        neg[i] += ys[i];
                }
            }
            for (double v : pos) y_range.include(v);
            for (double v : neg) y_range.include(v);
        }
    }

    // Stacked totals per category (positive and negative separately).
    std::vector<double> stack_pos, stack_neg;
    bool any_stacked = false, stacked_horizontal = false;

    for (const auto& s : series) {
        if (!s->is_visible()) continue;
        const auto* stacked_area = dynamic_cast<const AreaSeries*>(s.get());
        if (stacked_area && stacked_area->is_stacked()) continue; // handled above
        const auto* bar = dynamic_cast<const BarSeries*>(s.get());
        if (bar && bar->is_stacked()) {
            any_stacked = true;
            stacked_horizontal = bar->is_horizontal();
            const auto& vals = bar->values();
            if (stack_pos.size() < vals.size()) {
                stack_pos.resize(vals.size(), 0.0);
                stack_neg.resize(vals.size(), 0.0);
            }
            for (std::size_t i = 0; i < vals.size(); ++i) {
                if (!std::isfinite(vals[i])) continue;
                if (vals[i] >= 0)
                    stack_pos[i] += vals[i];
                else
                    stack_neg[i] += vals[i];
            }
            continue; // extent handled via totals below
        }
        const Extent e = s->extent();
        if (!e.valid) continue;
        Range& xr = s->uses_x2() ? (any_x2 = true, x2_range) : x_range;
        xr.include(e.x_lo);
        xr.include(e.x_hi);
        Range& yr = s->uses_y2() ? (any_y2 = true, y2_range) : y_range;
        yr.include(e.y_lo);
        yr.include(e.y_hi);
    }
    for (const auto& a : axes.annotations()) {
        switch (a.kind) {
        case AnnotationKind::HLine:
            y_range.include(a.a);
            break;
        case AnnotationKind::VLine:
            x_range.include(a.a);
            break;
        case AnnotationKind::SpanX:
            x_range.include(a.a);
            x_range.include(a.b);
            break;
        case AnnotationKind::SpanY:
            y_range.include(a.a);
            y_range.include(a.b);
            break;
        case AnnotationKind::Text:
        case AnnotationKind::Callout:
            x_range.include(a.a);
            y_range.include(a.b);
            break;
        case AnnotationKind::Arrow:
            x_range.include(a.a);
            x_range.include(a.c);
            y_range.include(a.b);
            y_range.include(a.d);
            break;
        case AnnotationKind::SigBracket:
            // Keep the bracket (and room for its label) inside the panel.
            x_range.include(a.a);
            x_range.include(a.c);
            y_range.include(a.b);
            break;
        }
    }
    if (any_stacked) {
        Range& vr = stacked_horizontal ? x_range : y_range;
        vr.include(0.0);
        std::vector<double> bar_totals;
        if (stacked_bar_percent_mode(axes, bar_totals)) {
            vr.include(100.0);
        } else {
            for (double v : stack_pos) vr.include(v);
            for (double v : stack_neg) vr.include(v);
        }
    }

    // A mirrored magnitude x scale (unjoined population pyramid): nice
    // ticks are chosen over [0, magnitude] and mirrored onto both sides.
    bool mirrored_x = false;
    for (const auto& s : series) {
        if (s->is_visible() && s->wants_mirrored_value_axis()) mirrored_x = true;
    }

    ResolvedAxes r;
    r.has_y2 = any_y2;
    r.has_x2 = any_x2;
    if (cat_x) {
        r.x = Scale::category(x_categories);
    } else if (mirrored_x) {
        double magnitude = std::max(std::abs(x_range.lo), std::abs(x_range.hi));
        if (axes.x_axis().explicit_range()) {
            const auto& er = *axes.x_axis().explicit_range();
            magnitude = std::max(std::abs(er.first), std::abs(er.second));
        } else {
            magnitude = nice_range(0.0, magnitude, axes.x_axis().tick_count_target()).hi;
        }
        r.x = Scale::mirrored(magnitude);
    } else {
        r.x = make_numeric(axes.x_axis(), x_range);
    }
    r.y = cat_y ? Scale::category(y_categories) : make_numeric(axes.y_axis(), y_range);
    if (any_y2) r.y2 = make_numeric(axes.y2_axis(), y2_range);
    // The secondary (top) x axis is always numeric and, like y2, is never
    // overridden by SharedDomains.
    if (any_x2) r.x2 = make_numeric(axes.x2_axis(), x2_range);
    // Shared subplot axes replace the panel-local domain wholesale;
    // the panel keeps its own tick settings (format, count, minor).
    if (shared && shared->x) r.x = *shared->x;
    if (shared && shared->y) r.y = *shared->y;
    r.x_ticks = r.x.kind() == ScaleKind::Category ? r.x.ticks(0, "", false)
                                                  : make_ticks(axes.x_axis(), r.x);
    r.y_ticks = r.y.kind() == ScaleKind::Category ? r.y.ticks(0, "", false)
                                                  : make_ticks(axes.y_axis(), r.y);
    if (any_y2) r.y2_ticks = make_ticks(axes.y2_axis(), r.y2);
    if (any_x2) r.x2_ticks = make_ticks(axes.x2_axis(), r.x2);
    return r;
}

/// A stroke-only style. `cap`/`join` default to SVG's initial values;
/// the rounded variant is what line-like marks (legend swatches, label
/// leaders) want so their ends do not read as clipped.
ShapeStyle stroke_style(Color c, double w, LineCap cap = LineCap::Butt,
                        LineJoin join = LineJoin::Miter) {
    ShapeStyle s;
    s.stroke = c;
    s.stroke_width = w;
    s.cap = cap;
    s.join = join;
    return s;
}

// -- outside-legend sizing shared by reservation and rendering -----------
//
// Constants and helpers below are used both when compute_frame (or a
// simple panel's plot-rect setup) reserves space for a Top/Bottom/Left/
// Right legend and when append_legend actually draws it. They must
// agree exactly, so both call the same functions rather than duplicating
// the arithmetic.
constexpr double kLegendSwatchW = 22.0;
constexpr double kLegendPadIn = 9.0;
constexpr double kLegendSwatchGap = 8.0;
constexpr double kLegendItemGap = 18.0;   // gap between items sharing a row (Top/Bottom)
constexpr double kLegendLineGap = 6.0;    // gap between wrapped rows or columns
constexpr double kLegendOutsideGap = 10.0; // gap between the plot edge and an outside legend

// -- labeled (callout) legend for pie/doughnut --------------------------------
// Deterministic leader-line geometry for LegendPosition::Labeled. A slice's
// leader runs a short radial stub past its arc, then a straight connector to a
// per-side label column; the text sits a small gap beyond the column. See
// append_pie_callouts for the two-pass vertical declutter.
constexpr double kLabeledStub = 10.0;         // radial stub past the outer arc
constexpr double kLabeledConnector = 20.0;    // run from the stub to the label column
constexpr double kLabeledTextGap = 5.0;       // column -> text gap
constexpr double kLabeledLineGap = 4.0;       // extra vertical pitch between labels
constexpr double kMinLabeledSliceAngleDeg = 8.0; // below this smallest span, fall back

/// Per-entry measurements shared by every legend layout decision. All
/// entries share one box size (the widest label), so layout below is a
/// closed-form grid computation, not iterative packing.
struct LegendMetrics {
    double text_w = 0.0;
    double entry_h = 0.0;
};

LegendMetrics measure_legend(const std::vector<LegendItemInfo>& entries, const Theme& theme,
                             const TextMeasurer& tm) {
    LegendMetrics m;
    const Font legend_font = theme.legend_font();
    for (const auto& e : entries) {
        const TextMetrics tm_ = tm.measure(e.label, legend_font);
        m.text_w = std::max(m.text_w, tm_.width);
        m.entry_h = std::max(m.entry_h, tm_.height + 6.0);
    }
    return m;
}

double legend_column_width(const LegendMetrics& m) {
    return kLegendPadIn * 2.0 + kLegendSwatchW + kLegendSwatchGap + m.text_w;
}

/// Deterministic uniform-pitch wrap layout for an outside legend: `n`
/// entries are placed row-major (Top/Bottom, wrapping into more rows once
/// a row would exceed `available_cross` in width) or column-major
/// (Left/Right, wrapping into more columns once a column would exceed
/// `available_cross` in height). `items_per_line` is items-per-row for
/// horizontal, rows-per-column for vertical; `line_count` is the number
/// of rows (horizontal) or columns (vertical) that results.
struct LegendGrid {
    std::size_t items_per_line = 0;
    std::size_t line_count = 0;
    double total_w = 0.0; ///< meaningful for vertical (Left/Right) layouts
    double total_h = 0.0; ///< meaningful for horizontal (Top/Bottom) layouts
};

LegendGrid layout_legend_grid(const LegendMetrics& m, std::size_t n, double available_cross,
                              bool horizontal) {
    LegendGrid g;
    if (n == 0) return g;
    if (horizontal) {
        const double item_w = kLegendSwatchW + kLegendSwatchGap + m.text_w;
        const double usable = std::max(item_w, available_cross - kLegendPadIn * 2.0);
        g.items_per_line = std::max<std::size_t>(
            1, static_cast<std::size_t>(
                   std::floor((usable + kLegendItemGap) / (item_w + kLegendItemGap))));
        g.line_count = (n + g.items_per_line - 1) / g.items_per_line;
        g.total_h = kLegendPadIn * 2.0 + m.entry_h * static_cast<double>(g.line_count) +
                    kLegendLineGap * static_cast<double>(g.line_count - 1);
    } else {
        const double usable = std::max(m.entry_h, available_cross - kLegendPadIn * 2.0);
        g.items_per_line =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(usable / m.entry_h)));
        g.line_count = (n + g.items_per_line - 1) / g.items_per_line;
        const double col_w = legend_column_width(m);
        g.total_w = col_w * static_cast<double>(g.line_count) +
                    kLegendLineGap * static_cast<double>(g.line_count - 1);
    }
    return g;
}

/// Resolves LegendPosition::Auto against whether there is anything to
/// show. Shared by reservation and rendering so both agree on what
/// "the legend" resolves to for a given panel.
LegendPosition resolve_legend_position(LegendPosition lp, bool has_entries) {
    if (lp == LegendPosition::Auto) return has_entries ? LegendPosition::TopRight : LegendPosition::None;
    return lp;
}

bool is_outside_legend(LegendPosition lp) {
    return lp == LegendPosition::Top || lp == LegendPosition::Bottom ||
           lp == LegendPosition::Left || lp == LegendPosition::Right;
}

/// The extent (width for Left/Right, height for Top/Bottom) an outside
/// legend needs along its wrap axis. Zero for inside/None positions or
/// when there is nothing to draw -- callers must not reserve space for a
/// legend append_legend will not render.
double outside_legend_extent(LegendPosition resolved, const std::vector<LegendItemInfo>& entries,
                             double available_cross, const Theme& theme, const TextMeasurer& tm) {
    if (!is_outside_legend(resolved) || entries.empty()) return 0.0;
    const LegendMetrics m = measure_legend(entries, theme, tm);
    const bool horizontal = resolved == LegendPosition::Top || resolved == LegendPosition::Bottom;
    const LegendGrid grid = layout_legend_grid(m, entries.size(), available_cross, horizontal);
    return (horizontal ? grid.total_h : grid.total_w) + kLegendOutsideGap;
}

/// Collect legend entries in series order with the exact same per-series
/// resolved color every geometry-building loop uses (explicit color, else
/// the theme's palette by color index; hidden series still advance the
/// index so a toggled series doesn't reshuffle everyone else's color).
/// Independent of plot geometry, so it can run before the plot rect --
/// and therefore any outside-legend reservation -- is finalized.
std::vector<LegendItemInfo> collect_legend_entries(const Axes& axes, const Theme& theme) {
    std::vector<LegendItemInfo> entries;
    std::size_t color_index = 0;
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible()) {
            ++color_index;
            continue;
        }
        const Color c = s->explicit_color() ? *s->explicit_color() : theme.series_color(color_index);
        for (auto& entry : s->legend_items(c, theme)) entries.push_back(std::move(entry));
        ++color_index;
    }
    return entries;
}

// -- inside-legend collision avoidance ------------------------------------
//
// An inside-corner legend is drawn *over* the plot (it never reserves its
// own margin, unlike an outside legend), so nothing stops it from landing
// on top of the very data it labels. The helpers below test a candidate
// corner box against the panel's own already-built data marks -- the exact
// bars/lines/markers/points/wedges build_geometry produced -- so the corner
// search agrees byte-for-byte with what gets rendered.
//
// ⚠ THE INPUT IS THE DATA MARKS, AND ONLY THE DATA MARKS. It is passed in
// explicitly, and named `data_marks` the whole way down, because the
// distinction is invisible at a call site that has a whole panel in hand:
// hand this the panel's finished child list instead and the plot
// background -- a rectangle covering every corner -- joins the collision
// test, no corner is ever clear, and every inside legend in the suite
// silently relocates or refuses to move when it should. Nothing about that
// failure is a compile error, so test_layout.cpp's "inside legend collides
// with data marks only, never the backdrop or the grid" pins it.
//
// Because every axes-panel type (Cartesian, pie/doughnut/funnel, radar,
// polar) fills its marks layer before its single append_legend() call,
// placing this logic inside append_legend covers them all with one
// implementation.

/// Uniform box size for an inside-corner legend: all four corners share
/// one box (only the anchor differs), so this is computed once and reused
/// by both the collision test and the draw.
struct InsideLegendBox {
    double w = 0.0;
    double h = 0.0;
};

InsideLegendBox inside_legend_box_size(const LegendMetrics& m, std::size_t entry_count) {
    InsideLegendBox box;
    box.w = legend_column_width(m);
    box.h = kLegendPadIn * 2.0 + m.entry_h * static_cast<double>(entry_count) -
           (m.entry_h > 0 ? 6.0 : 0.0) + 2.0;
    return box;
}

/// The box rect anchored at a specific inside corner -- shared by the
/// collision test and the draw so the two can never disagree.
RectF inside_legend_rect(LegendPosition corner, const InsideLegendBox& box, const RectF& plot,
                         double inset) {
    RectF r{plot.x + plot.w - box.w - inset, plot.y + inset, box.w, box.h};
    if (corner == LegendPosition::TopLeft || corner == LegendPosition::BottomLeft)
        r.x = plot.x + inset;
    if (corner == LegendPosition::BottomLeft || corner == LegendPosition::BottomRight)
        r.y = plot.y + plot.h - box.h - inset;
    return r;
}

/// Axis-aligned bounding box of a point set (a one- or two-point set
/// collapses to a point or a segment's own bbox).
RectF points_bounds(const std::vector<Point>& pts) {
    if (pts.empty()) return {};
    double x0 = pts.front().x, x1 = pts.front().x;
    double y0 = pts.front().y, y1 = pts.front().y;
    for (const Point& p : pts) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

double rect_overlap_area(const RectF& a, const RectF& b) {
    const double x0 = std::max(a.x, b.x);
    const double x1 = std::min(a.x + a.w, b.x + b.w);
    const double y0 = std::max(a.y, b.y);
    const double y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return 0.0;
    return (x1 - x0) * (y1 - y0);
}

/// Ray-casting point-in-polygon test (Jordan curve rule): valid for any
/// simple polygon, convex or not.
bool point_in_polygon(Point p, const std::vector<Point>& poly) {
    if (poly.size() < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Point& a = poly[i];
        const Point& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) &&
            p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
            inside = !inside;
        }
    }
    return inside;
}

/// Length of the part of segment a->b that lies inside `box`
/// (Liang-Barsky parametric clip). Exactly zero when the segment does not
/// enter the box -- so a long, steep line whose *bounding box* clips a
/// corner, but whose thin body passes well to one side of the legend box,
/// contributes nothing. A per-segment bounding-box test cannot make that
/// distinction: a single diagonal segment's bbox is a large rectangle the
/// line only cuts across, which would fake an overlap in corners the line
/// never actually touches (e.g. a straight trend line from the bottom-left
/// to the top-right of the plot).
double clipped_segment_length(Point a, Point b, const RectF& box) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    double t0 = 0.0, t1 = 1.0;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a.x - box.x, box.x + box.w - a.x, a.y - box.y, box.y + box.h - a.y};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) return 0.0; // parallel to this edge and outside it
        } else {
            const double r = q[i] / p[i];
            if (p[i] < 0.0) {
                if (r > t1) return 0.0;
                if (r > t0) t0 = r;
            } else {
                if (r < t0) return 0.0;
                if (r < t1) t1 = r;
            }
        }
    }
    if (t1 <= t0) return 0.0;
    return (t1 - t0) * std::sqrt(dx * dx + dy * dy);
}

/// Whether (and by how much) a multi-point path intrudes into `box`. An
/// unfilled stroke (line/polyline, or a filled shape's outline)
/// contributes the length of each segment that actually passes through
/// the box -- via clipped_segment_length, not the segment's bounding box,
/// so a long diagonal that misses the box scores zero. `filled`
/// additionally counts the whole box as covered when its centre falls
/// inside the closed path (a filled area can swallow a small box with no
/// edge crossing it). Only the zero/non-zero distinction is used by the
/// placement decision (see legend_box_is_clear), and it is exact: a
/// segment that does not enter the box contributes exactly 0, so a corner
/// clear of the data reads as clear and the legend stays byte-for-byte
/// where it was.
double path_overlap(const std::vector<Point>& pts, bool closed, bool filled, const RectF& box) {
    double total = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i)
        total += clipped_segment_length(pts[i - 1], pts[i], box);
    if (closed && pts.size() >= 3)
        total += clipped_segment_length(pts.back(), pts.front(), box);
    if (filled && point_in_polygon({box.x + box.w / 2.0, box.y + box.h / 2.0}, pts))
        total = std::max(total, box.w * box.h);
    return total;
}

/// Overlap area between `box` and one rendered scene item, in the legend
/// box's own pixel space. Text uses the same alignment-aware measurement
/// `text_halo` draws its backdrop from, so a data label counts as real
/// geometry too -- not just marks and fills.
double item_overlap(const SceneItem& item, const RectF& box, const TextMeasurer& tm) {
    return std::visit(
        [&](const auto& value) -> double {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, LineItem>) {
                return path_overlap({value.a, value.b}, false, false, box);
            } else if constexpr (std::is_same_v<T, PolylineItem>) {
                return path_overlap(value.points, false, false, box);
            } else if constexpr (std::is_same_v<T, PolygonItem>) {
                return path_overlap(value.points, true, value.style.fill.has_value(), box);
            } else if constexpr (std::is_same_v<T, RectItem>) {
                RectF r = value.rect;
                if (r.w < 0.0) { r.x += r.w; r.w = -r.w; }
                if (r.h < 0.0) { r.y += r.h; r.h = -r.h; }
                return rect_overlap_area(box, r);
            } else if constexpr (std::is_same_v<T, CircleItem>) {
                return rect_overlap_area(
                    box, {value.center.x - value.radius, value.center.y - value.radius,
                         value.radius * 2.0, value.radius * 2.0});
            } else if constexpr (std::is_same_v<T, PathItem>) {
                // The flattened path, as the raster backends draw it. Each
                // subpath scores on its own so a multi-loop path is not
                // read as one loop through a spurious closing segment.
                double total = 0.0;
                for (const std::vector<Point>& run : flatten_path(value))
                    total += path_overlap(run, false, value.style.fill.has_value(), box);
                return total;
            } else if constexpr (std::is_same_v<T, TextItem>) {
                const SceneItem halo = text_halo(value, tm, 0.0, ShapeStyle{});
                if (const auto* r = std::get_if<RectItem>(&halo))
                    return rect_overlap_area(box, r->rect);
                return rect_overlap_area(box, points_bounds(std::get<PolygonItem>(halo).points));
            } else if constexpr (std::is_same_v<T, ImageItem>) {
                // A picture covers its placement box, whatever its own
                // pixels do: a legend must not be dropped on top of one on
                // the strength of a transparent corner.
                RectF r = value.dest;
                if (r.w < 0.0) { r.x += r.w; r.w = -r.w; }
                if (r.h < 0.0) { r.y += r.h; r.h = -r.h; }
                return rect_overlap_area(box, r);
            } else { // SectorItem: the tessellated wedge, as raster/SIXEL draw it
                return path_overlap(tessellate_sector(value), true, true, box);
            }
        },
        item);
}

/// True when `box` is clear of every data mark -- the placement decision
/// below turns only on this yes/no, never on the magnitude, so the
/// different units the item functions return (stroke length in px,
/// fill/mark area in px^2) never have to be compared against each other.
bool legend_box_is_clear(const RectF& box, const std::vector<SceneItem>& data_marks,
                         const TextMeasurer& tm) {
    for (const auto& item : data_marks) {
        if (item_overlap(item, box, tm) > 0.0) return false;
    }
    return true;
}

/// Fixed corner order: the deterministic order in which a relocated
/// legend prefers a clear corner.
constexpr LegendPosition kInsideCorners[4] = {
    LegendPosition::TopRight,
    LegendPosition::TopLeft,
    LegendPosition::BottomRight,
    LegendPosition::BottomLeft,
};

/// Resolves an inside legend corner against `data_marks` -- the panel's
/// rendered data marks, and nothing else (see the ⚠ above). The preferred
/// corner (the explicit setting, or TopRight for Auto) is kept whenever it
/// is already clear of the data -- so every chart whose default corner is
/// unobstructed renders byte-identically to before. When the preferred
/// corner is obstructed, the legend relocates to the first inside corner
/// (in kInsideCorners order) that is completely clear. When NO corner is
/// clear -- a
/// plot dense enough that data reaches every corner, e.g. a contour field
/// or a Pareto curve sweeping the top -- the preferred corner is kept
/// unchanged: relocating to another equally obstructed corner would not
/// uncover the data, and such a chart wants an outside legend (which
/// reserves its own margin and cannot overlap anything) instead.
LegendPosition choose_inside_legend_corner(LegendPosition preferred, const InsideLegendBox& box,
                                           double inset, const RectF& plot,
                                           const std::vector<SceneItem>& data_marks,
                                           const TextMeasurer& tm) {
    if (legend_box_is_clear(inside_legend_rect(preferred, box, plot, inset), data_marks, tm))
        return preferred;
    for (LegendPosition corner : kInsideCorners) {
        if (corner == preferred) continue;
        if (legend_box_is_clear(inside_legend_rect(corner, box, plot, inset), data_marks, tm))
            return corner;
    }
    return preferred;
}

/// Render the legend box for the collected entries. `cell` is the
/// panel's outer rect and `plot` is its final (post-reservation) plot
/// rect; both are needed to place an outside legend flush against the
/// content it sits beside without re-deriving frame internals here (see
/// the derivation in cplot-google-charts-parity.md GC1: placing a Bottom
/// legend flush against `cell`'s bottom edge lands it exactly after the
/// x-axis tick/label block precisely because that block's own reserved
/// thickness is unaffected by the legend's addition to `frame.bottom`).
///
/// `data_marks` is the panel's marks layer and is used for one thing only:
/// deciding which inside corner is free (see the ⚠ note above
/// choose_inside_legend_corner). It is a separate parameter, not something
/// read back off a panel, so that the one input the corner search may see
/// is the one input a caller must name.
void append_legend(std::vector<SceneItem>& over, const std::vector<SceneItem>& data_marks,
                   const std::vector<LegendItemInfo>& entries, const RectF& cell,
                   const RectF& plot, LegendPosition lp, const Theme& theme,
                   const TextMeasurer& tm, double top_reserved = 0.0) {
    LegendPosition resolved = resolve_legend_position(lp, !entries.empty());
    if (resolved == LegendPosition::None || entries.empty()) return;

    const Font legend_font = theme.legend_font();
    const double swatch_w = kLegendSwatchW;
    const double pad_in = kLegendPadIn;
    const double gap = kLegendSwatchGap;

    const auto draw_entry = [&](const LegendItemInfo& e, double sx, double cy) {
        if (e.filled) {
            if (!e.square && e.marker != Marker::None) {
                ShapeStyle fs;
                fs.fill = e.color;
                over.push_back(CircleItem{{sx + swatch_w / 2.0, cy}, 3.5, fs});
            } else {
                ShapeStyle fs;
                fs.fill = e.color;
                over.push_back(
                    RectItem{{sx + swatch_w / 2.0 - 6.0, cy - 6.0, 12.0, 12.0}, fs});
            }
        } else {
            over.push_back(
                LineItem{{sx, cy}, {sx + swatch_w, cy}, stroke_style(e.color, 2.0, LineCap::Round, LineJoin::Round)});
            if (e.marker != Marker::None) {
                ShapeStyle fs;
                fs.fill = e.color;
                over.push_back(CircleItem{{sx + swatch_w / 2.0, cy}, 3.0, fs});
            }
        }
        TextItem label;
        label.pos = {sx + swatch_w + gap, cy};
        label.text = e.label;
        label.font = legend_font;
        label.color = theme.text_color;
        label.halign = HAlign::Left;
        label.valign = VAlign::Middle;
        over.push_back(std::move(label));
    };

    if (!is_outside_legend(resolved)) {
        // Inside corners: single-column box drawn over the plot, relocated
        // away from the preferred corner when it collides with this panel's
        // own rendered series geometry (see choose_inside_legend_corner).
        const LegendMetrics m = measure_legend(entries, theme, tm);
        const InsideLegendBox box = inside_legend_box_size(m, entries.size());
        const double inset = kLegendOutsideGap;
        resolved = choose_inside_legend_corner(resolved, box, inset, plot, data_marks, tm);
        const RectF box_rect = inside_legend_rect(resolved, box, plot, inset);

        ShapeStyle box_style;
        box_style.fill = theme.legend_background;
        box_style.stroke = theme.legend_border;
        box_style.stroke_width = 1.0;
        over.push_back(RectItem{box_rect, box_style});

        double ey = box_rect.y + pad_in + m.entry_h / 2.0 - 2.0;
        for (const auto& e : entries) {
            draw_entry(e, box_rect.x + pad_in, ey);
            ey += m.entry_h;
        }
        return;
    }

    // Outside positions: grid-wrapped, drawn beside (not over) the plot.
    const LegendMetrics m = measure_legend(entries, theme, tm);
    const bool horizontal = resolved == LegendPosition::Top || resolved == LegendPosition::Bottom;
    const double available_cross = horizontal ? plot.w : plot.h;
    const LegendGrid grid = layout_legend_grid(m, entries.size(), available_cross, horizontal);
    if (grid.line_count == 0) return;

    ShapeStyle box_style;
    box_style.fill = theme.legend_background;
    box_style.stroke = theme.legend_border;
    box_style.stroke_width = 1.0;

    if (horizontal) {
        const double item_w = swatch_w + gap + m.text_w;
        // Top sits directly above the plot (offset above any secondary top
        // x-axis band via top_reserved); Bottom anchors flush against the
        // cell's own outer padding so it lands exactly past the x-axis
        // tick/label block (see the derivation in the function doc comment
        // above) rather than overlapping it.
        const double top_y = resolved == LegendPosition::Top
                                 ? plot.y - top_reserved - kLegendOutsideGap - grid.total_h
                                 : cell.y + cell.h - theme.padding - grid.total_h;
        over.push_back(
            RectItem{{plot.x, top_y, plot.w, grid.total_h}, box_style});

        for (std::size_t row = 0; row < grid.line_count; ++row) {
            const std::size_t begin = row * grid.items_per_line;
            const std::size_t end = std::min(entries.size(), begin + grid.items_per_line);
            const std::size_t count = end - begin;
            const double row_w = static_cast<double>(count) * item_w +
                                 static_cast<double>(count > 0 ? count - 1 : 0) * kLegendItemGap;
            double sx = plot.x + (plot.w - row_w) / 2.0;
            const double cy = top_y + pad_in + m.entry_h * static_cast<double>(row) +
                              kLegendLineGap * static_cast<double>(row) + m.entry_h / 2.0 - 2.0;
            for (std::size_t i = begin; i < end; ++i) {
                draw_entry(entries[i], sx, cy);
                sx += item_w + kLegendItemGap;
            }
        }
    } else {
        const double col_w = legend_column_width(m);
        // Outermost on both sides, like Bottom: Left must clear the
        // mandatory y-axis ticks/label immediately beside the plot, and
        // Right must clear a y2 axis or colorbar when either is present
        // (nothing to clear when neither is -- flush-to-cell-edge is
        // still correct there, just with a larger gap). See the GC1
        // derivation this mirrors for Bottom's x-axis-label placement.
        const double left_x = resolved == LegendPosition::Right
                                  ? cell.x + cell.w - theme.padding - grid.total_w
                                  : cell.x + theme.padding;
        const std::size_t rows_shown =
            std::min(grid.items_per_line, entries.size());
        const double col_h = pad_in * 2.0 + m.entry_h * static_cast<double>(rows_shown);
        const double top_y = plot.y + plot.h / 2.0 - col_h / 2.0;
        over.push_back(RectItem{{left_x, top_y, grid.total_w, col_h}, box_style});

        for (std::size_t col = 0; col < grid.line_count; ++col) {
            const double cx = left_x + static_cast<double>(col) * (col_w + kLegendLineGap);
            const std::size_t begin = col * grid.items_per_line;
            const std::size_t end = std::min(entries.size(), begin + grid.items_per_line);
            for (std::size_t i = begin; i < end; ++i) {
                const double cy =
                    top_y + pad_in + m.entry_h * static_cast<double>(i - begin) + m.entry_h / 2.0 - 2.0;
                draw_entry(entries[i], cx + pad_in, cy);
            }
        }
    }
}

/// Panel title, and an optional subtitle stacked 3px below it in the theme's
/// muted color, both centered over the plot area (matches build_panel).
void append_panel_title(std::vector<SceneItem>& over, const std::string& title,
                        const std::string& subtitle, const RectF& plot, const RectF& cell,
                        const Theme& theme, const TextMeasurer& tm) {
    double heading_y = cell.y + theme.padding;
    if (!title.empty()) {
        TextItem label;
        label.pos = {plot.x + plot.w / 2.0, heading_y};
        label.text = title;
        label.font = theme.title_font();
        label.color = theme.text_color;
        label.halign = HAlign::Center;
        label.valign = VAlign::Top;
        over.push_back(std::move(label));
        heading_y += tm.measure(title, theme.title_font()).height + 3.0;
    }
    if (!subtitle.empty()) {
        TextItem label;
        label.pos = {plot.x + plot.w / 2.0, heading_y};
        label.text = subtitle;
        label.font = theme.subtitle_font();
        label.color = theme.muted_text_color;
        label.halign = HAlign::Center;
        label.valign = VAlign::Top;
        over.push_back(std::move(label));
    }
}

// The first visible series that asks for a colorbar (heatmap, or a
// color-by scatter/bubble) — any series exposing a ColorScale via the
// virtual colorbar hooks. One implementation serves every such series.
const Series* find_colorbar_series(const Axes& axes) {
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible()) continue;
        if (s->colorbar_enabled() && s->colorbar_scale()) return s.get();
    }
    return nullptr;
}

void append_colorbar(std::vector<SceneItem>& over, const Series& series, const RectF& plot,
                     const Theme& theme) {
    const auto [data_min, data_max] = series.colorbar_value_range();
    append_colorbar_scale(over, *series.colorbar_scale(), data_min, data_max, plot, theme);
}

/// True when a visible series draws its own category labels inside the
/// plot (population pyramid centre labels): the category axis then keeps
/// its tick marks but drops the tick-label text and its margin.
bool series_draw_category_labels(const Axes& axes) {
    for (const auto& s : axes.series_list()) {
        if (s->is_visible() && s->draws_own_category_labels()) return true;
    }
    return false;
}

/// The vertical band a secondary (top) x axis reserves above the plot:
/// tick marks, tick labels, and — when set — the axis label. Zero when no
/// series uses x2. Shared by the top-margin reservation (compute_frame) and
/// the Top-legend offset (append_legend) so the two never disagree. This is
/// the transpose of the y2 right-margin sizing: a top axis reserves label
/// HEIGHT, not WIDTH.
double top_axis_band(const Axes& axes, const ResolvedAxes& ra, const Theme& theme,
                     const TextMeasurer& tm) {
    if (!ra.has_x2) return 0.0;
    constexpr double tick_gap = 4.0;
    const double tick_h = tm.measure("0", theme.tick_font()).height;
    const std::string x2_label = axes.x2_axis().display_label();
    double band = theme.tick_length + tick_gap + tick_h;
    if (!x2_label.empty()) band += tm.measure(x2_label, theme.axis_label_font()).height + 8.0;
    return band;
}

/// Measure tick and axis labels and derive the panel margins. The
/// suppress flags implement shared axes: a suppressed side keeps its
/// tick marks but drops the label text — and the margin it would need.
PanelFrame compute_frame(const Axes& axes, const ResolvedAxes& ra, const RectF& cell,
                         const Theme& theme, const TextMeasurer& tm,
                         const Series* colorbar_series,
                         const std::vector<LegendItemInfo>& legend_entries,
                         bool suppress_x_text, bool suppress_y_text) {
    const bool cat_x = ra.x.kind() == ScaleKind::Category;
    const bool own_cat_labels = series_draw_category_labels(axes);
    const bool hide_x_tick_text = own_cat_labels && cat_x;
    const bool hide_y_tick_text = own_cat_labels && ra.y.kind() == ScaleKind::Category;
    const Font tick_font = theme.tick_font();
    const Font label_font = theme.axis_label_font();
    const Font title_font = theme.title_font();
    const Font subtitle_font = theme.subtitle_font();

    double y_tick_w = 0.0;
    for (const auto& t : ra.y_ticks.ticks) {
        y_tick_w = std::max(y_tick_w, tm.measure(t.label, tick_font).width);
    }
    double y2_tick_w = 0.0;
    for (const auto& t : ra.y2_ticks.ticks) {
        y2_tick_w = std::max(y2_tick_w, tm.measure(t.label, tick_font).width);
    }
    double x_tick_w_max = 0.0;
    for (const auto& t : ra.x_ticks.ticks) {
        x_tick_w_max = std::max(x_tick_w_max, tm.measure(t.label, tick_font).width);
    }
    const double tick_h = tm.measure("0", tick_font).height;

    const std::string x_label = axes.x_axis().display_label();
    const std::string y_label = axes.y_axis().display_label();
    const std::string y2_label = axes.y2_axis().display_label();
    const std::string& title = axes.title_text();
    const std::string& subtitle = axes.subtitle_text();

    PanelFrame frame;
    // Rotate crowded x labels (categories or long datetime labels).
    if (!suppress_x_text && !hide_x_tick_text && !ra.x_ticks.ticks.empty()) {
        const double approx_band = (cell.w - 100.0) / ra.x_ticks.ticks.size();
        frame.rotate_x_labels = x_tick_w_max + 8.0 > approx_band;
    }
    constexpr double kRotDeg = 35.0;
    // The bottom margin below is derived from these, and the margin fixes
    // the plot rectangle every coordinate in the panel is mapped into — so
    // a one-ulp difference here would move the whole panel. Degrees, and
    // cworks' trigonometry rather than the platform's.
    double rot_sin = 0.0;
    double rot_cos = 0.0;
    cworks::sincos_deg(kRotDeg, rot_sin, rot_cos);

    const double pad = theme.padding;
    const double tick_gap = 4.0;

    double left = pad;
    if (suppress_y_text) {
        left += theme.tick_length + 2.0;
    } else {
        if (!y_label.empty()) left += tm.measure(y_label, label_font).height + 8.0;
        left += (hide_y_tick_text ? 0.0 : y_tick_w + tick_gap) + theme.tick_length + 2.0;
    }

    double bottom = pad;
    if (suppress_x_text) {
        bottom += theme.tick_length;
    } else {
        if (!x_label.empty()) bottom += tm.measure(x_label, label_font).height + 8.0;
        if (frame.rotate_x_labels) {
            bottom += rot_sin * x_tick_w_max + rot_cos * tick_h +
                      tick_gap + theme.tick_length;
        } else {
            bottom += (hide_x_tick_text ? 0.0 : tick_h + tick_gap) + theme.tick_length;
        }
    }

    // Title, then subtitle (3px below it), then a single 10px gap to the
    // plot if either is present. This collapses to the historical
    // `title_h + 10` for a title-only panel and to nothing when both are
    // empty, so existing goldens stay byte-identical.
    double top = pad;
    if (!title.empty()) top += tm.measure(title, title_font).height;
    if (!subtitle.empty())
        top += (title.empty() ? 0.0 : 3.0) + tm.measure(subtitle, subtitle_font).height;
    if (!title.empty() || !subtitle.empty()) top += 10.0;
    // A secondary top x axis reserves its tick/label band above the plot;
    // without one the historical half-tick breathing room is kept exactly.
    if (ra.has_x2)
        top += top_axis_band(axes, ra, theme, tm);
    else
        top += tick_h * 0.5;

    double right = pad;
    if (ra.has_y2) {
        right = pad + y2_tick_w + tick_gap + theme.tick_length + 2.0;
        if (!y2_label.empty()) right += tm.measure(y2_label, label_font).height + 8.0;
    } else if (!suppress_x_text && !cat_x && !ra.x_ticks.ticks.empty() &&
               !frame.rotate_x_labels) {
        const double last_w = tm.measure(ra.x_ticks.ticks.back().label, tick_font).width;
        right = std::max(right, last_w / 2.0 + 4.0);
    }
    if (colorbar_series) right += 56.0;

    // Outside legend: reserved along whichever side it occupies, using
    // the OTHER axis's already-finalized bases -- so this never creates
    // a circular dependency (a panel's legend claims exactly one side).
    const LegendPosition resolved_legend =
        resolve_legend_position(axes.legend_position(), !legend_entries.empty());
    if (resolved_legend == LegendPosition::Left || resolved_legend == LegendPosition::Right) {
        const double available_h = std::max(10.0, cell.h - top - bottom);
        const double extent =
            outside_legend_extent(resolved_legend, legend_entries, available_h, theme, tm);
        if (resolved_legend == LegendPosition::Left) left += extent;
        else right += extent;
    } else if (resolved_legend == LegendPosition::Top || resolved_legend == LegendPosition::Bottom) {
        const double available_w = std::max(10.0, cell.w - left - right);
        const double extent =
            outside_legend_extent(resolved_legend, legend_entries, available_w, theme, tm);
        if (resolved_legend == LegendPosition::Top) top += extent;
        else bottom += extent;
    }

    frame.left = left;
    frame.right = right;
    frame.top = top;
    frame.bottom = bottom;
    return frame;
}

/// legend: labeled is the pie/doughnut callout legend; reject it on any other
/// panel family so it can never silently degrade to a plain legend elsewhere.
[[noreturn]] void throw_labeled_not_pie() {
    throw Error(cworks::validation_failed(
        "legend: labeled applies only to pie and doughnut charts"));
}
void reject_labeled_legend(const Axes& axes) {
    if (axes.legend_position() == LegendPosition::Labeled) throw_labeled_not_pie();
}

/// The smallest plot rect a panel is laid out with. Below this a chart says
/// nothing anyway, and the scales still need a non-degenerate pixel range.
constexpr double kMinPlotExtent = 10.0;

/// The plot rect inside `cell` once the margins around it are known.
///
/// The margins are what the tick text, axis labels, titles and outside
/// legends asked for, and a small enough cell cannot pay them: at a 64x40
/// preview pane a titled chart asks for more than the whole canvas. Clamping
/// only the extent would leave the origin at `cell + margin` and slide the
/// rect clean off the canvas, so every mark is drawn outside it and the chart
/// renders as an empty page — a silent blank, with nothing to say which of
/// the margins was the one that did not fit. Clamping the origin as well
/// keeps the minimum rect inside the cell: the marks are cramped, which is
/// the honest answer at that size, rather than missing.
///
/// Whenever the margins do fit — every layout that is not degenerate —
/// `cell.w - plot.w` is exactly `left + right`, so the origin is `left` and
/// nothing moves.
///
/// The extent along one axis: the cell minus its margins, never below
/// kMinPlotExtent and never larger than the cell itself. A cell smaller than
/// that minimum is given all of itself — there is nothing else to give it,
/// and a rect that overruns the page hides exactly as much as one that
/// starts past its edge.
double plot_extent(double cell_extent, double before, double after) {
    if (!(cell_extent > 0.0)) return kMinPlotExtent; // nothing to fit into
    return std::clamp(cell_extent - before - after,
                      std::min(kMinPlotExtent, cell_extent), cell_extent);
}

RectF clamp_plot_rect(const RectF& cell, double left, double top, double right,
                      double bottom) {
    RectF plot;
    plot.w = plot_extent(cell.w, left, right);
    plot.h = plot_extent(cell.h, top, bottom);
    plot.x = cell.x + std::min(left, std::max(0.0, cell.w - plot.w));
    plot.y = cell.y + std::min(top, std::max(0.0, cell.h - plot.h));
    return plot;
}

/// Build one panel (a full mini-chart) inside the given cell rectangle.
Group build_panel(const Axes& axes, const RectF& cell, const Theme& theme,
                  const TextMeasurer& tm, const PanelOptions& opts = {}) {
    detail::PanelLayers panel;
    reject_labeled_legend(axes);

    ResolvedAxes ra = resolve_scales(axes, opts.shared);
    const bool cat_x = ra.x.kind() == ScaleKind::Category;
    const bool cat_y = ra.y.kind() == ScaleKind::Category;
    const bool own_cat_labels = series_draw_category_labels(axes);
    const bool hide_x_tick_text = own_cat_labels && cat_x;
    const bool hide_y_tick_text = own_cat_labels && cat_y;
    const Series* colorbar_series = find_colorbar_series(axes);

    const Font tick_font = theme.tick_font();
    const Font label_font = theme.axis_label_font();
    const Font title_font = theme.title_font();
    const Font subtitle_font = theme.subtitle_font();

    const std::string x_label = axes.x_axis().display_label();
    const std::string y_label = axes.y_axis().display_label();
    const std::string y2_label = axes.y2_axis().display_label();
    const std::string x2_label = axes.x2_axis().display_label();
    const std::string& title = axes.title_text();
    const std::string& subtitle = axes.subtitle_text();

    // Collected once, before the plot rect (and so any outside-legend
    // reservation) is finalized: legend_items() depends only on the
    // series list and the theme's resolved per-series color, never on
    // plot geometry.
    const std::vector<LegendItemInfo> legend_entries = collect_legend_entries(axes, theme);

    const PanelFrame frame =
        opts.frame ? *opts.frame
                   : compute_frame(axes, ra, cell, theme, tm, colorbar_series, legend_entries,
                                   opts.suppress_x_text, opts.suppress_y_text);
    const bool rotate_x_labels = frame.rotate_x_labels;
    constexpr double kRotDeg = 35.0;

    const double pad = theme.padding;
    const double tick_gap = 4.0;

    const RectF plot =
        clamp_plot_rect(cell, frame.left, frame.top, frame.right, frame.bottom);
    // The data marks are cut to the plot rectangle; the frame, grid and
    // labels around them are not. That is the whole of the clip in a
    // Cartesian chart -- a marker disc or a trend line's end sitting on the
    // border is trimmed there instead of spilling into the axis gutter.
    panel.clip = plot;

    // A reversed axis swaps its pixel endpoints; the y axis already runs
    // bottom→top, so a reversed y un-inverts to top→bottom. band_width()
    // takes the absolute span, so bars and categories stay correct.
    const bool rev_x = axes.x_axis().reversed();
    const bool rev_y = axes.y_axis().reversed();
    ra.x.set_pixel_range(rev_x ? plot.x + plot.w : plot.x, rev_x ? plot.x : plot.x + plot.w);
    ra.y.set_pixel_range(rev_y ? plot.y : plot.y + plot.h, rev_y ? plot.y + plot.h : plot.y);
    if (ra.x.kind() == ScaleKind::Mirrored) {
        // Size the centre gap: wide enough for the category label column
        // when a series draws its labels there, else a slim separation.
        double gap = 14.0;
        if (own_cat_labels && cat_y) {
            double label_w = 0.0;
            for (const auto& s : axes.series_list()) {
                if (!s->is_visible() || !s->draws_own_category_labels()) continue;
                for (const auto& name : s->y_category_names()) {
                    label_w = std::max(label_w, tm.measure(name, tick_font).width);
                }
            }
            gap = label_w + 16.0;
        }
        ra.x.set_center_gap(std::min(gap, plot.w / 2.0));
    }
    // Native hit targets use the very same resolved scales as the marks.
    // Clip their rectangles to the plot because marks are clipped there too.
    for (const HitPoint& hit : axes.hit_points()) {
        const double x = ra.x.map(hit.x);
        const double y = ra.y.map(hit.y);
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        const double left = std::max(plot.x, x - hit.radius);
        const double right = std::min(plot.x + plot.w, x + hit.radius);
        const double top = std::max(plot.y, y - hit.radius);
        const double bottom = std::min(plot.y + plot.h, y + hit.radius);
        if (right > left && bottom > top)
            panel.hit_regions.push_back({RectF{left, top, right - left, bottom - top}, hit.id});
    }
    if (ra.has_y2) {
        const bool rev_y2 = axes.y2_axis().reversed();
        ra.y2.set_pixel_range(rev_y2 ? plot.y : plot.y + plot.h,
                              rev_y2 ? plot.y + plot.h : plot.y);
    }
    // The secondary (top) x axis shares the plot's horizontal pixel range,
    // mapped left→right like the primary x axis (a reversed x2 swaps ends).
    if (ra.has_x2) {
        const bool rev_x2 = axes.x2_axis().reversed();
        ra.x2.set_pixel_range(rev_x2 ? plot.x + plot.w : plot.x,
                              rev_x2 ? plot.x : plot.x + plot.w);
    }

    // -- plot background and grid ------------------------------------------
    if (theme.plot_background.a > 0.0) {
        ShapeStyle bg;
        bg.fill = theme.plot_background;
        panel.under.push_back(RectItem{plot, bg});
    }

    const ShapeStyle grid_style = stroke_style(theme.grid_color, theme.grid_stroke_width);
    const ShapeStyle minor_grid_style =
        stroke_style(theme.minor_grid_color, theme.grid_stroke_width);

    if (axes.x_axis().grid_enabled() && !cat_x) {
        for (double v : ra.x_ticks.minor) {
            const double px = ra.x.map(v);
            panel.under.push_back(
                LineItem{{px, plot.y}, {px, plot.y + plot.h}, minor_grid_style});
        }
        for (const auto& t : ra.x_ticks.ticks) {
            const double px = ra.x.map(t.value);
            panel.under.push_back(LineItem{{px, plot.y}, {px, plot.y + plot.h}, grid_style});
        }
    }
    if (axes.y_axis().grid_enabled() && !cat_y) {
        for (double v : ra.y_ticks.minor) {
            const double py = ra.y.map(v);
            panel.under.push_back(
                LineItem{{plot.x, py}, {plot.x + plot.w, py}, minor_grid_style});
        }
        for (const auto& t : ra.y_ticks.ticks) {
            const double py = ra.y.map(t.value);
            panel.under.push_back(LineItem{{plot.x, py}, {plot.x + plot.w, py}, grid_style});
        }
    }

    // Span annotations sit behind data marks.
    for (const auto& a : axes.annotations()) {
        const Color c = a.color.value_or(theme.muted_text_color);
        ShapeStyle span_style;
        span_style.fill = c.with_alpha(0.10);
        if (a.kind == AnnotationKind::SpanX) {
            const double x0 = ra.x.map(a.a);
            const double x1 = ra.x.map(a.b);
            panel.under.push_back(
                RectItem{{std::min(x0, x1), plot.y, std::abs(x1 - x0), plot.h}, span_style});
        } else if (a.kind == AnnotationKind::SpanY) {
            const double y0 = ra.y.map(a.a);
            const double y1 = ra.y.map(a.b);
            panel.under.push_back(
                RectItem{{plot.x, std::min(y0, y1), plot.w, std::abs(y1 - y0)}, span_style});
        }
    }

    // -- series geometry -----------------------------------------------------
    // Bar column assignment: every non-stacked bar series gets its own column;
    // all stacked bar series share one column.
    std::size_t nonstacked_bars = 0;
    bool any_stacked = false;
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible() || !s->is_bar_like()) continue;
        const auto* bar = dynamic_cast<const BarSeries*>(s.get());
        if (bar && bar->is_stacked())
            any_stacked = true;
        else
            ++nonstacked_bars;
    }
    const std::size_t bar_columns = nonstacked_bars + (any_stacked ? 1 : 0);

    std::vector<double> stack_pos, stack_neg;
    std::vector<std::vector<double>> stack_bases; // keep alive during geometry
    std::vector<double> bar_totals;               // per-category totals (100% stacks)
    const bool percent_bars = stacked_bar_percent_mode(axes, bar_totals);

    // Stacked area bookkeeping (per-point cumulative sums).
    const std::vector<const AreaSeries*> stacked_areas = stacked_area_series(axes);
    const bool percent_areas =
        !stacked_areas.empty() && stacked_areas.front()->is_percent_stacked();
    const std::vector<double> area_totals =
        percent_areas ? stacked_area_totals(stacked_areas) : std::vector<double>{};
    std::vector<double> area_pos, area_neg;
    std::vector<std::vector<double>> area_stack_keep; // bases and tops, kept alive
    area_stack_keep.reserve(stacked_areas.size() * 2); // stable pointers

    std::size_t color_index = 0;
    std::size_t bar_index = 0;
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible()) {
            ++color_index;
            continue;
        }
        const auto* bar = dynamic_cast<const BarSeries*>(s.get());
        const bool stacked = bar && bar->is_stacked();
        const auto* area = dynamic_cast<const AreaSeries*>(s.get());
        const bool area_stacked = area && area->is_stacked();

        detail::GeomContext ctx{s->uses_x2() && ra.has_x2 ? ra.x2 : ra.x,
                                s->uses_y2() && ra.has_y2 ? ra.y2 : ra.y,
                                theme,
                                s->explicit_color() ? *s->explicit_color()
                                                    : theme.series_color(color_index),
                                s->stroke_width_or(theme.series_stroke_width),
                                stacked ? nonstacked_bars : bar_index,
                                std::max<std::size_t>(bar_columns, 1),
                                nullptr};
        if (s->is_bar_like() && !stacked) ++bar_index;

        if (stacked) {
            std::vector<double> vals = bar->values();
            if (percent_bars) {
                for (std::size_t i = 0; i < vals.size(); ++i) {
                    if (!std::isfinite(vals[i])) continue;
                    const double total = i < bar_totals.size() ? bar_totals[i] : 0.0;
                    vals[i] = total > 0.0 ? vals[i] / total * 100.0 : 0.0;
                }
                ctx.stack_totals = &bar_totals;
            }
            if (stack_pos.size() < vals.size()) {
                stack_pos.resize(vals.size(), 0.0);
                stack_neg.resize(vals.size(), 0.0);
            }
            std::vector<double> base(vals.size(), 0.0);
            for (std::size_t i = 0; i < vals.size(); ++i) {
                base[i] = (std::isfinite(vals[i]) && vals[i] < 0) ? stack_neg[i]
                                                                  : stack_pos[i];
            }
            stack_bases.push_back(std::move(base));
            ctx.stack_base = &stack_bases.back();
            for (std::size_t i = 0; i < vals.size(); ++i) {
                if (!std::isfinite(vals[i])) continue;
                if (vals[i] >= 0)
                    stack_pos[i] += vals[i];
                else
                    stack_neg[i] += vals[i];
            }
        }

        if (area_stacked) {
            const auto& ys = area->y_data();
            const std::size_t n = ys.size();
            if (area_pos.size() < n) {
                area_pos.resize(n, 0.0);
                area_neg.resize(n, 0.0);
            }
            std::vector<double> base(n, 0.0), top(n, 0.0);
            for (std::size_t i = 0; i < n; ++i) {
                double v = ys[i];
                if (percent_areas) {
                    const double total = i < area_totals.size() ? area_totals[i] : 0.0;
                    v = total > 0.0 ? v / total * 100.0 : 0.0;
                }
                double& acc = v >= 0.0 ? area_pos[i] : area_neg[i];
                base[i] = acc;
                acc += v;
                top[i] = acc;
            }
            area_stack_keep.push_back(std::move(base));
            ctx.stack_base = &area_stack_keep.back();
            area_stack_keep.push_back(std::move(top));
            ctx.stack_top = &area_stack_keep.back();
        }

        s->build_geometry(ctx, panel.marks);
        ++color_index;
    }

    // Line annotations sit above data marks but below axes and labels.
    for (const auto& a : axes.annotations()) {
        const Color c = a.color.value_or(theme.muted_text_color);
        ShapeStyle style;
        style.stroke = c;
        style.stroke_width = 1.1;
        style.dash = DashPattern{{4.0, 3.0}};
        if (a.kind == AnnotationKind::HLine) {
            const double y = ra.y.map(a.a);
            panel.over.push_back(LineItem{{plot.x, y}, {plot.x + plot.w, y}, style});
            if (!a.label.empty()) {
                TextItem label;
                label.pos = {plot.x + plot.w - 4.0, y - 3.0};
                label.text = a.label;
                label.font = theme.tick_font();
                label.color = c;
                label.halign = HAlign::Right;
                label.valign = VAlign::Bottom;
                panel.over.push_back(std::move(label));
            }
        } else if (a.kind == AnnotationKind::VLine) {
            const double x = ra.x.map(a.a);
            panel.over.push_back(LineItem{{x, plot.y}, {x, plot.y + plot.h}, style});
            if (!a.label.empty()) {
                TextItem label;
                label.pos = {x + 3.0, plot.y + 4.0};
                label.text = a.label;
                label.font = theme.tick_font();
                label.color = c;
                label.halign = HAlign::Left;
                label.valign = VAlign::Top;
                label.rotation = 90.0;
                panel.over.push_back(std::move(label));
            }
        } else if (a.kind == AnnotationKind::Text) {
            TextItem label;
            label.pos = {ra.x.map(a.a) + a.dx, ra.y.map(a.b) + a.dy};
            label.text = a.label;
            label.font = theme.tick_font();
            label.color = c;
            label.halign = HAlign::Center;
            label.valign = VAlign::Middle;
            panel.over.push_back(std::move(label));
        } else if (a.kind == AnnotationKind::Arrow) {
            const Point tail{ra.x.map(a.a), ra.y.map(a.b)};
            const Point head{ra.x.map(a.c), ra.y.map(a.d)};
            ShapeStyle shaft;
            shaft.stroke = c;
            shaft.stroke_width = 1.4;
            panel.over.push_back(LineItem{tail, head, shaft});
            // Arrowhead: a filled triangle at the head, pointing tail→head.
            const double hx = head.x - tail.x, hy = head.y - tail.y;
            const double len = std::sqrt(hx * hx + hy * hy);
            if (len > 1e-9) {
                const double ux = hx / len, uy = hy / len;
                const double head_len = 9.0, head_half = 4.0;
                const Point base{head.x - ux * head_len, head.y - uy * head_len};
                ShapeStyle fill;
                fill.fill = c;
                panel.over.push_back(PolygonItem{{head,
                                                  {base.x - uy * head_half, base.y + ux * head_half},
                                                  {base.x + uy * head_half, base.y - ux * head_half}},
                                                 fill});
            }
            if (!a.label.empty()) {
                // Label at the tail, nudged away from the head so it clears
                // the shaft; alignment follows the nudge direction.
                const double tx = tail.x - head.x, ty = tail.y - head.y;
                const double tlen = std::sqrt(tx * tx + ty * ty);
                const double nx = tlen > 1e-9 ? tx / tlen : 0.0;
                const double ny = tlen > 1e-9 ? ty / tlen : -1.0;
                TextItem label;
                label.pos = {tail.x + nx * 4.0, tail.y + ny * 4.0};
                label.text = a.label;
                label.font = theme.tick_font();
                label.color = c;
                label.halign =
                    nx > 0.3 ? HAlign::Left : (nx < -0.3 ? HAlign::Right : HAlign::Center);
                label.valign =
                    ny > 0.3 ? VAlign::Top : (ny < -0.3 ? VAlign::Bottom : VAlign::Middle);
                panel.over.push_back(std::move(label));
            }
        } else if (a.kind == AnnotationKind::Callout) {
            const Point anchor{ra.x.map(a.a), ra.y.map(a.b)};
            const Point tip{anchor.x + a.dx, anchor.y + a.dy};
            ShapeStyle leader;
            leader.stroke = c;
            leader.stroke_width = 1.0;
            panel.over.push_back(LineItem{anchor, tip, leader});
            // A small filled dot marks the anchored point.
            ShapeStyle dot;
            dot.fill = c;
            panel.over.push_back(CircleItem{anchor, 2.0, dot});
            if (!a.label.empty()) {
                TextItem label;
                label.pos = {tip.x + (a.dx >= 0.0 ? 3.0 : -3.0), tip.y};
                label.text = a.label;
                label.font = theme.tick_font();
                label.color = c;
                label.halign = a.dx >= 0.0 ? HAlign::Left : HAlign::Right;
                label.valign = VAlign::Middle;
                panel.over.push_back(std::move(label));
            }
        } else if (a.kind == AnnotationKind::SigBracket) {
            // A solid bracket: horizontal bar at height y, end ticks dropping
            // toward the marks (downward in pixel space), star/p label above.
            const double x0 = ra.x.map(a.a);
            const double x1 = ra.x.map(a.c);
            const double y = ra.y.map(a.b);
            const double drop = a.d > 0.0 ? a.d : 6.0;
            ShapeStyle bracket;
            bracket.stroke = a.color.value_or(theme.text_color);
            bracket.stroke_width = 1.1;
            panel.over.push_back(LineItem{{x0, y}, {x1, y}, bracket});
            panel.over.push_back(LineItem{{x0, y}, {x0, y + drop}, bracket});
            panel.over.push_back(LineItem{{x1, y}, {x1, y + drop}, bracket});
            if (!a.label.empty()) {
                TextItem label;
                label.pos = {(x0 + x1) / 2.0, y - 2.0};
                label.text = a.label;
                label.font = theme.tick_font();
                label.color = a.color.value_or(theme.text_color);
                label.halign = HAlign::Center;
                label.valign = VAlign::Bottom;
                panel.over.push_back(std::move(label));
            }
        }
    }

    // -- axis spines and ticks -------------------------------------------------
    const ShapeStyle axis_style = stroke_style(theme.axis_color, theme.axis_stroke_width);
    const double bottom_y = plot.y + plot.h;
    const double right_x = plot.x + plot.w;

    panel.over.push_back(LineItem{{plot.x, plot.y}, {plot.x, bottom_y}, axis_style});
    panel.over.push_back(LineItem{{plot.x, bottom_y}, {right_x, bottom_y}, axis_style});
    if (theme.draw_axis_frame || ra.has_y2) {
        panel.over.push_back(LineItem{{right_x, plot.y}, {right_x, bottom_y}, axis_style});
    }
    if (theme.draw_axis_frame || ra.has_x2) {
        panel.over.push_back(LineItem{{plot.x, plot.y}, {right_x, plot.y}, axis_style});
    }

    // x ticks and labels (shared axes keep the marks, drop the text)
    for (const auto& t : ra.x_ticks.ticks) {
        const double px = ra.x.map(t.value);
        panel.over.push_back(
            LineItem{{px, bottom_y}, {px, bottom_y + theme.tick_length}, axis_style});
        if (opts.suppress_x_text || hide_x_tick_text) continue;
        TextItem label;
        label.text = t.label;
        label.font = tick_font;
        label.color = theme.text_color;
        label.pos = {px, bottom_y + theme.tick_length + tick_gap};
        if (rotate_x_labels) {
            label.halign = HAlign::Right;
            label.valign = VAlign::Top;
            label.rotation = -kRotDeg;
        } else {
            label.halign = HAlign::Center;
            label.valign = VAlign::Top;
        }
        panel.over.push_back(std::move(label));
    }
    for (double v : ra.x_ticks.minor) {
        const double px = ra.x.map(v);
        panel.over.push_back(
            LineItem{{px, bottom_y}, {px, bottom_y + theme.tick_length * 0.6}, axis_style});
    }
    // A mirrored axis carries one zero per half: a tick mark and label
    // at each inner edge of the centre gap (its tick set excludes zero).
    if (ra.x.kind() == ScaleKind::Mirrored) {
        const auto [zero_lo, zero_hi] = ra.x.center_edges();
        const std::string& f = axes.x_axis().tick_format_text();
        const std::string zero_label = f.empty() ? "0" : format_with(f, 0.0);
        for (const double px : {zero_lo, zero_hi}) {
            panel.over.push_back(
                LineItem{{px, bottom_y}, {px, bottom_y + theme.tick_length}, axis_style});
            if (opts.suppress_x_text) continue;
            TextItem label;
            label.text = zero_label;
            label.font = tick_font;
            label.color = theme.text_color;
            label.pos = {px, bottom_y + theme.tick_length + tick_gap};
            label.halign = HAlign::Center;
            label.valign = VAlign::Top;
            panel.over.push_back(std::move(label));
        }
    }

    // y ticks and labels
    for (const auto& t : ra.y_ticks.ticks) {
        const double py = ra.y.map(t.value);
        panel.over.push_back(
            LineItem{{plot.x - theme.tick_length, py}, {plot.x, py}, axis_style});
        if (opts.suppress_y_text || hide_y_tick_text) continue;
        TextItem label;
        label.pos = {plot.x - theme.tick_length - tick_gap, py};
        label.text = t.label;
        label.font = tick_font;
        label.color = theme.text_color;
        label.halign = HAlign::Right;
        label.valign = VAlign::Middle;
        panel.over.push_back(std::move(label));
    }
    for (double v : ra.y_ticks.minor) {
        const double py = ra.y.map(v);
        panel.over.push_back(
            LineItem{{plot.x - theme.tick_length * 0.6, py}, {plot.x, py}, axis_style});
    }

    // y2 ticks and labels
    if (ra.has_y2) {
        for (const auto& t : ra.y2_ticks.ticks) {
            const double py = ra.y2.map(t.value);
            panel.over.push_back(
                LineItem{{right_x, py}, {right_x + theme.tick_length, py}, axis_style});
            TextItem label;
            label.pos = {right_x + theme.tick_length + tick_gap, py};
            label.text = t.label;
            label.font = tick_font;
            label.color = theme.text_color;
            label.halign = HAlign::Left;
            label.valign = VAlign::Middle;
            panel.over.push_back(std::move(label));
        }
        for (double v : ra.y2_ticks.minor) {
            const double py = ra.y2.map(v);
            panel.over.push_back(
                LineItem{{right_x, py}, {right_x + theme.tick_length * 0.6, py}, axis_style});
        }
    }

    // x2 ticks and labels (secondary/top x axis): the transpose of the y2
    // block — ticks extend up from the top spine, labels sit above them.
    // Like y2, x2 is never a shared axis, so it always keeps its text.
    if (ra.has_x2) {
        for (const auto& t : ra.x2_ticks.ticks) {
            const double px = ra.x2.map(t.value);
            panel.over.push_back(
                LineItem{{px, plot.y}, {px, plot.y - theme.tick_length}, axis_style});
            TextItem label;
            label.pos = {px, plot.y - theme.tick_length - tick_gap};
            label.text = t.label;
            label.font = tick_font;
            label.color = theme.text_color;
            label.halign = HAlign::Center;
            label.valign = VAlign::Bottom;
            panel.over.push_back(std::move(label));
        }
        for (double v : ra.x2_ticks.minor) {
            const double px = ra.x2.map(v);
            panel.over.push_back(
                LineItem{{px, plot.y}, {px, plot.y - theme.tick_length * 0.6}, axis_style});
        }
    }

    // -- axis labels and title ---------------------------------------------------
    // An outside legend on that same side is now the outermost margin
    // content (see append_legend / the GC1 derivation), so a label that
    // used to anchor flush against the cell edge must yield exactly the
    // extent the legend reserved on its side -- recomputed here from the
    // same inputs compute_frame used, so the two always agree.
    const LegendPosition resolved_legend_for_labels =
        resolve_legend_position(axes.legend_position(), !legend_entries.empty());
    const double legend_bottom_extent =
        resolved_legend_for_labels == LegendPosition::Bottom
            ? outside_legend_extent(resolved_legend_for_labels, legend_entries, plot.w, theme, tm)
            : 0.0;
    const double legend_left_extent =
        resolved_legend_for_labels == LegendPosition::Left
            ? outside_legend_extent(resolved_legend_for_labels, legend_entries, plot.h, theme, tm)
            : 0.0;
    const double legend_right_extent =
        resolved_legend_for_labels == LegendPosition::Right
            ? outside_legend_extent(resolved_legend_for_labels, legend_entries, plot.h, theme, tm)
            : 0.0;
    const double legend_top_extent =
        resolved_legend_for_labels == LegendPosition::Top
            ? outside_legend_extent(resolved_legend_for_labels, legend_entries, plot.w, theme, tm)
            : 0.0;

    if (!x_label.empty() && !opts.suppress_x_text) {
        TextItem label;
        label.pos = {plot.x + plot.w / 2.0, cell.y + cell.h - pad - legend_bottom_extent};
        label.text = x_label;
        label.font = label_font;
        label.color = theme.text_color;
        label.halign = HAlign::Center;
        label.valign = VAlign::Bottom;
        panel.over.push_back(std::move(label));
    }
    if (!y_label.empty() && !opts.suppress_y_text) {
        TextItem label;
        label.pos = {cell.x + pad + legend_left_extent, plot.y + plot.h / 2.0};
        label.text = y_label;
        label.font = label_font;
        label.color = theme.text_color;
        label.halign = HAlign::Center;
        label.valign = VAlign::Top;
        label.rotation = -90.0;
        panel.over.push_back(std::move(label));
    }
    if (ra.has_y2 && !y2_label.empty()) {
        TextItem label;
        label.pos = {cell.x + cell.w - pad - legend_right_extent, plot.y + plot.h / 2.0};
        label.text = y2_label;
        label.font = label_font;
        label.color = theme.text_color;
        label.halign = HAlign::Center;
        label.valign = VAlign::Top;
        label.rotation = 90.0;
        panel.over.push_back(std::move(label));
    }
    // The secondary (top) x axis label: horizontal, centred over the plot,
    // stacked directly beneath the heading (and below a Top legend when one
    // is present) — the transpose of the vertical y2 label at the right edge.
    if (ra.has_x2 && !x2_label.empty()) {
        double x2_label_y = cell.y + pad + legend_top_extent;
        if (!title.empty()) x2_label_y += tm.measure(title, title_font).height;
        if (!subtitle.empty())
            x2_label_y += (title.empty() ? 0.0 : 3.0) + tm.measure(subtitle, subtitle_font).height;
        if (!title.empty() || !subtitle.empty()) x2_label_y += 10.0;
        TextItem label;
        label.pos = {plot.x + plot.w / 2.0, x2_label_y};
        label.text = x2_label;
        label.font = label_font;
        label.color = theme.text_color;
        label.halign = HAlign::Center;
        label.valign = VAlign::Top;
        panel.over.push_back(std::move(label));
    }
    // The panel title and, stacked 3px below it, an optional subtitle in the
    // theme's muted color at the subtitle font size. The figure-level
    // metadata `title` (scene.meta_title -> SVG <title>/PDF /Title) is never
    // drawn; this heading block is the only rendered title/subtitle surface.
    double heading_y = cell.y + pad;
    if (!title.empty()) {
        TextItem label;
        label.pos = {plot.x + plot.w / 2.0, heading_y};
        label.text = title;
        label.font = title_font;
        label.color = theme.text_color;
        label.halign = HAlign::Center;
        label.valign = VAlign::Top;
        panel.over.push_back(std::move(label));
        heading_y += tm.measure(title, title_font).height + 3.0;
    }
    if (!subtitle.empty()) {
        TextItem label;
        label.pos = {plot.x + plot.w / 2.0, heading_y};
        label.text = subtitle;
        label.font = subtitle_font;
        label.color = theme.muted_text_color;
        label.halign = HAlign::Center;
        label.valign = VAlign::Top;
        panel.over.push_back(std::move(label));
    }

    if (colorbar_series) append_colorbar(panel.over, *colorbar_series, plot, theme);

    // -- legend ---------------------------------------------------------------
    append_legend(panel.over, panel.marks, legend_entries, cell, plot, axes.legend_position(),
                  theme, tm, top_axis_band(axes, ra, theme, tm));

    return std::move(panel).build();
}

/// Pie/doughnut panel: no rectangular axes, ticks, or grid. Title,
/// legend, themes, and metadata behave exactly like Cartesian panels.
/// Plot-rect setup shared by the three non-Cartesian panel types
/// (pie/doughnut/funnel, radar, polar): a title-only top margin and a
/// uniform pad on the other sides, widened on whichever side an outside
/// legend claims -- the same reservation math compute_frame uses for
/// Cartesian panels (see outside_legend_extent).
RectF simple_plot_rect(const RectF& cell, const Theme& theme, const TextMeasurer& tm,
                       const std::string& title, const std::string& subtitle,
                       LegendPosition lp,
                       const std::vector<LegendItemInfo>& legend_entries,
                       double labeled_inset = 0.0) {
    const double pad = theme.padding;
    // Mirrors compute_frame's top reservation so an empty subtitle reduces
    // to the historical `title_h + 10` (byte-identical goldens).
    double top = pad;
    if (!title.empty()) top += tm.measure(title, theme.title_font()).height;
    if (!subtitle.empty())
        top += (title.empty() ? 0.0 : 3.0) + tm.measure(subtitle, theme.subtitle_font()).height;
    if (!title.empty() || !subtitle.empty()) top += 10.0;
    double bottom = pad, left = pad, right = pad;

    const LegendPosition resolved = resolve_legend_position(lp, !legend_entries.empty());
    if (resolved == LegendPosition::Left || resolved == LegendPosition::Right) {
        const double available_h = std::max(10.0, cell.h - top - bottom);
        const double extent =
            outside_legend_extent(resolved, legend_entries, available_h, theme, tm);
        (resolved == LegendPosition::Left ? left : right) += extent;
    } else if (resolved == LegendPosition::Top || resolved == LegendPosition::Bottom) {
        const double available_w = std::max(10.0, cell.w - left - right);
        const double extent =
            outside_legend_extent(resolved, legend_entries, available_w, theme, tm);
        (resolved == LegendPosition::Top ? top : bottom) += extent;
    } else if (resolved == LegendPosition::Labeled) {
        // Callout labels sit on both sides; reserve symmetrically so the pie
        // stays centred in the cell (mutually exclusive with the branches
        // above -- Labeled is a pie/doughnut-only position).
        left += labeled_inset;
        right += labeled_inset;
    }

    return clamp_plot_rect(cell, left, top, right, bottom);
}

/// Draw the callout labels and leader lines for LegendPosition::Labeled.
/// Fully deterministic: slices are split into a right (dx>=0) and a left
/// column preserving draw order, each column is stable-sorted by anchor y
/// (ties keep draw order), then a two-pass vertical declutter spreads the
/// labels to a uniform pitch without any iterative/force-directed placement.
void append_pie_callouts(std::vector<SceneItem>& over,
                         const std::vector<PieSeries::SliceCallout>& callouts,
                         Point center, double r_outer, const RectF& plot, const Theme& theme,
                         const TextMeasurer& tm) {
    std::vector<const PieSeries::SliceCallout*> right, left;
    for (const auto& c : callouts) (c.dx >= 0.0 ? right : left).push_back(&c);

    const Font tick_font = theme.tick_font();
    const double H = tm.measure("Ag", tick_font).height + kLabeledLineGap;
    const double ymin = plot.y + H / 2.0;
    const double ymax = plot.y + plot.h - H / 2.0;

    const auto emit_column = [&](std::vector<const PieSeries::SliceCallout*>& col, bool is_right) {
        const std::size_t n = col.size();
        if (n == 0) return;
        std::stable_sort(col.begin(), col.end(),
                         [](const PieSeries::SliceCallout* a, const PieSeries::SliceCallout* b) {
                             return a->anchor.y < b->anchor.y;
                         });
        // Two-pass declutter to a uniform pitch H, clamped to [ymin, ymax].
        std::vector<double> y(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double ideal = col[i]->anchor.y;
            y[i] = (i == 0) ? std::max(ideal, ymin) : std::max(ideal, y[i - 1] + H);
        }
        if (y[n - 1] > ymax) {
            y[n - 1] = ymax;
            for (std::size_t i = n - 1; i-- > 0;) y[i] = std::min(y[i], y[i + 1] - H);
        }

        const double col_x = is_right ? center.x + r_outer + kLabeledStub + kLabeledConnector
                                      : center.x - r_outer - kLabeledStub - kLabeledConnector;
        const double text_dir = is_right ? 1.0 : -1.0;
        for (std::size_t i = 0; i < n; ++i) {
            const PieSeries::SliceCallout& c = *col[i];
            PolylineItem leader;
            leader.points = {c.anchor,
                             {c.anchor.x + kLabeledStub * c.dx, c.anchor.y + kLabeledStub * c.dy},
                             {col_x, y[i]}};
            leader.style = stroke_style(c.color, 1.2, LineCap::Round, LineJoin::Round);
            over.push_back(std::move(leader));

            TextItem label;
            label.pos = {col_x + text_dir * kLabeledTextGap, y[i]};
            label.text = c.text;
            label.font = tick_font;
            label.color = theme.text_color;
            label.halign = is_right ? HAlign::Left : HAlign::Right;
            label.valign = VAlign::Middle;
            over.push_back(std::move(label));
        }
    };
    emit_column(right, true);
    emit_column(left, false);
}

Group build_part_to_whole_panel(const Axes& axes, const RectF& cell, const Theme& theme,
                                const TextMeasurer& tm) {
    detail::PanelLayers panel;

    std::size_t pie_count = 0;
    const Series* visible_series = nullptr;
    for (const auto& s : axes.series_list()) {
        if (s->is_visible()) {
            ++pie_count;
            visible_series = s.get();
        }
    }
    if (pie_count > 1) {
        throw Error(cworks::validation_failed(
            "only one part-to-whole series (pie, doughnut, funnel) per axes "
            "panel; use subplots for more"));
    }

    // legend: labeled is a pie/doughnut-only callout legend. A funnel panel is
    // also PartToWhole but is not a PieSeries, so reject it here. When the
    // smallest drawn slice is thinner than kMinLabeledSliceAngleDeg a leader is
    // ambiguous, so the whole panel deterministically falls back to a plain
    // right-side legend with normal inside-slice labels.
    const LegendPosition lp = axes.legend_position();
    const auto* pie = dynamic_cast<const PieSeries*>(visible_series);
    bool labeled_active = false;
    LegendPosition effective_lp = lp;
    double labeled_inset = 0.0;
    if (lp == LegendPosition::Labeled) {
        if (!pie) throw_labeled_not_pie(); // a funnel is PartToWhole but not a pie
        labeled_active = pie->min_slice_angle_deg() >= kMinLabeledSliceAngleDeg;
        if (labeled_active) {
            double max_w = 0.0;
            for (const std::string& t : pie->callout_texts())
                max_w = std::max(max_w, tm.measure(t, theme.tick_font()).width);
            labeled_inset = kLabeledStub + kLabeledConnector + kLabeledTextGap + max_w;
        } else {
            effective_lp = LegendPosition::Right; // fallback
        }
    }

    const std::string& title = axes.title_text();
    const std::string& subtitle = axes.subtitle_text();
    const std::vector<LegendItemInfo> legend_entries = collect_legend_entries(axes, theme);
    const RectF plot = simple_plot_rect(cell, theme, tm, title, subtitle,
                                        labeled_active ? LegendPosition::Labeled : effective_lp,
                                        legend_entries, labeled_active ? labeled_inset : 0.0);
    panel.clip = plot;

    if (theme.plot_background.a > 0.0) {
        ShapeStyle bg;
        bg.fill = theme.plot_background;
        panel.under.push_back(RectItem{plot, bg});
    }

    Scale sx = Scale::linear(0.0, 1.0);
    sx.set_pixel_range(plot.x, plot.x + plot.w);
    Scale sy = Scale::linear(0.0, 1.0);
    sy.set_pixel_range(plot.y + plot.h, plot.y);

    const Point pie_center{plot.x + plot.w / 2.0, plot.y + plot.h / 2.0};
    const double pie_r_outer = std::max(5.0, std::min(plot.w, plot.h) / 2.0 - 6.0);

    std::size_t color_index = 0;
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible()) {
            ++color_index;
            continue;
        }
        detail::GeomContext ctx{sx,
                                sy,
                                theme,
                                s->explicit_color() ? *s->explicit_color()
                                                    : theme.series_color(color_index),
                                s->stroke_width_or(theme.series_stroke_width),
                                0,
                                1,
                                nullptr};
        ctx.polar_center = pie_center;
        ctx.polar_radius = pie_r_outer;
        ctx.pie_callout_labels = labeled_active;
        s->build_geometry(ctx, panel.marks);
        ++color_index;
    }

    append_panel_title(panel.over, title, subtitle, plot, cell, theme, tm);
    if (labeled_active) {
        append_pie_callouts(panel.over, pie->slice_callouts(pie_center, pie_r_outer, theme),
                            pie_center, pie_r_outer, plot, theme, tm);
    } else {
        append_legend(panel.over, panel.marks, legend_entries, cell, plot, effective_lp, theme,
                      tm);
    }
    return std::move(panel).build();
}

/// Radar panel: category spokes, straight-sided ring grid at nice radial
/// tick values, one polygon per series.
Group build_radar_panel(const Axes& axes, const RectF& cell, const Theme& theme,
                        const TextMeasurer& tm) {
    detail::PanelLayers panel;
    reject_labeled_legend(axes);

    // Collect radar series and validate shared category order.
    std::vector<const RadarSeries*> radars;
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible()) continue;
        const auto* radar = dynamic_cast<const RadarSeries*>(s.get());
        if (radar) radars.push_back(radar);
    }
    if (radars.empty()) throw Error(cworks::validation_failed(
        "radar axes contains no radar series"));
    const std::vector<std::string>& categories = radars.front()->categories();
    for (const RadarSeries* radar : radars) {
        if (radar->categories() != categories) {
            throw Error(cworks::validation_failed(
                "all radar series in one axes must share the same categories "
                "in the same order"));
        }
    }

    // Radial range: explicit .range() wins, otherwise 0..nice data maximum.
    double lo = 0.0, hi = 1.0;
    bool have_explicit = false;
    for (const RadarSeries* radar : radars) {
        if (radar->explicit_range()) {
            lo = radar->explicit_range()->first;
            hi = radar->explicit_range()->second;
            have_explicit = true;
            break;
        }
    }
    if (!have_explicit) {
        Range data;
        data.include(0.0);
        for (const RadarSeries* radar : radars) {
            for (double v : radar->values()) data.include(v);
        }
        const NiceRange nice = nice_range(data.lo, data.hi, 5);
        lo = std::min(0.0, nice.lo);
        hi = nice.hi;
        if (!(hi > lo)) hi = lo + 1.0;
    }

    const std::string& title = axes.title_text();
    const std::string& subtitle = axes.subtitle_text();
    const std::vector<LegendItemInfo> legend_entries = collect_legend_entries(axes, theme);
    const RectF plot = simple_plot_rect(cell, theme, tm, title, subtitle,
                                        axes.legend_position(), legend_entries);
    panel.clip = plot;

    if (theme.plot_background.a > 0.0) {
        ShapeStyle bg;
        bg.fill = theme.plot_background;
        panel.under.push_back(RectItem{plot, bg});
    }

    // Leave room around the polygon for category labels.
    const Font tick_font = theme.tick_font();
    double label_w = 0.0;
    for (const auto& c : categories) {
        label_w = std::max(label_w, tm.measure(c, tick_font).width);
    }
    const double tick_h = tm.measure("0", tick_font).height;
    const Point center{plot.x + plot.w / 2.0, plot.y + plot.h / 2.0};
    const double radius = std::max(
        10.0, std::min(plot.w / 2.0 - label_w - 14.0, plot.h / 2.0 - tick_h - 14.0));

    Scale r_scale = Scale::linear(lo, hi);
    r_scale.set_pixel_range(0.0, radius);

    const std::size_t n = categories.size();
    // A whole turn split n ways, expressed in degrees rather than radians:
    // 360/n is exact for every spoke count a radar chart plausibly has
    // (3, 4, 5, 6, 8, 10, 12, ...), so a four-spoke radar puts its spokes
    // exactly on the axes instead of a rounding away from them.
    const auto spoke_degrees = [&](std::size_t i) {
        return 360.0 * static_cast<double>(i) / static_cast<double>(n);
    };
    const auto spoke_point = [&](std::size_t i, double r) {
        double sine = 0.0;
        double cosine = 0.0;
        cworks::sincos_deg(spoke_degrees(i), sine, cosine);
        return Point{center.x + r * sine, center.y - r * cosine};
    };

    // Ring grid at radial tick values, spokes, and tick labels.
    const TickSet r_ticks = r_scale.ticks(5, "", false);
    const ShapeStyle grid_style = stroke_style(theme.grid_color, theme.grid_stroke_width);
    for (const auto& t : r_ticks.ticks) {
        if (t.value <= lo || t.value > hi) continue;
        const double r = r_scale.map(t.value);
        std::vector<Point> ring;
        ring.reserve(n);
        for (std::size_t i = 0; i < n; ++i) ring.push_back(spoke_point(i, r));
        // A closed path, not a polyline that repeats its first point: the
        // seam then carries the grid's join like every other vertex of the
        // ring, instead of two caps meeting at one spoke.
        PolygonItem closed;
        closed.points = std::move(ring);
        closed.style = grid_style;
        closed.style.fill.reset();
        panel.under.push_back(std::move(closed));
    }
    const ShapeStyle spoke_style = stroke_style(theme.grid_color, theme.grid_stroke_width);
    for (std::size_t i = 0; i < n; ++i) {
        panel.under.push_back(LineItem{center, spoke_point(i, radius), spoke_style});
    }
    for (const auto& t : r_ticks.ticks) {
        if (t.value < lo || t.value > hi) continue;
        TextItem label;
        label.pos = {center.x + 5.0, center.y - r_scale.map(t.value)};
        label.text = t.label;
        label.font = tick_font;
        label.color = theme.muted_text_color;
        label.halign = HAlign::Left;
        label.valign = VAlign::Middle;
        panel.over.push_back(std::move(label));
    }

    // Category labels around the outside.
    for (std::size_t i = 0; i < n; ++i) {
        double sin_a = 0.0;
        double cos_a = 0.0;
        cworks::sincos_deg(spoke_degrees(i), sin_a, cos_a);
        TextItem label;
        label.pos = {center.x + (radius + 10.0) * sin_a,
                     center.y - (radius + 10.0) * cos_a};
        label.text = categories[i];
        label.font = tick_font;
        label.color = theme.text_color;
        label.halign = sin_a > 0.3 ? HAlign::Left
                       : sin_a < -0.3 ? HAlign::Right
                                      : HAlign::Center;
        label.valign = cos_a > 0.3 ? VAlign::Bottom
                       : cos_a < -0.3 ? VAlign::Top
                                      : VAlign::Middle;
        panel.over.push_back(std::move(label));
    }

    // Series polygons.
    Scale sx = Scale::linear(0.0, 1.0);
    sx.set_pixel_range(plot.x, plot.x + plot.w);
    std::size_t color_index = 0;
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible()) {
            ++color_index;
            continue;
        }
        detail::GeomContext ctx{sx,
                                r_scale,
                                theme,
                                s->explicit_color() ? *s->explicit_color()
                                                    : theme.series_color(color_index),
                                s->stroke_width_or(theme.series_stroke_width),
                                0,
                                1,
                                nullptr};
        ctx.polar_center = center;
        ctx.polar_radius = radius;
        s->build_geometry(ctx, panel.marks);
        ++color_index;
    }

    append_panel_title(panel.over, title, subtitle, plot, cell, theme, tm);
    append_legend(panel.over, panel.marks, legend_entries, cell, plot, axes.legend_position(),
                  theme, tm);
    return std::move(panel).build();
}

} // namespace

// Shared colorbar drawing, reused by every value-encoded series (heatmap,
// contour, color-by scatter/bubble, above) and by the treemap and calendar
// charts' color-by-value legends (structured_chart_layout.cpp) — declared
// in internal.hpp since callers outside this translation unit need it.
void append_colorbar_scale(std::vector<SceneItem>& over, const ColorScale& scale,
                           double data_min, double data_max, const RectF& plot,
                           const Theme& theme) {
    const double lo = scale.minimum.value_or(data_min);
    const double hi = scale.maximum.value_or(data_max);
    const double x = plot.x + plot.w + 14.0;
    const double y = plot.y + plot.h * 0.12;
    const double w = 12.0;
    const double h = plot.h * 0.76;
    constexpr int steps = 32;
    for (int i = 0; i < steps; ++i) {
        const double t0 = static_cast<double>(i) / steps;
        const double t1 = static_cast<double>(i + 1) / steps;
        const double value = hi + (lo - hi) * (t0 + t1) / 2.0;
        ShapeStyle style;
        style.fill = scale.color(value, data_min, data_max);
        over.push_back(
            RectItem{{x, y + h * t0, w, h / steps + 0.5}, style});
    }
    ShapeStyle border;
    border.stroke = theme.axis_color;
    border.stroke_width = 0.8;
    over.push_back(RectItem{{x, y, w, h}, border});

    const Font tick_font = theme.tick_font();
    TextItem hi_label;
    hi_label.pos = {x + w + 5.0, y};
    hi_label.text = format_tick_value(hi, 0.0);
    hi_label.font = tick_font;
    hi_label.color = theme.text_color;
    hi_label.halign = HAlign::Left;
    hi_label.valign = VAlign::Middle;
    over.push_back(std::move(hi_label));

    TextItem lo_label;
    lo_label.pos = {x + w + 5.0, y + h};
    lo_label.text = format_tick_value(lo, 0.0);
    lo_label.font = tick_font;
    lo_label.color = theme.text_color;
    lo_label.halign = HAlign::Left;
    lo_label.valign = VAlign::Middle;
    over.push_back(std::move(lo_label));
}

/// Polar panel: continuous (theta, r) series over an angular grid
/// (spokes every 30 degrees, labels in degrees) with radial rings at
/// nice r ticks. Angles are counterclockwise from the positive x axis.
Group build_polar_panel(const Axes& axes, const RectF& cell, const Theme& theme,
                        const TextMeasurer& tm) {
    detail::PanelLayers panel;
    reject_labeled_legend(axes);

    std::vector<const Series*> polars;
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible()) continue;
        if (s->coordinate_system() == CoordinateSystem::Polar)
            polars.push_back(s.get());
    }
    if (polars.empty()) throw Error(cworks::validation_failed(
        "polar axes contains no polar series"));

    // Radial range: explicit .range() wins, otherwise 0..nice data maximum.
    double lo = 0.0, hi = 1.0;
    bool have_explicit = false;
    for (const Series* series : polars) {
        std::optional<std::pair<double, double>> range;
        if (const auto* polar = dynamic_cast<const PolarSeries*>(series))
            range = polar->explicit_range();
        else if (const auto* rose = dynamic_cast<const WindRoseSeries*>(series))
            range = rose->explicit_range();
        if (range) {
            lo = range->first;
            hi = range->second;
            have_explicit = true;
            break;
        }
    }
    if (!have_explicit) {
        Range data;
        data.include(0.0);
        for (const Series* series : polars) {
            const Extent extent = series->extent();
            if (!extent.valid) continue;
            data.include(extent.y_lo);
            data.include(extent.y_hi);
        }
        const NiceRange nice = nice_range(data.lo, data.hi, 5);
        lo = std::min(0.0, nice.lo);
        hi = nice.hi;
        if (!(hi > lo)) hi = lo + 1.0;
    }

    const std::string& title = axes.title_text();
    const std::string& subtitle = axes.subtitle_text();
    const std::vector<LegendItemInfo> legend_entries = collect_legend_entries(axes, theme);
    const RectF plot = simple_plot_rect(cell, theme, tm, title, subtitle,
                                        axes.legend_position(), legend_entries);
    panel.clip = plot;

    if (theme.plot_background.a > 0.0) {
        ShapeStyle bg;
        bg.fill = theme.plot_background;
        panel.under.push_back(RectItem{plot, bg});
    }

    // Room for the angle labels around the circle ("330°" is widest).
    const Font tick_font = theme.tick_font();
    const double angle_label_w = tm.measure("330°", tick_font).width;
    const double tick_h = tm.measure("0", tick_font).height;
    const Point center{plot.x + plot.w / 2.0, plot.y + plot.h / 2.0};
    const double radius =
        std::max(10.0, std::min(plot.w / 2.0 - angle_label_w - 14.0,
                                plot.h / 2.0 - tick_h - 14.0));

    Scale r_scale = Scale::linear(lo, hi);
    r_scale.set_pixel_range(0.0, radius);

    // Degrees throughout: the polar grid is laid out on whole degrees, and
    // in degrees the spokes at 0, 90, 180 and 270 land exactly on the axes.
    const auto at = [&](double angle_deg, double r_px) {
        double sine = 0.0;
        double cosine = 0.0;
        cworks::sincos_deg(angle_deg, sine, cosine);
        return Point{center.x + r_px * cosine, center.y - r_px * sine};
    };

    // Rings at radial ticks, spokes every 30 degrees.
    const TickSet r_ticks = r_scale.ticks(5, "", false);
    const ShapeStyle grid_style = stroke_style(theme.grid_color, theme.grid_stroke_width);
    for (const auto& t : r_ticks.ticks) {
        if (t.value <= lo || t.value > hi) continue;
        ShapeStyle ring = grid_style;
        panel.under.push_back(CircleItem{center, r_scale.map(t.value), ring});
    }
    for (int deg = 0; deg < 360; deg += 30) {
        panel.under.push_back(LineItem{center, at(deg, radius), grid_style});
    }

    // Radial tick labels along the 90-degree (up) spoke.
    for (const auto& t : r_ticks.ticks) {
        if (t.value < lo || t.value > hi) continue;
        TextItem label;
        label.pos = {center.x + 5.0, center.y - r_scale.map(t.value)};
        label.text = t.label;
        label.font = tick_font;
        label.color = theme.muted_text_color;
        label.halign = HAlign::Left;
        label.valign = VAlign::Middle;
        panel.over.push_back(std::move(label));
    }

    // Angle labels in degrees around the outside.
    for (int deg = 0; deg < 360; deg += 30) {
        double sin_a = 0.0;
        double cos_a = 0.0;
        cworks::sincos_deg(deg, sin_a, cos_a);
        TextItem label;
        label.pos = {center.x + (radius + 8.0) * cos_a, center.y - (radius + 8.0) * sin_a};
        label.text = std::to_string(deg) + "°";
        label.font = tick_font;
        label.color = theme.text_color;
        label.halign = cos_a > 0.3 ? HAlign::Left
                       : cos_a < -0.3 ? HAlign::Right
                                      : HAlign::Center;
        label.valign = sin_a > 0.3 ? VAlign::Bottom
                       : sin_a < -0.3 ? VAlign::Top
                                      : VAlign::Middle;
        panel.over.push_back(std::move(label));
    }

    // Series geometry. ctx.y carries the radial scale (like radar).
    Scale sx = Scale::linear(0.0, 1.0);
    sx.set_pixel_range(plot.x, plot.x + plot.w);
    std::size_t color_index = 0;
    for (const auto& s : axes.series_list()) {
        if (!s->is_visible()) {
            ++color_index;
            continue;
        }
        detail::GeomContext ctx{sx,
                                r_scale,
                                theme,
                                s->explicit_color() ? *s->explicit_color()
                                                    : theme.series_color(color_index),
                                s->stroke_width_or(theme.series_stroke_width),
                                0,
                                1,
                                nullptr};
        ctx.polar_center = center;
        ctx.polar_radius = radius;
        s->build_geometry(ctx, panel.marks);
        ++color_index;
    }

    append_panel_title(panel.over, title, subtitle, plot, cell, theme, tm);
    append_legend(panel.over, panel.marks, legend_entries, cell, plot, axes.legend_position(),
                  theme, tm);
    return std::move(panel).build();
}

/// One shared scale from the panels' independently resolved scales:
/// numeric domains take the union range, category domains merge their
/// labels in first-seen order. Scale kinds must match.
/// The one scale kind the panels agree on, or the refusal that says they do not.
///
/// `axis_name` is the option a reader would go and change — "x_extent" — so a
/// diagnostic names the thing that was written rather than the thing that was
/// computed.
ScaleKind agreed_kind(const std::vector<const Scale*>& scales, const char* axis_name) {
    const ScaleKind kind = scales.front()->kind();
    for (const Scale* s : scales) {
        if (s->kind() != kind) {
            throw Error(cworks::validation_failed(
                std::string("figure.") + axis_name + "_extent: subplot panels mix incompatible " +
                axis_name + " scales (linear/log/datetime/category must match across panels)"));
        }
    }
    return kind;
}

/// A scale of the panels' agreed kind spanning [lo, hi].
///
/// The span comes from the caller because the two policies that give a grid one
/// domain differ in nothing else: `Union` computes it from the panels, `Pinned`
/// reads it off the author's own range, and everything after that — which
/// transform, which linthresh, which custom scale — is the same question.
Scale scale_over(const std::vector<const Scale*>& scales, ScaleKind kind, double lo, double hi,
                 const char* axis_name) {
    switch (kind) {
    case ScaleKind::Log10:
        return Scale::log10(lo, hi);
    case ScaleKind::Log2:
        return Scale::log2(lo, hi);
    case ScaleKind::Ln:
        return Scale::ln(lo, hi);
    case ScaleKind::Symlog:
        return Scale::symlog(lo, hi, scales.front()->linthresh());
    case ScaleKind::DateTime:
        return Scale::datetime(lo, hi);
    case ScaleKind::Custom: {
        const std::string& name = scales.front()->transform_name();
        for (const Scale* s : scales) {
            if (s->transform_name() != name) {
                throw Error(cworks::validation_failed(
                    std::string("figure.") + axis_name +
                    "_extent: subplot panels use different custom scales ('" + name + "' vs '" +
                    s->transform_name() + "')"));
            }
        }
        return Scale::transform(name, lo, hi);
    }
    case ScaleKind::Mirrored:
        return Scale::mirrored(std::max(std::abs(lo), std::abs(hi)));
    default:
        return Scale::linear(lo, hi);
    }
}

Scale union_scale(const std::vector<const Scale*>& scales, const char* axis_name) {
    const ScaleKind kind = agreed_kind(scales, axis_name);
    if (kind == ScaleKind::Category) {
        std::vector<std::string> merged;
        for (const Scale* s : scales) {
            for (const auto& c : s->categories()) {
                if (std::find(merged.begin(), merged.end(), c) == merged.end())
                    merged.push_back(c);
            }
        }
        return Scale::category(std::move(merged));
    }
    double lo = scales.front()->domain_lo();
    double hi = scales.front()->domain_hi();
    for (const Scale* s : scales) {
        lo = std::min(lo, s->domain_lo());
        hi = std::max(hi, s->domain_hi());
    }
    Scale s = scale_over(scales, kind, lo, hi, axis_name);
    if (kind == ScaleKind::Linear) {
        // Merge the frames (breathing room) and the tick domains (nice bounds)
        // separately, so a shared linear axis keeps stable ticks too.
        double tlo = scales.front()->tick_domain_lo();
        double thi = scales.front()->tick_domain_hi();
        for (const Scale* sc : scales) {
            tlo = std::min(tlo, sc->tick_domain_lo());
            thi = std::max(thi, sc->tick_domain_hi());
        }
        s.set_tick_domain(tlo, thi);
    }
    return s;
}

/// The scale a `Pinned` axis puts on every panel: the author's range, exactly.
///
/// No breathing room and no nice bounds, on purpose — those are what a scale
/// does when it is inferring a domain from data, and this domain was stated.
/// An author who writes [0, 100] and gets an axis to 105 has been given a
/// different answer to the one question this policy exists to settle.
///
/// A category axis is refused rather than approximated: a range of numbers
/// says nothing about which categories the panels should hold in common, and
/// unioning them is what `Union` is for.
Scale pinned_scale(const std::vector<const Scale*>& scales, std::pair<double, double> range,
                   const char* axis_name) {
    const ScaleKind kind = agreed_kind(scales, axis_name);
    if (kind == ScaleKind::Category) {
        throw Error(cworks::validation_failed(
            std::string("figure.") + axis_name +
            "_extent: a category axis cannot be pinned to a numeric range; write union to hold "
            "the panels' categories in common"));
    }
    Scale s = scale_over(scales, kind, range.first, range.second, axis_name);
    if (kind == ScaleKind::Linear) s.set_tick_domain(range.first, range.second);
    return s;
}

Scene build_scene(const Figure& fig) {
    const Theme& theme = fig.current_theme();
    const TextMeasurer& tm = default_text_measurer();

    Scene scene;
    scene.width = fig.width();
    scene.height = fig.height();
    scene.background = theme.page_background;

    scene.meta_title =
        !fig.metadata().title.empty() ? fig.metadata().title : fig.axes().title_text();
    scene.meta_description = !fig.metadata().description.empty() ? fig.metadata().description
                                                                 : fig.metadata().alt_text;
    scene.meta_extra = fig.metadata().extra;
    if (!fig.compatibility_profile().empty()) {
        scene.meta_generator =
            "cplot-version: " CPLOT_VERSION "; compatibility: " + fig.compatibility_profile();
    }
    if (fig.physical_size()) {
        scene.svg_width_attr = fig.physical_size()->first.svg_attribute();
        scene.svg_height_attr = fig.physical_size()->second.svg_attribute();
    }

    const int rows = fig.subplot_rows();
    const int cols = fig.subplot_cols();
    const double cell_w = fig.width() / cols;
    const double cell_h = fig.height() / rows;

    // EVERY POLICY BUT `Independent` RESOLVES A DOMAIN, whatever the grid's
    // size. A `Union` over one panel is that panel's own domain, so the path
    // below is a longer way to the same picture; a `Pinned` over one panel is
    // the range the author stated, and skipping it because there was nobody to
    // share it with would ignore a range silently — the one outcome this whole
    // policy exists to prevent.
    const AxisExtent& x_extent = fig.x_extent();
    const AxisExtent& y_extent = fig.y_extent();
    const bool shared_x = x_extent.shared();
    const bool shared_y = y_extent.shared();

    if (!shared_x && !shared_y) {
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const RectF cell{c * cell_w, r * cell_h, cell_w, cell_h};
                const Axes& axes = fig.axes(r, c);
                switch (panel_coordinate_system(axes)) {
                case CoordinateSystem::PartToWhole:
                    scene.root.add(build_part_to_whole_panel(axes, cell, theme, tm));
                    break;
                case CoordinateSystem::Radar:
                    scene.root.add(build_radar_panel(axes, cell, theme, tm));
                    break;
                case CoordinateSystem::Polar:
                    scene.root.add(build_polar_panel(axes, cell, theme, tm));
                    break;
                default:
                    scene.root.add(build_panel(axes, cell, theme, tm));
                    break;
                }
            }
        }
        return scene;
    }

    // -- shared subplot axes -------------------------------------------------
    // Pass 1: resolve every panel independently, then give the grid its one
    // domain per shared axis — the union of the panels' for `Union`, the
    // author's own range for `Pinned`.
    std::vector<ResolvedAxes> resolved;
    resolved.reserve(static_cast<std::size_t>(rows) * cols);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const Axes& axes = fig.axes(r, c);
            if (panel_coordinate_system(axes) != CoordinateSystem::Cartesian) {
                throw Error(cworks::validation_failed(
                    "figure.x_extent/y_extent require Cartesian subplot panels "
                    "(pie, doughnut, funnel, radar, and polar panels have no axis to share)"));
            }
            resolved.push_back(resolve_scales(axes));
        }
    }
    const auto domain_of = [&resolved](const AxisExtent& extent, auto member,
                                       const char* axis_name) {
        std::vector<const Scale*> scales;
        scales.reserve(resolved.size());
        for (const auto& ra : resolved) scales.push_back(&(ra.*member));
        return extent.policy == ExtentPolicy::Pinned
                   ? pinned_scale(scales, *extent.range, axis_name)
                   : union_scale(scales, axis_name);
    };
    SharedDomains shared;
    if (shared_x) shared.x = domain_of(x_extent, &ResolvedAxes::x, "x");
    if (shared_y) shared.y = domain_of(y_extent, &ResolvedAxes::y, "y");

    // Pass 2: measure margins under the shared domains, then align them —
    // left/right per column, top/bottom per row — so plot areas line up
    // and the shared tick positions coincide across panels.
    std::vector<PanelFrame> frames(resolved.size());
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * cols + c;
            const RectF cell{c * cell_w, r * cell_h, cell_w, cell_h};
            const Axes& axes = fig.axes(r, c);
            const ResolvedAxes ra = resolve_scales(axes, &shared);
            frames[i] = compute_frame(axes, ra, cell, theme, tm, find_colorbar_series(axes),
                                      collect_legend_entries(axes, theme),
                                      shared_x && r != rows - 1, shared_y && c != 0);
        }
    }
    for (int c = 0; c < cols; ++c) {
        double left = 0.0, right = 0.0;
        for (int r = 0; r < rows; ++r) {
            const std::size_t i = static_cast<std::size_t>(r) * cols + c;
            left = std::max(left, frames[i].left);
            right = std::max(right, frames[i].right);
        }
        for (int r = 0; r < rows; ++r) {
            const std::size_t i = static_cast<std::size_t>(r) * cols + c;
            frames[i].left = left;
            frames[i].right = right;
        }
    }
    for (int r = 0; r < rows; ++r) {
        double top = 0.0, bottom = 0.0;
        for (int c = 0; c < cols; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * cols + c;
            top = std::max(top, frames[i].top);
            bottom = std::max(bottom, frames[i].bottom);
        }
        for (int c = 0; c < cols; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * cols + c;
            frames[i].top = top;
            frames[i].bottom = bottom;
        }
    }

    // Pass 3: build the panels with the shared domains, aligned frames,
    // and label text only on the outer edges.
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * cols + c;
            const RectF cell{c * cell_w, r * cell_h, cell_w, cell_h};
            PanelOptions options;
            options.shared = &shared;
            options.frame = &frames[i];
            options.suppress_x_text = shared_x && r != rows - 1;
            options.suppress_y_text = shared_y && c != 0;
            scene.root.add(build_panel(fig.axes(r, c), cell, theme, tm, options));
        }
    }
    return scene;
}

} // namespace cplot::detail
