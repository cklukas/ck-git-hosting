// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "colormap.hpp"
#include "color.hpp"
#include "scale.hpp"
#include "scene.hpp"

namespace cplot {

class Theme;

/// How missing values (NaN) are treated.
enum class MissingPolicy {
    Gap,         ///< break the line at missing values (default for lines)
    Drop,        ///< remove missing points (default for scatter)
    Zero,        ///< replace missing values with zero
    Error,       ///< throw on missing values
    Interpolate, ///< linearly fill interior gaps (line/area/step only —
                 ///< a series with no continuous domain to interpolate
                 ///< across rejects it; see each series' build_geometry)
};

/// A fitted trend overlay for a scatter or line series. Linear is ordinary
/// least squares on (x, y). Exponential fits y = a·e^(b·x) via OLS on
/// (x, ln y) — y must be positive everywhere the fit is evaluated (see
/// LineSeries::trendline). Polynomial fits a degree-N curve (see
/// SeriesBase::trendline's `degree` parameter) through a numerically stable
/// QR solve.
enum class FitKind { None, Linear, Exponential, Polynomial };

/// Legend-entry configuration for a series' trendline overlay. The fitted
/// line itself is always drawn once trendline() sets a FitKind; this only
/// controls whether it also gets its own legend entry (which additionally
/// requires the series itself to carry a legend entry — see
/// Series::legend_items). `text`, if non-empty, overrides the
/// auto-generated equation string.
struct TrendlineLabel {
    bool enabled = false;
    bool show_r2 = false;
    std::string text;
};

enum class Marker { None, Circle, Square, Diamond, TriangleUp, Cross, Plus };

/// Shared colour-by-value encoding for scatter and bubble series: a
/// per-point value column plus the ColorScale that maps it to a colormap.
/// When no values are set the series keeps its single resolved colour.
struct ColorEncoding {
    std::vector<double> values;
    ColorScale scale;
    bool colorbar = false;
    bool active() const { return !values.empty(); }
    /// Finite [min, max] of `values`; a degenerate range is padded so the
    /// colormap still spans a visible interval. Compute once per series
    /// and reuse across points (do not call per point).
    std::pair<double, double> value_range() const;
};

// StepSeries (a pure line) and AreaSeries::step() (a filled staircase) both
// draw this shape and stay separate types: StepSeries has no fill/baseline
// concerns, so it remains the lighter-weight choice when only the outline
// is wanted.
enum class StepMode {
    Post, ///< value holds until the next x (staircase, most common)
    Pre,  ///< value jumps at the current x
    Mid,  ///< step centered between points
};

enum class MatrixNormalize { None, Rows, Columns, All };

/// Coordinate system a series draws in. All series sharing one axes panel
/// must agree; the layout rejects incompatible mixes with a clear error.
enum class CoordinateSystem {
    Cartesian,   ///< rectangular x/y axes (default)
    PartToWhole, ///< pie/doughnut: no axes, circular sectors
    Radar,       ///< polar spokes with a shared radial value scale
    Polar,       ///< continuous (theta, r): angular grid + radial rings
};

/// How pie/doughnut slices are annotated.
enum class PieLabelMode {
    None,         ///< legend only (default)
    Label,        ///< category name inside the slice
    Percent,      ///< percentage of the total
    Value,        ///< raw value
    LabelPercent, ///< "name 42%"
};

/// How bubble sizes map to radii.
enum class BubbleScale {
    Area,   ///< radius ∝ sqrt(size): perceptually honest (default)
    Radius, ///< radius ∝ size
};

/// What an error bar (or shaded error band) represents. This is carried for
/// the legend/caption and selects the magnitude when the value is computed
/// from raw replicates (see cplot::stats::error_delta). `Custom` means the
/// caller supplied the magnitude directly and cplot must not relabel it.
enum class ErrorMeaning {
    Custom,   ///< magnitude supplied verbatim; no implied statistic
    StdDev,   ///< sample standard deviation (spread of the observations)
    StdError, ///< standard error of the mean, SD / sqrt(n)
    CI95,     ///< 95% confidence interval of the mean (t-based)
    Range,    ///< min..max of the observations
    IQR,      ///< inter-quartile range, Q1..Q3
};

/// How uncertainty on the value dimension is drawn for a line series.
enum class ErrorDisplay {
    Bars,        ///< discrete whiskers with caps at each point (default)
    Band,        ///< a continuous shaded ribbon (the "error shadow")
    BarsAndBand, ///< both, for emphasis
};

/// Styling of error-bar whiskers. All members have neutral defaults so an
/// unset style renders the conventional thin capped whisker.
struct ErrorBarStyle {
    double cap = 3.0;            ///< half cap width in px; 0 = capless whisker
    double width = 0.0;         ///< stroke width in px; 0 = 0.7 × series stroke
    std::optional<Color> color; ///< whisker colour; unset = series colour
    double band_alpha = 0.18;   ///< fill opacity when drawn as a band
    bool one_sided = false;     ///< draw only the half pointing away from the
                                ///< baseline (tidy on zero-based bars)
};

/// Per-point asymmetric error magnitudes along one dimension. Values are
/// absolute, non-negative deltas from the data point; a symmetric setter
/// stores identical lower and upper vectors. A non-finite entry suppresses
/// that point's whisker (null propagates, it is never treated as zero).
struct ErrorData {
    std::vector<double> lower;
    std::vector<double> upper;
    ErrorMeaning meaning = ErrorMeaning::Custom;

    bool empty() const { return lower.empty() && upper.empty(); }
    /// Lower delta at index i (0 when absent or non-finite).
    double lo(std::size_t i) const {
        return i < lower.size() && std::isfinite(lower[i]) ? lower[i] : 0.0;
    }
    /// Upper delta at index i (0 when absent or non-finite).
    double hi(std::size_t i) const {
        return i < upper.size() && std::isfinite(upper[i]) ? upper[i] : 0.0;
    }
    /// True when index i carries a drawable (finite, non-empty) whisker.
    bool has(std::size_t i) const {
        return (i < lower.size() && std::isfinite(lower[i])) ||
               (i < upper.size() && std::isfinite(upper[i]));
    }
    void set_symmetric(std::span<const double> e) {
        lower.assign(e.begin(), e.end());
        upper.assign(e.begin(), e.end());
    }
    void set_asymmetric(std::span<const double> lo_, std::span<const double> hi_) {
        lower.assign(lo_.begin(), lo_.end());
        upper.assign(hi_.begin(), hi_.end());
    }
};

/// Rule for choosing a histogram's bin count automatically when neither an
/// explicit bin count nor a bin width is given. Sqrt (ceil(sqrt(n))) is the
/// default: for a sample size around 100 it lands at 10 bins, matching this
/// engine's long-standing flat default, and it is the most common default
/// among comparable plotting libraries.
enum class HistogramBinRule {
    Sqrt,    ///< ceil(sqrt(n)) -- the default
    Sturges, ///< ceil(log2(n) + 1)
    Rice,    ///< ceil(2 * cbrt(n))
};

/// One legend entry. Most series contribute a single entry; pie/doughnut
/// contribute one entry per slice.
struct LegendItemInfo {
    std::string label;
    Color color;
    Marker marker = Marker::None;
    bool filled = false;
    bool square = false; ///< draw a filled square swatch (bars, areas)
};

/// Data extent of a series in data coordinates.
struct Extent {
    double x_lo = 0.0, x_hi = 0.0, y_lo = 0.0, y_hi = 0.0;
    bool valid = false;

    void include(double x, double y);
    void merge(const Extent& other);
};

namespace detail {

/// Everything a series needs to turn data into scene items.
struct GeomContext {
    const Scale& x;
    const Scale& y;
    const Theme& theme;
    Color color;                     ///< resolved series color
    double stroke_width = 1.6;       ///< resolved stroke width
    std::size_t bar_group_index = 0; ///< position within grouped bars
    std::size_t bar_group_count = 1; ///< number of bar series sharing the axis
    const std::vector<double>* stack_base = nullptr; ///< per-category stack offsets
    const std::vector<double>* stack_top = nullptr;  ///< per-point stacked area tops
    const std::vector<double>* stack_totals = nullptr; ///< per-category totals (100% stacks)
    Point polar_center{0.0, 0.0}; ///< center for pie/radar panels
    double polar_radius = 0.0;    ///< outer radius for pie/radar panels
    bool pie_callout_labels = false; ///< pie: labels are drawn outside via
                                     ///< leaders (layout owns them); suppress
                                     ///< the inside-slice labels
};

} // namespace detail

/// Base class of all data series.
///
/// Styling setters live in SeriesBase (CRTP) so fluent chains keep the
/// derived type: ax.line(x, y).label("A").stroke_width(2.0);
class Series {
public:
    virtual ~Series() = default;

    const std::string& label_text() const { return label_; }
    const std::optional<Color>& explicit_color() const { return color_; }
    double stroke_width_or(double fallback) const {
        return stroke_width_ > 0 ? stroke_width_ : fallback;
    }
    bool is_visible() const { return visible_; }
    MissingPolicy missing_policy() const { return missing_; }
    bool uses_y2() const { return y2_; }
    bool uses_x2() const { return x2_; }

