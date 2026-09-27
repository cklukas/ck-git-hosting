// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Series data handling and geometry generation. Series emit
// backend-independent scene items; they never touch SVG or pixels.
#include "cplot/series.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <cworks/app_error.hpp>
#include <cworks/format.hpp>
#include <cworks/math.hpp>
#include <cworks/trig.hpp>

#include "cplot/figure.hpp"
#include "cplot/text.hpp"
#include "cplot/theme.hpp"
#include "cplot/ticks.hpp"
#include "format_c.hpp"
#include "internal.hpp"

namespace cplot {

namespace {

bool is_missing(double v) { return !std::isfinite(v); }

/// Linearly fill interior gaps in `y` — a run of one or more consecutive
/// missing values with a real value strictly before AND after it — in the
/// numeric x/y domain, before any scale mapping. Leading/trailing missing
/// runs have nothing to interpolate from on one side and are left missing;
/// a missing x has no domain position to interpolate to and is skipped
/// wherever it falls.
std::vector<double> interpolate_interior_gaps(const std::vector<double>& x,
                                              const std::vector<double>& y) {
    const std::size_t n = std::min(x.size(), y.size());
    std::vector<double> filled(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(n));
    std::size_t i = 0;
    while (i < n) {
        if (!is_missing(filled[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && is_missing(filled[j])) ++j;
        // [i, j) is a maximal run of missing y values; it is interior only
        // when both bracketing indices exist and carry a real x position.
        if (i > 0 && j < n && !is_missing(x[i - 1]) && !is_missing(x[j])) {
            const double x0 = x[i - 1], y0 = filled[i - 1];
            const double x1 = x[j], y1 = filled[j];
            for (std::size_t k = i; k < j; ++k) {
                if (is_missing(x[k])) continue; // no domain position to fill
                const double t = (x1 != x0) ? (x[k] - x0) / (x1 - x0) : 0.5;
                filled[k] = y0 + t * (y1 - y0);
            }
        }
        i = j;
    }
    return filled;
}

/// Apply the missing-value policy to paired x/y data.
/// Returns runs of consecutive valid points (Gap policy yields several runs).
std::vector<std::vector<Point>> resolve_runs(const std::vector<double>& x,
                                             const std::vector<double>& y,
                                             MissingPolicy policy,
                                             const detail::GeomContext& ctx) {
    std::vector<std::vector<Point>> runs;
    std::vector<Point> current;
    // Interpolate is resolved once, up front, into a same-shaped y array:
    // every interior gap is already a real number by the time the main
    // loop below runs, so it only has to deal with the (rarer) edge case
    // of a leading/trailing gap or an unmappable missing x.
    const std::vector<double> filled_y =
        policy == MissingPolicy::Interpolate ? interpolate_interior_gaps(x, y)
                                              : std::vector<double>{};
    for (std::size_t i = 0; i < x.size() && i < y.size(); ++i) {
        double xv = x[i];
        double yv = policy == MissingPolicy::Interpolate ? filled_y[i] : y[i];
        const bool missing = is_missing(xv) || is_missing(yv);
        if (missing) {
            switch (policy) {
            case MissingPolicy::Error:
                throw Error(cworks::validation_failed(
                    "series contains missing (NaN/inf) value at index " + std::to_string(i)));
            case MissingPolicy::Zero:
                if (is_missing(yv)) yv = 0.0;
                if (is_missing(xv)) continue; // x cannot be zero-filled meaningfully
                break;
            case MissingPolicy::Gap:
            case MissingPolicy::Interpolate:
                // Interpolate already filled every interior gap above;
                // anything still missing here is a leading/trailing run
                // (nothing to interpolate from on one side) or an
                // unmappable missing x — both break the run like Gap.
                if (!current.empty()) {
                    runs.push_back(std::move(current));
                    current.clear();
                }
                continue;
            case MissingPolicy::Drop:
                continue;
            }
        }
        current.push_back(Point{ctx.x.map(xv), ctx.y.map(yv)});
    }
    if (!current.empty()) runs.push_back(std::move(current));
    return runs;
}

/// Expand consecutive (already scale-mapped) points into a staircase per
/// StepMode. Shared by StepSeries and AreaSeries::step() so the corner
/// geometry is defined exactly once. Two point sequences that share x
/// positions — required of stacked/percent-stacked area series already —
/// place their risers at the identical x when each is expanded on its own,
/// since a riser's position depends only on the shared x values and mode,
/// never on y.
std::vector<Point> expand_steps(const std::vector<Point>& pts, StepMode mode) {
    std::vector<Point> out;
    if (pts.empty()) return out;
    out.reserve(pts.size() * 2);
    out.push_back(pts[0]);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const Point prev = pts[i - 1];
        const Point cur = pts[i];
        switch (mode) {
        case StepMode::Post:
            out.push_back({cur.x, prev.y});
            break;
        case StepMode::Pre:
            out.push_back({prev.x, cur.y});
            break;
        case StepMode::Mid: {
            const double mid = (prev.x + cur.x) / 2.0;
            out.push_back({mid, prev.y});
            out.push_back({mid, cur.y});
            break;
        }
        }
        out.push_back(cur);
    }
    return out;
}

void emit_marker(std::vector<SceneItem>& out, Marker marker, Point p, double size, Color color,
                 double stroke_width) {
    ShapeStyle fill_style;
    fill_style.fill = color;
    ShapeStyle line_style;
    line_style.stroke = color;
    line_style.stroke_width = stroke_width;
    line_style.cap = LineCap::Round;
    line_style.join = LineJoin::Round;

    switch (marker) {
    case Marker::None:
        break;
    case Marker::Circle:
        out.push_back(CircleItem{p, size, fill_style});
        break;
    case Marker::Square:
        out.push_back(RectItem{{p.x - size, p.y - size, 2 * size, 2 * size}, fill_style});
        break;
    case Marker::Diamond:
        out.push_back(PolygonItem{{{p.x, p.y - size * 1.3},
                                   {p.x + size * 1.3, p.y},
                                   {p.x, p.y + size * 1.3},
                                   {p.x - size * 1.3, p.y}},
                                  fill_style});
        break;
    case Marker::TriangleUp:
        out.push_back(PolygonItem{{{p.x, p.y - size * 1.3},
                                   {p.x + size * 1.2, p.y + size},
                                   {p.x - size * 1.2, p.y + size}},
                                  fill_style});
        break;
    case Marker::Cross:
        out.push_back(LineItem{{p.x - size, p.y - size}, {p.x + size, p.y + size}, line_style});
        out.push_back(LineItem{{p.x - size, p.y + size}, {p.x + size, p.y - size}, line_style});
        break;
    case Marker::Plus:
        out.push_back(LineItem{{p.x - size * 1.2, p.y}, {p.x + size * 1.2, p.y}, line_style});
        out.push_back(LineItem{{p.x, p.y - size * 1.2}, {p.x, p.y + size * 1.2}, line_style});
        break;
    }
}

/// Resolve the whisker stroke from the series' error style and context.
ShapeStyle resolve_error_style(const ErrorBarStyle& es, const detail::GeomContext& ctx) {
    ShapeStyle style;
    style.stroke = es.color.value_or(ctx.color);
    style.stroke_width = es.width > 0.0 ? es.width : std::max(1.0, ctx.stroke_width * 0.7);
    return style;
}

/// Emit one error whisker. The whisker runs along the value axis `val_axis`
/// from `value - lo` to `value + hi`; `cross_px` is the fixed pixel position
/// on the other axis. `vertical` selects a vertical stem (value on y, caps
/// horizontal) versus a horizontal stem (value on x, caps vertical). Caps of
/// half-width `cap` are drawn only at ends that carry a real delta (0 = capless).
/// `one_sided` collapses the half nearer `baseline`, the tidy convention for
/// zero-based bars. Ends are compared in data space so inverted pixel axes
/// (value on a downward-mapped y) stay correct.
void emit_whisker(std::vector<SceneItem>& out, double cross_px, double value, double lo,
                  double hi, const Scale& val_axis, bool vertical, const ShapeStyle& style,
                  double cap, bool one_sided, double baseline) {
    double lo_end = value - lo;
    double hi_end = value + hi;
    if (one_sided) {
        if (value >= baseline) lo_end = value;
        else hi_end = value;
    }
    const bool has_lo = lo_end < value;
    const bool has_hi = hi_end > value;
    if (!has_lo && !has_hi) return;
    const double p_lo = val_axis.map(lo_end);
    const double p_hi = val_axis.map(hi_end);
    if (vertical) {
        out.push_back(LineItem{{cross_px, p_lo}, {cross_px, p_hi}, style});
        if (cap > 0.0 && has_lo)
            out.push_back(LineItem{{cross_px - cap, p_lo}, {cross_px + cap, p_lo}, style});
        if (cap > 0.0 && has_hi)
            out.push_back(LineItem{{cross_px - cap, p_hi}, {cross_px + cap, p_hi}, style});
    } else {
        out.push_back(LineItem{{p_lo, cross_px}, {p_hi, cross_px}, style});
        if (cap > 0.0 && has_lo)
            out.push_back(LineItem{{p_lo, cross_px - cap}, {p_lo, cross_px + cap}, style});
        if (cap > 0.0 && has_hi)
            out.push_back(LineItem{{p_hi, cross_px - cap}, {p_hi, cross_px + cap}, style});
    }
}

/// Emit x- and/or y-direction error whiskers for a point series.
void emit_xy_error(std::vector<SceneItem>& out, const std::vector<double>& x,
                   const std::vector<double>& y, const ErrorData& ey, const ErrorData& ex,
                   const ErrorBarStyle& es, const detail::GeomContext& ctx) {
    const ShapeStyle style = resolve_error_style(es, ctx);
    for (std::size_t i = 0; i < x.size() && i < y.size(); ++i) {
        if (is_missing(x[i]) || is_missing(y[i])) continue;
        const double px = ctx.x.map(x[i]);
        const double py = ctx.y.map(y[i]);
        if (ey.has(i))
            emit_whisker(out, px, y[i], ey.lo(i), ey.hi(i), ctx.y, true, style, es.cap, false,
                         0.0);
        if (ex.has(i))
            emit_whisker(out, py, x[i], ex.lo(i), ex.hi(i), ctx.x, false, style, es.cap, false,
                         0.0);
    }
}

/// Build a shaded error ribbon polygon between (y - lo) and (y + hi) along the
/// finite run of points. Returns fewer than 3 points when nothing is drawable.
std::vector<Point> error_band_polygon(const std::vector<double>& x, const std::vector<double>& y,
                                      const ErrorData& ey, const detail::GeomContext& ctx) {
    std::vector<Point> poly;
    poly.reserve(x.size() * 2);
    for (std::size_t i = 0; i < x.size() && i < y.size(); ++i) {
        if (is_missing(x[i]) || is_missing(y[i])) continue;
        poly.push_back({ctx.x.map(x[i]), ctx.y.map(y[i] + ey.hi(i))});
    }
    for (std::size_t i = std::min(x.size(), y.size()); i-- > 0;) {
        if (is_missing(x[i]) || is_missing(y[i])) continue;
        poly.push_back({ctx.x.map(x[i]), ctx.y.map(y[i] - ey.lo(i))});
    }
    return poly;
}

// -- trend line fitting --------------------------------------------------
//
// Three fit kinds, three numeric strategies, chosen per the stability each
// one actually needs:
//   * Linear: the classic two-parameter OLS closed form. A 2x2 system is
//     well-conditioned for any non-degenerate x range, so the direct sums
//     are exact and this path is untouched from before (byte-identical
//     `trendline: linear` output).
//   * Exponential: the same closed-form OLS, run on (x, ln y) instead of
//     (x, y) — still a 2-parameter fit, still well-conditioned. The
//     logarithm and the exponential are cworks::log and cworks::exp, not
//     libm's: a fitted coefficient is not only drawn, it is PRINTED, through
//     format_double_fixed(..., 2). Rounding to two decimals is a discrete
//     decision exactly as a `ceil` is, so a coefficient sitting on a .005
//     boundary reads 1.23 on one C library and 1.24 on another. The cost is
//     one deterministic logarithm per observation, paid once when the fit is
//     computed rather than once per drawn point.
//   * Polynomial (degree 1..6): a Vandermonde design matrix in the raw x
//     domain is ill-conditioned past degree ~2, so this path shifts/scales
//     x to a centred domain and solves via Householder QR (never a normal-
//     equations matrix inverse), then converts the coefficients back to
//     plain-x terms for drawing and for the equation label.
// All accumulation is in double, in index order, so results are
// byte-reproducible across platforms (-ffp-contract=off).

/// Integer power via repeated multiplication — deterministic and exact for
/// the small (<= 6) exponents the polynomial coefficient conversion needs;
/// avoids std::pow's general (and library-dependent) real-exponent path.
double ipow(double base, unsigned exponent) {
    double result = 1.0;
    for (unsigned i = 0; i < exponent; ++i) result *= base;
    return result;
}

/// Solve the linear least-squares problem minimizing ||A c - b|| via
/// Householder QR. `a` is n x m (n >= m, rows = observations); both `a` and
/// `b` are overwritten as scratch space. Returns false on a degenerate
/// (rank-deficient) column — the caller treats that as "fit not possible"
/// rather than dividing by a near-zero pivot.
bool solve_least_squares_qr(std::vector<std::vector<double>>& a, std::vector<double>& b,
                            std::vector<double>& coeffs) {
    const std::size_t n = a.size();
    const std::size_t m = a.empty() ? 0 : a[0].size();
    for (std::size_t k = 0; k < m; ++k) {
        double norm_sq = 0.0;
        for (std::size_t i = k; i < n; ++i) norm_sq += a[i][k] * a[i][k];
        const double norm = std::sqrt(norm_sq);
        if (norm == 0.0) return false;
        const double alpha = a[k][k] >= 0.0 ? -norm : norm;
        std::vector<double> v(n, 0.0);
        v[k] = a[k][k] - alpha;
        for (std::size_t i = k + 1; i < n; ++i) v[i] = a[i][k];
        double v_norm_sq = 0.0;
        for (std::size_t i = k; i < n; ++i) v_norm_sq += v[i] * v[i];
        if (v_norm_sq == 0.0) return false;
        for (std::size_t j = k; j < m; ++j) {
            double dot = 0.0;
            for (std::size_t i = k; i < n; ++i) dot += v[i] * a[i][j];
            const double factor = 2.0 * dot / v_norm_sq;
            for (std::size_t i = k; i < n; ++i) a[i][j] -= factor * v[i];
        }
        double dot_b = 0.0;
        for (std::size_t i = k; i < n; ++i) dot_b += v[i] * b[i];
        const double factor_b = 2.0 * dot_b / v_norm_sq;
        for (std::size_t i = k; i < n; ++i) b[i] -= factor_b * v[i];
    }
    coeffs.assign(m, 0.0);
    for (std::size_t ii = m; ii-- > 0;) {
        double sum = b[ii];
        for (std::size_t j = ii + 1; j < m; ++j) sum -= a[ii][j] * coeffs[j];
        if (a[ii][ii] == 0.0) return false;
        coeffs[ii] = sum / a[ii][ii];
    }
    return true;
}

/// A computed trend fit, kept minimal: the drawn curve and the legend label
/// both evaluate it on demand rather than materializing a fitted series.
/// `coeffs` meaning depends on `kind`: Linear is [intercept, slope] (y =
/// coeffs[0] + coeffs[1]*x); Exponential is [a, b] (y = a * e^(b*x));
/// Polynomial is [c0, c1, ..., c_degree] in plain-x terms (y = sum c_k x^k).
/// `r2` is computed in the fit's own residual space: plain y for
/// Linear/Polynomial, ln(y) for Exponential.
struct TrendFit {
    bool ok = false;
    FitKind kind = FitKind::None;
    std::vector<double> coeffs;
    double r2 = 0.0;
    double x_lo = 0.0, x_hi = 0.0; ///< data x-range actually used by the fit

    double eval(double x) const {
        switch (kind) {
        case FitKind::Linear:
            return coeffs[0] + coeffs[1] * x;
        case FitKind::Exponential:
            return coeffs[0] * cworks::exp(coeffs[1] * x);
        case FitKind::Polynomial: {
            double result = 0.0;
            for (std::size_t k = coeffs.size(); k-- > 0;) result = result * x + coeffs[k];
            return result;
        }
        case FitKind::None:
            return 0.0;
        }
        return 0.0;
    }
};

/// Fit `kind` to the finite (x, y) pairs. Missing (NaN/inf) points are
/// excluded, exactly like the pre-existing linear behaviour. For
/// Exponential specifically, a finite but non-positive y is *not* treated
/// as missing — ln(y) is undefined there, so silently excluding it would
/// quietly misrepresent the fit (the suite's "never silently coerce" rule);
/// it is a hard error naming the offending index instead.
TrendFit fit_trend(const std::vector<double>& x, const std::vector<double>& y, FitKind kind,
                   int degree) {
    TrendFit result;
    result.kind = kind;
    if (kind == FitKind::None) return result;

    std::vector<double> xs, ys; // ys is the residual-space target: ln(y) for Exponential
    xs.reserve(x.size());
    ys.reserve(x.size());
    bool any = false;
    for (std::size_t i = 0; i < x.size() && i < y.size(); ++i) {
        const double xv = x[i], yv = y[i];
        if (is_missing(xv) || is_missing(yv)) continue;
        if (kind == FitKind::Exponential && yv <= 0.0) {
            throw Error(cworks::validation_failed("exponential trendline: y must be positive, got " +
                                                  cworks::format_double(yv) + " at index " + std::to_string(i)));
        }
        xs.push_back(xv);
        ys.push_back(kind == FitKind::Exponential ? cworks::log(yv) : yv);
        if (!any) {
            result.x_lo = result.x_hi = xv;
            any = true;
        } else {
            result.x_lo = std::min(result.x_lo, xv);
            result.x_hi = std::max(result.x_hi, xv);
        }
    }
    if (!any) return result;

    if (kind == FitKind::Linear || kind == FitKind::Exponential) {
        double n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (std::size_t i = 0; i < xs.size(); ++i) {
            n += 1.0;
            sx += xs[i];
            sy += ys[i];
            sxx += xs[i] * xs[i];
            sxy += xs[i] * ys[i];
        }
        const double denom = n * sxx - sx * sx;
        if (n < 2.0 || denom == 0.0) return result; // need at least two distinct x values
        const double slope = (n * sxy - sx * sy) / denom;
        const double intercept = (sy - slope * sx) / n;
        double ss_res = 0.0, ss_tot = 0.0;
        const double mean = sy / n;
        for (std::size_t i = 0; i < xs.size(); ++i) {
            const double pred = intercept + slope * xs[i];
            ss_res += (ys[i] - pred) * (ys[i] - pred);
            ss_tot += (ys[i] - mean) * (ys[i] - mean);
        }
        result.r2 = ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : 1.0;
        result.coeffs = kind == FitKind::Linear
                            ? std::vector<double>{intercept, slope}
                            : std::vector<double>{cworks::exp(intercept), slope};
        result.ok = true;
        return result;
    }

    // Polynomial: fit in a centred/scaled x domain via Householder QR, then
    // expand the coefficients back to plain-x terms via the binomial
    // theorem so drawing and labelling can both work directly in x.
    const std::size_t m = static_cast<std::size_t>(degree) + 1;
    if (xs.size() < m) return result; // not enough points to determine the fit
    double xmean = 0.0;
    for (double v : xs) xmean += v;
    xmean /= static_cast<double>(xs.size());
    double xscale = 0.0;
    for (double v : xs) xscale = std::max(xscale, std::abs(v - xmean));
    if (xscale == 0.0) return result; // all x identical: degree >= 1 is not determined

    std::vector<std::vector<double>> a(xs.size(), std::vector<double>(m));
    std::vector<double> b(xs.size());
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const double xhat = (xs[i] - xmean) / xscale;
        double p = 1.0;
        for (std::size_t k = 0; k < m; ++k) {
            a[i][k] = p;
            p *= xhat;
        }
        b[i] = ys[i];
    }
    std::vector<double> coeffs_hat;
    if (!solve_least_squares_qr(a, b, coeffs_hat)) return result;

    // ((x - xmean)/xscale)^k = sum_{j=0}^{k} C(k,j) x^j (-xmean)^(k-j) / xscale^k
    std::vector<double> coeffs(m, 0.0);
    for (std::size_t k = 0; k < m; ++k) {
        double binom = 1.0; // C(k, 0)
        for (std::size_t j = 0; j <= k; ++j) {
            coeffs[j] += coeffs_hat[k] * binom * ipow(-xmean, static_cast<unsigned>(k - j)) /
                        ipow(xscale, static_cast<unsigned>(k));
            binom *= static_cast<double>(k - j) / static_cast<double>(j + 1);
        }
    }
    result.coeffs = std::move(coeffs);
    result.ok = true;

    double ss_res = 0.0, ss_tot = 0.0, mean = 0.0;
    for (double v : ys) mean += v;
    mean /= static_cast<double>(ys.size());
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const double pred = result.eval(xs[i]);
        ss_res += (ys[i] - pred) * (ys[i] - pred);
        ss_tot += (ys[i] - mean) * (ys[i] - mean);
    }
    result.r2 = ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : 1.0;
    return result;
}

/// Append " + 1.23" or " - 1.23" (never "+ -1.23") for a coefficient that
/// follows an earlier term in an equation string being built left to right.
void append_signed_term(std::string& out, double value, const std::string& suffix) {
    out += (value < 0.0 ? " - " : " + ") +
           cworks::format_double_fixed(std::abs(value), 2) + suffix;
}

/// The auto-generated equation text for a fitted trend, e.g.
/// "y = 2.1x + 0.3" (Linear), "y = 1.05 * e^(0.2x)" (Exponential), or
/// "y = 0.1x^2 + x + 0.5"-shaped text (Polynomial) — coefficients are
/// quantized via cworks::format_double_fixed so the label reads like a
/// rounded equation, not raw floating text.
std::string trend_equation_text(const TrendFit& fit) {
    switch (fit.kind) {
    case FitKind::Linear: {
        std::string s = "y = " + cworks::format_double_fixed(fit.coeffs[1], 2) + "x";
        append_signed_term(s, fit.coeffs[0], "");
        return s;
    }
    case FitKind::Exponential:
        return "y = " + cworks::format_double_fixed(fit.coeffs[0], 2) + " * e^(" +
               cworks::format_double_fixed(fit.coeffs[1], 2) + "x)";
    case FitKind::Polynomial: {
        std::string s = "y = ";
        bool first = true;
        for (std::size_t k = fit.coeffs.size(); k-- > 0;) {
            const std::string suffix =
                k == 0 ? "" : (k == 1 ? "x" : ("x^" + std::to_string(k)));
            if (first) {
                s += cworks::format_double_fixed(fit.coeffs[k], 2) + suffix;
                first = false;
            } else {
                append_signed_term(s, fit.coeffs[k], suffix);
            }
        }
        return s;
    }
    case FitKind::None:
        return {};
    }
    return {};
}

/// The trendline's legend label, or nullopt when it should not appear
/// (label not requested, or the fit could not be computed — no drawn line,
/// no dangling legend entry for it). Text is the explicit override when
/// set, else the auto-generated equation; an R^2 suffix is appended when
/// requested, in the fit's own residual space (see TrendFit).
std::optional<std::string> resolve_trend_label(const std::vector<double>& x,
                                               const std::vector<double>& y, FitKind kind,
                                               int degree, const TrendlineLabel& label) {
    if (!label.enabled) return std::nullopt;
    const TrendFit fit = fit_trend(x, y, kind, degree);
    if (!fit.ok) return std::nullopt;
    std::string text = label.text.empty() ? trend_equation_text(fit) : label.text;
    if (label.show_r2) text += " (R^2 = " + cworks::format_double_fixed(fit.r2, 2) + ")";
    return text;
}

/// Overlay a fitted trend line for a scatter/line series, drawn as a dashed
/// curve over the fitted x-range. Linear draws its two endpoints exactly as
/// before (byte-identical `trendline: linear` output); Exponential and
/// Polynomial are genuinely curved, so they are sampled at evenly spaced
/// points across the range.
void emit_trendline(std::vector<SceneItem>& out, const std::vector<double>& x,
                    const std::vector<double>& y, const detail::GeomContext& ctx, FitKind kind,
                    int degree, const std::optional<Color>& trend_color) {
    const TrendFit fit = fit_trend(x, y, kind, degree);
    if (!fit.ok) return;
    ShapeStyle style;
    style.stroke = trend_color.value_or(ctx.color);
    style.stroke_width = std::max(1.2, ctx.stroke_width);
    style.dash = DashPattern{{6.0, 4.0}};
    style.cap = LineCap::Round;
    style.join = LineJoin::Round;
    if (kind == FitKind::Linear) {
        const Point a{ctx.x.map(fit.x_lo), ctx.y.map(fit.eval(fit.x_lo))};
        const Point b{ctx.x.map(fit.x_hi), ctx.y.map(fit.eval(fit.x_hi))};
        out.push_back(PolylineItem{{a, b}, style});
        return;
    }
    constexpr int kSamples = 64;
    std::vector<Point> points;
    points.reserve(kSamples + 1);
    for (int i = 0; i <= kSamples; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kSamples);
        const double xv = fit.x_lo + t * (fit.x_hi - fit.x_lo);
        points.push_back({ctx.x.map(xv), ctx.y.map(fit.eval(xv))});
    }
    out.push_back(PolylineItem{std::move(points), style});
}

