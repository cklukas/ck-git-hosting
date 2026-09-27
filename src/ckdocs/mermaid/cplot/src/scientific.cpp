// ckplot — scientific chart families and convenience recipes
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cplot/axes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <tuple>
#include <utility>

#include <cworks/app_error.hpp>
#include <cworks/math.hpp>
#include <cworks/trig.hpp>

#include "cplot/figure.hpp"
#include "cplot/theme.hpp"
#include "internal.hpp"

namespace cplot {

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kSqrt3 = 1.732050807568877293527446341505872367;

std::pair<double, double> padded_constant(double input) {
    const long double value = static_cast<long double>(input);
    const long double delta = std::max(0.5L, std::abs(value) * 1.0e-12L);
    const long double max = std::numeric_limits<double>::max();
    double lo = static_cast<double>(std::max(-max, value - delta));
    double hi = static_cast<double>(std::min(max, value + delta));
    if (!(hi > lo)) {
        lo = std::nextafter(input, -std::numeric_limits<double>::infinity());
        hi = std::nextafter(input, std::numeric_limits<double>::infinity());
    }
    if (!std::isfinite(lo)) lo = input;
    if (!std::isfinite(hi)) hi = input;
    return {lo, hi};
}

void require_grid(std::span<const double> x, std::span<const double> y,
                  std::span<const double> values, std::size_t rows,
                  std::size_t cols, const char* what) {
    if (rows == 0 || cols == 0)
        throw Error(cworks::validation_failed(std::string(what) +
            ": rows and columns must be positive"));
    if (cols > std::numeric_limits<std::size_t>::max() / rows)
        throw Error(cworks::validation_failed(std::string(what) +
            ": rows * columns overflows size_t"));
    if (x.size() != cols || y.size() != rows || values.size() != rows * cols) {
        throw Error(cworks::validation_failed(std::string(what) +
            ": x/y/value sizes must be cols/rows/rows*cols (got " + std::to_string(x.size()) + "/" +
            std::to_string(y.size()) + "/" + std::to_string(values.size()) + ", expected " +
            std::to_string(cols) + "/" + std::to_string(rows) + "/" + std::to_string(rows * cols) +
            ")"));
    }
    const auto increasing = [](std::span<const double> values) {
        if (values.empty()) return false;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (!std::isfinite(values[i])) return false;
            if (i && !(values[i] > values[i - 1])) return false;
        }
        return true;
    };
    if (!increasing(x) || !increasing(y))
        throw Error(cworks::validation_failed(std::string(what) +
            ": x and y coordinates must be finite and strictly increasing"));
}

std::vector<double> cell_edges(const std::vector<double>& centers) {
    if (centers.empty()) return {};
    std::vector<double> edges(centers.size() + 1);
    if (centers.size() == 1) {
        std::tie(edges[0], edges[1]) = padded_constant(centers[0]);
        return edges;
    }
    edges[0] = centers[0] - (centers[1] - centers[0]) / 2.0;
    for (std::size_t i = 1; i < centers.size(); ++i)
        edges[i] = (centers[i - 1] + centers[i]) / 2.0;
    edges.back() = centers.back() + (centers.back() - centers[centers.size() - 2]) / 2.0;
    return edges;
}

std::pair<double, double> finite_range(std::span<const double> values) {
    double lo = 0.0, hi = 1.0;
    bool valid = false;
    for (double value : values) {
        if (!std::isfinite(value)) continue;
        if (!valid) {
            lo = hi = value;
            valid = true;
        } else {
            lo = std::min(lo, value);
            hi = std::max(hi, value);
        }
    }
    if (!valid) return {0.0, 1.0};
    if (lo == hi) return padded_constant(lo);
    return {lo, hi};
}

Extent finite_xy_extent(std::span<const double> x, std::span<const double> y) {
    Extent extent;
    for (std::size_t i = 0; i < std::min(x.size(), y.size()); ++i) {
        if (std::isfinite(x[i]) && std::isfinite(y[i])) extent.include(x[i], y[i]);
    }
    const auto expand_constant = [](double& lo, double& hi) {
        if (lo != hi) return;
        std::tie(lo, hi) = padded_constant(lo);
    };
    if (extent.valid) {
        expand_constant(extent.x_lo, extent.x_hi);
        expand_constant(extent.y_lo, extent.y_hi);
    }
    return extent;
}