    // Type-erased setters (used by the C API; prefer the fluent setters).
    void set_label_text(std::string text) { label_ = std::move(text); }
    void set_color_value(Color c) { color_ = c; }
    void set_stroke_width_value(double w) { stroke_width_ = w; }
    void set_missing_policy(MissingPolicy p) { missing_ = p; }
    void set_uses_y2(bool v) { y2_ = v; }
    void set_uses_x2(bool v) { x2_ = v; }

    // -- internal pipeline hooks ------------------------------------------
    virtual Extent extent() const = 0;
    /// Number of data points the series carries — drives the
    /// large-data limits (RenderLimits, checked in Figure::build_scene).
    virtual std::size_t point_count() const = 0;
    virtual void build_geometry(const detail::GeomContext& ctx,
                                std::vector<SceneItem>& out) const = 0;
    virtual bool wants_category_x() const { return false; }
    virtual bool wants_category_y() const { return false; }
    virtual bool wants_zero_baseline() const { return false; }
    virtual bool is_bar_like() const { return false; }
    /// True when the series draws its category labels inside the plot
    /// area itself; the panel then keeps the category axis' tick marks
    /// but drops the tick-label text (population pyramid centre labels).
    virtual bool draws_own_category_labels() const { return false; }
    /// True when the value (x) axis should be a mirrored magnitude scale
    /// with a centre gap (Scale::mirrored) instead of a plain signed
    /// linear axis — the unjoined population pyramid layout.
    virtual bool wants_mirrored_value_axis() const { return false; }
    virtual std::vector<std::string> category_names() const { return {}; }
    virtual std::vector<std::string> x_category_names() const { return category_names(); }
    virtual std::vector<std::string> y_category_names() const { return category_names(); }
    /// Marker drawn in the legend swatch; Marker::None means a line/box swatch.
    virtual Marker legend_marker() const { return Marker::None; }
    virtual bool legend_filled() const { return false; }
    /// Coordinate system this series draws in (Cartesian by default).
    virtual CoordinateSystem coordinate_system() const { return CoordinateSystem::Cartesian; }
    /// Legend entries. Default: one entry when the series has a label;
    /// pie/doughnut override this to return one entry per slice.
    virtual std::vector<LegendItemInfo> legend_items(Color resolved_color,
                                                     const Theme& theme) const;

    // -- colorbar (value-encoded series: heatmap, contour, color-by scatter/
    //    bubble). One colorbar implementation in layout.cpp reads these. ----
    /// True when a colorbar legend should be drawn for this series.
    virtual bool colorbar_enabled() const { return false; }
    /// The colour scale the colorbar samples, or nullptr when there is none.
    virtual const ColorScale* colorbar_scale() const { return nullptr; }
    /// Finite [min, max] of the values the colorbar spans.
    virtual std::pair<double, double> colorbar_value_range() const { return {0.0, 1.0}; }

protected:
    std::string label_;
    std::optional<Color> color_;
    double stroke_width_ = -1.0; ///< <= 0 means "use theme default"
    bool visible_ = true;
    bool y2_ = false;
    bool x2_ = false;
    MissingPolicy missing_ = MissingPolicy::Gap;
};

/// CRTP layer providing fluent setters that preserve the derived type.
template <class Derived>
class SeriesBase : public Series {
public:
    Derived& label(std::string text) {
        label_ = std::move(text);
        return self();
    }
    Derived& color(Color c) {
        color_ = c;
        return self();
    }
    Derived& stroke_width(double width) {
        stroke_width_ = width;
        return self();
    }
    Derived& visible(bool value) {
        visible_ = value;
        return self();
    }
    Derived& missing(MissingPolicy policy) {
        missing_ = policy;
        return self();
    }
    /// Attach this series to the secondary (right) y axis.
    Derived& on_y2(bool value = true) {
        y2_ = value;
        return self();
    }
    /// Attach this series to the secondary (top) x axis.
    Derived& on_x2(bool value = true) {
        x2_ = value;
        return self();
    }

    // Explicit-style aliases.
    Derived& set_label(std::string text) { return label(std::move(text)); }
    Derived& set_color(Color c) { return color(c); }
    Derived& set_stroke_width(double w) { return stroke_width(w); }

private:
    Derived& self() { return static_cast<Derived&>(*this); }
};

// ---------------------------------------------------------------------------

class LineSeries : public SeriesBase<LineSeries> {
public:
    LineSeries() = default;
    LineSeries(std::span<const double> x, std::span<const double> y);
    LineSeries(std::vector<std::string> categories,
               std::span<const double> y);

    LineSeries& set_data(std::span<const double> x, std::span<const double> y);
    /// A line over an ordered categorical x axis. Points are centred in the
    /// same category bands used by bars, so categorical lines and bars can
    /// be combined without a second layout implementation.
    LineSeries& set_data(std::vector<std::string> categories,
                         std::span<const double> y);
    LineSeries& marker(Marker m, double size = 3.0);
    LineSeries& dash(std::string svg_dash_array);
    /// Symmetric vertical error bars, one value per point.
    LineSeries& y_error(std::span<const double> err);
    /// Asymmetric vertical error bars: separate below/above magnitudes.
    LineSeries& y_error(std::span<const double> lower, std::span<const double> upper);
    /// Symmetric horizontal error bars, one value per point.
    LineSeries& x_error(std::span<const double> err);
    /// Asymmetric horizontal error bars: separate left/right magnitudes.
    LineSeries& x_error(std::span<const double> lower, std::span<const double> upper);
    /// Whether vertical error is drawn as whiskers, a shaded band, or both.
    /// Band mode uses the y-error magnitude as the ribbon half-width unless an
    /// explicit band() was supplied.
    LineSeries& error_display(ErrorDisplay display);
    /// Label the meaning of the error (SD / SEM / CI95 / …) for the caption.
    LineSeries& error_meaning(ErrorMeaning meaning);
    /// Half cap width of the whisker in px (0 = capless).
    LineSeries& error_cap_width(double px);
    /// Whisker stroke width in px (0 = derive from the series stroke).
    LineSeries& error_color(Color color);
    LineSeries& error_width(double px);
    /// Confidence band between lower and upper, one value per point.
    LineSeries& band(std::span<const double> lower, std::span<const double> upper);
    LineSeries& band_alpha(double alpha);
    /// Reduce to ~points using largest-triangle-three-buckets before drawing.
    LineSeries& downsample(std::size_t points);
    /// Draw a smooth spline through the points (monotone cubic, no
    /// overshoot). Markers and error bars stay at the data points.
    LineSeries& smooth(bool value = true);
    /// Overlay a fitted trend line: Linear is ordinary least squares;
    /// Exponential fits ln(y) against x (y must be positive everywhere —
    /// a non-positive y throws, naming the offending index, rather than
    /// silently dropping or clamping it); Polynomial fits a degree-`degree`
    /// curve (1..6) via a numerically stable QR solve. `degree` is only
    /// meaningful, and validated, for Polynomial; it is ignored otherwise.
    /// Null/missing (x, y) points are excluded from the fit.
    LineSeries& trendline(FitKind kind = FitKind::Linear, int degree = 2);
    /// Colour of the trend line (defaults to the series colour).
    LineSeries& trend_color(Color color);
    /// Show a legend entry for the trendline once it can be fit (also
    /// requires the series itself to carry a legend entry, i.e. label() is
    /// set). Default text is an auto-generated, quantized equation string;
    /// trend_label_text() overrides it.
    LineSeries& trend_label(bool value = true);
    /// Append an " (R^2 = ...)" suffix — computed in the fit's own residual
    /// space (plain y for Linear/Polynomial, ln(y) for Exponential) — to
    /// the trendline legend label.
    LineSeries& trend_show_r2(bool value = true);
    /// Explicit override for the trendline legend text; implies
    /// trend_label(true).
    LineSeries& trend_label_text(std::string text);
    /// Label each point with its own y-value, formatted the same way as
    /// bar/funnel/waterfall's value_labels() (detail::format_tick_value).
    /// Placement is a fixed pixel offset above-and-right of the point --
    /// deterministic by design; this engine never does force-directed or
    /// iterative label placement, so labels may overlap on dense data.
    LineSeries& point_labels(bool value = true);
    /// Label each point from a separate name/annotation column instead of
    /// its y-value (e.g. per-point identifiers). Must match the series
    /// length.
    LineSeries& point_labels(std::vector<std::string> names);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return x_.size(); }
    Marker legend_marker() const override { return marker_; }
    bool wants_category_x() const override { return !categories_.empty(); }
    std::vector<std::string> x_category_names() const override {
        return categories_;
    }
    std::vector<LegendItemInfo> legend_items(Color resolved_color,
                                             const Theme& theme) const override;

    const std::vector<double>& x_data() const { return x_; }
    const std::vector<double>& y_data() const { return y_; }
    const ErrorData& y_error_data() const { return err_y_; }
    const ErrorData& x_error_data() const { return err_x_; }

protected:
    std::vector<double> x_, y_, band_lo_, band_hi_;
    ErrorData err_y_, err_x_;
    ErrorBarStyle err_style_;
    ErrorDisplay err_display_ = ErrorDisplay::Bars;
    std::vector<std::string> categories_;
    Marker marker_ = Marker::None;
    double marker_size_ = 3.0;
    double band_alpha_ = 0.18;
    std::size_t downsample_ = 0; ///< 0 = off
    bool smooth_ = false;
    DashPattern dash_;
    FitKind fit_ = FitKind::None;
    int fit_degree_ = 2; ///< meaningful only for FitKind::Polynomial
    std::optional<Color> trend_color_;
    TrendlineLabel trend_label_;
    bool point_labels_ = false;
    std::vector<std::string> point_label_names_;
};