Extent xy_extent(const std::vector<double>& x, const std::vector<double>& y,
                 const std::vector<double>* y_err = nullptr) {
    Extent e;
    for (std::size_t i = 0; i < x.size() && i < y.size(); ++i) {
        if (is_missing(x[i]) || is_missing(y[i])) continue;
        e.include(x[i], y[i]);
        if (y_err && i < y_err->size() && !is_missing((*y_err)[i])) {
            e.include(x[i], y[i] + (*y_err)[i]);
            e.include(x[i], y[i] - (*y_err)[i]);
        }
    }
    return e;
}

/// Data extent of a point series widened by its x/y error whiskers so the
/// axis range never clips a cap. Empty ErrorData widens nothing.
Extent xy_extent_err(const std::vector<double>& x, const std::vector<double>& y,
                     const ErrorData& ey, const ErrorData& ex) {
    Extent e;
    for (std::size_t i = 0; i < x.size() && i < y.size(); ++i) {
        if (is_missing(x[i]) || is_missing(y[i])) continue;
        e.include(x[i], y[i]);
        if (ey.has(i)) {
            e.include(x[i], y[i] + ey.hi(i));
            e.include(x[i], y[i] - ey.lo(i));
        }
        if (ex.has(i)) {
            e.include(x[i] + ex.hi(i), y[i]);
            e.include(x[i] - ex.lo(i), y[i]);
        }
    }
    return e;
}

void check_sizes(std::size_t nx, std::size_t ny, const char* what) {
    if (nx != ny) {
        throw Error(cworks::validation_failed(std::string(what) +
            ": x and y must have the same length (" + std::to_string(nx) + " vs " +
            std::to_string(ny) + ")"));
    }
}

/// `degree` only matters for FitKind::Polynomial; validated eagerly here
/// (rather than left to fit time) so a bad call fails at the call site,
/// never silently clamped to the valid range.
void check_trend_degree(FitKind kind, int degree) {
    if (kind != FitKind::Polynomial) return;
    if (degree < 1 || degree > 6) {
        throw Error(cworks::validation_failed("polynomial trendline: degree must be between 1 and 6, got " +
                                              std::to_string(degree)));
    }
}

std::string format_number(double value, const std::string& format) {
    // Single authority for the tick_format/value_format string channel:
    // the "compact" keyword, printf/brace numeric conversions (C locale so
    // a comma-radix process locale never corrupts "12.50" into "12,50"),
    // and the safe format_tick_value fallback all live in format_with.
    return detail::format_with(format, value);
}

/// Default up/down colors for OHLC/candlestick and waterfall charts.
const Color kUpColor = Color::rgb(0x55A868);   // calm green
const Color kDownColor = Color::rgb(0xC44E52); // calm red

std::string format_percent(double fraction) {
    // Pin the "C" locale: besides keeping the radix a '.', it guarantees the
    // ".0%" trim below still matches (a comma-radix locale would render
    // "25,0%" and silently skip the trim). See format_c.hpp.
    std::string s = detail::format_c("%.1f%%", fraction * 100.0);
    // "25.0%" → "25%"
    const auto pos = s.find(".0%");
    if (pos != std::string::npos) s = s.substr(0, pos) + "%";
    return s;
}

/// Perceptual-luminance text colour so a label stays legible when drawn
/// directly on a fill that can be either light or dark (funnel bars,
/// heatmap cells, pie slices, bubble markers). Below the threshold the
/// fill reads as "dark", so a light label is used; otherwise a dark one.
Color contrast_text_color(Color fill, const Theme& theme) {
    const int luminance =
        static_cast<int>(0.299 * fill.r + 0.587 * fill.g + 0.114 * fill.b);
    return luminance < 120 ? theme.background : theme.text_color;
}

/// Fixed pixel offset for line/scatter point labels: above and to the
/// right of the point. Deterministic by design -- this engine never does
/// force-directed or iterative collision avoidance, so labels may overlap
/// on dense data (the caller's problem, same as every other static
/// labelling feature here).
constexpr double kPointLabelDx = 6.0;
constexpr double kPointLabelDy = -6.0;

void emit_point_label(std::vector<SceneItem>& out, Point p, std::string text,
                      const Theme& theme) {
    if (text.empty()) return;
    TextItem label;
    label.pos = {p.x + kPointLabelDx, p.y + kPointLabelDy};
    label.text = std::move(text);
    label.font = theme.tick_font();
    label.color = theme.muted_text_color;
    label.halign = HAlign::Left;
    label.valign = VAlign::Bottom;
    out.push_back(std::move(label));
}

/// Bubble point label: centered on the bubble (its "on-bubble identifier"
/// mode, matching how bubble charts elsewhere always show an ID on the
/// bubble itself), coloured for contrast against the bubble's own fill.
void emit_bubble_label(std::vector<SceneItem>& out, Point p, std::string text, Color fill,
                       const Theme& theme) {
    if (text.empty()) return;
    TextItem label;
    label.pos = p;
    label.text = std::move(text);
    label.font = theme.tick_font();
    label.color = contrast_text_color(fill, theme);
    label.halign = HAlign::Center;
    label.valign = VAlign::Middle;
    out.push_back(std::move(label));
}

} // namespace

// -- ColorEncoding -----------------------------------------------------------

std::pair<double, double> ColorEncoding::value_range() const {
    double lo = 0.0, hi = 0.0;
    bool any = false;
    for (double v : values) {
        if (!std::isfinite(v)) continue;
        if (!any) {
            lo = hi = v;
            any = true;
        } else {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    if (!any) return {0.0, 1.0};
    if (lo == hi) return {lo - 0.5, hi + 0.5}; // pad a degenerate range
    return {lo, hi};
}

// -- Series ------------------------------------------------------------------

std::vector<LegendItemInfo> Series::legend_items(Color resolved_color,
                                                 const Theme& theme) const {
    (void)theme;
    if (label_.empty()) return {};
    LegendItemInfo item;
    item.label = label_;
    item.color = resolved_color;
    item.marker = legend_marker();
    item.filled = legend_filled();
    item.square = legend_filled() && (legend_marker() == Marker::None || is_bar_like());
    return {item};
}

// -- Extent -----------------------------------------------------------------

void Extent::include(double x, double y) {
    if (!valid) {
        x_lo = x_hi = x;
        y_lo = y_hi = y;
        valid = true;
        return;
    }
    x_lo = std::min(x_lo, x);
    x_hi = std::max(x_hi, x);
    y_lo = std::min(y_lo, y);
    y_hi = std::max(y_hi, y);
}

void Extent::merge(const Extent& other) {
    if (!other.valid) return;
    include(other.x_lo, other.y_lo);
    include(other.x_hi, other.y_hi);
}

// -- LineSeries ---------------------------------------------------------------

LineSeries::LineSeries(std::span<const double> x, std::span<const double> y) {
    set_data(x, y);
}

LineSeries::LineSeries(std::vector<std::string> categories,
                       std::span<const double> y) {
    set_data(std::move(categories), y);
}

LineSeries& LineSeries::set_data(std::span<const double> x, std::span<const double> y) {
    check_sizes(x.size(), y.size(), "line series");
    categories_.clear();
    x_.assign(x.begin(), x.end());
    y_.assign(y.begin(), y.end());
    return *this;
}

LineSeries& LineSeries::set_data(std::vector<std::string> categories,
                                 std::span<const double> y) {
    check_sizes(categories.size(), y.size(), "categorical line series");
    if (categories.empty())
        throw Error(cworks::validation_failed(
            "categorical line series: needs at least one category"));
    std::set<std::string> unique;
    for (const std::string& category : categories) {
        if (category.empty())
            throw Error(cworks::validation_failed(
                "categorical line series: category labels must not be empty"));
        if (!unique.insert(category).second)
            throw Error(cworks::validation_failed("categorical line series: duplicate category '" +
                category + "'"));
    }
    categories_ = std::move(categories);
    x_.resize(categories_.size());
    for (std::size_t i = 0; i < x_.size(); ++i)
        x_[i] = static_cast<double>(i) + 0.5;
    y_.assign(y.begin(), y.end());
    return *this;
}

LineSeries& LineSeries::marker(Marker m, double size) {
    marker_ = m;
    marker_size_ = size;
    return *this;
}

LineSeries& LineSeries::dash(std::string svg_dash_array) {
    dash_ = DashPattern::parse(svg_dash_array);
    return *this;
}

LineSeries& LineSeries::y_error(std::span<const double> err) {
    err_y_.set_symmetric(err);
    return *this;
}

LineSeries& LineSeries::y_error(std::span<const double> lower, std::span<const double> upper) {
    err_y_.set_asymmetric(lower, upper);
    return *this;
}

LineSeries& LineSeries::x_error(std::span<const double> err) {
    err_x_.set_symmetric(err);
    return *this;
}

LineSeries& LineSeries::x_error(std::span<const double> lower, std::span<const double> upper) {
    err_x_.set_asymmetric(lower, upper);
    return *this;
}

LineSeries& LineSeries::error_display(ErrorDisplay display) {
    err_display_ = display;
    return *this;
}

LineSeries& LineSeries::error_meaning(ErrorMeaning meaning) {
    err_y_.meaning = meaning;
    err_x_.meaning = meaning;
    return *this;
}

LineSeries& LineSeries::error_cap_width(double px) {
    err_style_.cap = px;
    return *this;
}

LineSeries& LineSeries::error_color(Color color) {
    err_style_.color = color;
    return *this;
}

LineSeries& LineSeries::error_width(double px) {
    err_style_.width = px;
    return *this;
}

LineSeries& LineSeries::band(std::span<const double> lower, std::span<const double> upper) {
    if (lower.size() != x_.size() || upper.size() != x_.size()) {
        throw Error(cworks::validation_failed("band: lower/upper must match the series length"));
    }
    band_lo_.assign(lower.begin(), lower.end());
    band_hi_.assign(upper.begin(), upper.end());
    return *this;
}

LineSeries& LineSeries::band_alpha(double alpha) {
    band_alpha_ = alpha;
    return *this;
}

LineSeries& LineSeries::downsample(std::size_t points) {
    downsample_ = points;
    return *this;
}

LineSeries& LineSeries::trendline(FitKind kind, int degree) {
    check_trend_degree(kind, degree);
    fit_ = kind;
    fit_degree_ = degree;
    return *this;
}

LineSeries& LineSeries::trend_color(Color color) {
    trend_color_ = color;
    return *this;
}

LineSeries& LineSeries::trend_label(bool value) {
    trend_label_.enabled = value;
    return *this;
}

LineSeries& LineSeries::trend_show_r2(bool value) {
    trend_label_.show_r2 = value;
    return *this;
}

LineSeries& LineSeries::trend_label_text(std::string text) {
    trend_label_.text = std::move(text);
    trend_label_.enabled = true;
    return *this;
}

LineSeries& LineSeries::smooth(bool value) {
    smooth_ = value;
    return *this;
}

LineSeries& LineSeries::point_labels(bool value) {
    point_labels_ = value;
    if (!value) point_label_names_.clear();
    return *this;
}

LineSeries& LineSeries::point_labels(std::vector<std::string> names) {
    if (!x_.empty() && names.size() != x_.size()) {
        throw Error(cworks::validation_failed("line series: point_labels names must match the series length (" +
                                              std::to_string(names.size()) + " vs " + std::to_string(x_.size()) + ")"));
    }
    point_label_names_ = std::move(names);
    point_labels_ = true;
    return *this;
}

Extent LineSeries::extent() const {
    Extent e = xy_extent_err(x_, y_, err_y_, err_x_);
    if (!band_lo_.empty()) {
        e.merge(xy_extent(x_, band_lo_));
        e.merge(xy_extent(x_, band_hi_));
    }
    return e;
}

std::vector<LegendItemInfo> LineSeries::legend_items(Color resolved_color,
                                                     const Theme& theme) const {
    std::vector<LegendItemInfo> items = Series::legend_items(resolved_color, theme);
    if (items.empty() || fit_ == FitKind::None) return items;
    if (auto text = resolve_trend_label(x_, y_, fit_, fit_degree_, trend_label_)) {
        LegendItemInfo item;
        item.label = std::move(*text);
        item.color = trend_color_.value_or(resolved_color);
        items.push_back(std::move(item));
    }
    return items;
}

namespace {

/// Largest-Triangle-Three-Buckets downsampling. Keeps visual shape while
/// reducing point count; first and last points are always retained.
void lttb(const std::vector<double>& x, const std::vector<double>& y, std::size_t target,
          std::vector<double>& out_x, std::vector<double>& out_y) {
    const std::size_t n = std::min(x.size(), y.size());
    if (target < 3 || n <= target) {
        out_x.assign(x.begin(), x.begin() + static_cast<long>(n));
        out_y.assign(y.begin(), y.begin() + static_cast<long>(n));
        return;
    }
    out_x.reserve(target);
    out_y.reserve(target);
    out_x.push_back(x[0]);
    out_y.push_back(y[0]);
    const double bucket = static_cast<double>(n - 2) / static_cast<double>(target - 2);
    std::size_t a = 0;
    for (std::size_t i = 0; i < target - 2; ++i) {
        const std::size_t lo = static_cast<std::size_t>(std::floor(i * bucket)) + 1;
        const std::size_t hi =
            std::min(static_cast<std::size_t>(std::floor((i + 1) * bucket)) + 1, n - 1);
        const std::size_t next_lo = hi;
        const std::size_t next_hi =
            std::min(static_cast<std::size_t>(std::floor((i + 2) * bucket)) + 1, n);
        // Average of the next bucket.
        double avg_x = 0, avg_y = 0;
        std::size_t count = 0;
        for (std::size_t j = next_lo; j < next_hi; ++j) {
            if (!std::isfinite(x[j]) || !std::isfinite(y[j])) continue;
            avg_x += x[j];
            avg_y += y[j];
            ++count;
        }
        if (count) {
            avg_x /= static_cast<double>(count);
            avg_y /= static_cast<double>(count);
        }
        // Point in this bucket forming the largest triangle.
        double best_area = -1.0;
        std::size_t best = lo;
        for (std::size_t j = lo; j < hi; ++j) {
            if (!std::isfinite(x[j]) || !std::isfinite(y[j])) continue;
            const double area = std::abs((x[a] - avg_x) * (y[j] - y[a]) -
                                         (x[a] - x[j]) * (avg_y - y[a]));
            if (area > best_area) {
                best_area = area;
                best = j;
            }
        }
        out_x.push_back(x[best]);
        out_y.push_back(y[best]);
        a = best;
    }
    out_x.push_back(x[n - 1]);
    out_y.push_back(y[n - 1]);
}

/// Monotone cubic interpolation (Fritsch–Carlson) through pixel-space
/// points: smooth, deterministic, and free of overshoot so splined lines
/// never invent extrema that are not in the data.
std::vector<Point> monotone_spline(const std::vector<Point>& pts, int samples_per_seg = 16) {
    const std::size_t n = pts.size();
    if (n < 3) return pts;

    std::vector<double> dx(n - 1), dy(n - 1), slope(n - 1), m(n);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        dx[i] = pts[i + 1].x - pts[i].x;
        dy[i] = pts[i + 1].y - pts[i].y;
        slope[i] = dx[i] != 0.0 ? dy[i] / dx[i] : 0.0;
    }
    m[0] = slope[0];
    m[n - 1] = slope[n - 2];
    for (std::size_t i = 1; i + 1 < n; ++i) {
        if (slope[i - 1] * slope[i] <= 0.0) {
            m[i] = 0.0; // local extremum: flat tangent prevents overshoot
        } else {
            const double w1 = 2.0 * dx[i] + dx[i - 1];
            const double w2 = dx[i] + 2.0 * dx[i - 1];
            m[i] = (w1 + w2) / (w1 / slope[i - 1] + w2 / slope[i]);
        }
    }

    std::vector<Point> out;
    out.reserve((n - 1) * static_cast<std::size_t>(samples_per_seg) + 1);
    out.push_back(pts[0]);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (dx[i] == 0.0) {
            out.push_back(pts[i + 1]);
            continue;
        }
        for (int k = 1; k <= samples_per_seg; ++k) {
            const double t = static_cast<double>(k) / samples_per_seg;
            const double t2 = t * t, t3 = t2 * t;
            const double h00 = 2 * t3 - 3 * t2 + 1;
            const double h10 = t3 - 2 * t2 + t;
            const double h01 = -2 * t3 + 3 * t2;
            const double h11 = t3 - t2;
            out.push_back({pts[i].x + t * dx[i],
                           h00 * pts[i].y + h10 * dx[i] * m[i] + h01 * pts[i + 1].y +
                               h11 * dx[i] * m[i + 1]});
        }
    }
    return out;
}

} // namespace

void LineSeries::build_geometry(const detail::GeomContext& ctx,
                                std::vector<SceneItem>& out) const {
    const bool band_from_error =
        !err_y_.empty() &&
        (err_display_ == ErrorDisplay::Band || err_display_ == ErrorDisplay::BarsAndBand);

    // Shaded band first, underneath the line. An explicit band() wins; a
    // Band/BarsAndBand display derives the ribbon from the y-error magnitude.
    std::vector<Point> band_poly;
    if (!band_lo_.empty()) {
        band_poly.reserve(x_.size() * 2);
        // Both boundary chains must span the identical index set, so a point
        // is skipped only when x, lower, OR upper is missing — otherwise the
        // top and bottom chains diverge and the closed polygon tears.
        const auto skip = [&](std::size_t i) {
            return is_missing(x_[i]) || is_missing(band_lo_[i]) || is_missing(band_hi_[i]);
        };
        for (std::size_t i = 0; i < x_.size(); ++i) {
            if (skip(i)) continue;
            band_poly.push_back({ctx.x.map(x_[i]), ctx.y.map(band_hi_[i])});
        }
        for (std::size_t i = x_.size(); i-- > 0;) {
            if (skip(i)) continue;
            band_poly.push_back({ctx.x.map(x_[i]), ctx.y.map(band_lo_[i])});
        }
    } else if (band_from_error) {
        band_poly = error_band_polygon(x_, y_, err_y_, ctx);
    }
    if (band_poly.size() >= 3) {
        ShapeStyle band_style;
        const double alpha = band_lo_.empty() ? err_style_.band_alpha : band_alpha_;
        band_style.fill = err_style_.color.value_or(ctx.color).with_alpha(alpha);
        out.push_back(PolygonItem{std::move(band_poly), band_style});
    }

    // Discrete whiskers unless the display is band-only. Horizontal (x) error
    // always draws as whiskers.
    const bool draw_y_bars =
        !err_y_.empty() &&
        (err_display_ == ErrorDisplay::Bars || err_display_ == ErrorDisplay::BarsAndBand);
    if (draw_y_bars || !err_x_.empty()) {
        emit_xy_error(out, x_, y_, draw_y_bars ? err_y_ : ErrorData{}, err_x_, err_style_, ctx);
    }

    const std::vector<double>* px = &x_;
    const std::vector<double>* py = &y_;
    std::vector<double> ds_x, ds_y;
    if (downsample_ > 0 && x_.size() > downsample_) {
        lttb(x_, y_, downsample_, ds_x, ds_y);
        px = &ds_x;
        py = &ds_y;
    }

    ShapeStyle style;
    style.stroke = ctx.color;
    style.stroke_width = ctx.stroke_width;
    style.dash = dash_;
    style.cap = LineCap::Round;
    style.join = LineJoin::Round;
    // An isolated point has no segment to stroke, so it is drawn as a dot.
    ShapeStyle dot_style;
    dot_style.fill = ctx.color;
    dot_style.stroke_width = 0.0;
    for (auto& run : resolve_runs(*px, *py, missing_, ctx)) {
        if (run.size() == 1) {
            out.push_back(CircleItem{run[0], ctx.stroke_width * 0.9, dot_style});
        } else if (smooth_) {
            out.push_back(PolylineItem{monotone_spline(run), style});
        } else {
            out.push_back(PolylineItem{std::move(run), style});
        }
    }
    if (marker_ != Marker::None) {
        for (std::size_t i = 0; i < x_.size() && i < y_.size(); ++i) {
            if (is_missing(x_[i]) || is_missing(y_[i])) continue;
            emit_marker(out, marker_, {ctx.x.map(x_[i]), ctx.y.map(y_[i])}, marker_size_,
                        ctx.color, ctx.stroke_width);
        }
    }
    if (point_labels_) {
        for (std::size_t i = 0; i < x_.size() && i < y_.size(); ++i) {
            if (is_missing(x_[i]) || is_missing(y_[i])) continue;
            std::string text = point_label_names_.empty()
                                   ? detail::format_tick_value(y_[i], 0.0)
                                   : (i < point_label_names_.size() ? point_label_names_[i]
                                                                    : std::string{});
            emit_point_label(out, {ctx.x.map(x_[i]), ctx.y.map(y_[i])}, std::move(text),
                             ctx.theme);
        }
    }
    if (fit_ != FitKind::None) emit_trendline(out, x_, y_, ctx, fit_, fit_degree_, trend_color_);
}

// -- ScatterSeries -------------------------------------------------------------

ScatterSeries::ScatterSeries(std::span<const double> x, std::span<const double> y) {
    set_data(x, y);
    missing_ = MissingPolicy::Drop;
}

ScatterSeries& ScatterSeries::set_data(std::span<const double> x, std::span<const double> y) {
    check_sizes(x.size(), y.size(), "scatter series");
    x_.assign(x.begin(), x.end());
    y_.assign(y.begin(), y.end());
    return *this;
}

