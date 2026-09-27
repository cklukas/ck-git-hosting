// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "color.hpp"
#include "axis.hpp"
#include "series.hpp"


namespace cplot {

enum class LegendPosition {
    Auto, ///< inside the plot when any series has a label, preferring the
          ///< top-right corner but relocating to another corner if the data
          ///< reaches into it (see the corner search on the inside positions)
    None,
    TopLeft,     ///< inside the plot area, upper-left corner (relocates to a
                 ///< clear corner if the data reaches into this one)
    TopRight,    ///< inside the plot area, upper-right corner (relocates to a
                 ///< clear corner if the data reaches into this one)
    BottomLeft,  ///< inside the plot area, lower-left corner (relocates to a
                 ///< clear corner if the data reaches into this one)
    BottomRight, ///< inside the plot area, lower-right corner (relocates to a
                 ///< clear corner if the data reaches into this one)
    Top,         ///< outside, above the plot area; entries wrap into rows
    Bottom,      ///< outside, below the plot area; entries wrap into rows
    Left,        ///< outside, left of the plot area; entries wrap into columns
    Right,       ///< outside, right of the plot area; entries wrap into columns
    Labeled,     ///< pie/doughnut only: outside callout labels with leader lines,
                 ///< replacing the swatch legend
};

enum class AnnotationKind {
    HLine,      ///< horizontal reference line at y = a
    VLine,      ///< vertical reference line at x = a
    SpanX,      ///< shaded vertical band between x = a and x = b
    SpanY,      ///< shaded horizontal band between y = a and y = b
    Text,       ///< free text anchored at data point (a, b), offset (dx, dy) px
    Arrow,      ///< arrow from data point (a, b) to (c, d)
    Callout,    ///< label offset (dx, dy) px from data point (a, b), with a leader
    SigBracket, ///< significance bracket from x = a to x = c at height y = b,
                ///< end ticks dropping `d` px toward the marks, `label` (stars
                ///< or a p-value) centred above
};

/// A data-coordinate point that a native viewer can select after layout.
/// The rectangle is clipped to the primary Cartesian plot area.
struct HitPoint {
    double x = 0.0;
    double y = 0.0;
    double radius = 9.0; ///< hit half-width in CSS pixels
    std::uint64_t id = 0;
};

/// One annotation. The two data scalars `a`/`b` are the primary point (or
/// the lo/hi pair for spans); `c`/`d` are the arrow head; `dx`/`dy` are a
/// pixel offset for the text of Text/Callout so a label can clear the mark
/// it points at. `label` carries the text; `color` overrides the theme.
struct Annotation {
    AnnotationKind kind = AnnotationKind::HLine;
    double a = 0.0;
    double b = 0.0;
    std::string label;
    std::optional<Color> color;
    double c = 0.0;
    double d = 0.0;
    double dx = 0.0;
    double dy = 0.0;
};

/// One plotting area: axes, grid, series, legend, annotations.
class Axes {
public:
    Axes() = default;
    Axes(const Axes&) = delete;
    Axes& operator=(const Axes&) = delete;

    // -- labels ------------------------------------------------------------
    Axes& title(std::string text) {
        title_ = std::move(text);
        return *this;
    }
    /// A smaller, muted line stacked below the panel title. Empty means none.
    Axes& subtitle(std::string text) {
        subtitle_ = std::move(text);
        return *this;
    }
    Axes& x_label(std::string text) {
        x_axis_.label(std::move(text));
        return *this;
    }
    Axes& y_label(std::string text) {
        y_axis_.label(std::move(text));
        return *this;
    }
    Axes& grid(bool enabled) {
        x_axis_.grid(enabled);
        y_axis_.grid(enabled);
        return *this;
    }
    Axes& legend(LegendPosition position) {
        legend_ = position;
        return *this;
    }

    // Explicit-style aliases.
    Axes& set_title(std::string t) { return title(std::move(t)); }
    Axes& set_subtitle(std::string t) { return subtitle(std::move(t)); }
    Axes& set_x_label(std::string t) { return x_label(std::move(t)); }
    Axes& set_y_label(std::string t) { return y_label(std::move(t)); }