class ScatterSeries : public SeriesBase<ScatterSeries> {
public:
    ScatterSeries() = default;
    ScatterSeries(std::span<const double> x, std::span<const double> y);

    ScatterSeries& set_data(std::span<const double> x, std::span<const double> y);
    ScatterSeries& marker(Marker m);
    ScatterSeries& marker_size(double size);
    /// Symmetric vertical error bars, one value per point.
    ScatterSeries& y_error(std::span<const double> err);
    /// Asymmetric vertical error bars: separate below/above magnitudes.
    ScatterSeries& y_error(std::span<const double> lower, std::span<const double> upper);
    /// Symmetric horizontal error bars, one value per point.
    ScatterSeries& x_error(std::span<const double> err);
    /// Asymmetric horizontal error bars: separate left/right magnitudes.
    ScatterSeries& x_error(std::span<const double> lower, std::span<const double> upper);
    ScatterSeries& error_meaning(ErrorMeaning meaning);
    ScatterSeries& error_cap_width(double px);
    ScatterSeries& error_color(Color color);
    ScatterSeries& error_width(double px);
    /// Overlay a fitted trend line: Linear is ordinary least squares;
    /// Exponential fits ln(y) against x (y must be positive everywhere —
    /// a non-positive y throws, naming the offending index, rather than
    /// silently dropping or clamping it); Polynomial fits a degree-`degree`
    /// curve (1..6) via a numerically stable QR solve. `degree` is only
    /// meaningful, and validated, for Polynomial; it is ignored otherwise.
    /// Null/missing (x, y) points are excluded from the fit.
    ScatterSeries& trendline(FitKind kind = FitKind::Linear, int degree = 2);
    /// Colour of the trend line (defaults to the series colour).
    ScatterSeries& trend_color(Color color);
    /// Show a legend entry for the trendline once it can be fit (also
    /// requires the series itself to carry a legend entry, i.e. label() is
    /// set). Default text is an auto-generated, quantized equation string;
    /// trend_label_text() overrides it.
    ScatterSeries& trend_label(bool value = true);
    /// Append an " (R^2 = ...)" suffix — computed in the fit's own residual
    /// space (plain y for Linear/Polynomial, ln(y) for Exponential) — to
    /// the trendline legend label.
    ScatterSeries& trend_show_r2(bool value = true);
    /// Explicit override for the trendline legend text; implies
    /// trend_label(true).
    ScatterSeries& trend_label_text(std::string text);

    /// Colour each marker by a third value column through a colormap
    /// (one value per point). Overrides the single series colour.
    ScatterSeries& color_by(std::span<const double> values);
    ScatterSeries& color_by(std::initializer_list<double> values) {
        return color_by(std::span<const double>(values.begin(), values.size()));
    }
    ScatterSeries& colormap(Colormap map);
    ScatterSeries& color_range(double minimum, double maximum);
    ScatterSeries& color_midpoint(double midpoint);
    ScatterSeries& reverse_colormap(bool value = true);
    ScatterSeries& missing_color(Color color);
    /// Draw a colorbar legend for the color-by scale.
    ScatterSeries& colorbar(bool value = true);
    /// Label each point with its own y-value (detail::format_tick_value),
    /// placed at a fixed pixel offset above-and-right of the marker --
    /// deterministic; no force-directed or iterative collision avoidance.
    ScatterSeries& point_labels(bool value = true);
    /// Label each point from a separate name/annotation column instead of
    /// its y-value. Must match the series length.
    ScatterSeries& point_labels(std::vector<std::string> names);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return x_.size(); }
    Marker legend_marker() const override { return marker_; }
    bool legend_filled() const override { return true; }
    bool colorbar_enabled() const override { return color_.colorbar && color_.active(); }
    const ColorScale* colorbar_scale() const override {
        return color_.active() ? &color_.scale : nullptr;
    }
    std::pair<double, double> colorbar_value_range() const override {
        return color_.value_range();
    }
    const ErrorData& y_error_data() const { return err_y_; }
    const ErrorData& x_error_data() const { return err_x_; }
    std::vector<LegendItemInfo> legend_items(Color resolved_color,
                                             const Theme& theme) const override;

private:
    std::vector<double> x_, y_;
    ErrorData err_y_, err_x_;
    ErrorBarStyle err_style_;
    Marker marker_ = Marker::Circle;
    double marker_size_ = 3.2;
    FitKind fit_ = FitKind::None;
    int fit_degree_ = 2; ///< meaningful only for FitKind::Polynomial
    std::optional<Color> trend_color_;
    TrendlineLabel trend_label_;
    ColorEncoding color_;
    bool point_labels_ = false;
    std::vector<std::string> point_label_names_;
};

class BarSeries : public SeriesBase<BarSeries> {
public:
    BarSeries() = default;
    BarSeries(std::vector<std::string> categories, std::span<const double> values,
              bool horizontal = false);

    BarSeries& set_data(std::vector<std::string> categories, std::span<const double> values);
    BarSeries& horizontal(bool value = true);
    /// Stack this series on top of previously added stacked bar series.
    BarSeries& stacked(bool value = true);
    /// Stack and normalize each category to 100% (implies stacked).
    BarSeries& percent_stacked(bool value = true);
    /// Draw the numeric value next to each bar.
    BarSeries& value_labels(bool value = true);

    /// Symmetric error whiskers on the value dimension, one per category.
    /// Whiskers are centred on each bar (grouped bars keep their offset) and
    /// extend along the value axis — vertically for bars, horizontally for
    /// barh. Not supported on stacked/percent bars (the geometry is
    /// ambiguous); the config layer rejects that combination.
    BarSeries& value_error(std::span<const double> err);
    /// Asymmetric error whiskers on the value dimension.
    BarSeries& value_error(std::span<const double> lower, std::span<const double> upper);
    BarSeries& error_meaning(ErrorMeaning meaning);
    BarSeries& error_cap_width(double px);
    BarSeries& error_color(Color color);
    BarSeries& error_width(double px);
    /// Draw only the whisker half pointing away from the baseline (a common
    /// tidy convention for zero-based bars). Off by default (two-sided).
    BarSeries& error_one_sided(bool value = true);

    const ErrorData& value_error_data() const { return err_; }

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }
    bool wants_category_x() const override { return !horizontal_; }
    bool wants_category_y() const override { return horizontal_; }
    bool wants_zero_baseline() const override { return true; }
    bool is_bar_like() const override { return true; }
    std::vector<std::string> category_names() const override { return categories_; }
    bool legend_filled() const override { return true; }

    bool is_horizontal() const { return horizontal_; }
    bool is_stacked() const { return stacked_; }
    bool is_percent_stacked() const { return percent_; }
    const std::vector<double>& values() const { return values_; }

private:
    std::vector<std::string> categories_;
    std::vector<double> values_;
    ErrorData err_;
    ErrorBarStyle err_style_;
    bool horizontal_ = false;
    bool stacked_ = false;
    bool percent_ = false;
    bool value_labels_ = false;
};

/// Floating (range) bars: one bar per category spanning low..high.
/// Unlike regular bars there is no zero baseline.
class RangeBarSeries : public SeriesBase<RangeBarSeries> {
public:
    RangeBarSeries() = default;
    RangeBarSeries(std::vector<std::string> categories, std::span<const double> low,
                   std::span<const double> high);

    RangeBarSeries& set_data(std::vector<std::string> categories,
                             std::span<const double> low, std::span<const double> high);
    RangeBarSeries& horizontal(bool value = true);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return low_.size(); }
    bool wants_category_x() const override { return !horizontal_; }
    bool wants_category_y() const override { return horizontal_; }
    bool is_bar_like() const override { return true; }
    std::vector<std::string> category_names() const override { return categories_; }
    bool legend_filled() const override { return true; }

    bool is_horizontal() const { return horizontal_; }

private:
    std::vector<std::string> categories_;
    std::vector<double> low_, high_;
    bool horizontal_ = false;
};

/// Gantt / timeline chart: one horizontal bar per task, the first task on
/// top. Tasks, starts, and ends are stored in input order (tasks_[0] is
/// the top row); build_geometry maps input row i to category slot
/// (n-1-i) so the top-of-chart reading order matches the data order while
/// giving later dependency work a clean input-index model. Use
/// ax.x_axis().datetime() for calendar time.
///
/// Beyond the plain schedule bars each task may carry a completion
/// fraction (percent_complete: the bar is drawn in a lightened track and
/// the completed span overdrawn in full colour) and a resource group
/// (resource: distinct groups get a palette colour by first-appearance
/// order, with one legend entry per group).
///
/// Tasks may also declare finish-to-start dependencies (dependencies:
/// per-task predecessor lists referencing other tasks by their label).
/// Each edge is drawn as a deterministic elbow connector — a horizontal
/// stub out of the predecessor's end, a vertical segment, then a
/// horizontal run into the successor's start with an arrowhead. With
/// critical_path() the longest-duration chain through the dependency DAG
/// is emphasised: its bars gain a heavier outline and its connectors an
/// accent weight/colour.
class GanttSeries : public SeriesBase<GanttSeries> {
public:
    GanttSeries() = default;
    GanttSeries(std::vector<std::string> tasks, std::span<const double> start,
                std::span<const double> end);