ScatterSeries& ScatterSeries::marker(Marker m) {
    marker_ = m;
    return *this;
}

ScatterSeries& ScatterSeries::marker_size(double size) {
    marker_size_ = size;
    return *this;
}

ScatterSeries& ScatterSeries::y_error(std::span<const double> err) {
    err_y_.set_symmetric(err);
    return *this;
}

ScatterSeries& ScatterSeries::y_error(std::span<const double> lower, std::span<const double> upper) {
    err_y_.set_asymmetric(lower, upper);
    return *this;
}

ScatterSeries& ScatterSeries::x_error(std::span<const double> err) {
    err_x_.set_symmetric(err);
    return *this;
}

ScatterSeries& ScatterSeries::x_error(std::span<const double> lower, std::span<const double> upper) {
    err_x_.set_asymmetric(lower, upper);
    return *this;
}

ScatterSeries& ScatterSeries::error_meaning(ErrorMeaning meaning) {
    err_y_.meaning = meaning;
    err_x_.meaning = meaning;
    return *this;
}

ScatterSeries& ScatterSeries::error_cap_width(double px) {
    err_style_.cap = px;
    return *this;
}

ScatterSeries& ScatterSeries::error_color(Color color) {
    err_style_.color = color;
    return *this;
}

ScatterSeries& ScatterSeries::error_width(double px) {
    err_style_.width = px;
    return *this;
}

ScatterSeries& ScatterSeries::trendline(FitKind kind, int degree) {
    check_trend_degree(kind, degree);
    fit_ = kind;
    fit_degree_ = degree;
    return *this;
}

ScatterSeries& ScatterSeries::trend_color(Color color) {
    trend_color_ = color;
    return *this;
}

ScatterSeries& ScatterSeries::trend_label(bool value) {
    trend_label_.enabled = value;
    return *this;
}

ScatterSeries& ScatterSeries::trend_show_r2(bool value) {
    trend_label_.show_r2 = value;
    return *this;
}

ScatterSeries& ScatterSeries::trend_label_text(std::string text) {
    trend_label_.text = std::move(text);
    trend_label_.enabled = true;
    return *this;
}

ScatterSeries& ScatterSeries::color_by(std::span<const double> values) {
    if (!x_.empty() && values.size() != x_.size())
        throw Error(cworks::validation_failed("scatter color_by: values must match x/y length (" +
            std::to_string(values.size()) + " vs " + std::to_string(x_.size()) + ")"));
    color_.values.assign(values.begin(), values.end());
    return *this;
}

ScatterSeries& ScatterSeries::colormap(Colormap map) {
    color_.scale.colormap = map;
    return *this;
}

ScatterSeries& ScatterSeries::color_range(double minimum, double maximum) {
    color_.scale.minimum = minimum;
    color_.scale.maximum = maximum;
    return *this;
}

ScatterSeries& ScatterSeries::color_midpoint(double midpoint) {
    color_.scale.midpoint = midpoint;
    return *this;
}

ScatterSeries& ScatterSeries::reverse_colormap(bool value) {
    color_.scale.reverse = value;
    return *this;
}

ScatterSeries& ScatterSeries::missing_color(Color color) {
    color_.scale.missing = color;
    return *this;
}

ScatterSeries& ScatterSeries::colorbar(bool value) {
    color_.colorbar = value;
    return *this;
}

ScatterSeries& ScatterSeries::point_labels(bool value) {
    point_labels_ = value;
    if (!value) point_label_names_.clear();
    return *this;
}

ScatterSeries& ScatterSeries::point_labels(std::vector<std::string> names) {
    if (!x_.empty() && names.size() != x_.size()) {
        throw Error(cworks::validation_failed("scatter series: point_labels names must match the series length (" +
                                              std::to_string(names.size()) + " vs " + std::to_string(x_.size()) + ")"));
    }
    point_label_names_ = std::move(names);
    point_labels_ = true;
    return *this;
}

Extent ScatterSeries::extent() const {
    return xy_extent_err(x_, y_, err_y_, err_x_);
}

std::vector<LegendItemInfo> ScatterSeries::legend_items(Color resolved_color,
                                                        const Theme& theme) const {
    std::vector<LegendItemInfo> items = Series::legend_items(resolved_color, theme);
    if (items.empty() || fit_ == FitKind::None) return items;
    if (auto text = resolve_trend_label(x_, y_, fit_, fit_degree_, trend_label_)) {
        LegendItemInfo item;
        item.label = std::move(*text);
        item.color = trend_color_.value_or(resolved_color);
        items.push_back(std::move(item));
    }
    return items;
}

void ScatterSeries::build_geometry(const detail::GeomContext& ctx,
                                   std::vector<SceneItem>& out) const {
    if (fit_ != FitKind::None) emit_trendline(out, x_, y_, ctx, fit_, fit_degree_, trend_color_);
    if (!err_y_.empty() || !err_x_.empty())
        emit_xy_error(out, x_, y_, err_y_, err_x_, err_style_, ctx);
    const bool by_value = color_.active();
    const auto [vmin, vmax] = by_value ? color_.value_range() : std::pair<double, double>{0.0, 1.0};
    for (std::size_t i = 0; i < x_.size() && i < y_.size(); ++i) {
        if (is_missing(x_[i]) || is_missing(y_[i])) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed(
                    "scatter series contains missing value at index " + std::to_string(i)));
            detail::reject_unsupported_interpolate(missing_, "scatter");
            continue;
        }
        const Color c = by_value ? color_.scale.color(color_.values[i], vmin, vmax) : ctx.color;
        emit_marker(out, marker_, {ctx.x.map(x_[i]), ctx.y.map(y_[i])}, marker_size_, c, 1.2);
        if (point_labels_) {
            std::string text = point_label_names_.empty()
                                   ? detail::format_tick_value(y_[i], 0.0)
                                   : (i < point_label_names_.size() ? point_label_names_[i]
                                                                    : std::string{});
            emit_point_label(out, {ctx.x.map(x_[i]), ctx.y.map(y_[i])}, std::move(text),
                             ctx.theme);
        }
    }
}

// -- BarSeries ------------------------------------------------------------------

BarSeries::BarSeries(std::vector<std::string> categories, std::span<const double> values,
                     bool horizontal)
    : horizontal_(horizontal) {
    set_data(std::move(categories), values);
}

BarSeries& BarSeries::set_data(std::vector<std::string> categories,
                               std::span<const double> values) {
    if (categories.size() != values.size()) {
        throw Error(cworks::validation_failed(
            "bar series: categories and values must have the same length (" +
            std::to_string(categories.size()) + " vs " + std::to_string(values.size()) + ")"));
    }
    categories_ = std::move(categories);
    values_.assign(values.begin(), values.end());
    return *this;
}

BarSeries& BarSeries::horizontal(bool value) {
    horizontal_ = value;
    return *this;
}

BarSeries& BarSeries::stacked(bool value) {
    stacked_ = value;
    return *this;
}

BarSeries& BarSeries::percent_stacked(bool value) {
    percent_ = value;
    if (value) stacked_ = true;
    return *this;
}

BarSeries& BarSeries::value_labels(bool value) {
    value_labels_ = value;
    return *this;
}

BarSeries& BarSeries::value_error(std::span<const double> err) {
    err_.set_symmetric(err);
    return *this;
}

BarSeries& BarSeries::value_error(std::span<const double> lower, std::span<const double> upper) {
    err_.set_asymmetric(lower, upper);
    return *this;
}

BarSeries& BarSeries::error_meaning(ErrorMeaning meaning) {
    err_.meaning = meaning;
    return *this;
}

BarSeries& BarSeries::error_cap_width(double px) {
    err_style_.cap = px;
    return *this;
}

BarSeries& BarSeries::error_color(Color color) {
    err_style_.color = color;
    return *this;
}

BarSeries& BarSeries::error_width(double px) {
    err_style_.width = px;
    return *this;
}

BarSeries& BarSeries::error_one_sided(bool value) {
    err_style_.one_sided = value;
    return *this;
}

Extent BarSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < values_.size(); ++i) {
        if (is_missing(values_[i])) continue;
        const double c = static_cast<double>(i) + 0.5;
        const double vhi = values_[i] + err_.hi(i);
        const double vlo = values_[i] - err_.lo(i);
        if (horizontal_) {
            e.include(values_[i], c);
            e.include(0.0, c);
            if (err_.has(i)) { e.include(vhi, c); e.include(vlo, c); }
        } else {
            e.include(c, values_[i]);
            e.include(c, 0.0);
            if (err_.has(i)) { e.include(c, vhi); e.include(c, vlo); }
        }
    }
    return e;
}

void BarSeries::build_geometry(const detail::GeomContext& ctx,
                               std::vector<SceneItem>& out) const {
    ShapeStyle style;
    style.fill = ctx.color;

    const Scale& cat_scale = horizontal_ ? ctx.y : ctx.x;
    const Scale& val_scale = horizontal_ ? ctx.x : ctx.y;
    const double band = cat_scale.band_width();
    const double group_width = band * 0.72;
    const double bar_width = group_width / static_cast<double>(ctx.bar_group_count);
    const double group_lo = -group_width / 2.0 + static_cast<double>(ctx.bar_group_index) * bar_width;

    for (std::size_t i = 0; i < values_.size(); ++i) {
        double v = values_[i];
        if (is_missing(v)) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed("bar series contains missing value at index "
                    + std::to_string(i)));
            detail::reject_unsupported_interpolate(missing_, "bar");
            if (missing_ == MissingPolicy::Zero)
                v = 0.0;
            else
                continue;
        }
        if (percent_ && ctx.stack_totals && i < ctx.stack_totals->size()) {
            const double total = (*ctx.stack_totals)[i];
            v = total > 0.0 ? v / total * 100.0 : 0.0;
        }
        double base_value = 0.0;
        if (ctx.stack_base && i < ctx.stack_base->size()) base_value = (*ctx.stack_base)[i];
        const double center = cat_scale.map_category(i);
        const double base = val_scale.map(base_value);
        const double end = val_scale.map(base_value + v);
        if (horizontal_) {
            const double y0 = center + group_lo;
            out.push_back(RectItem{{std::min(base, end), y0, std::abs(end - base), bar_width},
                                   style});
        } else {
            const double x0 = center + group_lo;
            out.push_back(RectItem{{x0, std::min(base, end), bar_width, std::abs(end - base)},
                                   style});
        }
        if (value_labels_) {
            TextItem label;
            label.text = detail::format_tick_value(v, 0.0);
            label.font = ctx.theme.tick_font();
            label.color = ctx.theme.muted_text_color;
            if (horizontal_) {
                const double plot_right = std::max(ctx.x.pixel_lo(), ctx.x.pixel_hi());
                const double tip = std::max(base, end);
                label.pos = {tip + 4.0, center + group_lo + bar_width / 2.0};
                label.halign = HAlign::Left;
                label.valign = VAlign::Middle;
                if (tip + 8.0 + label.font.size * label.text.size() * 0.6 > plot_right) {
                    // No room to the right: place the label inside the bar.
                    label.pos.x = tip - 4.0;
                    label.halign = HAlign::Right;
                    label.color = ctx.theme.background;
                }
            } else {
                const double plot_top = std::min(ctx.y.pixel_lo(), ctx.y.pixel_hi());
                const double tip = std::min(base, end);
                label.pos = {center + group_lo + bar_width / 2.0, tip - 3.0};
                label.halign = HAlign::Center;
                label.valign = VAlign::Bottom;
                if (tip - label.font.size - 6.0 < plot_top) {
                    // No headroom: place the label inside the bar.
                    label.pos.y = tip + 4.0;
                    label.valign = VAlign::Top;
                    label.color = ctx.theme.background;
                }
            }
            out.push_back(std::move(label));
        }
    }

    // Error whiskers on the value dimension, centred on each bar. Only drawn
    // for plain (non-stacked) bars; the config layer rejects error + stacked.
    if (!err_.empty()) {
        const ShapeStyle estyle = resolve_error_style(err_style_, ctx);
        for (std::size_t i = 0; i < values_.size(); ++i) {
            if (is_missing(values_[i]) || !err_.has(i)) continue;
            const double cross = cat_scale.map_category(i) + group_lo + bar_width / 2.0;
            emit_whisker(out, cross, values_[i], err_.lo(i), err_.hi(i), val_scale,
                         /*vertical*/ !horizontal_, estyle, err_style_.cap,
                         err_style_.one_sided, 0.0);
        }
    }
}

// -- RangeBarSeries ---------------------------------------------------------------

RangeBarSeries::RangeBarSeries(std::vector<std::string> categories,
                               std::span<const double> low, std::span<const double> high) {
    set_data(std::move(categories), low, high);
}

RangeBarSeries& RangeBarSeries::set_data(std::vector<std::string> categories,
                                         std::span<const double> low,
                                         std::span<const double> high) {
    if (categories.size() != low.size() || categories.size() != high.size()) {
        throw Error(cworks::validation_failed(
            "range bar series: categories, low, and high must have the same " "length (" +
            std::to_string(categories.size()) + " vs " + std::to_string(low.size()) + " vs " +
            std::to_string(high.size()) + ")"));
    }
    for (std::size_t i = 0; i < low.size(); ++i) {
        if (std::isfinite(low[i]) && std::isfinite(high[i]) && low[i] > high[i]) {
            throw Error(cworks::validation_failed("range bar series: low > high at index " +
                std::to_string(i)));
        }
    }
    categories_ = std::move(categories);
    low_.assign(low.begin(), low.end());
    high_.assign(high.begin(), high.end());
    return *this;
}

RangeBarSeries& RangeBarSeries::horizontal(bool value) {
    horizontal_ = value;
    return *this;
}

Extent RangeBarSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < low_.size(); ++i) {
        if (is_missing(low_[i]) || is_missing(high_[i])) continue;
        const double c = static_cast<double>(i) + 0.5;
        if (horizontal_) {
            e.include(low_[i], c);
            e.include(high_[i], c);
        } else {
            e.include(c, low_[i]);
            e.include(c, high_[i]);
        }
    }
    return e;
}

void RangeBarSeries::build_geometry(const detail::GeomContext& ctx,
                                    std::vector<SceneItem>& out) const {
    ShapeStyle style;
    style.fill = ctx.color;

    const Scale& cat_scale = horizontal_ ? ctx.y : ctx.x;
    const Scale& val_scale = horizontal_ ? ctx.x : ctx.y;
    const double band = cat_scale.band_width();
    const double group_width = band * 0.72;
    const double bar_width = group_width / static_cast<double>(ctx.bar_group_count);
    const double group_lo =
        -group_width / 2.0 + static_cast<double>(ctx.bar_group_index) * bar_width;

    for (std::size_t i = 0; i < low_.size(); ++i) {
        if (is_missing(low_[i]) || is_missing(high_[i])) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed(
                    "range bar series contains missing value at index " + std::to_string(i)));
            detail::reject_unsupported_interpolate(missing_, "range bar");
            continue;
        }
        const double center = cat_scale.map_category(i);
        const double a = val_scale.map(low_[i]);
        const double b = val_scale.map(high_[i]);
        const double lo = std::min(a, b);
        const double extent_px = std::max(1.0, std::abs(b - a));
        if (horizontal_) {
            out.push_back(
                RectItem{{lo, center + group_lo, extent_px, bar_width}, style});
        } else {
            out.push_back(
                RectItem{{center + group_lo, lo, bar_width, extent_px}, style});
        }
    }
}

// -- GanttSeries ------------------------------------------------------------------

GanttSeries::GanttSeries(std::vector<std::string> tasks, std::span<const double> start,
                         std::span<const double> end) {
    set_data(std::move(tasks), start, end);
}

GanttSeries& GanttSeries::set_data(std::vector<std::string> tasks,
                                   std::span<const double> start,
                                   std::span<const double> end) {
    if (tasks.size() != start.size() || tasks.size() != end.size()) {
        throw Error(cworks::validation_failed(
            "gantt series: tasks, start, and end must have the same length (" +
            std::to_string(tasks.size()) + " vs " + std::to_string(start.size()) + " vs " +
            std::to_string(end.size()) + ")"));
    }
    for (std::size_t i = 0; i < start.size(); ++i) {
        if (std::isfinite(start[i]) && std::isfinite(end[i]) && start[i] > end[i]) {
            throw Error(cworks::validation_failed("gantt series: start > end for task '" +
                                                  tasks[i] + "'"));
        }
    }
    tasks_ = std::move(tasks);
    start_.assign(start.begin(), start.end());
    end_.assign(end.begin(), end.end());
    return *this;
}

GanttSeries& GanttSeries::percent_complete(std::span<const double> pct) {
    if (pct.size() != tasks_.size()) {
        throw Error(cworks::validation_failed(
            "gantt series: percent_complete must have one value per task (" +
            std::to_string(pct.size()) + " vs " + std::to_string(tasks_.size()) + ")"));
    }
    percent_.resize(pct.size());
    for (std::size_t i = 0; i < pct.size(); ++i) {
        const double v = pct[i];
        if (is_missing(v)) {
            percent_[i] = std::numeric_limits<double>::quiet_NaN();
            continue;
        }
        if (v < 0.0 || v > 100.0) {
            throw Error(cworks::validation_failed(
                "gantt series: percent_complete for task '" + tasks_[i] +
                "' must be in [0, 100], got " + cworks::format_double(v)));
        }
        percent_[i] = v / 100.0;
    }
    return *this;
}

GanttSeries& GanttSeries::resources(std::vector<std::string> resource_per_task) {
    if (resource_per_task.size() != tasks_.size()) {
        throw Error(cworks::validation_failed(
            "gantt series: resource must have one value per task (" +
            std::to_string(resource_per_task.size()) + " vs " +
            std::to_string(tasks_.size()) + ")"));
    }
    resources_ = std::move(resource_per_task);
    return *this;
}

GanttSeries& GanttSeries::dependencies(std::vector<std::string> predecessor_lists) {
    if (predecessor_lists.size() != tasks_.size()) {
        throw Error(cworks::validation_failed(
            "gantt series: dependencies must have one value per task (" +
            std::to_string(predecessor_lists.size()) + " vs " +
            std::to_string(tasks_.size()) + ")"));
    }
    // Predecessors are named by task label, so labels must resolve to a
    // unique row.
    std::unordered_map<std::string, std::size_t> index_of;
    for (std::size_t i = 0; i < tasks_.size(); ++i) {
        if (!index_of.emplace(tasks_[i], i).second) {
            throw Error(cworks::validation_failed(
                "gantt series: dependencies require unique task labels, but '" +
                tasks_[i] + "' appears more than once"));
        }
    }

    const auto trim = [](std::string s) {
        const auto not_space = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        return s;
    };

    std::vector<std::vector<std::size_t>> resolved(tasks_.size());
    for (std::size_t i = 0; i < predecessor_lists.size(); ++i) {
        const std::string& cell = predecessor_lists[i];
        std::size_t pos = 0;
        while (pos <= cell.size()) {
            const std::size_t sep = cell.find(';', pos);
            const std::size_t end = sep == std::string::npos ? cell.size() : sep;
            const std::string name = trim(cell.substr(pos, end - pos));
            pos = end + 1;
            if (name.empty()) {
                if (sep == std::string::npos) break;
                continue;
            }
            const auto it = index_of.find(name);
            if (it == index_of.end()) {
                throw Error(cworks::validation_failed(
                    "gantt series: task '" + tasks_[i] +
                    "' depends on unknown task '" + name + "'"));
            }
            const std::size_t pred = it->second;
            if (pred == i) {
                throw Error(cworks::validation_failed(
                    "gantt series: task '" + tasks_[i] + "' cannot depend on itself"));
            }
            // A repeated predecessor in one cell would draw the same
            // connector twice; keep each edge once.
            if (std::find(resolved[i].begin(), resolved[i].end(), pred) ==
                resolved[i].end())
                resolved[i].push_back(pred);
            if (sep == std::string::npos) break;
        }
    }

    // Reject cycles up front (Kahn): a task that can never reach in-degree 0
    // sits on a cycle. Name the lowest-index such task for a stable message.
    const std::size_t n = tasks_.size();
    std::vector<std::size_t> indeg(n, 0);
    for (std::size_t i = 0; i < n; ++i) indeg[i] = resolved[i].size();
    std::vector<std::vector<std::size_t>> succ(n);
    for (std::size_t i = 0; i < n; ++i)
        for (const std::size_t p : resolved[i]) succ[p].push_back(i);
    std::set<std::size_t> ready;
    for (std::size_t i = 0; i < n; ++i)
        if (indeg[i] == 0) ready.insert(i);
    std::size_t processed = 0;
    while (!ready.empty()) {
        const std::size_t u = *ready.begin();
        ready.erase(ready.begin());
        ++processed;
        for (const std::size_t v : succ[u])
            if (--indeg[v] == 0) ready.insert(v);
    }
    if (processed < n) {
        for (std::size_t i = 0; i < n; ++i) {
            if (indeg[i] > 0) {
                throw Error(cworks::validation_failed(
                    "gantt series: dependency cycle detected involving task '" +
                    tasks_[i] + "'"));
            }
        }
    }

    deps_ = std::move(resolved);
    return *this;
}

GanttSeries& GanttSeries::critical_path(bool on) {
    critical_path_ = on;
    return *this;
}

std::vector<std::size_t> GanttSeries::topological_order() const {
    const std::size_t n = tasks_.size();
    std::vector<std::size_t> indeg(n, 0);
    for (std::size_t i = 0; i < n; ++i)
        indeg[i] = i < deps_.size() ? deps_[i].size() : 0;
    std::vector<std::vector<std::size_t>> succ(n);
    for (std::size_t i = 0; i < deps_.size(); ++i)
        for (const std::size_t p : deps_[i]) succ[p].push_back(i);
    std::set<std::size_t> ready; // ascending input index -> deterministic
    for (std::size_t i = 0; i < n; ++i)
        if (indeg[i] == 0) ready.insert(i);
    std::vector<std::size_t> order;
    order.reserve(n);
    while (!ready.empty()) {
        const std::size_t u = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(u);
        for (const std::size_t v : succ[u])
            if (--indeg[v] == 0) ready.insert(v);
    }
    return order; // acyclic by construction (dependencies() rejects cycles)
}

bool GanttSeries::CriticalChain::is_edge(std::size_t pred, std::size_t succ) const {
    for (const auto& e : edge)
        if (e.first == pred && e.second == succ) return true;
    return false;
}

GanttSeries::CriticalChain GanttSeries::critical_chain() const {
    CriticalChain chain;
    const std::size_t n = tasks_.size();
    if (!critical_path_ || deps_.empty() || n == 0) return chain;

    // Longest path by task duration over the DAG. Process nodes in
    // topological order so every predecessor's earliest-finish is settled
    // before its successors. EF[v] = duration[v] + max over predecessors of
    // EF[u]; ties (equal predecessor EF) resolve to the smallest input
    // index, giving a bit-reproducible result independent of edge order.
    std::vector<double> ef(n, 0.0);
    const std::size_t kNone = static_cast<std::size_t>(-1);
    std::vector<std::size_t> best_pred(n, kNone);
    const auto duration = [&](std::size_t i) {
        return std::isfinite(start_[i]) && std::isfinite(end_[i]) ? end_[i] - start_[i]
                                                                  : 0.0;
    };

    for (const std::size_t v : topological_order()) {
        double best = 0.0;
        std::size_t arg = kNone;
        // Scan predecessors in ascending input index so equal EFs keep the
        // lowest-index predecessor.
        std::vector<std::size_t> preds = v < deps_.size() ? deps_[v]
                                                          : std::vector<std::size_t>{};
        std::sort(preds.begin(), preds.end());
        for (const std::size_t u : preds) {
            if (arg == kNone || ef[u] > best) {
                best = ef[u];
                arg = u;
            }
        }
        ef[v] = duration(v) + (arg == kNone ? 0.0 : best);
        best_pred[v] = arg;
    }

    // Terminal = the node with the greatest earliest-finish; ties resolve to
    // the smallest input index.
    std::size_t terminal = 0;
    for (std::size_t i = 1; i < n; ++i)
        if (ef[i] > ef[terminal]) terminal = i;

    chain.node.assign(n, false);
    std::size_t cur = terminal;
    while (cur != kNone) {
        chain.node[cur] = true;
        const std::size_t p = best_pred[cur];
        if (p != kNone) chain.edge.emplace_back(p, cur);
        cur = p;
    }
    return chain;
}

