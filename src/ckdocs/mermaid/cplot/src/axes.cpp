// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cplot/axes.hpp"

#include "cplot/figure.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <vector>

#include <cworks/app_error.hpp>

namespace cplot {

template <class T, class... Args>
T& Axes::emplace_series(Args&&... args) {
    auto owned = std::make_unique<T>(std::forward<Args>(args)...);
    T& ref = *owned;
    series_.push_back(std::move(owned));
    return ref;
}

LineSeries& Axes::line(std::span<const double> x, std::span<const double> y) {
    return emplace_series<LineSeries>(x, y);
}

LineSeries& Axes::line(std::initializer_list<double> x, std::initializer_list<double> y) {
    const std::vector<double> vx(x), vy(y);
    return emplace_series<LineSeries>(std::span<const double>(vx), std::span<const double>(vy));
}

LineSeries& Axes::line(std::vector<std::string> categories,
                       std::span<const double> y) {
    return emplace_series<LineSeries>(std::move(categories), y);
}

LineSeries& Axes::line(std::vector<std::string> categories,
                       std::initializer_list<double> y) {
    const std::vector<double> values(y);
    return line(std::move(categories), std::span<const double>(values));
}

ScatterSeries& Axes::scatter(std::span<const double> x, std::span<const double> y) {
    return emplace_series<ScatterSeries>(x, y);
}

ScatterSeries& Axes::scatter(std::initializer_list<double> x, std::initializer_list<double> y) {
    const std::vector<double> vx(x), vy(y);
    return emplace_series<ScatterSeries>(std::span<const double>(vx),
                                         std::span<const double>(vy));
}

BarSeries& Axes::bar(std::vector<std::string> categories, std::span<const double> values) {
    return emplace_series<BarSeries>(std::move(categories), values, false);
}

BarSeries& Axes::bar(std::vector<std::string> categories, std::initializer_list<double> values) {
    const std::vector<double> vv(values);
    return emplace_series<BarSeries>(std::move(categories), std::span<const double>(vv), false);
}

BarSeries& Axes::barh(std::vector<std::string> categories, std::span<const double> values) {
    return emplace_series<BarSeries>(std::move(categories), values, true);
}

RangeBarSeries& Axes::range_bar(std::vector<std::string> categories,
                                std::span<const double> low, std::span<const double> high) {
    return emplace_series<RangeBarSeries>(std::move(categories), low, high);
}

XBarSeries& Axes::xbar(std::span<const double> x, std::span<const double> heights) {
    return emplace_series<XBarSeries>(x, heights);
}

XBarSeries& Axes::xbar(std::initializer_list<double> x,
                       std::initializer_list<double> heights) {
    const std::vector<double> vx(x), vh(heights);
    return emplace_series<XBarSeries>(std::span<const double>(vx),
                                      std::span<const double>(vh));
}

GanttSeries& Axes::gantt(std::vector<std::string> tasks, std::span<const double> start,
                         std::span<const double> end) {
    // GanttSeries keeps tasks in input order and owns the top-of-chart flip
    // (input row i renders at category slot n-1-i); the factory no longer
    // reverses.
    return emplace_series<GanttSeries>(std::move(tasks), start, end);
}

LollipopSeries& Axes::lollipop(std::vector<std::string> categories,
                               std::span<const double> values) {
    return emplace_series<LollipopSeries>(std::move(categories), values);
}

LollipopSeries& Axes::lollipop(std::vector<std::string> categories,
                               std::initializer_list<double> values) {
    const std::vector<double> vv(values);
    return lollipop(std::move(categories), std::span<const double>(vv));
}

DumbbellSeries& Axes::dumbbell(std::vector<std::string> categories,
                               std::span<const double> start,
                               std::span<const double> end) {
    return emplace_series<DumbbellSeries>(std::move(categories), start, end);
}

PopulationPyramidSeries& Axes::population_pyramid(std::vector<std::string> categories,
                                                  std::span<const double> left,
                                                  std::span<const double> right) {
    // Both sides read as magnitudes: the value axis is symmetric about
    // zero, so its negative half shows absolute tick labels.
    x_axis_.absolute_labels();
    return emplace_series<PopulationPyramidSeries>(std::move(categories), left, right);
}

PopulationPyramidSeries& Axes::population_pyramid(std::vector<std::string> categories,
                                                  std::initializer_list<double> left,
                                                  std::initializer_list<double> right) {
    const std::vector<double> vl(left), vr(right);
    return population_pyramid(std::move(categories), std::span<const double>(vl),
                              std::span<const double>(vr));
}

FunnelSeries& Axes::funnel(std::vector<std::string> stages,
                           std::span<const double> values) {
    return emplace_series<FunnelSeries>(std::move(stages), values);
}

FunnelSeries& Axes::funnel(std::vector<std::string> stages,
                           std::initializer_list<double> values) {
    const std::vector<double> vv(values);
    return funnel(std::move(stages), std::span<const double>(vv));
}

AreaSeries& Axes::area(std::span<const double> x, std::span<const double> y) {
    return emplace_series<AreaSeries>(x, y);
}

HistogramSeries& Axes::histogram(std::span<const double> values, int bins) {
    return emplace_series<HistogramSeries>(values, bins);
}

StepSeries& Axes::step(std::span<const double> x, std::span<const double> y, StepMode mode) {
    return emplace_series<StepSeries>(x, y, mode);
}

BoxPlotSeries& Axes::boxplot(std::vector<std::string> categories,
                             std::vector<std::vector<double>> data) {
    return emplace_series<BoxPlotSeries>(std::move(categories), std::move(data));
}

DensitySeries& Axes::density(std::span<const double> values, double bandwidth) {
    return emplace_series<DensitySeries>(values, bandwidth);
}

ViolinSeries& Axes::violin(std::vector<std::string> categories,
                           std::vector<std::vector<double>> data) {
    return emplace_series<ViolinSeries>(std::move(categories), std::move(data));
}

PolarSeries& Axes::polar(std::span<const double> theta, std::span<const double> r) {
    return emplace_series<PolarSeries>(theta, r);
}

ContourSeries& Axes::contour(std::span<const double> x, std::span<const double> y,
                             std::span<const double> z, std::size_t rows,
                             std::size_t cols) {
    return emplace_series<ContourSeries>(x, y, z, rows, cols);
}

HeatmapSeries& Axes::heatmap(std::vector<std::string> x_labels,
                             std::vector<std::string> y_labels,
                             std::span<const double> values, std::size_t rows,
                             std::size_t cols) {
    return emplace_series<HeatmapSeries>(std::move(x_labels), std::move(y_labels), values, rows,
                                         cols);
}

HeatmapSeries& Axes::heatmap(std::vector<std::string> x_labels,
                             std::vector<std::string> y_labels,
                             std::initializer_list<double> values, std::size_t rows,
                             std::size_t cols) {
    const std::vector<double> vv(values);
    return heatmap(std::move(x_labels), std::move(y_labels), std::span<const double>(vv), rows,
                   cols);
}

HeatmapSeries& Axes::confusion_matrix(std::vector<std::string> labels,
                                      std::span<const double> counts, std::size_t n) {
    return heatmap(labels, labels, counts, n, n).cell_labels().colorbar();
}

HeatmapSeries& Axes::confusion_matrix(std::span<const std::string> actual,
                                      std::span<const std::string> predicted) {
    if (actual.size() != predicted.size()) {
        throw Error(cworks::validation_failed(
            "confusion matrix: actual and predicted must have the same length"));
    }
    std::vector<std::string> labels;
    std::map<std::string, std::size_t> pos;
    const auto add_label = [&](const std::string& label) {
        if (pos.find(label) != pos.end()) return;
        pos[label] = labels.size();
        labels.push_back(label);
    };
    for (const auto& s : actual) add_label(s);
    for (const auto& s : predicted) add_label(s);
    std::vector<double> counts(labels.size() * labels.size(), 0.0);
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const std::size_t r = pos[actual[i]];
        const std::size_t c = pos[predicted[i]];
        counts[r * labels.size() + c] += 1.0;
    }
    const std::size_t n = labels.size();
    return confusion_matrix(std::move(labels), counts, n);
}