    GanttSeries& set_data(std::vector<std::string> tasks, std::span<const double> start,
                          std::span<const double> end);
    /// Per-task completion as a percentage in [0, 100], stored internally
    /// as a fraction. Out-of-range values throw (naming the task); a
    /// missing/NaN value draws a plain bar with no progress overlay. The
    /// span must have one value per task.
    GanttSeries& percent_complete(std::span<const double> pct);
    /// Per-task resource/group label. Distinct labels are assigned palette
    /// colours in first-appearance order and each contributes one legend
    /// entry. The vector must have one value per task.
    GanttSeries& resources(std::vector<std::string> resource_per_task);
    /// Per-task finish-to-start predecessor lists: one ';'-delimited string
    /// per task, each entry naming a predecessor task by its label (blank
    /// entries and surrounding whitespace are ignored). Predecessor
    /// references resolve to the input row carrying that task label, which
    /// requires task labels to be unique. Throws (naming the offending
    /// task) on an unknown reference, a self-reference, a duplicate task
    /// label, or a dependency cycle. The vector must have one value per
    /// task.
    GanttSeries& dependencies(std::vector<std::string> predecessor_lists);
    /// Emphasise the critical path — the longest-duration chain through the
    /// dependency DAG. Only meaningful once dependencies() is set.
    GanttSeries& critical_path(bool on = true);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return start_.size(); }
    bool wants_category_y() const override { return true; } // always horizontal
    bool is_bar_like() const override { return true; }
    std::vector<std::string> category_names() const override; // tasks_ reversed
    bool legend_filled() const override { return true; }
    std::vector<LegendItemInfo> legend_items(Color resolved_color,
                                             const Theme& theme) const override;

private:
    /// Distinct resource labels in first-appearance (input) order. Empty
    /// when no resource grouping is set. The index into this list is the
    /// group's palette slot.
    std::vector<std::string> resource_order() const;

    /// Kahn topological order of the (validated, acyclic) dependency DAG.
    /// Ready nodes are drained in ascending input-row index so the order is
    /// deterministic. Precondition: deps_ is acyclic — dependencies()
    /// rejects cycles up front.
    std::vector<std::size_t> topological_order() const;

    /// The critical (longest-duration) chain through the dependency DAG.
    /// `node[i]` marks row i as critical; `edge` holds the critical
    /// (predecessor, successor) pairs. Both are empty when critical_path_
    /// is off or there are no dependencies.
    struct CriticalChain {
        std::vector<bool> node;
        std::vector<std::pair<std::size_t, std::size_t>> edge;
        bool is_edge(std::size_t pred, std::size_t succ) const;
    };
    CriticalChain critical_chain() const;

    std::vector<std::string> tasks_;
    std::vector<double> start_, end_;
    std::vector<double> percent_;        ///< fraction 0..1 per task; empty = none
    std::vector<std::string> resources_; ///< group per task; empty = none
    std::vector<std::vector<std::size_t>>
        deps_;                    ///< predecessor input-row indices per task; empty = none
    bool critical_path_ = false; ///< emphasise the longest-duration chain
};

/// Lollipop chart: a thin stem from the baseline with a dot at the value.
/// A calmer alternative to bars for many categories.
class LollipopSeries : public SeriesBase<LollipopSeries> {
public:
    LollipopSeries() = default;
    LollipopSeries(std::vector<std::string> categories, std::span<const double> values);

    LollipopSeries& set_data(std::vector<std::string> categories,
                             std::span<const double> values);
    LollipopSeries& horizontal(bool value = true);
    LollipopSeries& marker_size(double size);
    LollipopSeries& stem_width(double width);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }
    bool wants_category_x() const override { return !horizontal_; }
    bool wants_category_y() const override { return horizontal_; }
    bool wants_zero_baseline() const override { return true; }
    std::vector<std::string> category_names() const override { return categories_; }
    Marker legend_marker() const override { return Marker::Circle; }
    bool legend_filled() const override { return true; }

private:
    std::vector<std::string> categories_;
    std::vector<double> values_;
    bool horizontal_ = false;
    double marker_size_ = 4.5;
    double stem_width_ = 1.6;
};

/// Dumbbell (range dot) chart: two dots per category connected by a line
/// — before/after comparisons. Horizontal by default.
class DumbbellSeries : public SeriesBase<DumbbellSeries> {
public:
    DumbbellSeries() = default;
    DumbbellSeries(std::vector<std::string> categories, std::span<const double> start,
                   std::span<const double> end);

    DumbbellSeries& set_data(std::vector<std::string> categories,
                             std::span<const double> start, std::span<const double> end);
    DumbbellSeries& horizontal(bool value = true);
    /// Color of the start dots (default: muted gray).
    DumbbellSeries& start_color(Color color);
    /// Color of the end dots (default: the series color).
    DumbbellSeries& end_color(Color color);
    DumbbellSeries& marker_size(double size);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return start_.size(); }
    bool wants_category_x() const override { return !horizontal_; }
    bool wants_category_y() const override { return horizontal_; }
    std::vector<std::string> category_names() const override { return categories_; }
    Marker legend_marker() const override { return Marker::Circle; }
    bool legend_filled() const override { return true; }

private:
    std::vector<std::string> categories_;
    std::vector<double> start_, end_;
    bool horizontal_ = true;
    std::optional<Color> start_color_;
    std::optional<Color> end_color_;
    double marker_size_ = 4.5;
};

/// Population pyramid (age–sex pyramid): one bar pair per category,
/// diverging from a shared centre — left values extend left, right
/// values extend right. Also covers tornado and butterfly charts.
/// Categories keep their data order (age bands must not be re-sorted);
/// values must be non-negative and are mirrored internally. By default
/// the two halves are separated by a centre gap sized to the category
/// labels, each half carrying its own zero tick (a mirrored magnitude
/// scale); joined(true) makes the halves meet on one centre line
/// instead, with halo-backed labels on it. Category labels sit in the
/// centre (default) or at the left edge as ordinary axis labels.
class PopulationPyramidSeries : public SeriesBase<PopulationPyramidSeries> {
public:
    enum class CategoryLabels { Center, Edge };

    PopulationPyramidSeries() = default;
    PopulationPyramidSeries(std::vector<std::string> categories,
                            std::span<const double> left,
                            std::span<const double> right);

    PopulationPyramidSeries& set_data(std::vector<std::string> categories,
                                      std::span<const double> left,
                                      std::span<const double> right);
    /// Legend label of the left side (no label = no legend entry).
    PopulationPyramidSeries& left_label(std::string text);
    /// Legend label of the right side (no label = no legend entry).
    PopulationPyramidSeries& right_label(std::string text);
    /// Colour of the left bars (default: the series colour).
    PopulationPyramidSeries& left_color(Color color);
    /// Colour of the right bars (default: theme series colour 2).
    PopulationPyramidSeries& right_color(Color color);
    /// Draw the numeric value at each bar end.
    PopulationPyramidSeries& value_labels(bool value = true);
    /// Where the category labels go (default: Center).
    PopulationPyramidSeries& category_labels(CategoryLabels placement);
    /// Join the two halves on a single centre line (the labels then ride
    /// on halo backdrops). Default: false — a centre gap separates the
    /// halves and carries the category labels.
    PopulationPyramidSeries& joined(bool value = true);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return left_.size() + right_.size(); }
    bool wants_category_y() const override { return true; }
    bool wants_zero_baseline() const override { return true; }
    std::vector<std::string> category_names() const override { return categories_; }
    bool legend_filled() const override { return true; }
    bool draws_own_category_labels() const override {
        return category_labels_ == CategoryLabels::Center;
    }
    bool wants_mirrored_value_axis() const override { return !joined_; }
    bool is_joined() const { return joined_; }
    std::vector<LegendItemInfo> legend_items(Color resolved_color,
                                             const Theme& theme) const override;

    const std::vector<double>& left_values() const { return left_; }
    const std::vector<double>& right_values() const { return right_; }

private:
    std::vector<std::string> categories_;
    std::vector<double> left_, right_;
    std::string left_label_, right_label_;
    std::optional<Color> left_color_;
    std::optional<Color> right_color_;
    bool value_labels_ = false;
    bool joined_ = false;
    CategoryLabels category_labels_ = CategoryLabels::Center;
};

/// Funnel chart: centered horizontal bars, widths proportional to the
/// stage values, drawn top-to-bottom. Lives in an axis-free panel like
/// pie/doughnut; stage names sit in a left gutter.
class FunnelSeries : public SeriesBase<FunnelSeries> {
public:
    FunnelSeries() = default;
    FunnelSeries(std::vector<std::string> stages, std::span<const double> values);