std::vector<std::string> GanttSeries::resource_order() const {
    std::vector<std::string> order;
    for (const std::string& r : resources_) {
        if (std::find(order.begin(), order.end(), r) == order.end()) order.push_back(r);
    }
    return order;
}

std::vector<std::string> GanttSeries::category_names() const {
    return std::vector<std::string>(tasks_.rbegin(), tasks_.rend());
}

std::vector<LegendItemInfo> GanttSeries::legend_items(Color resolved_color,
                                                      const Theme& theme) const {
    // Unset resource: fall back to the base single-entry legend (a labelled
    // series contributes one filled swatch; an unlabelled one contributes
    // nothing).
    if (resources_.empty()) return Series::legend_items(resolved_color, theme);
    std::vector<LegendItemInfo> items;
    const std::vector<std::string> order = resource_order();
    items.reserve(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        LegendItemInfo item;
        item.label = order[i];
        item.color = theme.series_color(i);
        item.filled = true;
        item.square = true;
        items.push_back(std::move(item));
    }
    return items;
}

Extent GanttSeries::extent() const {
    Extent e;
    const std::size_t n = start_.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (is_missing(start_[i]) || is_missing(end_[i])) continue;
        // Value axis min/max is order-independent; the category centre only
        // needs to span the slots, so it matches the pre-extraction pixels.
        const double c = static_cast<double>(n - 1 - i) + 0.5;
        e.include(start_[i], c);
        e.include(end_[i], c);
    }
    return e;
}

void GanttSeries::build_geometry(const detail::GeomContext& ctx,
                                 std::vector<SceneItem>& out) const {
    const Scale& cat_scale = ctx.y; // gantt is intrinsically horizontal
    const Scale& val_scale = ctx.x;
    const double band = cat_scale.band_width();
    const double group_width = band * 0.72;
    const double bar_width = group_width / static_cast<double>(ctx.bar_group_count);
    const double group_lo =
        -group_width / 2.0 + static_cast<double>(ctx.bar_group_index) * bar_width;

    const std::vector<std::string> order = resources_.empty()
                                               ? std::vector<std::string>{}
                                               : resource_order();
    const auto group_index = [&](const std::string& r) -> std::size_t {
        return static_cast<std::size_t>(std::find(order.begin(), order.end(), r) -
                                        order.begin());
    };

    const std::size_t n = start_.size();
    const CriticalChain critical = critical_chain();
    // Vertical centre of task i's bar, or NaN when the task carries a
    // missing start/end (no bar, hence no connector endpoint).
    const auto bar_center_y = [&](std::size_t i) -> double {
        if (is_missing(start_[i]) || is_missing(end_[i]))
            return std::numeric_limits<double>::quiet_NaN();
        const std::size_t slot = n - 1 - i;
        return cat_scale.map_category(slot) + group_lo + bar_width / 2.0;
    };

    // Emit in slot order (bottom slot first) so the scene-item sequence is
    // identical to the pre-extraction factory, which walked the reversed
    // task list in index order. Input row i renders at slot (n-1-i).
    for (std::size_t slot = 0; slot < n; ++slot) {
        const std::size_t i = n - 1 - slot;
        if (is_missing(start_[i]) || is_missing(end_[i])) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed(
                    "gantt series contains missing value for task '" + tasks_[i] + "'"));
            detail::reject_unsupported_interpolate(missing_, "gantt");
            continue;
        }
        const double center = cat_scale.map_category(slot);
        const double a = val_scale.map(start_[i]);
        const double b = val_scale.map(end_[i]);
        const double lo = std::min(a, b);
        const double extent_px = std::max(1.0, std::abs(b - a));

        const Color bar_color =
            resources_.empty() ? ctx.color : ctx.theme.series_color(group_index(resources_[i]));

        const bool has_progress = !percent_.empty() && std::isfinite(percent_[i]);

        ShapeStyle base;
        // With a progress overlay the full bar is the lightened "remaining"
        // track; the completed span is overdrawn in full colour below.
        base.fill = has_progress ? lerp(bar_color, ctx.theme.background, 0.62) : bar_color;
        // Critical-path bars gain a heavier outline in the theme's text
        // colour so the longest-duration chain reads at a glance.
        if (!critical.node.empty() && critical.node[i]) {
            base.stroke = ctx.theme.text_color;
            base.stroke_width = 2.0;
        }
        out.push_back(RectItem{{lo, center + group_lo, extent_px, bar_width}, base});

        if (has_progress) {
            const double frac = std::clamp(percent_[i], 0.0, 1.0);
            if (frac > 0.0) {
                const double done_edge = a + frac * (b - a);
                const double done_lo = std::min(a, done_edge);
                const double done_w = std::abs(done_edge - a);
                ShapeStyle done;
                done.fill = bar_color;
                out.push_back(
                    RectItem{{done_lo, center + group_lo, done_w, bar_width}, done});
            }
        }
    }

    // Finish-to-start dependency connectors, drawn on top of the bars.
    // Iterate (successor input order, then predecessor input order) so the
    // scene-item sequence is deterministic. Each edge is a simple elbow:
    // a horizontal stub out of the predecessor's finish, a vertical
    // segment, then a horizontal run into the successor's start, closed by
    // an arrowhead (the same shaft + filled-triangle recipe as annotation
    // arrows).
    if (deps_.empty()) return;
    const double stub = 7.0;
    for (std::size_t succ = 0; succ < deps_.size(); ++succ) {
        const double y_succ = bar_center_y(succ);
        if (!std::isfinite(y_succ)) continue;
        const double x_succ_start = val_scale.map(start_[succ]);
        std::vector<std::size_t> preds = deps_[succ];
        std::sort(preds.begin(), preds.end());
        for (const std::size_t pred : preds) {
            const double y_pred = bar_center_y(pred);
            if (!std::isfinite(y_pred)) continue;
            const double x_pred_end = val_scale.map(end_[pred]);

            const std::vector<Point> pts{{x_pred_end, y_pred},
                                         {x_pred_end + stub, y_pred},
                                         {x_pred_end + stub, y_succ},
                                         {x_succ_start, y_succ}};

            const bool is_crit = critical.is_edge(pred, succ);
            const Color line_color = is_crit ? ctx.theme.text_color : ctx.theme.axis_color;

            ShapeStyle shaft;
            shaft.stroke = line_color;
            shaft.stroke_width = is_crit ? 2.2 : 1.4;
            out.push_back(PolylineItem{pts, shaft});

            // Arrowhead pointing along the connector's final approach. Use
            // the last segment with non-zero length so a degenerate final
            // run (successor starting exactly at the stub) still orients the
            // head off the vertical leg.
            double ux = 0.0, uy = 0.0, len = 0.0;
            for (std::size_t k = pts.size(); k-- > 1;) {
                const double dx = pts[k].x - pts[k - 1].x;
                const double dy = pts[k].y - pts[k - 1].y;
                len = std::sqrt(dx * dx + dy * dy);
                if (len > 1e-9) {
                    ux = dx / len;
                    uy = dy / len;
                    break;
                }
            }
            if (len > 1e-9) {
                const Point head = pts.back();
                const double head_len = 9.0, head_half = 4.0;
                const Point b{head.x - ux * head_len, head.y - uy * head_len};
                ShapeStyle fill;
                fill.fill = line_color;
                out.push_back(PolygonItem{{head,
                                           {b.x - uy * head_half, b.y + ux * head_half},
                                           {b.x + uy * head_half, b.y - ux * head_half}},
                                          fill});
            }
        }
    }
}

// -- LollipopSeries ---------------------------------------------------------------

LollipopSeries::LollipopSeries(std::vector<std::string> categories,
                               std::span<const double> values) {
    set_data(std::move(categories), values);
}

LollipopSeries& LollipopSeries::set_data(std::vector<std::string> categories,
                                         std::span<const double> values) {
    if (categories.size() != values.size()) {
        throw Error(cworks::validation_failed(
            "lollipop series: categories and values must have the same length (" +
            std::to_string(categories.size()) + " vs " + std::to_string(values.size()) + ")"));
    }
    categories_ = std::move(categories);
    values_.assign(values.begin(), values.end());
    return *this;
}

LollipopSeries& LollipopSeries::horizontal(bool value) {
    horizontal_ = value;
    return *this;
}

LollipopSeries& LollipopSeries::marker_size(double size) {
    marker_size_ = std::max(0.5, size);
    return *this;
}

LollipopSeries& LollipopSeries::stem_width(double width) {
    stem_width_ = std::max(0.1, width);
    return *this;
}

Extent LollipopSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < values_.size(); ++i) {
        if (is_missing(values_[i])) continue;
        const double c = static_cast<double>(i) + 0.5;
        if (horizontal_) {
            e.include(values_[i], c);
            e.include(0.0, c);
        } else {
            e.include(c, values_[i]);
            e.include(c, 0.0);
        }
    }
    return e;
}

void LollipopSeries::build_geometry(const detail::GeomContext& ctx,
                                    std::vector<SceneItem>& out) const {
    const Scale& cat_scale = horizontal_ ? ctx.y : ctx.x;
    const Scale& val_scale = horizontal_ ? ctx.x : ctx.y;
    ShapeStyle stem;
    stem.stroke = ctx.color;
    stem.stroke_width = stem_width_;
    ShapeStyle dot;
    dot.fill = ctx.color;
    const double base = val_scale.map(0.0);
    for (std::size_t i = 0; i < values_.size(); ++i) {
        if (is_missing(values_[i])) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed(
                    "lollipop series contains missing value at index " + std::to_string(i)));
            detail::reject_unsupported_interpolate(missing_, "lollipop");
            continue;
        }
        const double center = cat_scale.map_category(i);
        const double tip = val_scale.map(values_[i]);
        if (horizontal_) {
            out.push_back(LineItem{{base, center}, {tip, center}, stem});
            out.push_back(CircleItem{{tip, center}, marker_size_, dot});
        } else {
            out.push_back(LineItem{{center, base}, {center, tip}, stem});
            out.push_back(CircleItem{{center, tip}, marker_size_, dot});
        }
    }
}

// -- DumbbellSeries ---------------------------------------------------------------

DumbbellSeries::DumbbellSeries(std::vector<std::string> categories,
                               std::span<const double> start,
                               std::span<const double> end) {
    set_data(std::move(categories), start, end);
}

DumbbellSeries& DumbbellSeries::set_data(std::vector<std::string> categories,
                                         std::span<const double> start,
                                         std::span<const double> end) {
    if (categories.size() != start.size() || categories.size() != end.size()) {
        throw Error(cworks::validation_failed(
            "dumbbell series: categories, start, and end must have the same " "length (" +
            std::to_string(categories.size()) + " vs " + std::to_string(start.size()) + " vs " +
            std::to_string(end.size()) + ")"));
    }
    categories_ = std::move(categories);
    start_.assign(start.begin(), start.end());
    end_.assign(end.begin(), end.end());
    return *this;
}

DumbbellSeries& DumbbellSeries::horizontal(bool value) {
    horizontal_ = value;
    return *this;
}

DumbbellSeries& DumbbellSeries::start_color(Color color) {
    start_color_ = color;
    return *this;
}

DumbbellSeries& DumbbellSeries::end_color(Color color) {
    end_color_ = color;
    return *this;
}

DumbbellSeries& DumbbellSeries::marker_size(double size) {
    marker_size_ = std::max(0.5, size);
    return *this;
}

Extent DumbbellSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < start_.size(); ++i) {
        const double c = static_cast<double>(i) + 0.5;
        if (!is_missing(start_[i])) horizontal_ ? e.include(start_[i], c)
                                                : e.include(c, start_[i]);
        if (!is_missing(end_[i])) horizontal_ ? e.include(end_[i], c)
                                              : e.include(c, end_[i]);
    }
    return e;
}

void DumbbellSeries::build_geometry(const detail::GeomContext& ctx,
                                    std::vector<SceneItem>& out) const {
    const Scale& cat_scale = horizontal_ ? ctx.y : ctx.x;
    const Scale& val_scale = horizontal_ ? ctx.x : ctx.y;
    const Color start_c = start_color_.value_or(ctx.theme.muted_text_color);
    const Color end_c = end_color_.value_or(ctx.color);
    ShapeStyle connector;
    connector.stroke = ctx.theme.muted_text_color.with_alpha(0.55);
    connector.stroke_width = 1.6;
    connector.cap = LineCap::Round;
    connector.join = LineJoin::Round;
    ShapeStyle start_dot;
    start_dot.fill = start_c;
    ShapeStyle end_dot;
    end_dot.fill = end_c;
    for (std::size_t i = 0; i < start_.size(); ++i) {
        if (is_missing(start_[i]) || is_missing(end_[i])) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed(
                    "dumbbell series contains missing value at index " + std::to_string(i)));
            detail::reject_unsupported_interpolate(missing_, "dumbbell");
            continue;
        }
        const double center = cat_scale.map_category(i);
        const double a = val_scale.map(start_[i]);
        const double b = val_scale.map(end_[i]);
        if (horizontal_) {
            out.push_back(LineItem{{a, center}, {b, center}, connector});
            out.push_back(CircleItem{{a, center}, marker_size_, start_dot});
            out.push_back(CircleItem{{b, center}, marker_size_, end_dot});
        } else {
            out.push_back(LineItem{{center, a}, {center, b}, connector});
            out.push_back(CircleItem{{center, a}, marker_size_, start_dot});
            out.push_back(CircleItem{{center, b}, marker_size_, end_dot});
        }
    }
}

// -- PopulationPyramidSeries ------------------------------------------------------

PopulationPyramidSeries::PopulationPyramidSeries(std::vector<std::string> categories,
                                                 std::span<const double> left,
                                                 std::span<const double> right) {
    set_data(std::move(categories), left, right);
}

PopulationPyramidSeries& PopulationPyramidSeries::set_data(
    std::vector<std::string> categories, std::span<const double> left,
    std::span<const double> right) {
    if (categories.size() != left.size() || categories.size() != right.size()) {
        throw Error(cworks::validation_failed(
            "population pyramid series: categories, left, and right must have the same "
            "length (" + std::to_string(categories.size()) + " vs " +
            std::to_string(left.size()) + " vs " + std::to_string(right.size()) + ")"));
    }
    const auto reject_negative = [](std::span<const double> values, const char* side) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (std::isfinite(values[i]) && values[i] < 0.0) {
                throw Error(cworks::validation_failed(
                    std::string("population pyramid series: ") + side +
                    " values must be >= 0 (index " + std::to_string(i) +
                    "); the left side is mirrored automatically"));
            }
        }
    };
    reject_negative(left, "left");
    reject_negative(right, "right");
    categories_ = std::move(categories);
    left_.assign(left.begin(), left.end());
    right_.assign(right.begin(), right.end());
    return *this;
}

PopulationPyramidSeries& PopulationPyramidSeries::left_label(std::string text) {
    left_label_ = std::move(text);
    return *this;
}

PopulationPyramidSeries& PopulationPyramidSeries::right_label(std::string text) {
    right_label_ = std::move(text);
    return *this;
}

PopulationPyramidSeries& PopulationPyramidSeries::left_color(Color color) {
    left_color_ = color;
    return *this;
}

PopulationPyramidSeries& PopulationPyramidSeries::right_color(Color color) {
    right_color_ = color;
    return *this;
}

PopulationPyramidSeries& PopulationPyramidSeries::value_labels(bool value) {
    value_labels_ = value;
    return *this;
}

PopulationPyramidSeries& PopulationPyramidSeries::category_labels(CategoryLabels placement) {
    category_labels_ = placement;
    return *this;
}

PopulationPyramidSeries& PopulationPyramidSeries::joined(bool value) {
    joined_ = value;
    return *this;
}

Extent PopulationPyramidSeries::extent() const {
    // Symmetric about zero: both halves share one magnitude scale, so
    // bar lengths stay visually comparable across the centre line.
    double magnitude = 0.0;
    for (double v : left_) {
        if (!is_missing(v)) magnitude = std::max(magnitude, v);
    }
    for (double v : right_) {
        if (!is_missing(v)) magnitude = std::max(magnitude, v);
    }
    Extent e;
    for (std::size_t i = 0; i < categories_.size(); ++i) {
        const double c = static_cast<double>(i) + 0.5;
        e.include(-magnitude, c);
        e.include(magnitude, c);
    }
    return e;
}

std::vector<LegendItemInfo> PopulationPyramidSeries::legend_items(Color resolved_color,
                                                                  const Theme& theme) const {
    std::vector<LegendItemInfo> items;
    if (!left_label_.empty()) {
        LegendItemInfo item;
        item.label = left_label_;
        item.color = left_color_.value_or(resolved_color);
        item.filled = true;
        item.square = true;
        items.push_back(std::move(item));
    }
    if (!right_label_.empty()) {
        LegendItemInfo item;
        item.label = right_label_;
        item.color = right_color_.value_or(theme.series_color(1));
        item.filled = true;
        item.square = true;
        items.push_back(std::move(item));
    }
    return items;
}

void PopulationPyramidSeries::build_geometry(const detail::GeomContext& ctx,
                                             std::vector<SceneItem>& out) const {
    const Scale& cat_scale = ctx.y;
    const Scale& val_scale = ctx.x;
    const double band = cat_scale.band_width();
    const double bar_h = band * 0.72;
    // Joined: both halves share one centre line. Unjoined (mirrored
    // scale): each half grows from its own inner edge of the centre gap.
    const auto [base_left, base_right] = val_scale.center_edges();
    const double plot_left = std::min(val_scale.pixel_lo(), val_scale.pixel_hi());
    const double plot_right = std::max(val_scale.pixel_lo(), val_scale.pixel_hi());

    ShapeStyle left_style;
    left_style.fill = left_color_.value_or(ctx.color);
    ShapeStyle right_style;
    right_style.fill = right_color_.value_or(ctx.theme.series_color(1));

    // One half: bar from the centre line outward, value label at the tip
    // (falls back to inside the bar when the plot edge is too close).
    const auto draw_half = [&](std::size_t i, double v, bool left_side,
                               const ShapeStyle& style) {
        if (is_missing(v)) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed(
                    std::string("population pyramid series contains missing ") +
                    (left_side ? "left" : "right") + " value at index " + std::to_string(i)));
            if (missing_ != MissingPolicy::Zero) return;
            v = 0.0;
        }
        const double base = left_side ? base_left : base_right;
        const double center = cat_scale.map_category(i);
        const double y0 = center - bar_h / 2.0;
        const double tip = val_scale.map(left_side ? -v : v);
        out.push_back(RectItem{{std::min(base, tip), y0, std::abs(tip - base), bar_h}, style});
        if (!value_labels_) return;
        TextItem label;
        label.text = detail::format_tick_value(v, 0.0);
        label.font = ctx.theme.tick_font();
        label.color = ctx.theme.muted_text_color;
        label.valign = VAlign::Middle;
        label.pos.y = center;
        const double approx_w = label.font.size * static_cast<double>(label.text.size()) * 0.6;
        if (left_side) {
            label.pos.x = tip - 4.0;
            label.halign = HAlign::Right;
            if (tip - 8.0 - approx_w < plot_left) {
                // No room to the left: place the label inside the bar.
                label.pos.x = tip + 4.0;
                label.halign = HAlign::Left;
                label.color = ctx.theme.background;
            }
        } else {
            label.pos.x = tip + 4.0;
            label.halign = HAlign::Left;
            if (tip + 8.0 + approx_w > plot_right) {
                label.pos.x = tip - 4.0;
                label.halign = HAlign::Right;
                label.color = ctx.theme.background;
            }
        }
        out.push_back(std::move(label));
    };

    for (std::size_t i = 0; i < categories_.size(); ++i) {
        draw_half(i, left_[i], true, left_style);
        draw_half(i, right_[i], false, right_style);
    }

    if (category_labels_ == CategoryLabels::Center) {
        // The category labels sit in the centre: inside the gap between
        // the halves (unjoined), or on the centre line above the bars —
        // then over a translucent halo so bar ends stay readable.
        ShapeStyle halo_style;
        halo_style.fill = ctx.theme.background.with_alpha(0.7);
        const TextMeasurer& measurer = default_text_measurer();
        const double label_x = (base_left + base_right) / 2.0;
        for (std::size_t i = 0; i < categories_.size(); ++i) {
            TextItem label;
            label.text = categories_[i];
            label.font = ctx.theme.tick_font();
            label.color = ctx.theme.text_color;
            label.pos = {label_x, cat_scale.map_category(i)};
            label.halign = HAlign::Center;
            label.valign = VAlign::Middle;
            if (joined_) out.push_back(text_halo(label, measurer, 2.0, halo_style));
            out.push_back(std::move(label));
        }
    }
}

// -- FunnelSeries -----------------------------------------------------------------

FunnelSeries::FunnelSeries(std::vector<std::string> stages,
                           std::span<const double> values) {
    set_data(std::move(stages), values);
}

FunnelSeries& FunnelSeries::set_data(std::vector<std::string> stages,
                                     std::span<const double> values) {
    if (stages.size() != values.size()) {
        throw Error(cworks::validation_failed(
            "funnel series: stages and values must have the same length (" +
            std::to_string(stages.size()) + " vs " + std::to_string(values.size()) + ")"));
    }
    if (values.empty()) throw Error(cworks::validation_failed(
        "funnel series: needs at least one stage"));
    double max_value = 0.0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i]) || values[i] < 0.0) {
            throw Error(cworks::validation_failed(
                "funnel series: values must be finite and non-negative (value at " "index " +
                std::to_string(i) + " is invalid)"));
        }
        max_value = std::max(max_value, values[i]);
    }
    if (!(max_value > 0.0)) throw Error(cworks::validation_failed(
        "funnel series: needs a positive value"));
    stages_ = std::move(stages);
    values_.assign(values.begin(), values.end());
    return *this;
}

FunnelSeries& FunnelSeries::value_labels(bool value) {
    value_labels_ = value;
    return *this;
}

FunnelSeries& FunnelSeries::percent_labels(bool value) {
    percent_labels_ = value;
    if (value) value_labels_ = true;
    return *this;
}

FunnelSeries& FunnelSeries::stage_labels(bool value) {
    stage_labels_ = value;
    return *this;
}

Extent FunnelSeries::extent() const { return {}; } // axis-free panel