double normalized_between(double value, double lo, double hi) {
    if (!(hi > lo)) return 0.5;
    // Only opposite-signed endpoints can overflow a direct subtraction.
    // Halving first is exact in binary and keeps both differences finite.
    if (lo < 0.0 && hi > 0.0)
        return (value / 2.0 - lo / 2.0) / (hi / 2.0 - lo / 2.0);
    return (value - lo) / (hi - lo);
}

double interpolate_between(double lo, double hi, double fraction) {
    if (fraction >= 0.0 && fraction <= 1.0)
        return (1.0 - fraction) * lo + fraction * hi;
    const double half_span = hi / 2.0 - lo / 2.0;
    const double value = lo + (fraction * 2.0) * half_span;
    if (std::isfinite(value)) return value;
    return std::signbit(value) ? -std::numeric_limits<double>::max()
                               : std::numeric_limits<double>::max();
}

std::pair<double, double> sample_statistics(std::span<const double> values,
                                            const char* what) {
    if (values.size() < 2)
        throw Error(cworks::validation_failed(std::string(what) +
            " needs at least two finite values"));
    double mean = 0.0;
    std::size_t count = 0;
    for (double value : values) {
        ++count;
        const double weight = 1.0 / static_cast<double>(count);
        // Weighted form avoids the potentially overflowing (value - mean)
        // used by the conventional online update.
        mean = mean * (1.0 - weight) + value * weight;
    }
    // cworks::hypot, not libm's: this one is correctly rounded by
    // construction (it settles its last bit with exact integer arithmetic on
    // x²+y²), where the platform's is quality-of-implementation and was
    // measured misrounded on 13.7% of the arguments this suite gives it —
    // the largest value discrepancy of any function in it. The standard
    // deviation built here sets error-bar and control-limit geometry.
    double half_norm = 0.0;
    for (double value : values)
        half_norm = cworks::hypot(half_norm, value / 2.0 - mean / 2.0);
    const double sd = half_norm *
                      (2.0 / std::sqrt(static_cast<double>(values.size() - 1)));
    if (!std::isfinite(mean) || !std::isfinite(sd))
        throw Error(cworks::validation_failed(std::string(what) +
            ": sample statistics exceed the finite double range"));
    return {mean, sd};
}