    FunnelSeries& set_data(std::vector<std::string> stages, std::span<const double> values);
    FunnelSeries& value_labels(bool value);
    /// Show each stage as a percentage of the first stage.
    FunnelSeries& percent_labels(bool value = true);
    FunnelSeries& stage_labels(bool value);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }
    CoordinateSystem coordinate_system() const override {
        return CoordinateSystem::PartToWhole;
    }

    const std::vector<std::string>& stages() const { return stages_; }
    const std::vector<double>& values() const { return values_; }

private:
    std::vector<std::string> stages_;
    std::vector<double> values_;
    bool value_labels_ = true;
    bool percent_labels_ = false;
    bool stage_labels_ = true;
};

/// Bars at numeric (or datetime) x positions — volume panes, event
/// counts over time. Bar width defaults to 80% of the smallest x spacing.
class XBarSeries : public SeriesBase<XBarSeries> {
public:
    XBarSeries() = default;
    XBarSeries(std::span<const double> x, std::span<const double> heights);

    XBarSeries& set_data(std::span<const double> x, std::span<const double> heights);
    /// Bar width in pixels; 0 = automatic from the x spacing.
    XBarSeries& bar_width(double px);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return x_.size(); }
    bool wants_zero_baseline() const override { return true; }
    bool legend_filled() const override { return true; }

private:
    std::vector<double> x_, heights_;
    double bar_width_ = 0.0;
};

class AreaSeries : public SeriesBase<AreaSeries> {
public:
    AreaSeries() = default;
    AreaSeries(std::span<const double> x, std::span<const double> y);

    AreaSeries& set_data(std::span<const double> x, std::span<const double> y);
    AreaSeries& fill_alpha(double alpha);
    AreaSeries& baseline(double y0);
    /// Stack this series on top of previously added stacked area series.
    /// All stacked area series in a panel must share identical x values.
    AreaSeries& stacked(bool value = true);
    /// Stack and normalize each x position to 100% (implies stacked).
    AreaSeries& percent_stacked(bool value = true);
    /// Draw the fill boundary as a staircase (StepSeries's Post/Pre/Mid
    /// convention) instead of straight edges between points. Applies to
    /// both the plain fill and the stacked/percent-stacked polygon; when
    /// several stepped areas in a panel share x values (already required
    /// for stacking) their risers land at the same x, so the bands stack
    /// cleanly instead of tearing along interpolated slopes.
    AreaSeries& step(StepMode mode);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return x_.size(); }
    bool wants_zero_baseline() const override { return true; }
    bool legend_filled() const override { return true; }

    bool is_stacked() const { return stacked_ || percent_; }
    bool is_percent_stacked() const { return percent_; }
    const std::vector<double>& x_data() const { return x_; }
    const std::vector<double>& y_data() const { return y_; }
    double fill_alpha_value() const { return fill_alpha_; }
    const std::optional<StepMode>& step_mode() const { return step_; }

private:
    std::vector<double> x_, y_;
    double fill_alpha_ = 0.25;
    bool fill_alpha_explicit_ = false;
    double baseline_ = 0.0;
    bool stacked_ = false;
    bool percent_ = false;
    std::optional<StepMode> step_;
};

/// Histogram of one numeric sample: equal-width bins over an automatically
/// or explicitly chosen range, each drawn as a bar from the baseline.
///
/// The bin COUNT is resolved in this order: an explicit bins() (or the
/// `bins` argument to the constructor/set_data(), when > 0) always wins;
/// otherwise bin_width() picks a fixed width instead of a count; otherwise
/// bin_rule() (default Sqrt) derives a count from the sample size.
/// bin_width() is mutually exclusive with an explicit bins()/bin_rule() --
/// both name a bin count via a fundamentally different mechanism, so
/// mixing them is refused (a clear error) rather than silently favouring
/// one. An explicit bins() alongside bin_rule() is not a conflict: bins()
/// simply overrides the rule, same as before this option existed.
///
/// Bin edges are an EXACT equal-width partition of the covered range, not
/// rounded to "nice" numbers the way an axis picks tick spacing -- the bin
/// count/width is itself the quantity the caller asked for, so rounding
/// the edges would silently change the answer. Bins are half-open
/// [lo, hi) except the last, which is closed [lo, hi], so the maximum
/// value lands in the last bin rather than a phantom extra one.
///
/// range() pins the covered domain explicitly; values outside it are
/// still plotted, clamped into the first/last bin rather than dropped --
/// a histogram that silently discards data is worse than one that piles
/// outliers at the edges, and clamping keeps the bin grid exactly as
/// pinned instead of quietly growing it back out to fit the data.
/// trim_percentile() only affects how the covered range/bin width is
/// *chosen* when range() is not explicit: the top/bottom p% of values (by
/// rank) are ignored for that sizing decision, but every value --
/// including the trimmed-for-sizing ones -- is still counted into
/// whichever bin it falls in (mirrors Google Charts' `lastBucketPercentile`).
class HistogramSeries : public SeriesBase<HistogramSeries> {
public:
    HistogramSeries() = default;
    /// bins <= 0 leaves the count automatic (bin_width()/bin_rule()).
    explicit HistogramSeries(std::span<const double> values, int bins = 0);

    /// Replace the sample; previously configured options are kept.
    HistogramSeries& set_data(std::span<const double> values);
    /// Replace the sample and set an explicit bin count in one call
    /// (equivalent to set_data(values).bins(bins) when bins > 0).
    HistogramSeries& set_data(std::span<const double> values, int bins);

    /// Explicit bin count; always overrides bin_rule(). Mutually
    /// exclusive with bin_width().
    HistogramSeries& bins(int count);
    /// Rule used to derive the bin count when bins() is not set. Mutually
    /// exclusive with bin_width(). Default: Sqrt.
    HistogramSeries& bin_rule(HistogramBinRule rule);
    /// Fixed bin width; mutually exclusive with bins()/bin_rule().
    HistogramSeries& bin_width(double width);
    /// Explicit covered range; out-of-range values are clamped into the
    /// first/last bin rather than dropped. Default: the (optionally
    /// trim_percentile()-narrowed) finite data range.
    HistogramSeries& range(double minimum, double maximum);
    /// Ignore the top/bottom p percent of values (each side) when the bin
    /// range/width is chosen automatically -- every value is still
    /// plotted. No effect when range() is explicit. p must be in [0, 50).
    HistogramSeries& trim_percentile(double p);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }
    bool wants_zero_baseline() const override { return true; }
    bool legend_filled() const override { return true; }

    /// Computed bin edges (bins + 1 values) and counts.
    const std::vector<double>& bin_edges() const { return edges_; }
    const std::vector<double>& bin_counts() const { return counts_; }

private:
    void compute();
    std::vector<double> values_;
    int bins_ = 0; ///< explicit count; 0 = derive from bin_rule_/bin_width_
    HistogramBinRule bin_rule_ = HistogramBinRule::Sqrt;
    bool bin_rule_explicit_ = false;
    double bin_width_ = 0.0; ///< explicit fixed width; 0 = unset
    std::optional<std::pair<double, double>> range_;
    double trim_percentile_ = 0.0;
    std::vector<double> edges_, counts_;
};

/// Box-and-whisker plot: one box per category.
/// Boxes span Q1..Q3, the line marks the median, whiskers extend to the
/// most extreme data point within 1.5 × IQR, outliers are drawn as points.
class BoxPlotSeries : public SeriesBase<BoxPlotSeries> {
public:
    BoxPlotSeries() = default;
    BoxPlotSeries(std::vector<std::string> categories, std::vector<std::vector<double>> data);

    BoxPlotSeries& set_data(std::vector<std::string> categories,
                            std::vector<std::vector<double>> data);
    BoxPlotSeries& show_outliers(bool value);

    struct Stats {
        double q1 = 0, median = 0, q3 = 0;
        double whisker_lo = 0, whisker_hi = 0;
        std::vector<double> outliers;
        bool valid = false;
    };
    /// Computed statistics for category i (exposed for tests).
    static Stats compute_stats(std::vector<double> values);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override {
        std::size_t n = 0;
        for (const auto& v : data_) n += v.size();
        return n;
    }
    bool wants_category_x() const override { return true; }
    bool is_bar_like() const override { return false; }
    std::vector<std::string> category_names() const override { return categories_; }
    bool legend_filled() const override { return true; }

private:
    std::vector<std::string> categories_;
    std::vector<std::vector<double>> data_;
    bool show_outliers_ = true;
};

/// Smoothed distribution curve (gaussian kernel density estimate).
class DensitySeries : public SeriesBase<DensitySeries> {
public:
    DensitySeries() = default;
    explicit DensitySeries(std::span<const double> values, double bandwidth = 0.0);

    DensitySeries& set_data(std::span<const double> values);
    /// Kernel bandwidth; 0 = automatic (Scott's rule).
    DensitySeries& bandwidth(double h);
    DensitySeries& fill(bool value);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }
    bool wants_zero_baseline() const override { return true; }
    bool legend_filled() const override { return true; }

private:
    std::vector<double> values_;
    double bandwidth_ = 0.0;
    bool fill_ = true;
};