void FunnelSeries::build_geometry(const detail::GeomContext& ctx,
                                  std::vector<SceneItem>& out) const {
    // Recover the plot rectangle from the dummy scales of the axis-free panel.
    const double plot_x = std::min(ctx.x.pixel_lo(), ctx.x.pixel_hi());
    const double plot_w = std::abs(ctx.x.pixel_hi() - ctx.x.pixel_lo());
    const double plot_y = std::min(ctx.y.pixel_lo(), ctx.y.pixel_hi());
    const double plot_h = std::abs(ctx.y.pixel_hi() - ctx.y.pixel_lo());

    const std::size_t n = values_.size();
    double max_value = 0.0;
    for (double v : values_) max_value = std::max(max_value, v);
    if (!(max_value > 0.0) || n == 0) return;

    // Left gutter for stage names, remaining width for the centered bars.
    const double gutter = stage_labels_ ? plot_w * 0.22 : 0.0;
    const double funnel_x = plot_x + gutter;
    const double funnel_w = plot_w - gutter;
    const double row_h = plot_h / static_cast<double>(n);
    const double bar_h = row_h * 0.74;

    const Color label_color = contrast_text_color(ctx.color, ctx.theme);

    for (std::size_t i = 0; i < n; ++i) {
        const double cy = plot_y + row_h * (static_cast<double>(i) + 0.5);
        const double w = funnel_w * 0.94 * (values_[i] / max_value);
        const double cx = funnel_x + funnel_w / 2.0;

        if (w > 0.0) {
            ShapeStyle style;
            style.fill = ctx.color;
            style.stroke = ctx.theme.background;
            style.stroke_width = 1.0;
            out.push_back(RectItem{{cx - w / 2.0, cy - bar_h / 2.0, w, bar_h}, style});
        }

        if (stage_labels_) {
            TextItem stage;
            stage.pos = {funnel_x - 10.0, cy};
            stage.text = stages_[i];
            stage.font = ctx.theme.tick_font();
            stage.color = ctx.theme.text_color;
            stage.halign = HAlign::Right;
            stage.valign = VAlign::Middle;
            out.push_back(std::move(stage));
        }
        if (value_labels_) {
            std::string text;
            if (percent_labels_) {
                const double reference = values_.front() > 0.0 ? values_.front() : max_value;
                text = format_percent(values_[i] / reference);
            } else {
                text = detail::format_tick_value(values_[i], 0.0);
            }
            TextItem value;
            value.pos = {cx, cy};
            value.text = std::move(text);
            value.font = ctx.theme.tick_font();
            // Inside the bar when it is wide enough, otherwise beside it.
            const double approx_w =
                value.font.size * static_cast<double>(value.text.size()) * 0.62;
            if (approx_w + 10.0 < w) {
                value.color = label_color;
                value.halign = HAlign::Center;
            } else {
                value.pos.x = cx + w / 2.0 + 8.0;
                value.color = ctx.theme.text_color;
                value.halign = HAlign::Left;
            }
            value.valign = VAlign::Middle;
            out.push_back(std::move(value));
        }
    }
}

// -- XBarSeries -------------------------------------------------------------------

XBarSeries::XBarSeries(std::span<const double> x, std::span<const double> heights) {
    set_data(x, heights);
}

XBarSeries& XBarSeries::set_data(std::span<const double> x,
                                 std::span<const double> heights) {
    check_sizes(x.size(), heights.size(), "xbar series");
    x_.assign(x.begin(), x.end());
    heights_.assign(heights.begin(), heights.end());
    return *this;
}

XBarSeries& XBarSeries::bar_width(double px) {
    bar_width_ = std::max(0.0, px);
    return *this;
}

Extent XBarSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < x_.size(); ++i) {
        if (is_missing(x_[i]) || is_missing(heights_[i])) continue;
        e.include(x_[i], heights_[i]);
        e.include(x_[i], 0.0);
    }
    // Pad x by half the smallest spacing so edge bars are not clipped.
    if (e.valid) {
        std::vector<double> xs;
        for (double v : x_)
            if (!is_missing(v)) xs.push_back(v);
        std::sort(xs.begin(), xs.end());
        double min_gap = 0.0;
        for (std::size_t i = 1; i < xs.size(); ++i) {
            const double gap = xs[i] - xs[i - 1];
            if (gap > 0.0 && (min_gap == 0.0 || gap < min_gap)) min_gap = gap;
        }
        const double pad = min_gap > 0.0 ? min_gap * 0.6 : 0.5;
        e.x_lo -= pad;
        e.x_hi += pad;
    }
    return e;
}

void XBarSeries::build_geometry(const detail::GeomContext& ctx,
                                std::vector<SceneItem>& out) const {
    double width = bar_width_;
    if (width <= 0.0) {
        std::vector<double> px;
        px.reserve(x_.size());
        for (double v : x_)
            if (!is_missing(v)) px.push_back(ctx.x.map(v));
        std::sort(px.begin(), px.end());
        double min_gap = 0.0;
        for (std::size_t i = 1; i < px.size(); ++i) {
            const double gap = px[i] - px[i - 1];
            if (gap > 0.0 && (min_gap == 0.0 || gap < min_gap)) min_gap = gap;
        }
        width = min_gap > 0.0 ? std::clamp(min_gap * 0.8, 1.0, 40.0) : 8.0;
    }

    ShapeStyle style;
    style.fill = ctx.color.with_alpha(0.85);
    const double base = ctx.y.map(0.0);
    for (std::size_t i = 0; i < x_.size(); ++i) {
        if (is_missing(x_[i]) || is_missing(heights_[i])) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed("xbar series contains missing value at index "
                    + std::to_string(i)));
            detail::reject_unsupported_interpolate(missing_, "xbar");
            continue;
        }
        const double cx = ctx.x.map(x_[i]);
        const double top = ctx.y.map(heights_[i]);
        out.push_back(RectItem{{cx - width / 2.0, std::min(top, base), width,
                                std::max(1.0, std::abs(base - top))},
                               style});
    }
}

// -- BoxPlotSeries ----------------------------------------------------------------

BoxPlotSeries::BoxPlotSeries(std::vector<std::string> categories,
                             std::vector<std::vector<double>> data) {
    set_data(std::move(categories), std::move(data));
}

BoxPlotSeries& BoxPlotSeries::set_data(std::vector<std::string> categories,
                                       std::vector<std::vector<double>> data) {
    if (categories.size() != data.size()) {
        throw Error(cworks::validation_failed(
            "box plot: categories and data must have the same length (" +
            std::to_string(categories.size()) + " vs " + std::to_string(data.size()) + ")"));
    }
    categories_ = std::move(categories);
    data_ = std::move(data);
    return *this;
}

BoxPlotSeries& BoxPlotSeries::show_outliers(bool value) {
    show_outliers_ = value;
    return *this;
}

BoxPlotSeries::Stats BoxPlotSeries::compute_stats(std::vector<double> values) {
    values.erase(std::remove_if(values.begin(), values.end(),
                                [](double v) { return !std::isfinite(v); }),
                 values.end());
    Stats s;
    if (values.empty()) return s;
    std::sort(values.begin(), values.end());
    const auto quantile = [&](double p) {
        const double idx = p * static_cast<double>(values.size() - 1);
        const std::size_t lo = static_cast<std::size_t>(std::floor(idx));
        const std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
        const double frac = idx - static_cast<double>(lo);
        return values[lo] * (1.0 - frac) + values[hi] * frac;
    };
    s.q1 = quantile(0.25);
    s.median = quantile(0.5);
    s.q3 = quantile(0.75);
    const double iqr = s.q3 - s.q1;
    const double lo_fence = s.q1 - 1.5 * iqr;
    const double hi_fence = s.q3 + 1.5 * iqr;
    s.whisker_lo = s.q3;
    s.whisker_hi = s.q1;
    bool any_inside = false;
    for (double v : values) {
        if (v >= lo_fence && v <= hi_fence) {
            if (!any_inside) {
                s.whisker_lo = v;
                any_inside = true;
            }
            s.whisker_hi = v;
        } else {
            s.outliers.push_back(v);
        }
    }
    if (!any_inside) {
        s.whisker_lo = s.q1;
        s.whisker_hi = s.q3;
    }
    s.valid = true;
    return s;
}

Extent BoxPlotSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < data_.size(); ++i) {
        const Stats s = compute_stats(data_[i]);
        if (!s.valid) continue;
        const double c = static_cast<double>(i) + 0.5;
        e.include(c, s.whisker_lo);
        e.include(c, s.whisker_hi);
        for (double o : s.outliers) e.include(c, o);
    }
    return e;
}

void BoxPlotSeries::build_geometry(const detail::GeomContext& ctx,
                                   std::vector<SceneItem>& out) const {
    const double band = ctx.x.band_width();
    const double box_w = band * 0.5;
    const double cap_w = band * 0.25;

    ShapeStyle box_style;
    box_style.fill = ctx.color.with_alpha(0.35);
    box_style.stroke = ctx.color;
    box_style.stroke_width = 1.2;

    ShapeStyle line_style;
    line_style.stroke = ctx.color;
    line_style.stroke_width = 1.2;

    ShapeStyle median_style;
    median_style.stroke = ctx.color;
    median_style.stroke_width = 2.0;

    ShapeStyle outlier_style;
    outlier_style.stroke = ctx.color;
    outlier_style.stroke_width = 1.1;

    for (std::size_t i = 0; i < data_.size(); ++i) {
        const Stats s = compute_stats(data_[i]);
        if (!s.valid) continue;
        const double cx = ctx.x.map_category(i);
        const double q1 = ctx.y.map(s.q1);
        const double q3 = ctx.y.map(s.q3);
        const double med = ctx.y.map(s.median);
        const double wlo = ctx.y.map(s.whisker_lo);
        const double whi = ctx.y.map(s.whisker_hi);

        // whiskers
        out.push_back(LineItem{{cx, q1}, {cx, wlo}, line_style});
        out.push_back(LineItem{{cx, q3}, {cx, whi}, line_style});
        out.push_back(LineItem{{cx - cap_w / 2, wlo}, {cx + cap_w / 2, wlo}, line_style});
        out.push_back(LineItem{{cx - cap_w / 2, whi}, {cx + cap_w / 2, whi}, line_style});
        // box (q3 maps above q1 in pixels)
        out.push_back(
            RectItem{{cx - box_w / 2, std::min(q1, q3), box_w, std::abs(q1 - q3)}, box_style});
        // median
        out.push_back(LineItem{{cx - box_w / 2, med}, {cx + box_w / 2, med}, median_style});
        // outliers
        if (show_outliers_) {
            for (double o : s.outliers) {
                out.push_back(CircleItem{{cx, ctx.y.map(o)}, 2.4, outlier_style});
            }
        }
    }
}

// -- AreaSeries -------------------------------------------------------------------

AreaSeries::AreaSeries(std::span<const double> x, std::span<const double> y) { set_data(x, y); }

AreaSeries& AreaSeries::set_data(std::span<const double> x, std::span<const double> y) {
    check_sizes(x.size(), y.size(), "area series");
    x_.assign(x.begin(), x.end());
    y_.assign(y.begin(), y.end());
    return *this;
}

AreaSeries& AreaSeries::fill_alpha(double alpha) {
    fill_alpha_ = alpha;
    fill_alpha_explicit_ = true;
    return *this;
}

AreaSeries& AreaSeries::baseline(double y0) {
    baseline_ = y0;
    return *this;
}

AreaSeries& AreaSeries::stacked(bool value) {
    stacked_ = value;
    // Stacked bands do not overlap, so a stronger default fill reads better.
    if (value && !fill_alpha_explicit_) fill_alpha_ = 0.55;
    return *this;
}

AreaSeries& AreaSeries::percent_stacked(bool value) {
    percent_ = value;
    if (value) stacked(true);
    return *this;
}

AreaSeries& AreaSeries::step(StepMode mode) {
    step_ = mode;
    return *this;
}

Extent AreaSeries::extent() const {
    Extent e = xy_extent(x_, y_);
    if (e.valid) {
        e.y_lo = std::min(e.y_lo, baseline_);
        e.y_hi = std::max(e.y_hi, baseline_);
    }
    return e;
}

void AreaSeries::build_geometry(const detail::GeomContext& ctx,
                                std::vector<SceneItem>& out) const {
    // Stacked mode: the layout provides per-point base and top values.
    if (ctx.stack_base && ctx.stack_top) {
        const std::vector<double>& base = *ctx.stack_base;
        const std::vector<double>& top = *ctx.stack_top;
        const std::size_t n = std::min({x_.size(), base.size(), top.size()});
        if (n < 2) return;
        std::vector<Point> top_pts, base_pts;
        top_pts.reserve(n);
        base_pts.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            top_pts.push_back({ctx.x.map(x_[i]), ctx.y.map(top[i])});
            base_pts.push_back({ctx.x.map(x_[i]), ctx.y.map(base[i])});
        }
        if (step_) {
            // Each boundary is expanded independently from the series' own
            // x positions, so a riser lands at the same x whether it comes
            // from this series' top edge, its base edge, or the matching
            // edge of another stacked series — stacking requires identical
            // x values already, so the staircases stay in step-space
            // instead of tearing along an interpolated slope.
            top_pts = expand_steps(top_pts, *step_);
            base_pts = expand_steps(base_pts, *step_);
        }
        std::vector<Point> poly = top_pts;
        for (std::size_t i = base_pts.size(); i-- > 0;) poly.push_back(base_pts[i]);
        ShapeStyle fill_style;
        fill_style.fill = ctx.color.with_alpha(fill_alpha_);
        out.push_back(PolygonItem{std::move(poly), fill_style});
        ShapeStyle line_style;
        line_style.stroke = ctx.color;
        line_style.stroke_width = ctx.stroke_width;
        line_style.cap = LineCap::Round;
        line_style.join = LineJoin::Round;
        out.push_back(PolylineItem{std::move(top_pts), line_style});
        return;
    }

    const double base_px = ctx.y.map(baseline_);
    for (auto& run : resolve_runs(x_, y_, missing_, ctx)) {
        if (run.size() < 2) continue;
        std::vector<Point> top = step_ ? expand_steps(run, *step_) : std::move(run);
        // Filled polygon down to the baseline.
        std::vector<Point> poly = top;
        poly.push_back({top.back().x, base_px});
        poly.push_back({top.front().x, base_px});
        ShapeStyle fill_style;
        fill_style.fill = ctx.color.with_alpha(fill_alpha_);
        out.push_back(PolygonItem{std::move(poly), fill_style});
        // Stroked top edge.
        ShapeStyle line_style;
        line_style.stroke = ctx.color;
        line_style.stroke_width = ctx.stroke_width;
        line_style.cap = LineCap::Round;
        line_style.join = LineJoin::Round;
        out.push_back(PolylineItem{std::move(top), line_style});
    }
}

// -- HistogramSeries ------------------------------------------------------------------

namespace {

/// Rice's rule, ⌈2·∛n⌉, WITHOUT A CUBE ROOT.
///
/// The reason it has to be done this way is the reason the rule is a hazard
/// at all: 2·∛n is an exact integer at every perfect cube — 8, 27, 64, 125,
/// 216, 1000 — which is to say at ordinary sample sizes, and `ceil` of a
/// value one ulp either side of an integer differs by a whole bin. Every
/// bar in the histogram then moves. `std::cbrt` has no correct-rounding
/// guarantee, so which side it lands on is a property of the C library.
///
/// But the question "what is the smallest integer k with k ≥ 2·∛n" is the
/// question "what is the smallest integer k with k³ ≥ 8n", and that is a
/// comparison between two integers. Nothing rounds, so there is nothing for
/// a platform to disagree about, and the answer is exact by construction
/// rather than accurate to an ulp.
int rice_bin_count(std::size_t n) {
    // k ≤ ⌈2·∛n⌉ and the result is returned as an int, so a bound that
    // covers every sample a machine can hold is generous: 20736³ > 8·2^40,
    // and a sample of 2^40 doubles is eight terabytes.
    constexpr std::uint64_t kMaxSamples = std::uint64_t{1} << 40;
    const std::uint64_t cubed = 8 * std::min<std::uint64_t>(n, kMaxSamples);
    std::uint64_t lo = 1, hi = 20736;
    while (lo < hi) {
        const std::uint64_t mid = lo + (hi - lo) / 2;
        if (mid * mid * mid >= cubed) hi = mid;
        else lo = mid + 1;
    }
    return static_cast<int>(lo);
}

/// Bin count derived from the rule and the (untrimmed) sample size. Always
/// >= 1. trim_percentile() narrows the *range* the count is spread over,
/// not the n these formulas see -- they answer "how many bins does this
/// many observations deserve", which does not change because a few of
/// them sit in the tails.
int histogram_bin_count(HistogramBinRule rule, std::size_t n) {
    const std::size_t samples = std::max<std::size_t>(n, 1);
    const double fn = static_cast<double>(samples);
    double count;
    switch (rule) {
    case HistogramBinRule::Rice:
        return rice_bin_count(samples); // integer decision, no cube root
    // The `ceil` below is a discrete decision, so the logarithm feeding it
    // must not be the platform's: an ordinary sample size is a power of two
    // often enough (8, 16, 64, 256 …) that Sturges' rule lands exactly on an
    // integer, where a libm one ulp high adds a whole bin and moves every
    // bar. cworks::log2 is EXACT on every power of two by construction, so
    // the boundary is decided rather than rounded. std::sqrt needs no such
    // treatment — IEEE-754 mandates that one correctly rounded, so ⌈√n⌉ is
    // already exact at every perfect square.
    case HistogramBinRule::Sturges: count = cworks::log2(fn) + 1.0; break;
    case HistogramBinRule::Sqrt:
    default: count = std::sqrt(fn); break;
    }
    return std::max(1, static_cast<int>(std::ceil(count)));
}

} // namespace

HistogramSeries::HistogramSeries(std::span<const double> values, int bins) {
    set_data(values);
    if (bins > 0) this->bins(bins);
}

HistogramSeries& HistogramSeries::set_data(std::span<const double> values) {
    values_.assign(values.begin(), values.end());
    compute();
    return *this;
}

HistogramSeries& HistogramSeries::set_data(std::span<const double> values, int bins) {
    set_data(values);
    if (bins > 0) this->bins(bins);
    return *this;
}

HistogramSeries& HistogramSeries::bins(int count) {
    if (count < 1) throw Error(cworks::validation_failed("histogram: bins must be >= 1"));
    if (bin_width_ > 0.0)
        throw Error(cworks::validation_failed("histogram: bins and bin_width are mutually exclusive; remove one"));
    bins_ = count;
    compute();
    return *this;
}

HistogramSeries& HistogramSeries::bin_rule(HistogramBinRule rule) {
    if (bin_width_ > 0.0)
        throw Error(cworks::validation_failed("histogram: bin_rule and bin_width are mutually exclusive; remove one"));
    bin_rule_ = rule;
    bin_rule_explicit_ = true;
    compute();
    return *this;
}

HistogramSeries& HistogramSeries::bin_width(double width) {
    if (!(width > 0.0)) throw Error(cworks::validation_failed("histogram: bin_width must be > 0"));
    if (bins_ > 0 || bin_rule_explicit_)
        throw Error(cworks::validation_failed("histogram: bin_width is mutually exclusive with an explicit "
                                              "bins/bin_rule; remove one"));
    bin_width_ = width;
    compute();
    return *this;
}

HistogramSeries& HistogramSeries::range(double minimum, double maximum) {
    if (!(maximum > minimum)) throw Error(cworks::validation_failed("histogram: range maximum must exceed minimum"));
    range_ = {minimum, maximum};
    compute();
    return *this;
}

HistogramSeries& HistogramSeries::trim_percentile(double p) {
    if (!(p >= 0.0) || !(p < 50.0))
        throw Error(cworks::validation_failed("histogram: trim_percentile must be in [0, 50)"));
    trim_percentile_ = p;
    compute();
    return *this;
}

void HistogramSeries::compute() {
    edges_.clear();
    counts_.clear();
    std::vector<double> clean;
    clean.reserve(values_.size());
    for (double v : values_)
        if (std::isfinite(v)) clean.push_back(v);
    if (clean.empty()) return;

    // Resolve the covered range: an explicit range() always wins. Failing
    // that, trim_percentile() narrows it to the central (100 - 2p)% of the
    // sample (by rank) so a handful of extreme values don't dictate the
    // bin size -- those trimmed-for-sizing values are still binned below
    // via clamping, they just do not influence how wide a bin is.
    double lo, hi;
    if (range_) {
        lo = range_->first;
        hi = range_->second;
    } else {
        std::vector<double> sorted = clean;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t n = sorted.size();
        std::size_t trim = 0;
        if (trim_percentile_ > 0.0) {
            trim = static_cast<std::size_t>(
                std::floor(trim_percentile_ / 100.0 * static_cast<double>(n)));
            trim = std::min(trim, (n - 1) / 2); // never trim past the middle
        }
        lo = sorted[trim];
        hi = sorted[n - 1 - trim];
        if (lo == hi) {
            lo -= 0.5;
            hi += 0.5;
        }
    }

    // Resolve the bin count/width over [lo, hi]. bin_width() picks a fixed
    // width directly and expands the count to cover the range; otherwise
    // an explicit bins() (or bin_rule() applied to the full sample size)
    // picks a count and the width is an exact equal division of [lo, hi].
    // See the class comment for why edges are not rounded to "nice" values.
    int bin_count;
    double width;
    if (bin_width_ > 0.0) {
        width = bin_width_;
        bin_count = std::max(1, static_cast<int>(std::ceil((hi - lo) / width)));
    } else {
        bin_count = bins_ > 0 ? bins_ : histogram_bin_count(bin_rule_, clean.size());
        width = (hi - lo) / bin_count;
    }

    // Half-open bins [edge_i, edge_i+1) except the last, which is closed
    // ([edge_last-1, edge_last]) -- the maximum value lands in the last
    // bin instead of spilling into a phantom extra one. Values outside
    // [lo, hi] (an explicit range(), or the trimmed-for-sizing tails) are
    // clamped into the first/last bin rather than dropped.
    edges_.resize(static_cast<std::size_t>(bin_count) + 1);
    for (int i = 0; i <= bin_count; ++i) edges_[static_cast<std::size_t>(i)] = lo + i * width;
    counts_.assign(static_cast<std::size_t>(bin_count), 0.0);
    for (double v : clean) {
        auto idx = static_cast<long>((v - lo) / width);
        if (idx < 0) idx = 0;
        if (idx >= bin_count) idx = bin_count - 1; // right edge inclusive
        counts_[static_cast<std::size_t>(idx)] += 1.0;
    }
}

Extent HistogramSeries::extent() const {
    Extent e;
    if (edges_.empty()) return e;
    double max_count = 0.0;
    for (double c : counts_) max_count = std::max(max_count, c);
    e.include(edges_.front(), 0.0);
    e.include(edges_.back(), max_count);
    return e;
}

void HistogramSeries::build_geometry(const detail::GeomContext& ctx,
                                     std::vector<SceneItem>& out) const {
    if (edges_.empty()) return;
    ShapeStyle style;
    style.fill = ctx.color.with_alpha(0.85);
    style.stroke = ctx.theme.background;
    style.stroke_width = 0.8;
    const double base = ctx.y.map(0.0);
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        const double x0 = ctx.x.map(edges_[i]);
        const double x1 = ctx.x.map(edges_[i + 1]);
        const double top = ctx.y.map(counts_[i]);
        out.push_back(RectItem{{x0, std::min(top, base), x1 - x0, std::abs(base - top)}, style});
    }
}

// -- DensitySeries / ViolinSeries -----------------------------------------------