/// Peter J. Acklam's deterministic inverse-normal approximation.
///
/// The two tail branches take a logarithm, and it is cworks::log rather than
/// libm's for the usual reason: the rational approximation around it is a
/// fixed sequence of IEEE operations and reproduces exactly, so libm would
/// have been the one step in the function that a different platform is free
/// to round differently. Only probabilities outside [0.02425, 0.97575] reach
/// it, which on a Q-Q plot is a few points of the sample.
double inverse_normal(double p) {
    if (!(p > 0.0 && p < 1.0))
        throw Error(cworks::validation_failed(
            "Q-Q plot: probability must be strictly between zero and one"));
    constexpr std::array<double, 6> a{
        -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
        1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    constexpr std::array<double, 5> b{
        -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
        6.680131188771972e+01, -1.328068155288572e+01};
    constexpr std::array<double, 6> c{
        -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
        -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    constexpr std::array<double, 4> d{
        7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
        3.754408661907416e+00};
    constexpr double low = 0.02425;
    constexpr double high = 1.0 - low;
    if (p < low) {
        const double q = std::sqrt(-2.0 * cworks::log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (p > high) {
        const double q = std::sqrt(-2.0 * cworks::log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

template <class T, class... Args>
T& emplace_scientific(Axes& axes, Args&&... args) {
    auto owned = std::make_unique<T>(std::forward<Args>(args)...);
    T& result = *owned;
    axes.add_series(std::move(owned));
    return result;
}

} // namespace

// -- SpectrogramSeries -------------------------------------------------------

SpectrogramSeries::SpectrogramSeries(std::span<const double> x, std::span<const double> y,
                                     std::span<const double> values, std::size_t rows,
                                     std::size_t cols) {
    set_data(x, y, values, rows, cols);
}

SpectrogramSeries& SpectrogramSeries::set_data(std::span<const double> x,
                                                std::span<const double> y,
                                                std::span<const double> values,
                                                std::size_t rows, std::size_t cols) {
    require_grid(x, y, values, rows, cols, "spectrogram");
    x_.assign(x.begin(), x.end());
    y_.assign(y.begin(), y.end());
    values_.assign(values.begin(), values.end());
    rows_ = rows;
    cols_ = cols;
    return *this;
}

SpectrogramSeries& SpectrogramSeries::colormap(Colormap map) {
    color_scale_.colormap = map;
    return *this;
}
SpectrogramSeries& SpectrogramSeries::color_range(double minimum, double maximum) {
    if (!(maximum > minimum)) throw Error(cworks::validation_failed(
        "spectrogram: color range maximum must exceed minimum"));
    color_scale_.minimum = minimum;
    color_scale_.maximum = maximum;
    return *this;
}
SpectrogramSeries& SpectrogramSeries::color_midpoint(double midpoint) {
    color_scale_.midpoint = midpoint;
    return *this;
}
SpectrogramSeries& SpectrogramSeries::reverse_colormap(bool value) {
    color_scale_.reverse = value;
    return *this;
}
SpectrogramSeries& SpectrogramSeries::missing_color(Color color) {
    color_scale_.missing = color;
    return *this;
}
SpectrogramSeries& SpectrogramSeries::colorbar(bool value) {
    colorbar_ = value;
    return *this;
}

std::pair<double, double> SpectrogramSeries::colorbar_value_range() const {
    return finite_range(values_);
}

Extent SpectrogramSeries::extent() const {
    Extent extent;
    if (x_.empty() || y_.empty()) return extent;
    const std::vector<double> xe = cell_edges(x_);
    const std::vector<double> ye = cell_edges(y_);
    extent.include(xe.front(), ye.front());
    extent.include(xe.back(), ye.back());
    return extent;
}

void SpectrogramSeries::build_geometry(const detail::GeomContext& ctx,
                                       std::vector<SceneItem>& out) const {
    if (x_.empty() || y_.empty() || rows_ == 0 || cols_ == 0) return;
    const std::vector<double> xe = cell_edges(x_);
    const std::vector<double> ye = cell_edges(y_);
    const auto [lo, hi] = finite_range(values_);
    for (std::size_t row = 0; row < rows_; ++row) {
        for (std::size_t col = 0; col < cols_; ++col) {
            const double x0 = ctx.x.map(xe[col]);
            const double x1 = ctx.x.map(xe[col + 1]);
            const double y0 = ctx.y.map(ye[row]);
            const double y1 = ctx.y.map(ye[row + 1]);
            ShapeStyle style;
            style.fill = color_scale_.color(values_[row * cols_ + col], lo, hi);
            out.push_back(RectItem{{std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0),
                                    std::abs(y1 - y0)},
                                   style});
        }
    }
}

// -- HexbinSeries ------------------------------------------------------------

HexbinSeries::HexbinSeries(std::span<const double> x, std::span<const double> y, int bins) {
    set_data(x, y);
    this->bins(bins);
}

HexbinSeries& HexbinSeries::set_data(std::span<const double> x,
                                     std::span<const double> y) {
    if (x.size() != y.size())
        throw Error(cworks::validation_failed("hexbin: x and y must have the same length"));
    x_.assign(x.begin(), x.end());
    y_.assign(y.begin(), y.end());
    missing_ = MissingPolicy::Drop;
    return *this;
}

HexbinSeries& HexbinSeries::bins(int count) {
    if (count < 2 || count > 1000) throw Error(cworks::validation_failed(
        "hexbin: bins must be between 2 and 1000"));
    bins_ = count;
    return *this;
}
HexbinSeries& HexbinSeries::min_count(std::size_t count) {
    if (count == 0) throw Error(cworks::validation_failed("hexbin: min_count must be positive"));
    min_count_ = count;
    return *this;
}
HexbinSeries& HexbinSeries::colormap(Colormap map) {
    color_scale_.colormap = map;
    return *this;
}
HexbinSeries& HexbinSeries::color_range(double minimum, double maximum) {
    if (!(maximum > minimum)) throw Error(cworks::validation_failed(
        "hexbin: color range maximum must exceed minimum"));
    color_scale_.minimum = minimum;
    color_scale_.maximum = maximum;
    return *this;
}
HexbinSeries& HexbinSeries::color_midpoint(double midpoint) {
    color_scale_.midpoint = midpoint;
    return *this;
}
HexbinSeries& HexbinSeries::reverse_colormap(bool value) {
    color_scale_.reverse = value;
    return *this;
}
HexbinSeries& HexbinSeries::missing_color(Color color) {
    color_scale_.missing = color;
    return *this;
}
HexbinSeries& HexbinSeries::colorbar(bool value) {
    colorbar_ = value;
    return *this;
}

std::vector<HexBin> HexbinSeries::resolved_bins() const {
    const Extent raw = finite_xy_extent(x_, y_);
    if (!raw.valid) return {};
    const double size = 1.0 / (kSqrt3 * static_cast<double>(bins_));
    std::map<std::pair<int, int>, std::size_t> counts;
    for (std::size_t i = 0; i < x_.size(); ++i) {
        if (!std::isfinite(x_[i]) || !std::isfinite(y_[i])) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed("hexbin contains a missing value at index " +
                    std::to_string(i)));
            detail::reject_unsupported_interpolate(missing_, "hexbin");
            continue;
        }
        const double nx = normalized_between(x_[i], raw.x_lo, raw.x_hi);
        const double ny = normalized_between(y_[i], raw.y_lo, raw.y_hi);
        const double qf = (kSqrt3 / 3.0 * nx - ny / 3.0) / size;
        const double rf = (2.0 / 3.0 * ny) / size;
        const double xf = qf, zf = rf, yf = -xf - zf;
        int rx = static_cast<int>(std::llround(xf));
        int ry = static_cast<int>(std::llround(yf));
        int rz = static_cast<int>(std::llround(zf));
        const double dx = std::abs(rx - xf), dy = std::abs(ry - yf), dz = std::abs(rz - zf);
        if (dx > dy && dx > dz) rx = -ry - rz;
        else if (dy > dz) ry = -rx - rz;
        else rz = -rx - ry;
        ++counts[{rx, rz}];
    }
    std::vector<HexBin> result;
    for (const auto& [cell, count] : counts) {
        if (count < min_count_) continue;
        const double nx = size * kSqrt3 * (cell.first + cell.second / 2.0);
        const double ny = size * 1.5 * cell.second;
        result.push_back({interpolate_between(raw.x_lo, raw.x_hi, nx),
                          interpolate_between(raw.y_lo, raw.y_hi, ny), count});
    }
    return result;
}

std::pair<double, double> HexbinSeries::colorbar_value_range() const {
    std::size_t maximum = 1;
    for (const HexBin& bin : resolved_bins()) maximum = std::max(maximum, bin.count);
    return {0.0, static_cast<double>(maximum)};
}

Extent HexbinSeries::extent() const {
    Extent extent = finite_xy_extent(x_, y_);
    if (!extent.valid) return extent;
    const auto pad = [&](double& lo, double& hi) {
        const long double span = static_cast<long double>(hi) - lo;
        const long double amount = span * 0.7L / bins_;
        const long double max = std::numeric_limits<double>::max();
        lo = static_cast<double>(std::max(-max, static_cast<long double>(lo) - amount));
        hi = static_cast<double>(std::min(max, static_cast<long double>(hi) + amount));
    };
    pad(extent.x_lo, extent.x_hi);
    pad(extent.y_lo, extent.y_hi);
    return extent;
}

void HexbinSeries::build_geometry(const detail::GeomContext& ctx,
                                  std::vector<SceneItem>& out) const {
    const Extent raw = finite_xy_extent(x_, y_);
    if (!raw.valid) return;
    const double size = 1.0 / (kSqrt3 * static_cast<double>(bins_));
    const auto bins = resolved_bins();
    const auto [lo, hi] = colorbar_value_range();
    for (const HexBin& bin : bins) {
        const double cx = normalized_between(bin.x, raw.x_lo, raw.x_hi);
        const double cy = normalized_between(bin.y, raw.y_lo, raw.y_hi);
        PolygonItem item;
        item.points.reserve(6);
        for (int i = 0; i < 6; ++i) {
            // 30, 90, 150, 210, 270, 330 degrees: in degrees the two
            // vertical vertices come out with an exactly zero x offset, so
            // the hexagons of a bin grid tile without a sub-ulp seam.
            double sine = 0.0;
            double cosine = 0.0;
            cworks::sincos_deg(30.0 + i * 60.0, sine, cosine);
            const double nx = cx + size * cosine;
            const double ny = cy + size * sine;
            item.points.push_back(
                {ctx.x.map(interpolate_between(raw.x_lo, raw.x_hi, nx)),
                 ctx.y.map(interpolate_between(raw.y_lo, raw.y_hi, ny))});
        }
        const Color color = color_scale_.color(static_cast<double>(bin.count), lo, hi);
        item.style.fill = color;
        item.style.stroke = ctx.theme.plot_background;
        item.style.stroke_width = 0.45;
        out.push_back(std::move(item));
    }
}

// -- QQSeries ----------------------------------------------------------------

QQSeries::QQSeries(std::span<const double> values) { set_data(values); }

QQSeries& QQSeries::set_data(std::span<const double> values) {
    std::vector<double> samples;
    for (double value : values)
        if (std::isfinite(value)) samples.push_back(value);
    if (samples.size() < 2) throw Error(cworks::validation_failed(
        "Q-Q plot needs at least two finite values"));
    std::sort(samples.begin(), samples.end());
    std::vector<double> theoretical(samples.size());
    const double n = static_cast<double>(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        // Blom plotting positions: (rank - 3/8) / (n + 1/4), rank=i+1.
        const double p = (static_cast<double>(i) + 0.625) / (n + 0.25);
        theoretical[i] = inverse_normal(p);
    }
    const auto [mean, stddev] = sample_statistics(samples, "Q-Q plot");
    samples_ = std::move(samples);
    theoretical_ = std::move(theoretical);
    mean_ = mean;
    stddev_ = stddev;
    return *this;
}

QQSeries& QQSeries::marker_size(double size) {
    if (!(size > 0.0)) throw Error(cworks::validation_failed(
        "Q-Q plot: marker size must be positive"));
    marker_size_ = size;
    return *this;
}
QQSeries& QQSeries::reference(bool value) {
    reference_ = value;
    return *this;
}

Extent QQSeries::extent() const {
    Extent extent;
    for (std::size_t i = 0; i < samples_.size(); ++i)
        extent.include(theoretical_[i], samples_[i]);
    if (reference_ && !theoretical_.empty()) {
        extent.include(theoretical_.front(), mean_ + stddev_ * theoretical_.front());
        extent.include(theoretical_.back(), mean_ + stddev_ * theoretical_.back());
    }
    return extent;
}

void QQSeries::build_geometry(const detail::GeomContext& ctx,
                              std::vector<SceneItem>& out) const {
    if (reference_ && !theoretical_.empty()) {
        ShapeStyle style;
        style.stroke = ctx.color.with_alpha(0.72);
        style.stroke_width = std::max(1.0, ctx.stroke_width * 0.8);
        style.dash = DashPattern{{5.0, 3.0}};
        out.push_back(LineItem{{ctx.x.map(theoretical_.front()),
                                ctx.y.map(mean_ + stddev_ * theoretical_.front())},
                               {ctx.x.map(theoretical_.back()),
                                ctx.y.map(mean_ + stddev_ * theoretical_.back())},
                               style});
    }
    ShapeStyle point;
    point.fill = ctx.color;
    for (std::size_t i = 0; i < samples_.size(); ++i)
        out.push_back(CircleItem{{ctx.x.map(theoretical_[i]), ctx.y.map(samples_[i])},
                                 marker_size_, point});
}

// -- WindRoseSeries ----------------------------------------------------------

WindRoseSeries::WindRoseSeries(std::span<const double> theta,
                               std::span<const double> weights, int bins) {
    set_data(theta, weights);
    this->bins(bins);
}

WindRoseSeries& WindRoseSeries::set_data(std::span<const double> theta,
                                         std::span<const double> weights) {
    if (!weights.empty() && weights.size() != theta.size())
        throw Error(cworks::validation_failed(
            "wind rose: weights must be empty or match theta length"));
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (std::isfinite(weights[i]) && weights[i] < 0.0)
            throw Error(cworks::validation_failed("wind rose: weights must be non-negative"));
    }
    theta_.assign(theta.begin(), theta.end());
    weights_.assign(weights.begin(), weights.end());
    return *this;
}
WindRoseSeries& WindRoseSeries::bins(int count) {
    if (count < 4 || count > 360) throw Error(cworks::validation_failed(
        "wind rose: bins must be between 4 and 360"));
    bins_ = count;
    return *this;
}
WindRoseSeries& WindRoseSeries::degrees(bool value) {
    degrees_ = value;
    return *this;
}
WindRoseSeries& WindRoseSeries::fill_alpha(double alpha) {
    if (!(alpha >= 0.0 && alpha <= 1.0))
        throw Error(cworks::validation_failed(
            "wind rose: fill alpha must be between zero and one"));
    fill_alpha_ = alpha;
    return *this;
}
WindRoseSeries& WindRoseSeries::range(double minimum, double maximum) {
    if (minimum != 0.0 || !std::isfinite(maximum) || !(maximum > 0.0))
        throw Error(cworks::validation_failed(
            "wind rose: range must be [0, finite positive maximum]"));
    range_ = std::pair<double, double>{minimum, maximum};
    return *this;
}

std::vector<WindRoseBin> WindRoseSeries::resolved_bins() const {
    std::vector<double> totals(static_cast<std::size_t>(bins_), 0.0);
    // Bin edges in DEGREES. 360/n is exact for every bin count a wind rose
    // uses (4, 8, 12, 16, 32), so the sector boundaries of a 16-bin rose
    // are exactly 22.5 degrees apart and its north/east/south/west edges
    // land exactly on the axes. 2*pi/n could promise none of that.
    const double width = 360.0 / static_cast<double>(bins_);
    for (std::size_t i = 0; i < theta_.size(); ++i) {
        if (!std::isfinite(theta_[i]) || (!weights_.empty() && !std::isfinite(weights_[i]))) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed("wind rose contains a missing value at index "
                    + std::to_string(i)));
            detail::reject_unsupported_interpolate(missing_, "wind rose");
            continue;
        }
        double angle = degrees_ ? theta_[i] : theta_[i] * 180.0 / kPi;
        angle = std::fmod(angle, 360.0);
        if (angle < 0.0) angle += 360.0;
        std::size_t bin = static_cast<std::size_t>(std::floor(angle / width));
        if (bin >= totals.size()) bin = 0;
        totals[bin] += weights_.empty() ? 1.0 : weights_[i];
    }
    std::vector<WindRoseBin> result;
    result.reserve(totals.size());
    for (std::size_t i = 0; i < totals.size(); ++i)
        result.push_back({i * width, (i + 1) * width, totals[i]});
    return result;
}