    Axis& x_axis() { return x_axis_; }
    Axis& y_axis() { return y_axis_; }
    const Axis& x_axis() const { return x_axis_; }
    const Axis& y_axis() const { return y_axis_; }

    /// Secondary (right) y axis. Series opt in via .on_y2().
    Axis& y2_axis() {
        y2_axis_.grid(false); // secondary grid would be visually confusing
        return y2_axis_;
    }
    const Axis& y2_axis() const { return y2_axis_; }
    bool uses_y2() const;

    /// Secondary (top) x axis. Series opt in via .on_x2().
    Axis& x2_axis() {
        x2_axis_.grid(false); // secondary grid would be visually confusing
        return x2_axis_;
    }
    const Axis& x2_axis() const { return x2_axis_; }
    bool uses_x2() const;

    // -- series factories ----------------------------------------------------
    LineSeries& line(std::span<const double> x, std::span<const double> y);
    LineSeries& line(std::initializer_list<double> x, std::initializer_list<double> y);
    LineSeries& line(std::vector<std::string> categories,
                     std::span<const double> y);
    LineSeries& line(std::vector<std::string> categories,
                     std::initializer_list<double> y);
    ScatterSeries& scatter(std::span<const double> x, std::span<const double> y);
    ScatterSeries& scatter(std::initializer_list<double> x, std::initializer_list<double> y);
    BarSeries& bar(std::vector<std::string> categories, std::span<const double> values);
    BarSeries& bar(std::vector<std::string> categories, std::initializer_list<double> values);
    BarSeries& barh(std::vector<std::string> categories, std::span<const double> values);
    /// Floating (range) bars from low to high per category.
    RangeBarSeries& range_bar(std::vector<std::string> categories,
                              std::span<const double> low, std::span<const double> high);
    /// Bars at numeric x positions (volume panes, counts over time).
    XBarSeries& xbar(std::span<const double> x, std::span<const double> heights);
    XBarSeries& xbar(std::initializer_list<double> x, std::initializer_list<double> heights);
    /// Gantt/timeline: horizontal range bars, one row per task, first
    /// task on top. Use ax.x_axis().datetime() for calendar time.
    /// Optional per-task progress (GanttSeries::percent_complete),
    /// resource-group colouring (GanttSeries::resources), finish-to-start
    /// dependency connectors (GanttSeries::dependencies) and critical-path
    /// emphasis (GanttSeries::critical_path).
    GanttSeries& gantt(std::vector<std::string> tasks, std::span<const double> start,
                       std::span<const double> end);
    LollipopSeries& lollipop(std::vector<std::string> categories,
                             std::span<const double> values);
    LollipopSeries& lollipop(std::vector<std::string> categories,
                             std::initializer_list<double> values);
    DumbbellSeries& dumbbell(std::vector<std::string> categories,
                             std::span<const double> start, std::span<const double> end);
    /// Population pyramid: one bar pair per category diverging from a
    /// shared centre line (left/right must be >= 0). Enables absolute
    /// tick labels on the x axis so both sides read as magnitudes.
    PopulationPyramidSeries& population_pyramid(std::vector<std::string> categories,
                                                std::span<const double> left,
                                                std::span<const double> right);
    PopulationPyramidSeries& population_pyramid(std::vector<std::string> categories,
                                                std::initializer_list<double> left,
                                                std::initializer_list<double> right);
    FunnelSeries& funnel(std::vector<std::string> stages, std::span<const double> values);
    FunnelSeries& funnel(std::vector<std::string> stages,
                         std::initializer_list<double> values);
    AreaSeries& area(std::span<const double> x, std::span<const double> y);
    /// bins <= 0 leaves the count automatic (see HistogramSeries::bin_rule).
    HistogramSeries& histogram(std::span<const double> values, int bins = 0);
    StepSeries& step(std::span<const double> x, std::span<const double> y,
                     StepMode mode = StepMode::Post);
    BoxPlotSeries& boxplot(std::vector<std::string> categories,
                           std::vector<std::vector<double>> data);
    DensitySeries& density(std::span<const double> values, double bandwidth = 0.0);
    ViolinSeries& violin(std::vector<std::string> categories,
                         std::vector<std::vector<double>> data);
    /// Polar plot of continuous (theta, r) samples; theta in radians
    /// (PolarSeries::degrees switches the input unit).
    PolarSeries& polar(std::span<const double> theta, std::span<const double> r);
    /// Contour plot over a rectangular grid: x (cols entries) and y
    /// (rows entries) strictly increasing, z row-major rows x cols.
    ContourSeries& contour(std::span<const double> x, std::span<const double> y,
                           std::span<const double> z, std::size_t rows,
                           std::size_t cols);
    /// Numeric x/y raster grid (spectrogram); values are row-major.
    SpectrogramSeries& spectrogram(std::span<const double> x, std::span<const double> y,
                                   std::span<const double> values, std::size_t rows,
                                   std::size_t cols);
    SpectrogramSeries& spectrogram(std::initializer_list<double> x,
                                   std::initializer_list<double> y,
                                   std::initializer_list<double> values,
                                   std::size_t rows, std::size_t cols);
    /// Deterministic hexagonal 2-D density aggregation.
    HexbinSeries& hexbin(std::span<const double> x, std::span<const double> y,
                         int bins = 24);
    HexbinSeries& hexbin(std::initializer_list<double> x,
                         std::initializer_list<double> y, int bins = 24);
    /// Normal Q-Q plot (finite values only).
    QQSeries& qqplot(std::span<const double> values);
    QQSeries& qqplot(std::initializer_list<double> values);
    /// Direction-frequency sectors; weights may be empty (all ones).
    WindRoseSeries& wind_rose(std::span<const double> theta,
                              std::span<const double> weights = {}, int bins = 16);
    WindRoseSeries& wind_rose(std::initializer_list<double> theta,
                              std::initializer_list<double> weights, int bins = 16);
    HeatmapSeries& heatmap(std::vector<std::string> x_labels,
                           std::vector<std::string> y_labels,
                           std::span<const double> values, std::size_t rows,
                           std::size_t cols);
    HeatmapSeries& heatmap(std::vector<std::string> x_labels,
                           std::vector<std::string> y_labels,
                           std::initializer_list<double> values, std::size_t rows,
                           std::size_t cols);
    HeatmapSeries& confusion_matrix(std::vector<std::string> labels,
                                    std::span<const double> counts, std::size_t n);
    HeatmapSeries& confusion_matrix(std::span<const std::string> actual,
                                    std::span<const std::string> predicted);
    /// Pearson correlation matrix of the named columns (pairwise-complete
    /// rows), rendered as a diverging heatmap in [-1, 1].
    HeatmapSeries& correlation_matrix(std::vector<std::string> names,
                                      const std::vector<std::vector<double>>& columns);
    PieSeries& pie(std::vector<std::string> labels, std::span<const double> values);
    PieSeries& pie(std::vector<std::string> labels, std::initializer_list<double> values);
    PieSeries& doughnut(std::vector<std::string> labels, std::span<const double> values);
    PieSeries& doughnut(std::vector<std::string> labels,
                        std::initializer_list<double> values);
    BubbleSeries& bubble(std::span<const double> x, std::span<const double> y,
                         std::span<const double> size);
    BubbleSeries& bubble(std::initializer_list<double> x, std::initializer_list<double> y,
                         std::initializer_list<double> size);
    /// HLC bars: high–low range with a close tick.
    OhlcSeries& ohlc(std::span<const double> x, std::span<const double> high,
                     std::span<const double> low, std::span<const double> close);
    /// Full OHLC bars: open tick left, close tick right.
    OhlcSeries& ohlc(std::span<const double> x, std::span<const double> open,
                     std::span<const double> high, std::span<const double> low,
                     std::span<const double> close);
    OhlcSeries& candlestick(std::span<const double> x, std::span<const double> open,
                            std::span<const double> high, std::span<const double> low,
                            std::span<const double> close);
    RadarSeries& radar(std::vector<std::string> categories, std::span<const double> values);
    RadarSeries& radar(std::vector<std::string> categories,
                       std::initializer_list<double> values);
    WaterfallSeries& waterfall(std::vector<std::string> categories,
                               std::span<const double> values);
    WaterfallSeries& waterfall(std::vector<std::string> categories,
                               std::initializer_list<double> values);
    StripSeries& strip(std::vector<std::string> categories, std::span<const double> values);
    StripSeries& strip(std::vector<std::string> categories, std::initializer_list<double> values);
    ECDFSeries& ecdf(std::span<const double> values);
    ECDFSeries& ecdf(std::initializer_list<double> values);