namespace {

struct Kde {
    std::vector<double> xs, ys;
    double max_y = 0.0;
    bool valid = false;
};

/// Gaussian kernel density estimate over an automatically padded grid.
Kde compute_kde(const std::vector<double>& raw, double bandwidth, int samples = 160) {
    Kde kde;
    std::vector<double> vals;
    for (double v : raw)
        if (std::isfinite(v)) vals.push_back(v);
    if (vals.size() < 2) return kde;

    double sum = 0;
    for (double v : vals) sum += v;
    const double mean = sum / static_cast<double>(vals.size());
    double ss = 0;
    for (double v : vals) ss += (v - mean) * (v - mean);
    const double sigma = std::sqrt(ss / static_cast<double>(vals.size() - 1));

    double h = bandwidth;
    if (h <= 0) {
        // Scott's rule; fall back to a data-range fraction for constant data.
        //
        // n^(-1/5) is written as a logarithm and an exponential rather than
        // as std::pow(n, -0.2). `pow` was the single worst offender when the
        // suite's libm dependence was measured — the one function whose last
        // bit reached rendered structure — and this bandwidth sets the grid
        // the whole density curve is sampled on, so every drawn point
        // inherits it. One deterministic pair of calls, once per estimate.
        h = 1.06 * sigma / cworks::exp(cworks::log(static_cast<double>(vals.size())) / 5.0);
        if (h <= 0) {
            const auto [mn, mx] = std::minmax_element(vals.begin(), vals.end());
            h = std::max((*mx - *mn) / 10.0, 1e-9);
        }
    }

    const auto [mn_it, mx_it] = std::minmax_element(vals.begin(), vals.end());
    const double lo = *mn_it - 3.0 * h;
    const double hi = *mx_it + 3.0 * h;
    const double step = (hi - lo) / (samples - 1);
    const double norm =
        1.0 / (static_cast<double>(vals.size()) * h * std::sqrt(2.0 * 3.14159265358979323846));

    // WHY THE KERNEL KEEPS libm, when the bandwidth above it does not.
    //
    // This is the one exponential in the file that runs per (grid sample x
    // observation) rather than once: 160 samples over n values, so a violin
    // of a thousand points evaluates it 160 000 times. Measured at -O2 on
    // exactly this loop, std::exp costs 0.43 ms there and cworks::exp
    // 13.9 ms — 32x, rising to 37x at n = 100 000 (40 ms against 1.49 s).
    //
    // What it would buy is nothing that can be seen. A kernel value is a
    // density, a density becomes a polygon width or a y coordinate, and both
    // are printed through the `%.2f` grid of SVG and PDF or quantized to one
    // byte in 256 by the rasterizer — ten to fourteen orders of magnitude of
    // margin over an ulp. That is measured, not assumed: perturbing every
    // exponential in the suite by one ulp (313 295 of them) moved no byte of
    // any rendered golden, and this path needs a RELATIVE error near 1e-6
    // before one does. For scale: libm and the deterministic kernel return
    // different bits on 0.16% of the arguments this loop produces, and the
    // difference is always the last one.
    //
    // The same trade as Scale::map (see scale.cpp), decided the same way and
    // reversed by the same thing: a consumer that puts a discrete decision
    // on a density — a contour level, an integer snap — or an output format
    // without a coordinate grid in front of it.
    kde.xs.resize(static_cast<std::size_t>(samples));
    kde.ys.resize(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const double x = lo + i * step;
        double density = 0;
        for (double v : vals) {
            const double u = (x - v) / h;
            density += std::exp(-0.5 * u * u);
        }
        density *= norm;
        kde.xs[static_cast<std::size_t>(i)] = x;
        kde.ys[static_cast<std::size_t>(i)] = density;
        kde.max_y = std::max(kde.max_y, density);
    }
    kde.valid = true;
    return kde;
}

} // namespace

DensitySeries::DensitySeries(std::span<const double> values, double bandwidth)
    : bandwidth_(bandwidth) {
    set_data(values);
}

DensitySeries& DensitySeries::set_data(std::span<const double> values) {
    values_.assign(values.begin(), values.end());
    return *this;
}

DensitySeries& DensitySeries::bandwidth(double h) {
    bandwidth_ = h;
    return *this;
}

DensitySeries& DensitySeries::fill(bool value) {
    fill_ = value;
    return *this;
}

Extent DensitySeries::extent() const {
    const Kde kde = compute_kde(values_, bandwidth_);
    Extent e;
    if (!kde.valid) return e;
    e.include(kde.xs.front(), 0.0);
    e.include(kde.xs.back(), kde.max_y);
    return e;
}

void DensitySeries::build_geometry(const detail::GeomContext& ctx,
                                   std::vector<SceneItem>& out) const {
    const Kde kde = compute_kde(values_, bandwidth_);
    if (!kde.valid) return;
    std::vector<Point> pts;
    pts.reserve(kde.xs.size());
    for (std::size_t i = 0; i < kde.xs.size(); ++i) {
        pts.push_back({ctx.x.map(kde.xs[i]), ctx.y.map(kde.ys[i])});
    }
    if (fill_) {
        std::vector<Point> poly = pts;
        const double base = ctx.y.map(0.0);
        poly.push_back({pts.back().x, base});
        poly.push_back({pts.front().x, base});
        ShapeStyle fill_style;
        fill_style.fill = ctx.color.with_alpha(0.25);
        out.push_back(PolygonItem{std::move(poly), fill_style});
    }
    ShapeStyle line_style;
    line_style.stroke = ctx.color;
    line_style.stroke_width = ctx.stroke_width;
    line_style.cap = LineCap::Round;
    line_style.join = LineJoin::Round;
    out.push_back(PolylineItem{std::move(pts), line_style});
}

ViolinSeries::ViolinSeries(std::vector<std::string> categories,
                           std::vector<std::vector<double>> data) {
    set_data(std::move(categories), std::move(data));
}

ViolinSeries& ViolinSeries::set_data(std::vector<std::string> categories,
                                     std::vector<std::vector<double>> data) {
    if (categories.size() != data.size()) {
        throw Error(cworks::validation_failed(
            "violin plot: categories and data must have the same length (" +
            std::to_string(categories.size()) + " vs " + std::to_string(data.size()) + ")"));
    }
    categories_ = std::move(categories);
    data_ = std::move(data);
    return *this;
}

ViolinSeries& ViolinSeries::bandwidth(double h) {
    bandwidth_ = h;
    return *this;
}

ViolinSeries& ViolinSeries::show_median(bool value) {
    show_median_ = value;
    return *this;
}

Extent ViolinSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < data_.size(); ++i) {
        const Kde kde = compute_kde(data_[i], bandwidth_);
        if (!kde.valid) continue;
        const double c = static_cast<double>(i) + 0.5;
        e.include(c, kde.xs.front()); // note: violin density runs along y
        e.include(c, kde.xs.back());
    }
    return e;
}

void ViolinSeries::build_geometry(const detail::GeomContext& ctx,
                                  std::vector<SceneItem>& out) const {
    const double band = ctx.x.band_width();
    const double half_width = band * 0.38;

    for (std::size_t i = 0; i < data_.size(); ++i) {
        const Kde kde = compute_kde(data_[i], bandwidth_);
        if (!kde.valid) continue;
        const double cx = ctx.x.map_category(i);
        // Mirrored outline: up the right side, down the left.
        std::vector<Point> poly;
        poly.reserve(kde.xs.size() * 2);
        for (std::size_t k = 0; k < kde.xs.size(); ++k) {
            const double w = kde.max_y > 0 ? kde.ys[k] / kde.max_y * half_width : 0.0;
            poly.push_back({cx + w, ctx.y.map(kde.xs[k])});
        }
        for (std::size_t k = kde.xs.size(); k-- > 0;) {
            const double w = kde.max_y > 0 ? kde.ys[k] / kde.max_y * half_width : 0.0;
            poly.push_back({cx - w, ctx.y.map(kde.xs[k])});
        }
        ShapeStyle style;
        style.fill = ctx.color.with_alpha(0.35);
        style.stroke = ctx.color;
        style.stroke_width = 1.1;
        out.push_back(PolygonItem{std::move(poly), style});

        if (show_median_) {
            std::vector<double> sorted;
            for (double v : data_[i])
                if (std::isfinite(v)) sorted.push_back(v);
            if (!sorted.empty()) {
                std::sort(sorted.begin(), sorted.end());
                const std::size_t n = sorted.size();
                const double median =
                    n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
                const double my = ctx.y.map(median);
                ShapeStyle med_style;
                med_style.stroke = ctx.color;
                med_style.stroke_width = 2.0;
                out.push_back(
                    LineItem{{cx - half_width * 0.5, my}, {cx + half_width * 0.5, my},
                             med_style});
            }
        }
    }
}

// -- StepSeries --------------------------------------------------------------------

StepSeries::StepSeries(std::span<const double> x, std::span<const double> y, StepMode mode)
    : mode_(mode) {
    set_data(x, y);
}

StepSeries& StepSeries::set_data(std::span<const double> x, std::span<const double> y) {
    check_sizes(x.size(), y.size(), "step series");
    x_.assign(x.begin(), x.end());
    y_.assign(y.begin(), y.end());
    return *this;
}

StepSeries& StepSeries::mode(StepMode m) {
    mode_ = m;
    return *this;
}

Extent StepSeries::extent() const { return xy_extent(x_, y_); }

void StepSeries::build_geometry(const detail::GeomContext& ctx,
                                std::vector<SceneItem>& out) const {
    ShapeStyle style;
    style.stroke = ctx.color;
    style.stroke_width = ctx.stroke_width;

    for (auto& run : resolve_runs(x_, y_, missing_, ctx)) {
        if (run.size() < 2) continue;
        out.push_back(PolylineItem{expand_steps(run, mode_), style});
    }
}

// -- HeatmapSeries -------------------------------------------------------------

HeatmapSeries::HeatmapSeries(std::vector<std::string> x_labels,
                             std::vector<std::string> y_labels,
                             std::span<const double> values, std::size_t rows,
                             std::size_t cols) {
    set_data(std::move(x_labels), std::move(y_labels), values, rows, cols);
}

HeatmapSeries& HeatmapSeries::set_data(std::vector<std::string> x_labels,
                                       std::vector<std::string> y_labels,
                                       std::span<const double> values, std::size_t rows,
                                       std::size_t cols) {
    if (rows == 0 || cols == 0) throw Error(cworks::validation_failed(
        "heatmap: rows and cols must be positive"));
    if (x_labels.size() != cols) {
        throw Error(cworks::validation_failed("heatmap: x_labels size must match cols (" +
            std::to_string(x_labels.size()) + " vs " + std::to_string(cols) + ")"));
    }
    if (y_labels.size() != rows) {
        throw Error(cworks::validation_failed("heatmap: y_labels size must match rows (" +
            std::to_string(y_labels.size()) + " vs " + std::to_string(rows) + ")"));
    }
    if (values.size() != rows * cols) {
        throw Error(cworks::validation_failed("heatmap: values must contain rows * cols entries (" +
            std::to_string(values.size()) + " vs " + std::to_string(rows * cols) + ")"));
    }
    x_labels_ = std::move(x_labels);
    y_labels_ = std::move(y_labels);
    values_.assign(values.begin(), values.end());
    rows_ = rows;
    cols_ = cols;
    return *this;
}

HeatmapSeries& HeatmapSeries::colormap(Colormap map) {
    color_scale_.colormap = map;
    return *this;
}

HeatmapSeries& HeatmapSeries::color_range(double minimum, double maximum) {
    color_scale_.minimum = minimum;
    color_scale_.maximum = maximum;
    return *this;
}

HeatmapSeries& HeatmapSeries::color_midpoint(double midpoint) {
    color_scale_.midpoint = midpoint;
    return *this;
}

HeatmapSeries& HeatmapSeries::reverse_colormap(bool value) {
    color_scale_.reverse = value;
    return *this;
}

HeatmapSeries& HeatmapSeries::missing_color(Color color) {
    color_scale_.missing = color;
    return *this;
}

HeatmapSeries& HeatmapSeries::cell_labels(bool value) {
    cell_labels_ = value;
    return *this;
}

HeatmapSeries& HeatmapSeries::value_format(std::string format) {
    value_format_ = std::move(format);
    return *this;
}

HeatmapSeries& HeatmapSeries::colorbar(bool value) {
    colorbar_ = value;
    return *this;
}

HeatmapSeries& HeatmapSeries::square_cells(bool value) {
    square_cells_ = value;
    return *this;
}

HeatmapSeries& HeatmapSeries::normalize(MatrixNormalize mode) {
    normalize_ = mode;
    return *this;
}

std::vector<double> HeatmapSeries::normalized_values() const {
    std::vector<double> out = values_;
    if (normalize_ == MatrixNormalize::None) return out;
    if (normalize_ == MatrixNormalize::All) {
        double total = 0.0;
        for (double v : out)
            if (std::isfinite(v)) total += v;
        if (total != 0.0) {
            for (double& v : out)
                if (std::isfinite(v)) v /= total;
        }
        return out;
    }
    if (normalize_ == MatrixNormalize::Rows) {
        for (std::size_t r = 0; r < rows_; ++r) {
            double total = 0.0;
            for (std::size_t c = 0; c < cols_; ++c) {
                const double v = out[r * cols_ + c];
                if (std::isfinite(v)) total += v;
            }
            if (total != 0.0) {
                for (std::size_t c = 0; c < cols_; ++c) {
                    double& v = out[r * cols_ + c];
                    if (std::isfinite(v)) v /= total;
                }
            }
        }
        return out;
    }
    for (std::size_t c = 0; c < cols_; ++c) {
        double total = 0.0;
        for (std::size_t r = 0; r < rows_; ++r) {
            const double v = out[r * cols_ + c];
            if (std::isfinite(v)) total += v;
        }
        if (total != 0.0) {
            for (std::size_t r = 0; r < rows_; ++r) {
                double& v = out[r * cols_ + c];
                if (std::isfinite(v)) v /= total;
            }
        }
    }
    return out;
}

std::pair<double, double> HeatmapSeries::finite_value_range() const {
    const std::vector<double> vals = normalized_values();
    double lo = 0.0, hi = 1.0;
    bool valid = false;
    for (double v : vals) {
        if (!std::isfinite(v)) continue;
        if (!valid) {
            lo = hi = v;
            valid = true;
        } else {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    if (!valid) return {0.0, 1.0};
    if (lo == hi) {
        lo -= 0.5;
        hi += 0.5;
    }
    return {lo, hi};
}

Extent HeatmapSeries::extent() const {
    Extent e;
    if (rows_ == 0 || cols_ == 0) return e;
    e.include(0.0, 0.0);
    e.include(static_cast<double>(cols_), static_cast<double>(rows_));
    return e;
}

void HeatmapSeries::build_geometry(const detail::GeomContext& ctx,
                                   std::vector<SceneItem>& out) const {
    const std::vector<double> vals = normalized_values();
    const auto [vmin, vmax] = finite_value_range();
    const double bw = ctx.x.band_width();
    const double bh = ctx.y.band_width();
    const double cell = square_cells_ ? std::min(bw, bh) : 0.0;

    ShapeStyle stroke;
    stroke.stroke = ctx.theme.background;
    stroke.stroke_width = 0.7;

    for (std::size_t r = 0; r < rows_; ++r) {
        for (std::size_t c = 0; c < cols_; ++c) {
            const double value = vals[r * cols_ + c];
            const double cx = ctx.x.map_category(c);
            const double cy = ctx.y.map_category(r);
            const double w = square_cells_ ? cell : bw;
            const double h = square_cells_ ? cell : bh;
            ShapeStyle style = stroke;
            style.fill = color_scale_.color(value, vmin, vmax);
            out.push_back(RectItem{{cx - w / 2.0, cy - h / 2.0, w, h}, style});

            if (cell_labels_ && std::isfinite(value)) {
                TextItem label;
                label.pos = {cx, cy};
                label.text = format_number(value, value_format_);
                label.font = ctx.theme.tick_font();
                label.color = contrast_text_color(*style.fill, ctx.theme);
                label.halign = HAlign::Center;
                label.valign = VAlign::Middle;
                out.push_back(std::move(label));
            }
        }
    }
}

// -- StripSeries ---------------------------------------------------------------

StripSeries::StripSeries(std::vector<std::string> categories, std::span<const double> values) {
    set_data(std::move(categories), values);
}

StripSeries& StripSeries::set_data(std::vector<std::string> categories,
                                   std::span<const double> values) {
    if (categories.size() != values.size()) {
        throw Error(cworks::validation_failed(
            "strip plot: categories and values must have the same length (" +
            std::to_string(categories.size()) + " vs " + std::to_string(values.size()) + ")"));
    }
    categories_ = std::move(categories);
    values_.assign(values.begin(), values.end());
    order_.clear();
    indices_.clear();
    std::map<std::string, std::size_t> pos;
    for (const auto& category : categories_) {
        auto [it, inserted] = pos.try_emplace(category, order_.size());
        if (inserted) order_.push_back(category);
        indices_.push_back(it->second);
    }
    missing_ = MissingPolicy::Drop;
    return *this;
}

StripSeries& StripSeries::marker_size(double size) {
    marker_size_ = size;
    return *this;
}

StripSeries& StripSeries::jitter(double width) {
    jitter_ = std::max(0.0, width);
    return *this;
}

StripSeries& StripSeries::alpha(double alpha) {
    alpha_ = std::clamp(alpha, 0.0, 1.0);
    return *this;
}

Extent StripSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < values_.size() && i < indices_.size(); ++i) {
        if (is_missing(values_[i])) continue;
        e.include(static_cast<double>(indices_[i]) + 0.5, values_[i]);
    }
    return e;
}

void StripSeries::build_geometry(const detail::GeomContext& ctx,
                                 std::vector<SceneItem>& out) const {
    const double band = ctx.x.band_width();
    const Color c = ctx.color.with_alpha(alpha_);
    for (std::size_t i = 0; i < values_.size() && i < indices_.size(); ++i) {
        if (is_missing(values_[i])) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed(
                    "strip series contains missing value at index " + std::to_string(i)));
            detail::reject_unsupported_interpolate(missing_, "strip");
            continue;
        }
        const std::uint32_t h =
            static_cast<std::uint32_t>((i + 1) * 2654435761u + indices_[i] * 374761393u);
        const double unit = static_cast<double>(h % 10000u) / 9999.0;
        const double offset = (unit - 0.5) * 2.0 * jitter_ * band;
        const Point p{ctx.x.map_category(indices_[i]) + offset, ctx.y.map(values_[i])};
        emit_marker(out, Marker::Circle, p, marker_size_, c, 1.0);
    }
}

// -- PieSeries -----------------------------------------------------------------

PieSeries::PieSeries(std::vector<std::string> labels, std::span<const double> values) {
    set_data(std::move(labels), values);
}

PieSeries& PieSeries::set_data(std::vector<std::string> labels,
                               std::span<const double> values) {
    if (labels.size() != values.size()) {
        throw Error(cworks::validation_failed(
            "pie series: labels and values must have the same length (" +
            std::to_string(labels.size()) + " vs " + std::to_string(values.size()) + ")"));
    }
    if (values.empty()) throw Error(cworks::validation_failed(
        "pie series: needs at least one value"));
    double total = 0.0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i]) || values[i] < 0.0) {
            throw Error(cworks::validation_failed(
                "pie series: values must be finite and non-negative (value at index " +
                std::to_string(i) + " is invalid)"));
        }
        total += values[i];
    }
    if (!(total > 0.0)) throw Error(cworks::validation_failed(
        "pie series: total of values must be positive"));
    labels_ = std::move(labels);
    values_.assign(values.begin(), values.end());
    return *this;
}

PieSeries& PieSeries::slice_labels(PieLabelMode mode) {
    label_mode_ = mode;
    return *this;
}

PieSeries& PieSeries::start_angle(double degrees) {
    start_angle_ = degrees;
    return *this;
}

PieSeries& PieSeries::clockwise(bool value) {
    clockwise_ = value;
    return *this;
}

PieSeries& PieSeries::inner_radius(double ratio) {
    if (!(ratio >= 0.0) || ratio >= 1.0) {
        throw Error(cworks::validation_failed("pie series: inner_radius must be in [0, 1)"));
    }
    inner_ = ratio;
    return *this;
}

PieSeries& PieSeries::explode(std::size_t index, double ratio) {
    if (index >= values_.size()) {
        throw Error(cworks::validation_failed("pie series: explode index " + std::to_string(index) +
            " out of range (have " + std::to_string(values_.size()) + " slices)"));
    }
    explode_.emplace_back(index, std::max(0.0, ratio));
    return *this;
}

PieSeries& PieSeries::other_threshold(double fraction) {
    if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) {
        throw Error(cworks::validation_failed("pie series: other_threshold must be a fraction in [0, 1]"));
    }
    other_threshold_ = fraction;
    return *this;
}

PieSeries& PieSeries::other_label(std::string text) {
    other_label_ = std::move(text);
    return *this;
}

PieSeries& PieSeries::other_color(Color color) {
    other_color_ = color;
    return *this;
}

Extent PieSeries::extent() const { return {}; } // non-Cartesian: no data extent

std::vector<PieSeries::PieSlice> PieSeries::resolved_slices() const {
    // Fold every slice whose share of the total is strictly below
    // other_threshold_ into one trailing "Other" slice, in input order.
    // A single pass keeps this deterministic and stable: ties (a share
    // exactly at the threshold) are NOT folded, and every folded slice
    // merges into the SAME "Other" entry rather than one bucket each.
    double total = 0.0;
    for (double v : values_) total += v;

    std::vector<PieSlice> slices;
    slices.reserve(values_.size());
    PieSlice other;
    other.label = other_label_;
    for (std::size_t i = 0; i < values_.size(); ++i) {
        const double share = total > 0.0 ? values_[i] / total : 0.0;
        if (other_threshold_ > 0.0 && share < other_threshold_) {
            other.value += values_[i];
            other.source_indices.push_back(i);
            continue;
        }
        slices.push_back(PieSlice{labels_[i], values_[i], false, {i}});
    }
    if (!other.source_indices.empty()) {
        other.is_other = true;
        slices.push_back(std::move(other));
    }
    return slices;
}

std::vector<LegendItemInfo> PieSeries::legend_items(Color resolved_color,
                                                    const Theme& theme) const {
    (void)resolved_color;
    const std::vector<PieSlice> slices = resolved_slices();
    std::vector<LegendItemInfo> items;
    items.reserve(slices.size());
    for (std::size_t i = 0; i < slices.size(); ++i) {
        LegendItemInfo item;
        item.label = slices[i].label;
        item.color = slices[i].is_other ? other_color_.value_or(theme.muted_text_color)
                                        : theme.series_color(i);
        item.filled = true;
        item.square = true;
        items.push_back(std::move(item));
    }
    return items;
}