Extent WindRoseSeries::extent() const {
    Extent extent;
    double maximum = 0.0;
    for (const WindRoseBin& bin : resolved_bins()) maximum = std::max(maximum, bin.value);
    extent.include(0.0, 0.0);
    extent.include(360.0, maximum > 0.0 ? maximum : 1.0);
    return extent;
}

void WindRoseSeries::build_geometry(const detail::GeomContext& ctx,
                                    std::vector<SceneItem>& out) const {
    for (const WindRoseBin& bin : resolved_bins()) {
        if (!(bin.value > 0.0)) continue;
        const double radius = std::clamp(ctx.y.map(bin.value), 0.0,
                                         ctx.polar_radius);
        const int steps = std::max(
            2, static_cast<int>(std::ceil((bin.angle_end - bin.angle_start) / 5.0)));
        PolygonItem item;
        item.points.push_back(ctx.polar_center);
        for (int i = 0; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            const double angle = bin.angle_start + (bin.angle_end - bin.angle_start) * t;
            double sine = 0.0;
            double cosine = 0.0;
            cworks::sincos_deg(angle, sine, cosine);
            item.points.push_back({ctx.polar_center.x + radius * cosine,
                                   ctx.polar_center.y - radius * sine});
        }
        Color fill = ctx.color;
        fill.a = fill_alpha_;
        item.style.fill = fill;
        item.style.stroke = ctx.color;
        item.style.stroke_width = std::max(0.7, ctx.stroke_width * 0.65);
        out.push_back(std::move(item));
    }
}