HeatmapSeries& Axes::correlation_matrix(std::vector<std::string> names,
                                        const std::vector<std::vector<double>>& columns) {
    if (names.size() != columns.size()) {
        throw Error(cworks::validation_failed(
            "correlation matrix: names and columns must have the same length (" +
            std::to_string(names.size()) + " vs " + std::to_string(columns.size()) + ")"));
    }
    if (columns.size() < 2) {
        throw Error(cworks::validation_failed("correlation matrix: needs at least two columns"));
    }
    const std::size_t n = columns.size();
    const auto pearson = [](const std::vector<double>& a, const std::vector<double>& b) {
        // Pairwise-complete: use only rows where both values are finite.
        double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        std::size_t count = 0;
        const std::size_t rows = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < rows; ++i) {
            if (!std::isfinite(a[i]) || !std::isfinite(b[i])) continue;
            sx += a[i];
            sy += b[i];
            sxx += a[i] * a[i];
            syy += b[i] * b[i];
            sxy += a[i] * b[i];
            ++count;
        }
        if (count < 2) return std::numeric_limits<double>::quiet_NaN();
        const double num = static_cast<double>(count) * sxy - sx * sy;
        const double den = std::sqrt((count * sxx - sx * sx) * (count * syy - sy * sy));
        return den > 0.0 ? num / den : std::numeric_limits<double>::quiet_NaN();
    };
    std::vector<double> r(n * n, 1.0);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const double v = pearson(columns[i], columns[j]);
            r[i * n + j] = v;
            r[j * n + i] = v;
        }
    }
    return heatmap(names, names, r, n, n)
        .colormap(Colormap::RedBlue)
        .color_range(-1.0, 1.0)
        .color_midpoint(0.0)
        .cell_labels(true)
        .value_format("%.2f")
        .colorbar(true);
}