std::vector<PieSeries::DrawnSlice> PieSeries::drawn_slices(Point center, double r_outer,
                                                          const Theme& theme) const {
    // The single authoritative slice walk. build_geometry() (sectors + inside
    // labels) and slice_callouts() (outside callouts) both consume this so
    // their angles, centers, and colors are provably identical -- the key
    // determinism guard for the labeled legend's leader anchors.
    const std::vector<PieSlice> slices = resolved_slices();
    double total = 0.0;
    for (const PieSlice& slice : slices) total += slice.value;

    std::vector<DrawnSlice> drawn;
    if (!(total > 0.0)) return drawn;
    drawn.reserve(slices.size());

    double angle = start_angle_;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const PieSlice& slice = slices[i];
        const double fraction = slice.value / total;
        const double span = 360.0 * fraction * (clockwise_ ? 1.0 : -1.0);
        if (slice.value <= 0.0) continue;
        const Color fill = slice.is_other ? other_color_.value_or(theme.muted_text_color)
                                          : theme.series_color(i);
        // Kept in degrees, the unit the slice was cut in: the sector, the
        // explode offset, the label anchor and the callout ray must all
        // agree on this direction to the last bit, and a conversion to
        // radians would be one rounding they no longer share.
        const double mid = angle + span / 2.0;

        Point slice_center = center;
        // A folded "Other" slice has no single pre-fold index of its own,
        // so explode() (which names an original slice) never applies to it.
        if (!slice.is_other) {
            const std::size_t original_index = slice.source_indices.front();
            double sine = 0.0;
            double cosine = 0.0;
            cworks::sincos_deg(mid, sine, cosine);
            for (const auto& [index, ratio] : explode_) {
                if (index != original_index) continue;
                slice_center.x += ratio * r_outer * sine;
                slice_center.y -= ratio * r_outer * cosine;
            }
        }

        drawn.push_back(DrawnSlice{slice.label, slice.value, fraction, slice.is_other,
                                   fill, slice_center, angle, span, mid});
        angle += span;
    }
    return drawn;
}

std::string PieSeries::callout_text_for(const PieSlice& slice, double fraction) const {
    // The labeled legend REPLACES the swatch legend, so every callout carries
    // the category name; the labels mode only chooses an optional trailing
    // metric. Reuses the exact formatters build_geometry uses inside slices.
    switch (label_mode_) {
    case PieLabelMode::Percent:
    case PieLabelMode::LabelPercent:
        return slice.label + " " + format_percent(fraction);
    case PieLabelMode::Value:
        return slice.label + " " + detail::format_tick_value(slice.value, 0.0);
    case PieLabelMode::None:
    case PieLabelMode::Label:
        return slice.label;
    }
    return slice.label;
}

std::vector<PieSeries::SliceCallout> PieSeries::slice_callouts(Point center, double r_outer,
                                                              const Theme& theme) const {
    std::vector<SliceCallout> callouts;
    for (const DrawnSlice& d : drawn_slices(center, r_outer, theme)) {
        SliceCallout c;
        double sine = 0.0;
        double cosine = 0.0;
        cworks::sincos_deg(d.mid_deg, sine, cosine);
        c.dx = sine;
        c.dy = -cosine;
        c.anchor = {d.center.x + r_outer * c.dx, d.center.y + r_outer * c.dy};
        c.color = d.color;
        c.text = callout_text_for(PieSlice{d.label, d.value, d.is_other, {}}, d.fraction);
        callouts.push_back(std::move(c));
    }
    return callouts;
}

std::vector<std::string> PieSeries::callout_texts() const {
    // Geometry-independent, so the outside-space reservation can be sized
    // before the radius is known. Uses the same total/skip rule as the walk.
    const std::vector<PieSlice> slices = resolved_slices();
    double total = 0.0;
    for (const PieSlice& slice : slices) total += slice.value;
    std::vector<std::string> texts;
    if (!(total > 0.0)) return texts;
    for (const PieSlice& slice : slices) {
        if (slice.value <= 0.0) continue;
        texts.push_back(callout_text_for(slice, slice.value / total));
    }
    return texts;
}

double PieSeries::min_slice_angle_deg() const {
    const std::vector<PieSlice> slices = resolved_slices();
    double total = 0.0;
    for (const PieSlice& slice : slices) total += slice.value;
    if (!(total > 0.0)) return 0.0;
    double min_span = 0.0;
    bool any = false;
    for (const PieSlice& slice : slices) {
        if (slice.value <= 0.0) continue;
        const double span = 360.0 * (slice.value / total);
        if (!any || span < min_span) {
            min_span = span;
            any = true;
        }
    }
    return any ? min_span : 0.0;
}

void PieSeries::build_geometry(const detail::GeomContext& ctx,
                               std::vector<SceneItem>& out) const {
    if (ctx.polar_radius <= 0.0) return;
    const double r_outer = ctx.polar_radius;
    const double r_inner = inner_ * r_outer;

    for (const DrawnSlice& d : drawn_slices(ctx.polar_center, r_outer, ctx.theme)) {
        SectorItem sector;
        sector.center = d.center;
        sector.radius_inner = r_inner;
        sector.radius_outer = r_outer;
        sector.start_angle = std::min(d.start_angle, d.start_angle + d.span);
        sector.end_angle = std::max(d.start_angle, d.start_angle + d.span);
        sector.style.fill = d.color;
        sector.style.stroke = ctx.theme.background;
        sector.style.stroke_width = 1.5;
        out.push_back(sector);

        // Inside-slice labels are suppressed when the panel draws outside
        // callouts (legend: labeled); layout owns those instead.
        if (label_mode_ != PieLabelMode::None && !ctx.pie_callout_labels) {
            std::string text;
            switch (label_mode_) {
            case PieLabelMode::Label: text = d.label; break;
            case PieLabelMode::Percent: text = format_percent(d.fraction); break;
            case PieLabelMode::Value: text = detail::format_tick_value(d.value, 0.0); break;
            case PieLabelMode::LabelPercent:
                text = d.label + " " + format_percent(d.fraction);
                break;
            case PieLabelMode::None: break;
            }
            const double r_label =
                r_inner > 0.0 ? (r_inner + r_outer) / 2.0 : r_outer * 0.62;
            double sine = 0.0;
            double cosine = 0.0;
            cworks::sincos_deg(d.mid_deg, sine, cosine);
            TextItem label;
            label.pos = {d.center.x + r_label * sine, d.center.y - r_label * cosine};
            label.text = std::move(text);
            label.font = ctx.theme.tick_font();
            label.color = contrast_text_color(d.color, ctx.theme);
            label.halign = HAlign::Center;
            label.valign = VAlign::Middle;
            out.push_back(std::move(label));
        }
    }
}

// -- BubbleSeries ----------------------------------------------------------------

BubbleSeries::BubbleSeries(std::span<const double> x, std::span<const double> y,
                           std::span<const double> size) {
    set_data(x, y, size);
    missing_ = MissingPolicy::Drop;
}

BubbleSeries& BubbleSeries::set_data(std::span<const double> x, std::span<const double> y,
                                     std::span<const double> size) {
    check_sizes(x.size(), y.size(), "bubble series");
    if (size.size() != x.size()) {
        throw Error(cworks::validation_failed("bubble series: size must match x/y length (" +
            std::to_string(size.size()) + " vs " + std::to_string(x.size()) + ")"));
    }
    for (std::size_t i = 0; i < size.size(); ++i) {
        if (std::isfinite(size[i]) && size[i] < 0.0) {
            throw Error(cworks::validation_failed(
                "bubble series: sizes must be non-negative (value at index " + std::to_string(i) +
                " is negative)"));
        }
    }
    x_.assign(x.begin(), x.end());
    y_.assign(y.begin(), y.end());
    size_.assign(size.begin(), size.end());
    return *this;
}

BubbleSeries& BubbleSeries::radius_range(double min_px, double max_px) {
    if (!(min_px >= 0.0) || !(max_px >= min_px)) {
        throw Error(cworks::validation_failed("bubble series: radius_range needs 0 <= min <= max"));
    }
    r_min_ = min_px;
    r_max_ = max_px;
    return *this;
}

BubbleSeries& BubbleSeries::alpha(double value) {
    alpha_ = std::clamp(value, 0.0, 1.0);
    return *this;
}

BubbleSeries& BubbleSeries::size_scale(BubbleScale scale) {
    scale_ = scale;
    return *this;
}

BubbleSeries& BubbleSeries::color_by(std::span<const double> values) {
    if (!x_.empty() && values.size() != x_.size())
        throw Error(cworks::validation_failed("bubble color_by: values must match x/y length (" +
            std::to_string(values.size()) + " vs " + std::to_string(x_.size()) + ")"));
    color_.values.assign(values.begin(), values.end());
    return *this;
}

BubbleSeries& BubbleSeries::colormap(Colormap map) {
    color_.scale.colormap = map;
    return *this;
}

BubbleSeries& BubbleSeries::color_range(double minimum, double maximum) {
    color_.scale.minimum = minimum;
    color_.scale.maximum = maximum;
    return *this;
}

BubbleSeries& BubbleSeries::color_midpoint(double midpoint) {
    color_.scale.midpoint = midpoint;
    return *this;
}

BubbleSeries& BubbleSeries::reverse_colormap(bool value) {
    color_.scale.reverse = value;
    return *this;
}

BubbleSeries& BubbleSeries::missing_color(Color color) {
    color_.scale.missing = color;
    return *this;
}

BubbleSeries& BubbleSeries::colorbar(bool value) {
    color_.colorbar = value;
    return *this;
}

BubbleSeries& BubbleSeries::point_labels(bool value) {
    point_labels_ = value;
    if (!value) point_label_names_.clear();
    return *this;
}

BubbleSeries& BubbleSeries::point_labels(std::vector<std::string> names) {
    if (!x_.empty() && names.size() != x_.size()) {
        throw Error(cworks::validation_failed("bubble series: point_labels names must match the series length (" +
                                              std::to_string(names.size()) + " vs " + std::to_string(x_.size()) + ")"));
    }
    point_label_names_ = std::move(names);
    point_labels_ = true;
    return *this;
}

double BubbleSeries::radius_for(double size) const {
    double max_size = 0.0;
    for (double s : size_) {
        if (std::isfinite(s)) max_size = std::max(max_size, s);
    }
    if (!(max_size > 0.0)) return r_min_;
    const double t = scale_ == BubbleScale::Area
                         ? std::sqrt(std::max(0.0, size) / max_size)
                         : std::max(0.0, size) / max_size;
    return r_min_ + (r_max_ - r_min_) * t;
}

Extent BubbleSeries::extent() const {
    Extent e = xy_extent(x_, y_);
    // Pad so that large edge bubbles are not clipped by the plot area.
    if (e.valid) {
        const double px = (e.x_hi - e.x_lo) * 0.06;
        const double py = (e.y_hi - e.y_lo) * 0.06;
        e.x_lo -= px;
        e.x_hi += px;
        e.y_lo -= py;
        e.y_hi += py;
    }
    return e;
}

void BubbleSeries::build_geometry(const detail::GeomContext& ctx,
                                  std::vector<SceneItem>& out) const {
    const bool by_value = color_.active();
    const auto [vmin, vmax] = by_value ? color_.value_range() : std::pair<double, double>{0.0, 1.0};
    for (std::size_t i = 0; i < x_.size(); ++i) {
        if (is_missing(x_[i]) || is_missing(y_[i]) || is_missing(size_[i])) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed(
                    "bubble series contains missing value at index " + std::to_string(i)));
            detail::reject_unsupported_interpolate(missing_, "bubble");
            continue;
        }
        const Color base = by_value ? color_.scale.color(color_.values[i], vmin, vmax) : ctx.color;
        ShapeStyle style;
        style.fill = base.with_alpha(alpha_);
        style.stroke = base;
        style.stroke_width = 1.0;
        const Point center{ctx.x.map(x_[i]), ctx.y.map(y_[i])};
        out.push_back(CircleItem{center, radius_for(size_[i]), style});
        if (point_labels_) {
            std::string text = point_label_names_.empty()
                                   ? detail::format_tick_value(y_[i], 0.0)
                                   : (i < point_label_names_.size() ? point_label_names_[i]
                                                                    : std::string{});
            // Contrast against the bubble's own (pre-alpha) fill colour --
            // the resolved marker colour, not the theme's series colour --
            // so the label stays legible whether color_by lightened or
            // darkened this particular bubble.
            emit_bubble_label(out, center, std::move(text), base, ctx.theme);
        }
    }
}

// -- OhlcSeries ----------------------------------------------------------------

OhlcSeries::OhlcSeries(std::span<const double> x, std::span<const double> high,
                       std::span<const double> low, std::span<const double> close) {
    x_.assign(x.begin(), x.end());
    high_.assign(high.begin(), high.end());
    low_.assign(low.begin(), low.end());
    close_.assign(close.begin(), close.end());
    validate();
}

OhlcSeries::OhlcSeries(std::span<const double> x, std::span<const double> open,
                       std::span<const double> high, std::span<const double> low,
                       std::span<const double> close, bool candles)
    : candles_(candles) {
    x_.assign(x.begin(), x.end());
    open_.assign(open.begin(), open.end());
    high_.assign(high.begin(), high.end());
    low_.assign(low.begin(), low.end());
    close_.assign(close.begin(), close.end());
    validate();
}

void OhlcSeries::validate() const {
    const char* what = candles_ ? "candlestick series" : "ohlc series";
    const std::size_t n = x_.size();
    if (high_.size() != n || low_.size() != n || close_.size() != n ||
        (!open_.empty() && open_.size() != n)) {
        throw Error(cworks::validation_failed(std::string(what) +
            ": all columns must have the same length"));
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isfinite(low_[i]) && std::isfinite(high_[i]) && low_[i] > high_[i]) {
            throw Error(cworks::validation_failed(std::string(what) + ": low > high at index " +
                std::to_string(i) + " (" + std::to_string(low_[i]) + " > " +
                std::to_string(high_[i]) + ")"));
        }
        if (candles_ && std::isfinite(x_[i]) &&
            (!std::isfinite(open_[i]) || !std::isfinite(close_[i]))) {
            throw Error(cworks::validation_failed(std::string(what) +
                ": open and close must be finite (index " + std::to_string(i) + ")"));
        }
    }
}

OhlcSeries& OhlcSeries::up_color(Color color) {
    up_color_ = color;
    return *this;
}

OhlcSeries& OhlcSeries::down_color(Color color) {
    down_color_ = color;
    return *this;
}

OhlcSeries& OhlcSeries::body_width(double px) {
    body_width_ = std::max(0.0, px);
    return *this;
}

OhlcSeries& OhlcSeries::wick_width(double px) {
    wick_width_ = std::max(0.0, px);
    return *this;
}

Extent OhlcSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < x_.size(); ++i) {
        if (is_missing(x_[i])) continue;
        if (!is_missing(low_[i])) e.include(x_[i], low_[i]);
        if (!is_missing(high_[i])) e.include(x_[i], high_[i]);
        if (!is_missing(close_[i])) e.include(x_[i], close_[i]);
        if (!open_.empty() && !is_missing(open_[i])) e.include(x_[i], open_[i]);
    }
    // Pad x by half the smallest spacing so edge bodies are not clipped.
    if (e.valid) {
        std::vector<double> xs;
        for (double v : x_)
            if (!is_missing(v)) xs.push_back(v);
        std::sort(xs.begin(), xs.end());
        double min_gap = 0.0;
        for (std::size_t i = 1; i < xs.size(); ++i) {
            const double gap = xs[i] - xs[i - 1];
            if (gap > 0.0 && (min_gap == 0.0 || gap < min_gap)) min_gap = gap;
        }
        const double pad = min_gap > 0.0 ? min_gap * 0.6 : 0.5;
        e.x_lo -= pad;
        e.x_hi += pad;
    }
    return e;
}

std::vector<LegendItemInfo> OhlcSeries::legend_items(Color resolved_color,
                                                     const Theme& theme) const {
    (void)theme;
    if (label_.empty()) return {};
    LegendItemInfo item;
    item.label = label_;
    item.color = explicit_color() ? resolved_color : up_color_.value_or(kUpColor);
    item.filled = true;
    item.square = true;
    return {item};
}

void OhlcSeries::build_geometry(const detail::GeomContext& ctx,
                                std::vector<SceneItem>& out) const {
    const Color up = up_color_.value_or(explicit_color() ? ctx.color : kUpColor);
    const Color down = down_color_.value_or(kDownColor);

    // Automatic body width: 70% of the smallest x spacing, clamped.
    double body_w = body_width_;
    if (body_w <= 0.0) {
        std::vector<double> px;
        px.reserve(x_.size());
        for (double v : x_) {
            if (!is_missing(v)) px.push_back(ctx.x.map(v));
        }
        std::sort(px.begin(), px.end());
        double min_gap = 0.0;
        for (std::size_t i = 1; i < px.size(); ++i) {
            const double gap = px[i] - px[i - 1];
            if (gap > 0.0 && (min_gap == 0.0 || gap < min_gap)) min_gap = gap;
        }
        body_w = min_gap > 0.0 ? std::clamp(min_gap * 0.7, 2.0, 24.0) : 8.0;
    }
    const double wick_w =
        wick_width_ > 0.0 ? wick_width_ : std::max(1.0, ctx.stroke_width * 0.75);

    double prev_close = std::numeric_limits<double>::quiet_NaN();
    for (std::size_t i = 0; i < x_.size(); ++i) {
        if (is_missing(x_[i]) || is_missing(high_[i]) || is_missing(low_[i]) ||
            is_missing(close_[i])) {
            if (missing_ == MissingPolicy::Error)
                throw Error(cworks::validation_failed(
                    "ohlc/candlestick series contains missing value at index " +
                    std::to_string(i)));
            detail::reject_unsupported_interpolate(missing_, "ohlc/candlestick");
            continue;
        }
        const bool rising = !open_.empty() ? close_[i] >= open_[i]
                            : std::isfinite(prev_close) ? close_[i] >= prev_close
                                                        : true;
        prev_close = close_[i];
        const Color color = rising ? up : down;

        const double px = ctx.x.map(x_[i]);
        const double py_hi = ctx.y.map(high_[i]);
        const double py_lo = ctx.y.map(low_[i]);

        ShapeStyle wick_style;
        wick_style.stroke = color;
        wick_style.stroke_width = wick_w;

        if (candles_) {
            const double py_open = ctx.y.map(open_[i]);
            const double py_close = ctx.y.map(close_[i]);
            const double body_top = std::min(py_open, py_close);
            const double body_bot = std::max(py_open, py_close);
            // Wicks above and below the body.
            if (py_hi < body_top)
                out.push_back(LineItem{{px, py_hi}, {px, body_top}, wick_style});
            if (py_lo > body_bot)
                out.push_back(LineItem{{px, body_bot}, {px, py_lo}, wick_style});
            ShapeStyle body_style;
            body_style.fill = color;
            body_style.stroke = color;
            body_style.stroke_width = 1.0;
            // Flat (doji) bodies keep at least one pixel of height.
            const double h = std::max(1.0, body_bot - body_top);
            out.push_back(RectItem{{px - body_w / 2.0, body_top, body_w, h}, body_style});
        } else {
            out.push_back(LineItem{{px, py_hi}, {px, py_lo}, wick_style});
            if (!open_.empty() && !is_missing(open_[i])) {
                const double py_open = ctx.y.map(open_[i]);
                out.push_back(
                    LineItem{{px - body_w / 2.0, py_open}, {px, py_open}, wick_style});
            }
            const double py_close = ctx.y.map(close_[i]);
            out.push_back(
                LineItem{{px, py_close}, {px + body_w / 2.0, py_close}, wick_style});
        }
    }
}

// -- RadarSeries ---------------------------------------------------------------

RadarSeries::RadarSeries(std::vector<std::string> categories,
                         std::span<const double> values) {
    set_data(std::move(categories), values);
}

RadarSeries& RadarSeries::set_data(std::vector<std::string> categories,
                                   std::span<const double> values) {
    if (categories.size() != values.size()) {
        throw Error(cworks::validation_failed(
            "radar series: categories and values must have the same length (" +
            std::to_string(categories.size()) + " vs " + std::to_string(values.size()) + ")"));
    }
    if (categories.size() < 3) {
        throw Error(cworks::validation_failed("radar series: needs at least three categories (got "
            + std::to_string(categories.size()) + ")"));
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i])) {
            throw Error(cworks::validation_failed(
                "radar series: values must be finite (value at index " + std::to_string(i) +
                " is missing)"));
        }
    }
    categories_ = std::move(categories);
    values_.assign(values.begin(), values.end());
    return *this;
}

RadarSeries& RadarSeries::filled(bool value) {
    filled_ = value;
    return *this;
}

RadarSeries& RadarSeries::markers(bool value) {
    markers_ = value;
    return *this;
}

RadarSeries& RadarSeries::range(double minimum, double maximum) {
    if (!(minimum < maximum)) throw Error(cworks::validation_failed(
        "radar series: range needs min < max"));
    range_ = {minimum, maximum};
    return *this;
}

Extent RadarSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < values_.size(); ++i) e.include(0.0, values_[i]);
    return e;
}

void RadarSeries::build_geometry(const detail::GeomContext& ctx,
                                 std::vector<SceneItem>& out) const {
    // ctx.y is the radial scale mapped to [0, polar_radius] pixels.
    const std::size_t n = categories_.size();
    if (n < 3 || ctx.polar_radius <= 0.0) return;

    std::vector<Point> pts;
    pts.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // 360/n degrees per spoke, matching build_radar_panel's grid
        // exactly — the series polygon has to sit on the spokes the panel
        // drew, and the only way to guarantee that is to compute the
        // direction the same way from the same units.
        double sine = 0.0;
        double cosine = 0.0;
        cworks::sincos_deg(360.0 * static_cast<double>(i) / static_cast<double>(n), sine,
                           cosine);
        const double r = std::clamp(ctx.y.map(values_[i]), 0.0, ctx.polar_radius);
        pts.push_back({ctx.polar_center.x + r * sine, ctx.polar_center.y - r * cosine});
    }

    if (filled_) {
        ShapeStyle fill_style;
        fill_style.fill = ctx.color.with_alpha(0.20);
        out.push_back(PolygonItem{pts, fill_style});
    }
    ShapeStyle line_style;
    line_style.stroke = ctx.color;
    line_style.stroke_width = ctx.stroke_width;
    line_style.cap = LineCap::Round;
    line_style.join = LineJoin::Round;
    // The outline is a loop, so it is drawn as one: a polyline that merely
    // repeats its first point meets itself with two caps at that spoke,
    // where a closed path carries the join the other vertices have.
    out.push_back(PolygonItem{pts, line_style});

    if (markers_) {
        ShapeStyle marker_style;
        marker_style.fill = ctx.color;
        for (const Point& p : pts) out.push_back(CircleItem{p, 3.0, marker_style});
    }
}

// -- WaterfallSeries -------------------------------------------------------------

WaterfallSeries::WaterfallSeries(std::vector<std::string> categories,
                                 std::span<const double> values) {
    set_data(std::move(categories), values);
}

WaterfallSeries& WaterfallSeries::set_data(std::vector<std::string> categories,
                                           std::span<const double> values) {
    if (categories.size() != values.size()) {
        throw Error(cworks::validation_failed(
            "waterfall series: categories and values must have the same length (" +
            std::to_string(categories.size()) + " vs " + std::to_string(values.size()) + ")"));
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i])) {
            throw Error(cworks::validation_failed(
                "waterfall series: values must be finite (value at index " + std::to_string(i) +
                " is missing)"));
        }
    }
    categories_ = std::move(categories);
    values_.assign(values.begin(), values.end());
    totals_.assign(values_.size(), false);
    return *this;
}

WaterfallSeries& WaterfallSeries::total(std::size_t index) {
    if (index >= totals_.size()) {
        throw Error(cworks::validation_failed("waterfall series: total index " +
            std::to_string(index) + " out of range (have " + std::to_string(totals_.size()) +
            " entries)"));
    }
    totals_[index] = true;
    return *this;
}

WaterfallSeries& WaterfallSeries::up_color(Color color) {
    up_color_ = color;
    return *this;
}