// -- Axes factories ----------------------------------------------------------

SpectrogramSeries& Axes::spectrogram(std::span<const double> x,
                                     std::span<const double> y,
                                     std::span<const double> values,
                                     std::size_t rows, std::size_t cols) {
    return emplace_scientific<SpectrogramSeries>(*this, x, y, values, rows, cols);
}

SpectrogramSeries& Axes::spectrogram(std::initializer_list<double> x,
                                     std::initializer_list<double> y,
                                     std::initializer_list<double> values,
                                     std::size_t rows, std::size_t cols) {
    const std::vector<double> vx(x), vy(y), vv(values);
    return spectrogram(vx, vy, vv, rows, cols);
}

HexbinSeries& Axes::hexbin(std::span<const double> x, std::span<const double> y, int bins) {
    return emplace_scientific<HexbinSeries>(*this, x, y, bins);
}

HexbinSeries& Axes::hexbin(std::initializer_list<double> x,
                           std::initializer_list<double> y, int bins) {
    const std::vector<double> vx(x), vy(y);
    return hexbin(vx, vy, bins);
}

QQSeries& Axes::qqplot(std::span<const double> values) {
    return emplace_scientific<QQSeries>(*this, values);
}

QQSeries& Axes::qqplot(std::initializer_list<double> values) {
    const std::vector<double> vv(values);
    return qqplot(vv);
}