PieSeries& Axes::pie(std::vector<std::string> labels, std::span<const double> values) {
    return emplace_series<PieSeries>(std::move(labels), values);
}

PieSeries& Axes::pie(std::vector<std::string> labels, std::initializer_list<double> values) {
    const std::vector<double> vv(values);
    return pie(std::move(labels), std::span<const double>(vv));
}

PieSeries& Axes::doughnut(std::vector<std::string> labels, std::span<const double> values) {
    return pie(std::move(labels), values).inner_radius(0.55);
}

PieSeries& Axes::doughnut(std::vector<std::string> labels,
                          std::initializer_list<double> values) {
    const std::vector<double> vv(values);
    return doughnut(std::move(labels), std::span<const double>(vv));
}

BubbleSeries& Axes::bubble(std::span<const double> x, std::span<const double> y,
                           std::span<const double> size) {
    return emplace_series<BubbleSeries>(x, y, size);
}

BubbleSeries& Axes::bubble(std::initializer_list<double> x, std::initializer_list<double> y,
                           std::initializer_list<double> size) {
    const std::vector<double> vx(x), vy(y), vs(size);
    return emplace_series<BubbleSeries>(std::span<const double>(vx),
                                        std::span<const double>(vy),
                                        std::span<const double>(vs));
}

OhlcSeries& Axes::ohlc(std::span<const double> x, std::span<const double> high,
                       std::span<const double> low, std::span<const double> close) {
    return emplace_series<OhlcSeries>(x, high, low, close);
}

OhlcSeries& Axes::ohlc(std::span<const double> x, std::span<const double> open,
                       std::span<const double> high, std::span<const double> low,
                       std::span<const double> close) {
    return emplace_series<OhlcSeries>(x, open, high, low, close, false);
}

OhlcSeries& Axes::candlestick(std::span<const double> x, std::span<const double> open,
                              std::span<const double> high, std::span<const double> low,
                              std::span<const double> close) {
    return emplace_series<OhlcSeries>(x, open, high, low, close, true);
}