WaterfallSeries& WaterfallSeries::down_color(Color color) {
    down_color_ = color;
    return *this;
}

WaterfallSeries& WaterfallSeries::total_color(Color color) {
    total_color_ = color;
    return *this;
}

WaterfallSeries& WaterfallSeries::connectors(bool value) {
    connectors_ = value;
    return *this;
}

WaterfallSeries& WaterfallSeries::value_labels(bool value) {
    value_labels_ = value;
    return *this;
}

std::pair<double, double> WaterfallSeries::bar_span(std::size_t index) const {
    double running = 0.0;
    for (std::size_t i = 0; i <= index && i < values_.size(); ++i) {
        if (totals_[i]) {
            // A total draws an absolute bar at the running value. A nonzero
            // value overrides it (useful for a "Start" baseline).
            if (values_[i] != 0.0) running = values_[i];
            if (i == index) return {0.0, running};
            continue;
        }
        const double from = running;
        running += values_[i];
        if (i == index) return {from, running};
    }
    return {0.0, 0.0};
}

Extent WaterfallSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < values_.size(); ++i) {
        const auto [lo, hi] = bar_span(i);
        const double c = static_cast<double>(i) + 0.5;
        e.include(c, lo);
        e.include(c, hi);
        e.include(c, 0.0);
    }
    return e;
}

void WaterfallSeries::build_geometry(const detail::GeomContext& ctx,
                                     std::vector<SceneItem>& out) const {
    const Color up = up_color_.value_or(kUpColor);
    const Color down = down_color_.value_or(kDownColor);
    const Color total = total_color_.value_or(explicit_color() ? ctx.color
                                                               : ctx.theme.series_color(0));
    const double band = ctx.x.band_width();
    const double bar_w = band * 0.62;

    ShapeStyle connector_style;
    connector_style.stroke = ctx.theme.muted_text_color;
    connector_style.stroke_width = 1.0;
    connector_style.dash = DashPattern{{3.0, 2.0}};

    double prev_edge_x = 0.0, prev_edge_y = 0.0;
    bool have_prev = false;
    for (std::size_t i = 0; i < values_.size(); ++i) {
        const auto [from, to] = bar_span(i);
        const double cx = ctx.x.map_category(i);
        const double y0 = ctx.y.map(from);
        const double y1 = ctx.y.map(to);

        ShapeStyle style;
        style.fill = totals_[i] ? total : (values_[i] >= 0.0 ? up : down);
        const double top = std::min(y0, y1);
        const double h = std::max(1.0, std::abs(y1 - y0));
        out.push_back(RectItem{{cx - bar_w / 2.0, top, bar_w, h}, style});

        if (connectors_ && have_prev) {
            out.push_back(LineItem{{prev_edge_x, prev_edge_y},
                                   {cx - bar_w / 2.0, prev_edge_y}, connector_style});
        }
        prev_edge_x = cx + bar_w / 2.0;
        prev_edge_y = y1;
        have_prev = true;

        if (value_labels_) {
            const double v = totals_[i] ? to : values_[i];
            TextItem label;
            label.text = detail::format_tick_value(v, 0.0);
            label.font = ctx.theme.tick_font();
            label.color = ctx.theme.muted_text_color;
            label.pos = {cx, top - 3.0};
            label.halign = HAlign::Center;
            label.valign = VAlign::Bottom;
            const double plot_top = std::min(ctx.y.pixel_lo(), ctx.y.pixel_hi());
            if (top - label.font.size - 6.0 < plot_top) {
                // No headroom: place the label inside the bar.
                label.pos.y = top + 4.0;
                label.valign = VAlign::Top;
                label.color = ctx.theme.background;
            }
            out.push_back(std::move(label));
        }
    }
}

// -- ECDFSeries ----------------------------------------------------------------

ECDFSeries::ECDFSeries(std::span<const double> values) { set_data(values); }

ECDFSeries& ECDFSeries::set_data(std::span<const double> values) {
    values_.assign(values.begin(), values.end());
    return *this;
}

Extent ECDFSeries::extent() const {
    std::vector<double> clean;
    for (double v : values_)
        if (std::isfinite(v)) clean.push_back(v);
    Extent e;
    if (clean.empty()) return e;
    const auto [lo, hi] = std::minmax_element(clean.begin(), clean.end());
    e.include(*lo, 0.0);
    e.include(*hi, 1.0);
    return e;
}

void ECDFSeries::build_geometry(const detail::GeomContext& ctx,
                                std::vector<SceneItem>& out) const {
    std::vector<double> clean;
    for (double v : values_)
        if (std::isfinite(v)) clean.push_back(v);
    if (clean.empty()) return;
    std::sort(clean.begin(), clean.end());
    std::vector<Point> pts;
    pts.reserve(clean.size() * 2 + 1);
    pts.push_back({ctx.x.map(clean.front()), ctx.y.map(0.0)});
    for (std::size_t i = 0; i < clean.size(); ++i) {
        const double y0 = static_cast<double>(i) / static_cast<double>(clean.size());
        const double y1 = static_cast<double>(i + 1) / static_cast<double>(clean.size());
        const double x = ctx.x.map(clean[i]);
        pts.push_back({x, ctx.y.map(y0)});
        pts.push_back({x, ctx.y.map(y1)});
    }
    ShapeStyle style;
    style.stroke = ctx.color;
    style.stroke_width = ctx.stroke_width;
    out.push_back(PolylineItem{std::move(pts), style});
}

// -- ContourSeries -----------------------------------------------------------

namespace {

/// A grid-cell corner carrying its position and value, for isoband
/// clipping (Sutherland–Hodgman with linear interpolation on z).
struct ContourVertex {
    double x, y, z;
};

/// Keep the polygon part where keep(z) holds; edges crossing the
/// threshold are cut at the interpolated position.
std::vector<ContourVertex> clip_contour(const std::vector<ContourVertex>& poly,
                                        double level, bool keep_greater) {
    std::vector<ContourVertex> out;
    const std::size_t n = poly.size();
    if (n == 0) return out;
    auto inside = [&](const ContourVertex& v) {
        return keep_greater ? v.z >= level : v.z <= level;
    };
    auto cut = [&](const ContourVertex& a, const ContourVertex& b) {
        const double t = (level - a.z) / (b.z - a.z);
        return ContourVertex{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), level};
    };
    for (std::size_t i = 0; i < n; ++i) {
        const ContourVertex& a = poly[i];
        const ContourVertex& b = poly[(i + 1) % n];
        const bool ia = inside(a), ib = inside(b);
        if (ia) out.push_back(a);
        if (ia != ib) out.push_back(cut(a, b));
    }
    return out;
}

} // namespace

ContourSeries::ContourSeries(std::span<const double> x, std::span<const double> y,
                             std::span<const double> z, std::size_t rows,
                             std::size_t cols) {
    set_data(x, y, z, rows, cols);
}

ContourSeries& ContourSeries::set_data(std::span<const double> x,
                                       std::span<const double> y,
                                       std::span<const double> z, std::size_t rows,
                                       std::size_t cols) {
    if (rows != 0 && cols > std::numeric_limits<std::size_t>::max() / rows)
        throw Error(cworks::validation_failed("contour: rows * columns overflows size_t"));
    if (x.size() != cols || y.size() != rows || z.size() != rows * cols) {
        throw Error(cworks::validation_failed(
            "contour: expected x with cols entries, y with rows entries, and "
            "z with rows*cols entries (got x " + std::to_string(x.size()) + ", y " +
            std::to_string(y.size()) + ", z " + std::to_string(z.size()) + " for " +
            std::to_string(rows) + "x" + std::to_string(cols) + ")"));
    }
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (!(x[i] > x[i - 1]))
            throw Error(cworks::validation_failed(
                "contour: x coordinates must be strictly increasing"));
    }
    for (std::size_t i = 1; i < y.size(); ++i) {
        if (!(y[i] > y[i - 1]))
            throw Error(cworks::validation_failed(
                "contour: y coordinates must be strictly increasing"));
    }
    x_.assign(x.begin(), x.end());
    y_.assign(y.begin(), y.end());
    z_.assign(z.begin(), z.end());
    rows_ = rows;
    cols_ = cols;
    return *this;
}

ContourSeries& ContourSeries::level_count(int count) {
    if (count < 1) throw Error(cworks::validation_failed("contour: level_count must be >= 1"));
    level_count_ = count;
    return *this;
}

ContourSeries& ContourSeries::levels(std::span<const double> values) {
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (!(values[i] > values[i - 1]))
            throw Error(cworks::validation_failed(
                "contour: explicit levels must be strictly ascending"));
    }
    explicit_levels_.assign(values.begin(), values.end());
    return *this;
}

ContourSeries& ContourSeries::filled(bool value) {
    filled_ = value;
    return *this;
}

ContourSeries& ContourSeries::colormap(Colormap map) {
    color_scale_.colormap = map;
    return *this;
}

ContourSeries& ContourSeries::color_range(double minimum, double maximum) {
    if (!(maximum > minimum))
        throw Error(cworks::validation_failed("contour: color range maximum must exceed minimum"));
    color_scale_.minimum = minimum;
    color_scale_.maximum = maximum;
    return *this;
}

ContourSeries& ContourSeries::color_midpoint(double midpoint) {
    color_scale_.midpoint = midpoint;
    return *this;
}

ContourSeries& ContourSeries::reverse_colormap(bool value) {
    color_scale_.reverse = value;
    return *this;
}

ContourSeries& ContourSeries::missing_color(Color color) {
    color_scale_.missing = color;
    return *this;
}

ContourSeries& ContourSeries::colorbar(bool value) {
    colorbar_ = value;
    return *this;
}

Extent ContourSeries::extent() const {
    Extent e;
    if (x_.empty() || y_.empty()) return e;
    e.include(x_.front(), y_.front());
    e.include(x_.back(), y_.back());
    return e;
}

std::pair<double, double> ContourSeries::finite_z_range() const {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (double v : z_) {
        if (!std::isfinite(v)) continue;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    return {lo, hi};
}

std::pair<double, double> ContourSeries::colorbar_value_range() const {
    auto [lo, hi] = finite_z_range();
    if (!std::isfinite(lo) || !std::isfinite(hi)) return {0.0, 1.0};
    if (lo == hi) return {lo - 0.5, hi + 0.5};
    return {lo, hi};
}

std::vector<double> ContourSeries::resolved_levels() const {
    if (!explicit_levels_.empty()) return explicit_levels_;
    const auto [lo, hi] = finite_z_range();
    if (!(lo < hi)) return {};
    // Nice interior levels, like axis ticks in data space.
    const TickSet set = detail::linear_ticks(lo, hi, level_count_ + 1, "", false);
    std::vector<double> out;
    for (const auto& t : set.ticks) {
        if (t.value > lo && t.value < hi) out.push_back(t.value);
    }
    return out;
}

void ContourSeries::build_geometry(const detail::GeomContext& ctx,
                                   std::vector<SceneItem>& out) const {
    if (rows_ < 2 || cols_ < 2) return;
    const std::vector<double> levels = resolved_levels();
    if (levels.empty()) return;
    const auto [zlo, zhi] = finite_z_range();

    auto level_color = [&](double value) { return color_scale_.color(value, zlo, zhi); };
    auto z_at = [&](std::size_t r, std::size_t c) { return z_[r * cols_ + c]; };
    auto cell_ok = [&](std::size_t r, std::size_t c) {
        return std::isfinite(z_at(r, c)) && std::isfinite(z_at(r, c + 1)) &&
               std::isfinite(z_at(r + 1, c)) && std::isfinite(z_at(r + 1, c + 1));
    };

    if (filled_) {
        // Band boundaries covering the full data range.
        std::vector<double> edges;
        edges.push_back(zlo);
        for (double v : levels) {
            if (v > edges.back() && v < zhi) edges.push_back(v);
        }
        edges.push_back(zhi);
        const std::size_t bands = edges.size() - 1;
        for (std::size_t k = 0; k < bands; ++k) {
            const double lo = edges[k];
            const double hi = edges[k + 1];
            ShapeStyle style;
            const Color color = level_color((lo + hi) / 2.0);
            style.fill = color;
            // Hairline stroke in the fill color hides antialiasing seams
            // between the per-cell fragments.
            style.stroke = color;
            style.stroke_width = 0.6;
            for (std::size_t r = 0; r + 1 < rows_; ++r) {
                for (std::size_t c = 0; c + 1 < cols_; ++c) {
                    if (!cell_ok(r, c)) continue;
                    std::vector<ContourVertex> poly = {
                        {x_[c], y_[r], z_at(r, c)},
                        {x_[c + 1], y_[r], z_at(r, c + 1)},
                        {x_[c + 1], y_[r + 1], z_at(r + 1, c + 1)},
                        {x_[c], y_[r + 1], z_at(r + 1, c)},
                    };
                    poly = clip_contour(poly, lo, true);
                    // The top band is closed [lo, zhi]; interior bands
                    // are half-open [lo, hi).
                    poly = clip_contour(poly, hi, false);
                    if (poly.size() < 3) continue;
                    PolygonItem item;
                    item.points.reserve(poly.size());
                    for (const auto& v : poly)
                        item.points.push_back({ctx.x.map(v.x), ctx.y.map(v.y)});
                    item.style = style;
                    out.push_back(std::move(item));
                }
            }
        }
        return;
    }

    // Iso-lines: marching squares per level, chained into polylines via
    // canonical grid-edge ids so output is deterministic and joined.
    for (std::size_t li = 0; li < levels.size(); ++li) {
        const double level = levels[li];
        ShapeStyle style;
        style.stroke = explicit_color()
                           ? ctx.color
                           : level_color(level);
        style.stroke_width = ctx.stroke_width;
        style.cap = LineCap::Round;
        style.join = LineJoin::Round;

        // Edge ids: horizontal (r,c)-(r,c+1) -> ((r*cols+c)<<1),
        // vertical (r,c)-(r+1,c) -> ((r*cols+c)<<1)|1.
        auto eh = [&](std::size_t r, std::size_t c) {
            return static_cast<long long>((r * cols_ + c) << 1);
        };
        auto ev = [&](std::size_t r, std::size_t c) {
            return static_cast<long long>(((r * cols_ + c) << 1) | 1);
        };
        std::map<long long, Point> points; // edge id -> data-space point
        auto point_h = [&](std::size_t r, std::size_t c) {
            const long long id = eh(r, c);
            if (!points.count(id)) {
                const double za = z_at(r, c), zb = z_at(r, c + 1);
                const double t = (level - za) / (zb - za);
                points[id] = {x_[c] + t * (x_[c + 1] - x_[c]), y_[r]};
            }
            return id;
        };
        auto point_v = [&](std::size_t r, std::size_t c) {
            const long long id = ev(r, c);
            if (!points.count(id)) {
                const double za = z_at(r, c), zb = z_at(r + 1, c);
                const double t = (level - za) / (zb - za);
                points[id] = {x_[c], y_[r] + t * (y_[r + 1] - y_[r])};
            }
            return id;
        };

        std::vector<std::pair<long long, long long>> segments;
        for (std::size_t r = 0; r + 1 < rows_; ++r) {
            for (std::size_t c = 0; c + 1 < cols_; ++c) {
                if (!cell_ok(r, c)) continue;
                const bool a = z_at(r, c) >= level;
                const bool b = z_at(r, c + 1) >= level;
                const bool d = z_at(r + 1, c + 1) >= level;
                const bool e = z_at(r + 1, c) >= level;
                const int mask = (a ? 1 : 0) | (b ? 2 : 0) | (d ? 4 : 0) | (e ? 8 : 0);
                if (mask == 0 || mask == 15) continue;
                const auto top = [&] { return point_h(r, c); };
                const auto bottom = [&] { return point_h(r + 1, c); };
                const auto left = [&] { return point_v(r, c); };
                const auto right = [&] { return point_v(r, c + 1); };
                switch (mask) {
                case 1: case 14: segments.emplace_back(left(), top()); break;
                case 2: case 13: segments.emplace_back(top(), right()); break;
                case 3: case 12: segments.emplace_back(left(), right()); break;
                case 4: case 11: segments.emplace_back(right(), bottom()); break;
                case 6: case 9: segments.emplace_back(top(), bottom()); break;
                case 7: case 8: segments.emplace_back(left(), bottom()); break;
                case 5: case 10: {
                    const double center = (z_at(r, c) + z_at(r, c + 1) +
                                           z_at(r + 1, c) + z_at(r + 1, c + 1)) / 4.0;
                    const bool center_high = center >= level;
                    // mask 5: A and C high. A high center joins them
                    // diagonally, so the line passes near B and D
                    // (top-right, bottom-left); a low center isolates
                    // the high corners (left-top, right-bottom).
                    const bool pair_a = (mask == 5) != center_high;
                    if (pair_a) {
                        segments.emplace_back(left(), top());
                        segments.emplace_back(right(), bottom());
                    } else {
                        segments.emplace_back(top(), right());
                        segments.emplace_back(bottom(), left());
                    }
                    break;
                }
                default: break;
                }
            }
        }

        // Chain segments into polylines (open chains first, then loops).
        std::multimap<long long, std::size_t> by_end;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            by_end.emplace(segments[i].first, i);
            by_end.emplace(segments[i].second, i);
        }
        std::vector<bool> used(segments.size(), false);
        auto emit_chain = [&](std::size_t start) {
            std::vector<long long> chain{segments[start].first, segments[start].second};
            used[start] = true;
            for (int dir = 0; dir < 2; ++dir) {
                for (;;) {
                    const long long tip = dir == 0 ? chain.back() : chain.front();
                    std::size_t next = segments.size();
                    for (auto it = by_end.lower_bound(tip);
                         it != by_end.end() && it->first == tip; ++it) {
                        if (!used[it->second]) {
                            next = it->second;
                            break;
                        }
                    }
                    if (next == segments.size()) break;
                    used[next] = true;
                    const long long other = segments[next].first == tip
                                                ? segments[next].second
                                                : segments[next].first;
                    if (dir == 0) chain.push_back(other);
                    else chain.insert(chain.begin(), other);
                }
            }
            PolylineItem item;
            item.points.reserve(chain.size());
            for (long long id : chain) {
                const Point& p = points.at(id);
                item.points.push_back({ctx.x.map(p.x), ctx.y.map(p.y)});
            }
            item.style = style;
            out.push_back(std::move(item));
        };
        for (std::size_t i = 0; i < segments.size(); ++i) {
            if (!used[i]) emit_chain(i);
        }
    }
}

// -- PolarSeries -------------------------------------------------------------

PolarSeries::PolarSeries(std::span<const double> theta, std::span<const double> r) {
    set_data(theta, r);
}

PolarSeries& PolarSeries::set_data(std::span<const double> theta,
                                   std::span<const double> r) {
    if (theta.size() != r.size()) {
        throw Error(cworks::validation_failed("polar: theta and r must have the same length (got " +
            std::to_string(theta.size()) + " and " + std::to_string(r.size()) + ")"));
    }
    theta_.assign(theta.begin(), theta.end());
    r_.assign(r.begin(), r.end());
    return *this;
}

PolarSeries& PolarSeries::degrees(bool value) {
    degrees_ = value;
    return *this;
}

PolarSeries& PolarSeries::marker(Marker m, double size) {
    marker_ = m;
    marker_size_ = size;
    return *this;
}

PolarSeries& PolarSeries::close(bool value) {
    close_ = value;
    return *this;
}

PolarSeries& PolarSeries::fill(bool value) {
    fill_ = value;
    if (value) close_ = true; // a fill needs a closed outline
    return *this;
}

PolarSeries& PolarSeries::fill_alpha(double alpha) {
    fill_alpha_ = alpha;
    return *this;
}

PolarSeries& PolarSeries::dash(std::string svg_dash_array) {
    dash_ = DashPattern::parse(svg_dash_array);
    return *this;
}

PolarSeries& PolarSeries::range(double minimum, double maximum) {
    if (!(maximum > minimum))
        throw Error(cworks::validation_failed("polar: range maximum must exceed the minimum"));
    range_ = std::pair<double, double>{minimum, maximum};
    return *this;
}

double PolarSeries::theta_degrees(std::size_t i) const {
    constexpr double kPi = 3.14159265358979323846;
    const double t = theta_[i];
    // Degrees are the unit the geometry is built in, so a series given in
    // degrees — the common case, and the one where the sample angles are
    // round numbers — passes through untouched and lands exactly on the
    // grid spokes. Only a radian-valued series pays a conversion.
    return degrees_ ? t : t * 180.0 / kPi;
}

Extent PolarSeries::extent() const {
    Extent e;
    for (std::size_t i = 0; i < r_.size(); ++i) {
        if (!std::isfinite(r_[i])) continue;
        e.include(theta_[i], r_[i]);
    }
    return e;
}

void PolarSeries::build_geometry(const detail::GeomContext& ctx,
                                 std::vector<SceneItem>& out) const {
    // ctx.y is the radial scale mapped to pixels [0, polar_radius];
    // angles run counterclockwise from the positive x axis.
    const auto at_angle = [&](double degrees, double radius) {
        double sine = 0.0;
        double cosine = 0.0;
        cworks::sincos_deg(degrees, sine, cosine);
        return Point{ctx.polar_center.x + radius * cosine,
                     ctx.polar_center.y - radius * sine};
    };
    const auto point = [&](std::size_t i) {
        return at_angle(theta_degrees(i), ctx.y.map(r_[i]));
    };

    // Split at NaN r values (Gap) or skip them (Drop/Zero handled as gap
    // and zero radius respectively; Error throws).
    std::vector<std::vector<Point>> runs(1);
    for (std::size_t i = 0; i < r_.size(); ++i) {
        if (!std::isfinite(r_[i]) || !std::isfinite(theta_[i])) {
            switch (missing_policy()) {
            case MissingPolicy::Error:
                throw Error(cworks::validation_failed("polar series '" + label_text() +
                    "' contains missing values (missing policy is error)"));
            case MissingPolicy::Zero:
                runs.back().push_back(at_angle(theta_degrees(i), ctx.y.map(0.0)));
                break;
            case MissingPolicy::Gap:
                if (!runs.back().empty()) runs.emplace_back();
                break;
            case MissingPolicy::Drop:
                break;
            case MissingPolicy::Interpolate:
                detail::reject_unsupported_interpolate(MissingPolicy::Interpolate, "polar");
                break;
            }
            continue;
        }
        runs.back().push_back(point(i));
    }
    if (runs.back().empty()) runs.pop_back();

    // A closed outline only makes sense for one unbroken run.
    const bool closed = close_ && runs.size() == 1 && runs.front().size() > 2;

    if (fill_ && closed) {
        Color fill_color = ctx.color;
        fill_color.a = fill_alpha_;
        ShapeStyle style;
        style.fill = fill_color;
        out.push_back(PolygonItem{runs.front(), style});
    }
    ShapeStyle line_style;
    line_style.stroke = ctx.color;
    line_style.stroke_width = ctx.stroke_width;
    line_style.dash = dash_;
    line_style.cap = LineCap::Round;
    line_style.join = LineJoin::Round;
    for (auto& run : runs) {
        if (run.size() < 2) continue;
        std::vector<Point> pts = run;
        if (closed) pts.push_back(pts.front());
        out.push_back(PolylineItem{std::move(pts), line_style});
    }
    if (marker_ != Marker::None) {
        for (const auto& run : runs) {
            for (const auto& p : run) {
                emit_marker(out, marker_, p, marker_size_, ctx.color, 1.0);
            }
        }
    }
}

} // namespace cplot
