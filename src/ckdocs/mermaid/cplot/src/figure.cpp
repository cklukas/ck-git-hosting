// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cplot/figure.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ostream>
#include <type_traits>

#include <cworks/app_error.hpp>
#include <cworks/limits.hpp>

#include "internal.hpp"

namespace cplot {

Figure::Figure() : theme_(Theme::print()) {
    cells_.push_back(std::make_unique<Axes>());
}
Figure::~Figure() = default;
Figure::Figure(Figure&&) noexcept = default;
Figure& Figure::operator=(Figure&&) noexcept = default;

Figure& Figure::size(double width, double height) {
    if (width <= 0 || height <= 0)
        throw Error(cworks::validation_failed("figure size must be positive"));
    width_ = width;
    height_ = height;
    return *this;
}

Figure& Figure::size(Length width, Length height) {
    size(width.to_px(), height.to_px());
    if (width.is_physical() || height.is_physical()) {
        physical_size_ = std::pair<Length, Length>{width, height};
    } else {
        physical_size_.reset();
    }
    return *this;
}

Figure& Figure::dpi(double dpi) {
    if (dpi <= 0) throw Error(cworks::validation_failed("dpi must be positive"));
    dpi_ = dpi;
    return *this;
}

Figure& Figure::theme(const Theme& theme) {
    theme_ = theme;
    return *this;
}

Figure& Figure::limits(const RenderLimits& limits) {
    limits_ = limits;
    return *this;
}

Figure& Figure::compatibility(std::string profile) {
    static const char* known[] = {"cplot-0.1", "cplot-0.2", "cplot-0.3"};
    bool ok = false;
    for (const char* k : known)
        if (profile == k) ok = true;
    if (!ok) {
        // A fixed vocabulary of legal values, not a registry of objects: the
        // value is wrong, and there is no chooser a frontend could offer.
        throw Error(cworks::validation_failed("unknown compatibility profile '" + profile +
                                              "' (cplot-0.1, cplot-0.2, cplot-0.3)"));
    }
    compatibility_ = std::move(profile);
    return *this;
}

Figure& Figure::title(std::string text) {
    meta_.title = std::move(text);
    return *this;
}

Figure& Figure::description(std::string text) {
    meta_.description = std::move(text);
    return *this;
}

Figure& Figure::alt_text(std::string text) {
    meta_.alt_text = std::move(text);
    return *this;
}

Figure& Figure::metadata(std::string key, std::string value) {
    meta_.extra.emplace_back(std::move(key), std::move(value));
    return *this;
}

namespace {

/// One axis's policy, validated where the author set it.
///
/// The two refusals are worth making here rather than at render time: a policy
/// that cannot be honoured is a mistake in the call that made it, and the
/// caller is still standing at that line. What cannot be checked here is
/// whether the panels are Cartesian — series arrive after this call — so the
/// scene build makes that one.
AxisExtent settled(ExtentPolicy policy, std::optional<std::pair<double, double>> range,
                   const char* what, bool structured) {
    if (policy != ExtentPolicy::Independent && structured) {
        throw Error(cworks::validation_failed(std::string("a structured chart has no panels to "
                                                          "share an axis across (") +
                                              what + ")"));
    }
    validate_extent_policy(policy, range.has_value(), what);
    if (range && !(range->first < range->second)) {
        throw Error(cworks::validation_failed(
            std::string(what) + ": a pinned range runs from its low bound to its high one, got [" +
            std::to_string(range->first) + ", " + std::to_string(range->second) + "]"));
    }
    return AxisExtent{policy, range};
}

} // namespace

Figure& Figure::x_extent(ExtentPolicy policy, std::optional<std::pair<double, double>> range) {
    x_extent_ = settled(policy, range, "figure.x_extent", has_structured_chart());
    return *this;
}

Figure& Figure::y_extent(ExtentPolicy policy, std::optional<std::pair<double, double>> range) {
    y_extent_ = settled(policy, range, "figure.y_extent", has_structured_chart());
    return *this;
}

Axes& Figure::axes() {
    if (has_structured_chart())
        throw Error(cworks::validation_failed("a structured chart does not expose Cartesian axes"));
    return *cells_.front();
}
const Axes& Figure::axes() const {
    if (has_structured_chart())
        throw Error(cworks::validation_failed("a structured chart does not expose Cartesian axes"));
    return *cells_.front();
}

Figure& Figure::sankey(SankeyChart chart) {
    if (rows_ != 1 || cols_ != 1 || x_extent_.shared() || y_extent_.shared() ||
        !cells_.front()->series_list().empty())
        throw Error(
            cworks::validation_failed("a sankey chart cannot be combined with axes or subplots"));
    structured_chart_ = std::move(chart);
    return *this;
}

Figure& Figure::treemap(TreemapChart chart) {
    if (rows_ != 1 || cols_ != 1 || x_extent_.shared() || y_extent_.shared() ||
        !cells_.front()->series_list().empty())
        throw Error(
            cworks::validation_failed("a treemap chart cannot be combined with axes or subplots"));
    structured_chart_ = std::move(chart);
    return *this;
}

Figure& Figure::quadrant(QuadrantChart chart) {
    if (rows_ != 1 || cols_ != 1 || x_extent_.shared() || y_extent_.shared() ||
        !cells_.front()->series_list().empty())
        throw Error(cworks::validation_failed(
            "a quadrant chart cannot be combined with axes or subplots"));
    structured_chart_ = std::move(chart);
    return *this;
}

Figure& Figure::calendar(CalendarChart chart) {
    if (rows_ != 1 || cols_ != 1 || x_extent_.shared() || y_extent_.shared() ||
        !cells_.front()->series_list().empty())
        throw Error(cworks::unsupported("a calendar chart cannot be combined with axes or subplots"));
    structured_chart_ = std::move(chart);
    return *this;
}

Figure& Figure::gauge(GaugeChart chart) {
    if (rows_ != 1 || cols_ != 1 || x_extent_.shared() || y_extent_.shared() ||
        !cells_.front()->series_list().empty())
        throw Error(cworks::unsupported("a gauge chart cannot be combined with axes or subplots"));
    structured_chart_ = std::move(chart);
    return *this;
}

Figure& Figure::subplots(int rows, int cols) {
    if (has_structured_chart())
        throw Error(
            cworks::validation_failed("a structured chart cannot be combined with subplots"));
    if (rows < 1 || cols < 1)
        throw Error(cworks::validation_failed("subplots: rows and cols must be >= 1"));
    rows_ = rows;
    cols_ = cols;
    cells_.clear();
    for (int i = 0; i < rows * cols; ++i) cells_.push_back(std::make_unique<Axes>());
    return *this;
}

Axes& Figure::axes(int row, int col) {
    if (has_structured_chart())
        throw Error(cworks::validation_failed("a structured chart does not expose Cartesian axes"));
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_)
        throw Error(cworks::validation_failed(
            "axes(" + std::to_string(row) + ", " + std::to_string(col) + "): out of range for " +
            std::to_string(rows_) + "x" + std::to_string(cols_) + " grid"));
    return *cells_[static_cast<std::size_t>(row * cols_ + col)];
}