RadarSeries& Axes::radar(std::vector<std::string> categories,
                         std::span<const double> values) {
    return emplace_series<RadarSeries>(std::move(categories), values);
}

RadarSeries& Axes::radar(std::vector<std::string> categories,
                         std::initializer_list<double> values) {
    const std::vector<double> vv(values);
    return radar(std::move(categories), std::span<const double>(vv));
}

WaterfallSeries& Axes::waterfall(std::vector<std::string> categories,
                                 std::span<const double> values) {
    return emplace_series<WaterfallSeries>(std::move(categories), values);
}

WaterfallSeries& Axes::waterfall(std::vector<std::string> categories,
                                 std::initializer_list<double> values) {
    const std::vector<double> vv(values);
    return waterfall(std::move(categories), std::span<const double>(vv));
}

StripSeries& Axes::strip(std::vector<std::string> categories, std::span<const double> values) {
    return emplace_series<StripSeries>(std::move(categories), values);
}

StripSeries& Axes::strip(std::vector<std::string> categories,
                         std::initializer_list<double> values) {
    const std::vector<double> vv(values);
    return strip(std::move(categories), std::span<const double>(vv));
}

ECDFSeries& Axes::ecdf(std::span<const double> values) {
    return emplace_series<ECDFSeries>(values);
}

ECDFSeries& Axes::ecdf(std::initializer_list<double> values) {
    const std::vector<double> vv(values);
    return ecdf(std::span<const double>(vv));
}

Axes& Axes::hline(double y, std::string label) {
    annotations_.push_back({AnnotationKind::HLine, y, y, std::move(label), std::nullopt});
    return *this;
}

Axes& Axes::vline(double x, std::string label) {
    annotations_.push_back({AnnotationKind::VLine, x, x, std::move(label), std::nullopt});
    return *this;
}

Axes& Axes::span_x(double lo, double hi, std::string label) {
    annotations_.push_back({AnnotationKind::SpanX, lo, hi, std::move(label), std::nullopt});
    return *this;
}

Axes& Axes::span_y(double lo, double hi, std::string label) {
    annotations_.push_back({AnnotationKind::SpanY, lo, hi, std::move(label), std::nullopt});
    return *this;
}

Axes& Axes::text(double x, double y, std::string text, double dx, double dy) {
    annotations_.push_back(
        {AnnotationKind::Text, x, y, std::move(text), std::nullopt, 0.0, 0.0, dx, dy});
    return *this;
}

Axes& Axes::arrow(double x0, double y0, double x1, double y1, std::string label) {
    annotations_.push_back(
        {AnnotationKind::Arrow, x0, y0, std::move(label), std::nullopt, x1, y1, 0.0, 0.0});
    return *this;
}

Axes& Axes::callout(double x, double y, std::string text, double dx, double dy) {
    annotations_.push_back(
        {AnnotationKind::Callout, x, y, std::move(text), std::nullopt, 0.0, 0.0, dx, dy});
    return *this;
}

Axes& Axes::sig_bracket(double x_lo, double x_hi, double y, std::string label, double tick_px) {
    annotations_.push_back({AnnotationKind::SigBracket, x_lo, y, std::move(label), std::nullopt,
                            x_hi, tick_px, 0.0, 0.0});
    return *this;
}

Axes& Axes::hit_point(double x, double y, std::uint64_t id, double radius) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius) || radius <= 0.0)
        throw Error(cworks::validation_failed("hit point needs finite coordinates and a positive radius"));
    hit_points_.push_back({x, y, radius, id});
    return *this;
}

Series& Axes::add_series(std::unique_ptr<Series> series) {
    series_.push_back(std::move(series));
    return *series_.back();
}

bool Axes::uses_y2() const {
    for (const auto& s : series_) {
        if (s->is_visible() && s->uses_y2()) return true;
    }
    return false;
}

bool Axes::uses_x2() const {
    for (const auto& s : series_) {
        if (s->is_visible() && s->uses_x2()) return true;
    }
    return false;
}

} // namespace cplot