    // -- convenience chart recipes ------------------------------------------
    /// Descending bars plus cumulative-percent line on y2.
    Axes& pareto(std::vector<std::string> categories, std::span<const double> values);
    Axes& pareto(std::vector<std::string> categories,
                 std::initializer_list<double> values);
    /// Line plus mean and +/- sigma standard-deviation limits.
    Axes& control_chart(std::span<const double> x, std::span<const double> y,
                        double sigma = 3.0);
    Axes& control_chart(std::initializer_list<double> x,
                        std::initializer_list<double> y, double sigma = 3.0);

    Axes& hline(double y, std::string label = {});
    Axes& vline(double x, std::string label = {});
    Axes& span_x(double lo, double hi, std::string label = {});
    Axes& span_y(double lo, double hi, std::string label = {});

    /// Free text anchored at data point (x, y), shifted by (dx, dy) pixels
    /// so a label can sit clear of the mark it names.
    Axes& text(double x, double y, std::string text, double dx = 0.0, double dy = 0.0);
    /// An arrow from data point (x0, y0) to (x1, y1); the head is at the
    /// second point. An optional label sits at the tail.
    Axes& arrow(double x0, double y0, double x1, double y1, std::string label = {});
    /// A callout: `text` offset (dx, dy) pixels from data point (x, y),
    /// joined to the point by a leader line and a small dot. The default
    /// offset places the label up and to the right of the point.
    Axes& callout(double x, double y, std::string text, double dx = 28.0, double dy = -28.0);