/// Violin plot: mirrored density per category.
class ViolinSeries : public SeriesBase<ViolinSeries> {
public:
    ViolinSeries() = default;
    ViolinSeries(std::vector<std::string> categories, std::vector<std::vector<double>> data);

    ViolinSeries& set_data(std::vector<std::string> categories,
                           std::vector<std::vector<double>> data);
    ViolinSeries& bandwidth(double h);
    /// Draw the median as a small horizontal line (default on).
    ViolinSeries& show_median(bool value);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override {
        std::size_t n = 0;
        for (const auto& v : data_) n += v.size();
        return n;
    }
    bool wants_category_x() const override { return true; }
    std::vector<std::string> category_names() const override { return categories_; }
    bool legend_filled() const override { return true; }

private:
    std::vector<std::string> categories_;
    std::vector<std::vector<double>> data_;
    double bandwidth_ = 0.0;
    bool show_median_ = true;
};

class StepSeries : public SeriesBase<StepSeries> {
public:
    StepSeries() = default;
    StepSeries(std::span<const double> x, std::span<const double> y,
               StepMode mode = StepMode::Post);

    StepSeries& set_data(std::span<const double> x, std::span<const double> y);
    StepSeries& mode(StepMode m);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return x_.size(); }

private:
    std::vector<double> x_, y_;
    StepMode mode_ = StepMode::Post;
};

class HeatmapSeries : public SeriesBase<HeatmapSeries> {
public:
    HeatmapSeries() = default;
    HeatmapSeries(std::vector<std::string> x_labels, std::vector<std::string> y_labels,
                  std::span<const double> values, std::size_t rows, std::size_t cols);

    HeatmapSeries& set_data(std::vector<std::string> x_labels,
                            std::vector<std::string> y_labels,
                            std::span<const double> values, std::size_t rows,
                            std::size_t cols);
    HeatmapSeries& colormap(Colormap map);
    HeatmapSeries& color_range(double minimum, double maximum);
    HeatmapSeries& color_midpoint(double midpoint);
    HeatmapSeries& reverse_colormap(bool value = true);
    HeatmapSeries& missing_color(Color color);
    HeatmapSeries& cell_labels(bool value = true);
    HeatmapSeries& value_format(std::string format);
    HeatmapSeries& colorbar(bool value = true);
    HeatmapSeries& square_cells(bool value = true);
    HeatmapSeries& normalize(MatrixNormalize mode);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }
    bool wants_category_x() const override { return true; }
    bool wants_category_y() const override { return true; }
    std::vector<std::string> x_category_names() const override { return x_labels_; }
    std::vector<std::string> y_category_names() const override { return y_labels_; }
    bool legend_filled() const override { return true; }

    const std::vector<std::string>& x_labels() const { return x_labels_; }
    const std::vector<std::string>& y_labels() const { return y_labels_; }
    const std::vector<double>& values() const { return values_; }
    std::size_t rows() const { return rows_; }
    std::size_t cols() const { return cols_; }
    const ColorScale& color_scale() const { return color_scale_; }
    bool colorbar_enabled() const override { return colorbar_; }
    const ColorScale* colorbar_scale() const override { return &color_scale_; }
    std::pair<double, double> colorbar_value_range() const override { return finite_value_range(); }
    std::pair<double, double> finite_value_range() const;

private:
    std::vector<double> normalized_values() const;

    std::vector<std::string> x_labels_;
    std::vector<std::string> y_labels_;
    std::vector<double> values_;
    std::size_t rows_ = 0;
    std::size_t cols_ = 0;
    ColorScale color_scale_;
    bool cell_labels_ = false;
    bool colorbar_ = false;
    bool square_cells_ = false;
    MatrixNormalize normalize_ = MatrixNormalize::None;
    std::string value_format_;
};

/// Polar plot of continuous (theta, r) samples: a line (optionally
/// closed and filled) drawn over an angular grid with radial rings.
/// Angles are mathematical convention — counterclockwise from the
/// positive x axis — in radians by default (degrees(true) opts in to
/// degree input). NaN r values follow the missing policy (Gap breaks
/// the line, Drop removes points).
class PolarSeries : public SeriesBase<PolarSeries> {
public:
    PolarSeries() = default;
    PolarSeries(std::span<const double> theta, std::span<const double> r);

    PolarSeries& set_data(std::span<const double> theta, std::span<const double> r);
    /// Interpret theta input as degrees instead of radians.
    PolarSeries& degrees(bool value = true);
    PolarSeries& marker(Marker m, double size = 3.0);
    /// Connect the last sample back to the first.
    PolarSeries& close(bool value = true);
    /// Fill the (closed) outline toward the center.
    PolarSeries& fill(bool value = true);
    PolarSeries& fill_alpha(double alpha);
    PolarSeries& dash(std::string svg_dash_array);
    /// Explicit radial range (default: 0 to a nice data maximum).
    PolarSeries& range(double minimum, double maximum);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return theta_.size(); }
    CoordinateSystem coordinate_system() const override { return CoordinateSystem::Polar; }
    Marker legend_marker() const override { return marker_; }

    const std::vector<double>& theta_data() const { return theta_; }
    const std::vector<double>& r_data() const { return r_; }
    /// Theta of sample i in DEGREES, whichever unit the data was given in.
    /// Degrees because that is the unit the polar geometry is built in.
    double theta_degrees(std::size_t i) const;
    const std::optional<std::pair<double, double>>& explicit_range() const { return range_; }

private:
    std::vector<double> theta_, r_;
    bool degrees_ = false;
    Marker marker_ = Marker::None;
    double marker_size_ = 3.0;
    bool close_ = false;
    bool fill_ = false;
    double fill_alpha_ = 0.18;
    DashPattern dash_;
    std::optional<std::pair<double, double>> range_;
};

/// Contour plot over a rectangular grid: iso-lines (default) or
/// filled bands between consecutive levels (marching squares /
/// isoband clipping). `x` has cols entries and `y` rows entries,
/// both strictly increasing; `z` is row-major rows × cols. Cells
/// touching NaN values are skipped.
class ContourSeries : public SeriesBase<ContourSeries> {
public:
    ContourSeries() = default;
    ContourSeries(std::span<const double> x, std::span<const double> y,
                  std::span<const double> z, std::size_t rows, std::size_t cols);

    ContourSeries& set_data(std::span<const double> x, std::span<const double> y,
                            std::span<const double> z, std::size_t rows,
                            std::size_t cols);
    /// Target number of automatically chosen (nice) levels (default 7).
    ContourSeries& level_count(int count);
    /// Explicit iso-levels (strictly ascending); overrides level_count.
    ContourSeries& levels(std::span<const double> values);
    /// Filled bands between consecutive levels instead of iso-lines.
    ContourSeries& filled(bool value = true);
    ContourSeries& colormap(Colormap map);
    ContourSeries& color_range(double minimum, double maximum);
    ContourSeries& color_midpoint(double midpoint);
    ContourSeries& reverse_colormap(bool value = true);
    ContourSeries& missing_color(Color color);
    ContourSeries& colorbar(bool value = true);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return z_.size(); }
    bool legend_filled() const override { return filled_; }
    bool colorbar_enabled() const override {
        return colorbar_ && (filled_ || !explicit_color().has_value());
    }
    const ColorScale* colorbar_scale() const override { return &color_scale_; }
    std::pair<double, double> colorbar_value_range() const override;

    /// Iso-levels resolved against the current data (exposed for tests).
    std::vector<double> resolved_levels() const;
    bool is_filled() const { return filled_; }

private:
    std::pair<double, double> finite_z_range() const;

    std::vector<double> x_, y_, z_;
    std::size_t rows_ = 0;
    std::size_t cols_ = 0;
    int level_count_ = 7;
    std::vector<double> explicit_levels_;
    bool filled_ = false;
    ColorScale color_scale_;
    bool colorbar_ = false;
};

/// A numeric-axis raster grid. Unlike HeatmapSeries, x/y are numeric
/// coordinates (strictly increasing cell centres), so the image composes
/// naturally with scientific axes, annotations, and other Cartesian series.
class SpectrogramSeries : public SeriesBase<SpectrogramSeries> {
public:
    SpectrogramSeries() = default;
    SpectrogramSeries(std::span<const double> x, std::span<const double> y,
                      std::span<const double> values, std::size_t rows,
                      std::size_t cols);

    SpectrogramSeries& set_data(std::span<const double> x, std::span<const double> y,
                                std::span<const double> values, std::size_t rows,
                                std::size_t cols);
    SpectrogramSeries& colormap(Colormap map);
    SpectrogramSeries& color_range(double minimum, double maximum);
    SpectrogramSeries& color_midpoint(double midpoint);
    SpectrogramSeries& reverse_colormap(bool value = true);
    SpectrogramSeries& missing_color(Color color);
    SpectrogramSeries& colorbar(bool value = true);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }
    bool legend_filled() const override { return true; }
    bool colorbar_enabled() const override { return colorbar_; }
    const ColorScale* colorbar_scale() const override { return &color_scale_; }
    std::pair<double, double> colorbar_value_range() const override;

private:
    std::vector<double> x_, y_, values_;
    std::size_t rows_ = 0;
    std::size_t cols_ = 0;
    ColorScale color_scale_;
    bool colorbar_ = false;
};