const Axes& Figure::axes(int row, int col) const {
    return const_cast<Figure*>(this)->axes(row, col);
}

Scene Figure::build_scene() const {
    // Enforce the point-count limits BEFORE layout: a runaway input
    // must fail on the (cheap) counts, not while allocating geometry.
    cworks::Diagnostics diagnostics;
    std::uint64_t figure_points = 0;
    std::size_t ordinal = 0;
    try {
        if (const auto* chart = std::get_if<SankeyChart>(&structured_chart_)) {
            figure_points = chart->mark_count();
            cworks::Limits::check(figure_points, limits_.series_points, "marks",
                                  "the sankey chart", diagnostics);
            cworks::Limits::check(figure_points, limits_.figure_points, "marks",
                                  "the sankey chart", diagnostics);
        } else if (const auto* chart = std::get_if<TreemapChart>(&structured_chart_)) {
            figure_points = chart->mark_count();
            cworks::Limits::check(figure_points, limits_.series_points, "marks",
                                  "the treemap chart", diagnostics);
            cworks::Limits::check(figure_points, limits_.figure_points, "marks",
                                  "the treemap chart", diagnostics);
        } else if (const auto* chart = std::get_if<QuadrantChart>(&structured_chart_)) {
            figure_points = chart->mark_count();
            cworks::Limits::check(figure_points, limits_.series_points, "marks",
                                  "the quadrant chart", diagnostics);
            cworks::Limits::check(figure_points, limits_.figure_points, "marks",
                                  "the quadrant chart", diagnostics);
        } else if (const auto* chart = std::get_if<CalendarChart>(&structured_chart_)) {
            figure_points = chart->mark_count();
            cworks::Limits::check(figure_points, limits_.series_points, "marks",
                                  "the calendar chart", diagnostics);
            cworks::Limits::check(figure_points, limits_.figure_points, "marks",
                                  "the calendar chart", diagnostics);
        } else if (const auto* chart = std::get_if<GaugeChart>(&structured_chart_)) {
            figure_points = chart->mark_count();
            cworks::Limits::check(figure_points, limits_.series_points, "marks",
                                  "the gauge chart", diagnostics);
            cworks::Limits::check(figure_points, limits_.figure_points, "marks",
                                  "the gauge chart", diagnostics);
        } else for (const auto& cell : cells_) {
            for (const auto& series : cell->series_list()) {
                ++ordinal;
                const std::uint64_t points = series->point_count();
                figure_points += points;
                const std::string& label = series->label_text();
                const std::string context =
                    label.empty() ? "series " + std::to_string(ordinal)
                                  : "series '" + label + "'";
                cworks::Limits::check(points, limits_.series_points, "points", context,
                                      diagnostics);
            }
        }
        if (!has_structured_chart())
            cworks::Limits::check(figure_points, limits_.figure_points, "points", "the figure",
                                  diagnostics);
    } catch (const cworks::Error& e) {
        // Limits::check sets no code today; keep whatever it sets and otherwise
        // say what this site knows — a figure larger than the configured bound
        // is out of range, not an engine fault.
        cworks::AppError err = e.structured() != nullptr ? *e.structured()
                                                         : cworks::validation_failed({});
        err.summary = e.what();
        throw Error(std::move(err));
    }

    if (!diagnostics.empty() && !limit_warnings_printed_.test_and_set()) {
        for (const auto& d : diagnostics)
            std::fprintf(stderr, "warning: %s\n", d.message.c_str());
    }

    Scene scene = std::visit(
        [this](const auto& chart) -> Scene {
            using T = std::decay_t<decltype(chart)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return detail::build_scene(*this);
            else
                return detail::build_scene(*this, chart);
        },
        structured_chart_);
    scene.limits = limits_;
    scene.diagnostics = std::move(diagnostics);
    return scene;
}

} // namespace cplot