WindRoseSeries& Axes::wind_rose(std::span<const double> theta,
                                std::span<const double> weights, int bins) {
    return emplace_scientific<WindRoseSeries>(*this, theta, weights, bins);
}

WindRoseSeries& Axes::wind_rose(std::initializer_list<double> theta,
                                std::initializer_list<double> weights, int bins) {
    const std::vector<double> vt(theta), vw(weights);
    return wind_rose(vt, vw, bins);
}

Axes& Axes::pareto(std::vector<std::string> categories,
                   std::span<const double> values) {
    if (categories.size() != values.size())
        throw Error(cworks::validation_failed(
            "Pareto chart: categories and values must have the same length"));
    std::set<std::string> distinct;
    for (const auto& category : categories) {
        if (!distinct.insert(category).second)
            throw Error(cworks::validation_failed("Pareto chart: category names must be unique ('" +
                category + "')"));
    }
    std::vector<std::size_t> order(values.size());
    std::iota(order.begin(), order.end(), 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i]) || values[i] < 0.0)
            throw Error(cworks::validation_failed(
                "Pareto chart: values must be finite and non-negative"));
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return values[a] > values[b]; });
    std::vector<std::string> sorted_categories;
    std::vector<double> sorted_values;
    sorted_categories.reserve(order.size());
    sorted_values.reserve(order.size());
    for (std::size_t index : order) {
        sorted_categories.push_back(std::move(categories[index]));
        sorted_values.push_back(values[index]);
    }
    const double scale = *std::max_element(sorted_values.begin(), sorted_values.end());
    if (!(scale > 0.0)) throw Error(cworks::validation_failed(
        "Pareto chart: values must have a positive total"));
    double total = 0.0;
    for (double value : sorted_values) total += value / scale;
    bar(std::move(sorted_categories), sorted_values).label("Count");
    std::vector<double> x(sorted_values.size()), cumulative(sorted_values.size());
    double running = 0.0;
    for (std::size_t i = 0; i < sorted_values.size(); ++i) {
        x[i] = static_cast<double>(i) + 0.5;
        running += sorted_values[i] / scale;
        cumulative[i] = running * 100.0 / total;
    }
    cumulative.back() = 100.0;
    line(x, cumulative).label("Cumulative %").marker(Marker::Circle, 2.8).on_y2();
    y2_axis().range(0.0, 100.0).label("Cumulative").unit("%");
    return *this;
}