/// One deterministic hexagonal aggregate produced by HexbinSeries.
struct HexBin {
    double x = 0.0;
    double y = 0.0;
    std::size_t count = 0;

    bool operator==(const HexBin&) const = default;
};

/// Hexagonal 2-D density aggregation for large scatter clouds. Binning is
/// performed in normalized data space, so it is independent of output size
/// and byte-deterministic across SVG/PNG/PDF/SIXEL backends.
class HexbinSeries : public SeriesBase<HexbinSeries> {
public:
    HexbinSeries() = default;
    HexbinSeries(std::span<const double> x, std::span<const double> y, int bins = 24);

    HexbinSeries& set_data(std::span<const double> x, std::span<const double> y);
    HexbinSeries& bins(int count);
    HexbinSeries& min_count(std::size_t count);
    HexbinSeries& colormap(Colormap map);
    HexbinSeries& color_range(double minimum, double maximum);
    HexbinSeries& color_midpoint(double midpoint);
    HexbinSeries& reverse_colormap(bool value = true);
    HexbinSeries& missing_color(Color color);
    HexbinSeries& colorbar(bool value = true);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return x_.size(); }
    bool legend_filled() const override { return true; }
    bool colorbar_enabled() const override { return colorbar_; }
    const ColorScale* colorbar_scale() const override { return &color_scale_; }
    std::pair<double, double> colorbar_value_range() const override;
    std::vector<HexBin> resolved_bins() const;

private:
    std::vector<double> x_, y_;
    int bins_ = 24;
    std::size_t min_count_ = 1;
    ColorScale color_scale_;
    bool colorbar_ = false;
};

/// Normal Q-Q plot against a standard-normal theoretical distribution.
/// Samples are sorted; non-finite values are ignored. The reference line
/// uses the sample mean and standard deviation and is emitted by the same
/// series so fluent styling stays coherent.
class QQSeries : public SeriesBase<QQSeries> {
public:
    QQSeries() = default;
    explicit QQSeries(std::span<const double> values);

    QQSeries& set_data(std::span<const double> values);
    QQSeries& marker_size(double size);
    QQSeries& reference(bool value = true);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return samples_.size(); }
    Marker legend_marker() const override { return Marker::Circle; }

    const std::vector<double>& theoretical_quantiles() const { return theoretical_; }
    const std::vector<double>& sample_quantiles() const { return samples_; }

private:
    std::vector<double> theoretical_;
    std::vector<double> samples_;
    double mean_ = 0.0;
    double stddev_ = 1.0;
    double marker_size_ = 3.2;
    bool reference_ = true;
};

struct WindRoseBin {
    /// Degrees, mathematical convention (counterclockwise from east).
    /// Degrees rather than radians because 360/n is exact for every bin
    /// count in use and 2*pi/n is exact for none: the sector boundaries of
    /// a 16-bin rose are then exactly 22.5 degrees apart, and the four
    /// cardinal edges land exactly on the axes.
    double angle_start = 0.0;
    double angle_end = 0.0;
    double value = 0.0;

    bool operator==(const WindRoseBin&) const = default;
};

/// Direction-frequency sectors on the continuous polar panel. Input angles
/// use the same mathematical convention as PolarSeries (radians by default,
/// .degrees() opt-in); optional weights default to one observation each.
/// The resolved bins are always in degrees, whichever the input was.
class WindRoseSeries : public SeriesBase<WindRoseSeries> {
public:
    WindRoseSeries() = default;
    WindRoseSeries(std::span<const double> theta, std::span<const double> weights,
                   int bins = 16);

    WindRoseSeries& set_data(std::span<const double> theta,
                             std::span<const double> weights = {});
    WindRoseSeries& bins(int count);
    WindRoseSeries& degrees(bool value = true);
    WindRoseSeries& fill_alpha(double alpha);
    WindRoseSeries& range(double minimum, double maximum);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return theta_.size(); }
    CoordinateSystem coordinate_system() const override { return CoordinateSystem::Polar; }
    bool legend_filled() const override { return true; }

    std::vector<WindRoseBin> resolved_bins() const;
    const std::optional<std::pair<double, double>>& explicit_range() const { return range_; }

private:
    std::vector<double> theta_, weights_;
    int bins_ = 16;
    bool degrees_ = false;
    double fill_alpha_ = 0.62;
    std::optional<std::pair<double, double>> range_;
};

class StripSeries : public SeriesBase<StripSeries> {
public:
    StripSeries() = default;
    StripSeries(std::vector<std::string> categories, std::span<const double> values);

    StripSeries& set_data(std::vector<std::string> categories, std::span<const double> values);
    StripSeries& marker_size(double size);
    StripSeries& jitter(double width);
    StripSeries& alpha(double alpha);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }
    bool wants_category_x() const override { return true; }
    std::vector<std::string> x_category_names() const override { return order_; }
    Marker legend_marker() const override { return Marker::Circle; }
    bool legend_filled() const override { return true; }

private:
    std::vector<std::string> categories_;
    std::vector<std::string> order_;
    std::vector<std::size_t> indices_;
    std::vector<double> values_;
    double marker_size_ = 2.6;
    double jitter_ = 0.22;
    double alpha_ = 0.55;
};

/// Pie or doughnut chart: one slice per category.
///
/// Values must be finite and non-negative with a positive total. Slices
/// are colored from the theme palette; the legend lists one entry per
/// slice. Lives in a PartToWhole panel (no rectangular axes).
class PieSeries : public SeriesBase<PieSeries> {
public:
    PieSeries() = default;
    PieSeries(std::vector<std::string> labels, std::span<const double> values);

    PieSeries& set_data(std::vector<std::string> labels, std::span<const double> values);
    /// Text drawn on each slice (default: none, legend only).
    PieSeries& slice_labels(PieLabelMode mode);
    /// Angle of the first slice edge in degrees clockwise from 12 o'clock.
    PieSeries& start_angle(double degrees);
    PieSeries& clockwise(bool value);
    /// Hole radius as a fraction of the outer radius (0 = pie). doughnut()
    /// uses 0.55 by default.
    PieSeries& inner_radius(double ratio);
    /// Pull slice `index` outward by `ratio` of the outer radius.
    PieSeries& explode(std::size_t index, double ratio = 0.08);
    /// Fold every slice whose share of the total is strictly below
    /// `fraction` (0..1) into one trailing "Other" slice, before
    /// percentages, labels, and the legend are computed -- so everything
    /// downstream reflects the folded set consistently. 0 (the default)
    /// disables folding. Folding is stable: slices are tested in input
    /// order and every slice below the threshold merges into the SAME
    /// single "Other" slice, never one bucket per tiny slice.
    PieSeries& other_threshold(double fraction);
    /// Legend/slice text for the folded slice (default "Other").
    PieSeries& other_label(std::string text);
    /// Fill colour of the folded slice (default: the theme's muted
    /// neutral colour, matching the "grouped-away" convention used
    /// elsewhere in this engine for de-emphasized elements).
    PieSeries& other_color(Color color);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }
    CoordinateSystem coordinate_system() const override {
        return CoordinateSystem::PartToWhole;
    }
    std::vector<LegendItemInfo> legend_items(Color resolved_color,
                                             const Theme& theme) const override;

    const std::vector<std::string>& labels() const { return labels_; }
    const std::vector<double>& values() const { return values_; }
    double inner_radius_ratio() const { return inner_; }

    /// One slice after other_threshold folding, in draw/legend order.
    struct PieSlice {
        std::string label;
        double value = 0.0;
        bool is_other = false;
        /// Original (pre-fold) slice indices this entry represents -- one
        /// element unless is_other, which may combine several.
        std::vector<std::size_t> source_indices;
    };
    /// The slices actually drawn/legended once other_threshold folding is
    /// applied. Exposed for tests; also used internally by legend_items()
    /// and build_geometry() so both stay consistent with the fold.
    std::vector<PieSlice> resolved_slices() const;

    PieLabelMode label_mode() const { return label_mode_; }

    /// One outside callout for legend:labeled, in pixel space for the given
    /// pie center and outer radius. `anchor` sits on the outer arc at the
    /// slice's mid-angle; `dx`/`dy` are the unit outward direction
    /// (sin(mid), -cos(mid)) and `dx >= 0` selects the right-hand column.
    struct SliceCallout {
        std::string text;
        Point anchor;
        double dx = 0.0;
        double dy = 0.0;
        Color color;
    };
    /// Callout descriptors for every drawn (value > 0) slice, in draw order.
    /// Shares the single slice walk with build_geometry() so the anchors can
    /// never drift from the sectors.
    std::vector<SliceCallout> slice_callouts(Point center, double r_outer,
                                             const Theme& theme) const;
    /// Callout texts (name plus the metric implied by the labels mode) for
    /// every drawn slice, in draw order. Geometry-independent, so layout can
    /// size the outside-space reservation before the radius is fixed.
    std::vector<std::string> callout_texts() const;
    /// Smallest drawn slice's angular span in degrees (0 when no positive
    /// slice exists); layout compares this to the labeled-legend fallback
    /// threshold.
    double min_slice_angle_deg() const;