    /// A significance bracket spanning x = x_lo … x_hi at data height y, with
    /// end ticks dropping `tick_px` pixels toward the marks and `label` (a
    /// star code or p-value) centred above. On a categorical (bar) chart the
    /// x positions are category centres, i.e. index + 0.5.
    Axes& sig_bracket(double x_lo, double x_hi, double y, std::string label,
                      double tick_px = 6.0);

    /// Register a native hit target at a primary-axis data point. It changes
    /// no rendered bytes; collect_hit_regions() maps it with the same scales
    /// and plot rectangle used by the marks. Later overlapping targets win.
    Axes& hit_point(double x, double y, std::uint64_t id, double radius = 9.0);

    /// Explicit API: add a pre-built series (takes ownership).
    Series& add_series(std::unique_ptr<Series> series);

    // -- getters -----------------------------------------------------------
    const std::string& title_text() const { return title_; }
    const std::string& subtitle_text() const { return subtitle_; }
    LegendPosition legend_position() const { return legend_; }
    const std::vector<std::unique_ptr<Series>>& series_list() const { return series_; }
    const std::vector<Annotation>& annotations() const { return annotations_; }
    const std::vector<HitPoint>& hit_points() const { return hit_points_; }

private:
    template <class T, class... Args>
    T& emplace_series(Args&&... args);

    std::string title_;
    std::string subtitle_;
    Axis x_axis_;
    Axis y_axis_;
    Axis y2_axis_;
    Axis x2_axis_;
    LegendPosition legend_ = LegendPosition::Auto;
    std::vector<std::unique_ptr<Series>> series_;
    std::vector<Annotation> annotations_;
    std::vector<HitPoint> hit_points_;
};

} // namespace cplot