Axes& Axes::pareto(std::vector<std::string> categories,
                   std::initializer_list<double> values) {
    const std::vector<double> vv(values);
    return pareto(std::move(categories), vv);
}

Axes& Axes::control_chart(std::span<const double> x, std::span<const double> y,
                          double sigma) {
    if (x.size() != y.size())
        throw Error(cworks::validation_failed("control chart: x and y must have the same length"));
    if (!(sigma > 0.0) || !std::isfinite(sigma))
        throw Error(cworks::validation_failed("control chart: sigma must be finite and positive"));
    std::vector<double> finite;
    for (std::size_t i = 0; i < y.size(); ++i)
        if (std::isfinite(x[i]) && std::isfinite(y[i])) finite.push_back(y[i]);
    const auto [mean, sd] = sample_statistics(finite, "control chart");
    const double upper = mean + sigma * sd;
    const double lower = mean - sigma * sd;
    if (!std::isfinite(upper) || !std::isfinite(lower))
        throw Error(cworks::validation_failed(
            "control chart: control limits exceed the finite double range"));
    line(x, y).label("Observed").marker(Marker::Circle, 2.5);
    hline(mean, "Center line");
    hline(upper, "UCL");
    hline(lower, "LCL");
    return *this;
}

Axes& Axes::control_chart(std::initializer_list<double> x,
                          std::initializer_list<double> y, double sigma) {
    const std::vector<double> vx(x), vy(y);
    return control_chart(vx, vy, sigma);
}

} // namespace cplot