private:
    /// One drawn (value > 0) slice with its resolved geometry. Produced by
    /// the single walk both build_geometry() and slice_callouts() consume, so
    /// their sectors and callout anchors are guaranteed identical.
    struct DrawnSlice {
        std::string label;
        double value = 0.0;
        double fraction = 0.0;
        bool is_other = false;
        Color color;
        Point center;         ///< slice center after any explode offset
        double start_angle = 0.0; ///< degrees, clockwise from 12 o'clock
        double span = 0.0;        ///< signed degrees
        double mid_deg = 0.0;     ///< (start + span/2), degrees
    };
    /// Walk the resolved slices once, applying start angle, winding, explode,
    /// and the skip-nonpositive rule identically for every consumer.
    std::vector<DrawnSlice> drawn_slices(Point center, double r_outer,
                                         const Theme& theme) const;
    /// Callout text for one slice: always the category name, plus the metric
    /// selected by the labels mode (percent/value); None/Label add none.
    std::string callout_text_for(const PieSlice& slice, double fraction) const;

    std::vector<std::string> labels_;
    std::vector<double> values_;
    PieLabelMode label_mode_ = PieLabelMode::None;
    double start_angle_ = 0.0;
    bool clockwise_ = true;
    double inner_ = 0.0;
    std::vector<std::pair<std::size_t, double>> explode_;
    double other_threshold_ = 0.0; ///< <= 0 disables folding
    std::string other_label_ = "Other";
    std::optional<Color> other_color_;
};

/// Scatter plot with a third dimension encoded as marker area.
class BubbleSeries : public SeriesBase<BubbleSeries> {
public:
    BubbleSeries() = default;
    BubbleSeries(std::span<const double> x, std::span<const double> y,
                 std::span<const double> size);

    BubbleSeries& set_data(std::span<const double> x, std::span<const double> y,
                           std::span<const double> size);
    /// Pixel radii for the smallest and largest size value (default 3..18).
    BubbleSeries& radius_range(double min_px, double max_px);
    BubbleSeries& alpha(double value);
    /// Area scaling (default) is perceptually honest; Radius exaggerates.
    BubbleSeries& size_scale(BubbleScale scale);

    /// Colour each bubble by a fourth value column through a colormap
    /// (one value per point). Overrides the single series colour.
    BubbleSeries& color_by(std::span<const double> values);
    BubbleSeries& color_by(std::initializer_list<double> values) {
        return color_by(std::span<const double>(values.begin(), values.size()));
    }
    BubbleSeries& colormap(Colormap map);
    BubbleSeries& color_range(double minimum, double maximum);
    BubbleSeries& color_midpoint(double midpoint);
    BubbleSeries& reverse_colormap(bool value = true);
    BubbleSeries& missing_color(Color color);
    /// Draw a colorbar legend for the color-by scale.
    BubbleSeries& colorbar(bool value = true);
    /// Label each bubble with its own y-value (detail::format_tick_value).
    /// Unlike line/scatter, the label is drawn CENTERED on the bubble
    /// itself -- a deterministic, fixed rule (the bubble's own center),
    /// matching how bubble charts elsewhere (e.g. Google Charts) always
    /// show an identifier on the bubble; see also the mapping overload,
    /// the usual way to supply a real per-bubble identifier. The text
    /// colour switches between a light and dark theme colour based on the
    /// bubble's own fill luminance so it stays legible on either.
    BubbleSeries& point_labels(bool value = true);
    /// Label each bubble from a separate name/annotation column (e.g. a
    /// country or product identifier) instead of its y-value. Must match
    /// the series length.
    BubbleSeries& point_labels(std::vector<std::string> names);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return x_.size(); }
    Marker legend_marker() const override { return Marker::Circle; }
    bool legend_filled() const override { return true; }
    bool colorbar_enabled() const override { return color_.colorbar && color_.active(); }
    const ColorScale* colorbar_scale() const override {
        return color_.active() ? &color_.scale : nullptr;
    }
    std::pair<double, double> colorbar_value_range() const override {
        return color_.value_range();
    }

    /// Pixel radius for one size value (exposed for tests).
    double radius_for(double size) const;

private:
    std::vector<double> x_, y_, size_;
    double r_min_ = 3.0;
    double r_max_ = 18.0;
    double alpha_ = 0.65;
    BubbleScale scale_ = BubbleScale::Area;
    ColorEncoding color_;
    bool point_labels_ = false;
    std::vector<std::string> point_label_names_;
};

/// OHLC bars or candlesticks for price/measurement ranges over x.
///
/// ohlc() draws a high–low range line with open (left) and close (right)
/// ticks; without open data only the close tick is drawn (HLC bars).
/// candlestick() draws open–close bodies with high–low wicks. Rising
/// entries (close >= open, or close >= previous close when open is
/// absent) use up_color, falling entries down_color.
class OhlcSeries : public SeriesBase<OhlcSeries> {
public:
    OhlcSeries() = default;
    /// HLC bars (no open values).
    OhlcSeries(std::span<const double> x, std::span<const double> high,
               std::span<const double> low, std::span<const double> close);
    /// Full OHLC; candles selects candlestick bodies instead of tick bars.
    OhlcSeries(std::span<const double> x, std::span<const double> open,
               std::span<const double> high, std::span<const double> low,
               std::span<const double> close, bool candles);

    OhlcSeries& up_color(Color color);
    OhlcSeries& down_color(Color color);
    /// Body (candlestick) or tick (OHLC) width in pixels; 0 = automatic.
    OhlcSeries& body_width(double px);
    OhlcSeries& wick_width(double px);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return x_.size(); }
    std::vector<LegendItemInfo> legend_items(Color resolved_color,
                                             const Theme& theme) const override;

    bool is_candlestick() const { return candles_; }
    bool has_open() const { return !open_.empty(); }

private:
    void validate() const;

    std::vector<double> x_, open_, high_, low_, close_;
    bool candles_ = false;
    std::optional<Color> up_color_;
    std::optional<Color> down_color_;
    double body_width_ = 0.0; ///< 0 = automatic from x spacing
    double wick_width_ = 0.0; ///< 0 = automatic from stroke width
};

/// Radar (spider) chart: one polygon over shared category spokes.
///
/// All radar series in a panel must share the same categories in the
/// same order; at least three categories are required.
class RadarSeries : public SeriesBase<RadarSeries> {
public:
    RadarSeries() = default;
    RadarSeries(std::vector<std::string> categories, std::span<const double> values);

    RadarSeries& set_data(std::vector<std::string> categories,
                          std::span<const double> values);
    RadarSeries& filled(bool value);
    RadarSeries& markers(bool value);
    /// Explicit radial value range (default: 0 to a nice data maximum).
    RadarSeries& range(double minimum, double maximum);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }
    CoordinateSystem coordinate_system() const override { return CoordinateSystem::Radar; }
    std::vector<std::string> category_names() const override { return categories_; }
    bool legend_filled() const override { return false; }

    const std::vector<std::string>& categories() const { return categories_; }
    const std::vector<double>& values() const { return values_; }
    const std::optional<std::pair<double, double>>& explicit_range() const { return range_; }

private:
    std::vector<std::string> categories_;
    std::vector<double> values_;
    bool filled_ = true;
    bool markers_ = false;
    std::optional<std::pair<double, double>> range_;
};

/// Waterfall chart: running total of signed contributions per category.
///
/// Entries marked as totals (by index) are drawn as absolute bars from
/// zero in a neutral color. Consecutive bars are linked by connectors.
class WaterfallSeries : public SeriesBase<WaterfallSeries> {
public:
    WaterfallSeries() = default;
    WaterfallSeries(std::vector<std::string> categories, std::span<const double> values);

    WaterfallSeries& set_data(std::vector<std::string> categories,
                              std::span<const double> values);
    /// Mark entry `index` as a total: an absolute bar from zero at the
    /// running value. A nonzero value overrides the running value, which
    /// makes an initial "Start" baseline possible.
    WaterfallSeries& total(std::size_t index);
    WaterfallSeries& up_color(Color color);
    WaterfallSeries& down_color(Color color);
    WaterfallSeries& total_color(Color color);
    WaterfallSeries& connectors(bool value);
    WaterfallSeries& value_labels(bool value = true);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }
    bool wants_category_x() const override { return true; }
    bool wants_zero_baseline() const override { return true; }
    std::vector<std::string> category_names() const override { return categories_; }
    bool legend_filled() const override { return true; }

    /// Bottom/top of bar i in data coordinates (exposed for tests).
    std::pair<double, double> bar_span(std::size_t index) const;

private:
    std::vector<std::string> categories_;
    std::vector<double> values_;
    std::vector<bool> totals_;
    std::optional<Color> up_color_;
    std::optional<Color> down_color_;
    std::optional<Color> total_color_;
    bool connectors_ = true;
    bool value_labels_ = false;
};

class ECDFSeries : public SeriesBase<ECDFSeries> {
public:
    ECDFSeries() = default;
    explicit ECDFSeries(std::span<const double> values);

    ECDFSeries& set_data(std::span<const double> values);

    Extent extent() const override;
    void build_geometry(const detail::GeomContext& ctx,
                        std::vector<SceneItem>& out) const override;
    std::size_t point_count() const override { return values_.size(); }

private:
    std::vector<double> values_;
};

} // namespace cplot
