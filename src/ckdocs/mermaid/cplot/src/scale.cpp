// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cplot/scale.hpp"
#include <cworks/app_error.hpp>
#include "cplot/error.hpp"

#include <cmath>

#include <cworks/math.hpp>

#include "decade.hpp"

namespace cplot {

Scale Scale::linear(double lo, double hi) {
    Scale s;
    s.kind_ = ScaleKind::Linear;
    s.lo_ = lo;
    s.hi_ = hi;
    s.cache_domain();
    return s;
}

Scale Scale::log10(double lo, double hi) {
    Scale s;
    s.kind_ = ScaleKind::Log10;
    s.lo_ = lo > 0 ? lo : 1e-12;
    s.hi_ = hi > 0 ? hi : 1.0;
    s.cache_domain();
    return s;
}

Scale Scale::log2(double lo, double hi) {
    Scale s;
    s.kind_ = ScaleKind::Log2;
    s.lo_ = lo > 0 ? lo : 1e-12;
    s.hi_ = hi > 0 ? hi : 1.0;
    s.cache_domain();
    return s;
}

Scale Scale::ln(double lo, double hi) {
    Scale s;
    s.kind_ = ScaleKind::Ln;
    s.lo_ = lo > 0 ? lo : 1e-12;
    s.hi_ = hi > 0 ? hi : 1.0;
    s.cache_domain();
    return s;
}

Scale Scale::symlog(double lo, double hi, double linthresh) {
    Scale s;
    s.kind_ = ScaleKind::Symlog;
    s.lo_ = lo;
    s.hi_ = hi;
    s.linthresh_ = linthresh > 0 ? linthresh : 1.0;
    s.cache_domain();
    return s;
}

Scale Scale::datetime(double lo, double hi) {
    Scale s;
    s.kind_ = ScaleKind::DateTime;
    s.lo_ = lo;
    s.hi_ = hi;
    s.cache_domain();
    return s;
}

Scale Scale::transform(const std::string& name, double lo, double hi) {
    Scale s;
    s.kind_ = ScaleKind::Custom;
    s.lo_ = lo;
    s.hi_ = hi;
    s.transform_name_ = name;
    throw Error(cworks::validation_failed("custom scales are not supported in ckdocs: " + name));
    return s;
}

Scale Scale::mirrored(double magnitude) {
    Scale s;
    s.kind_ = ScaleKind::Mirrored;
    const double m = magnitude > 0 ? magnitude : 1.0;
    s.lo_ = -m;
    s.hi_ = m;
    s.cache_domain();
    return s;
}

Scale Scale::category(std::vector<std::string> categories) {
    Scale s;
    s.kind_ = ScaleKind::Category;
    s.lo_ = 0.0;
    s.hi_ = static_cast<double>(categories.empty() ? 1 : categories.size());
    s.categories_ = std::move(categories);
    s.cache_domain();
    return s;
}

/// WHY THIS ONE KEEPS libm, when the tick and domain-snapping paths do not
/// (see decade.hpp). Everything else in this suite that reaches a *discrete*
/// decision — a floor, a ceil, a threshold ladder — now decides it by exact
/// comparison, because there a last-bit difference between two C libraries
/// becomes a different integer and a visibly different chart. This function
/// reaches no discrete decision: its result is a pixel coordinate, and every
/// consumer puts a coarse grid in front of it (SVG and PDF print through
/// `%.2f`, the rasterizer quantizes coverage to one byte in 256). A grid of
/// 10^-2 against an ulp of 10^-16 is fourteen orders of magnitude of margin,
/// and it was measured rather than assumed: perturbing every `log` result in
/// the suite by one ulp — 1 059 369 of them — moved not one byte of any
/// rendered golden, and the SVG path needs a relative error of 10^-4 before
/// a single byte changes.
///
/// The `log10` of the Symlog branch is the same decision on the same
/// evidence: 8 802 perturbed `log10` results across 28 executed sites moved
/// no byte of any golden either, and the value it produces is a position in
/// the same `%.2f` grid.
///
/// The price of converting it anyway is not small, because this runs once
/// per mapped point rather than once per axis. Measured at -O2 on 4 000 000
/// values: `Scale::map` on a log axis costs 4.4 ns with `std::log` and
/// 110.3 ns with the deterministic `cworks::log`, and a 1 000 000-point
/// log-axis `build_scene` goes from 9.5 ms to 115.0 ms — 12x for a
/// divergence that six to eleven orders of magnitude of headroom say cannot
/// reach the output. It is one function deliberately, and not half of one:
/// splitting the kernels — a deterministic `log` for the cached endpoints
/// and libm's per point — would stop `map(domain_lo())` landing exactly on
/// the pixel edge, so the choice is all or nothing.
///
/// What would reverse it: a consumer that puts a discrete decision on a
/// mapped coordinate (an integer pixel snap, a clip test at an exact
/// boundary), or a rendered format without the `%.2f` grid. unmap() below
/// is exactly that consumer — an interactive caller may persist or branch
/// on the value it returns — so it is built on the deterministic kernel
/// instead, at the same 12x this function's measurement would have paid.
double Scale::forward_value(double value) const {
    switch (kind_) {
    case ScaleKind::Log10:
    case ScaleKind::Log2:
    case ScaleKind::Ln:
        // Log position is base-independent: the base factor cancels in the
        // normalized ratio, so all log kinds share one mapping (the base
        // matters only for tick placement).
        return std::log(value > 0 ? value : lo_);
    case ScaleKind::Symlog: {
        const double ax = std::abs(value);
        if (ax <= linthresh_) return value / linthresh_; // linear core → [-1, 1]
        const double sign = value < 0 ? -1.0 : 1.0;
        return sign * (1.0 + std::log10(ax / linthresh_));
    }
    case ScaleKind::Custom:
        return forward_(value);
    case ScaleKind::Linear:
    case ScaleKind::DateTime:
    case ScaleKind::Category:
    case ScaleKind::Mirrored:
        break;
    }
    return value;
}

void Scale::cache_domain() {
    forward_lo_ = forward_value(lo_);
    forward_span_ = forward_value(hi_) - forward_lo_;
}

void Scale::set_pixel_range(double px_lo, double px_hi) {
    px_lo_ = px_lo;
    px_hi_ = px_hi;
}

void Scale::set_center_gap(double px) { center_gap_ = px > 0 ? px : 0.0; }

void Scale::set_tick_domain(double lo, double hi) {
    tick_lo_ = lo;
    tick_hi_ = hi;
    has_tick_domain_ = true;
}

std::pair<double, double> Scale::center_edges() const {
    if (kind_ != ScaleKind::Mirrored) {
        const double z = map(0.0);
        return {z, z};
    }
    const double center = (px_lo_ + px_hi_) / 2.0;
    const double dir = px_hi_ >= px_lo_ ? 1.0 : -1.0;
    return {center - dir * center_gap_ / 2.0, center + dir * center_gap_ / 2.0};
}

double Scale::map(double value) const {
    double t = 0.0;
    switch (kind_) {
    // Every kind but Mirrored interpolates linearly in the scale's forward
    // space, with the domain endpoints transformed once by cache_domain()
    // rather than on each of the (many) values a series maps.
    case ScaleKind::Linear:
    case ScaleKind::DateTime:
    case ScaleKind::Category:
    case ScaleKind::Log10:
    case ScaleKind::Log2:
    case ScaleKind::Ln:
    case ScaleKind::Symlog:
    case ScaleKind::Custom:
        t = forward_span_ != 0.0 ? (forward_value(value) - forward_lo_) / forward_span_ : 0.5;
        break;
    case ScaleKind::Mirrored: {
        // Two linear half-bands around the centre gap: magnitudes grow
        // outward from each inner edge. Zero sits on the negative-side
        // edge; bar geometry uses center_edges() and never relies on it.
        const auto [edge_lo, edge_hi] = center_edges();
        const double m = hi_; // domain is [-m, +m]
        if (m <= 0.0) return (px_lo_ + px_hi_) / 2.0;
        if (value > 0.0) return edge_hi + (value / m) * (px_hi_ - edge_hi);
        return px_lo_ + ((value + m) / m) * (edge_lo - px_lo_);
    }
    }
    return px_lo_ + t * (px_hi_ - px_lo_);
}

double Scale::unmap(double pixel) const {
    if (kind_ == ScaleKind::Mirrored) {
        // Structural inverse of map()'s own Mirrored branch: same two
        // linear half-bands, expressed with `dir` so an inverted pixel
        // range (px_hi_ < px_lo_) inverts correctly too. A pixel inside
        // the centre gap belongs to neither band; both formulas agree on
        // 0.0 at their shared boundary, so that is the answer there.
        const auto [edge_lo, edge_hi] = center_edges();
        const double m = hi_; // domain is [-m, +m]
        if (m <= 0.0) return 0.0;
        const double dir = px_hi_ >= px_lo_ ? 1.0 : -1.0;
        if (px_hi_ != edge_hi && dir * (pixel - edge_hi) >= 0.0) {
            return (pixel - edge_hi) / (px_hi_ - edge_hi) * m;
        }
        if (edge_lo != px_lo_ && dir * (pixel - edge_lo) <= 0.0) {
            return m * (pixel - px_lo_) / (edge_lo - px_lo_) - m;
        }
        return 0.0;
    }
    const double t = px_hi_ != px_lo_ ? (pixel - px_lo_) / (px_hi_ - px_lo_) : 0.5;
    const double fv = forward_lo_ + t * forward_span_;
    switch (kind_) {
    case ScaleKind::Log10:
    case ScaleKind::Log2:
    case ScaleKind::Ln:
        // One shared inverse for the same reason forward_value() shares one
        // forward transform: log position is base-independent.
        return cworks::exp(fv);
    case ScaleKind::Symlog: {
        if (std::abs(fv) <= 1.0) return fv * linthresh_; // linear core
        const double sign = fv < 0.0 ? -1.0 : 1.0;
        return sign * linthresh_ * cworks::pow(10.0, std::abs(fv) - 1.0);
    }
    case ScaleKind::Custom:
        return inverse_(fv);
    case ScaleKind::Linear:
    case ScaleKind::DateTime:
    case ScaleKind::Category:
    case ScaleKind::Mirrored:
        break;
    }
    return fv;
}

double Scale::map_category(std::size_t index) const {
    return map(static_cast<double>(index) + 0.5);
}

double Scale::band_width() const {
    const double n = hi_ - lo_;
    if (n <= 0) return 0.0;
    return std::abs(px_hi_ - px_lo_) / n;
}

TickSet Scale::ticks(int target_count, const std::string& format, bool minor) const {
    switch (kind_) {
    case ScaleKind::Linear:
    case ScaleKind::Custom: { // ticks are chosen in data space
        // Tick positions may live on a nice sub-range of the mapping domain
        // when the frame has been given a margin (see set_tick_domain).
        const double tlo = has_tick_domain_ ? tick_lo_ : lo_;
        const double thi = has_tick_domain_ ? tick_hi_ : hi_;
        return detail::linear_ticks(tlo, thi, target_count, format, minor);
    }
    case ScaleKind::Log10:
        return detail::log_ticks(lo_, hi_, format, minor, 10.0);
    case ScaleKind::Log2:
        return detail::log_ticks(lo_, hi_, format, minor, 2.0);
    case ScaleKind::Ln:
        return detail::log_ticks(lo_, hi_, format, minor, detail::kEuler);
    case ScaleKind::Symlog:
        return detail::symlog_ticks(lo_, hi_, linthresh_, format, minor);
    case ScaleKind::DateTime:
        return detail::datetime_ticks(lo_, hi_, target_count, format);
    case ScaleKind::Category: {
        TickSet set;
        for (std::size_t i = 0; i < categories_.size(); ++i) {
            set.ticks.push_back({static_cast<double>(i) + 0.5, categories_[i], false});
        }
        return set;
    }
    case ScaleKind::Mirrored: {
        // Nice ticks over [0, m], mirrored onto both sides and labelled
        // as magnitudes. Zero is excluded: it belongs to both halves, so
        // the panel draws one zero label per inner edge.
        const TickSet half = detail::linear_ticks(0.0, hi_, target_count, format, minor);
        TickSet set;
        set.step = half.step;
        for (auto it = half.ticks.rbegin(); it != half.ticks.rend(); ++it) {
            if (it->value > 0.0) set.ticks.push_back({-it->value, it->label, it->minor});
        }
        for (const auto& t : half.ticks) {
            if (t.value > 0.0) set.ticks.push_back(t);
        }
        for (auto it = half.minor.rbegin(); it != half.minor.rend(); ++it) {
            if (*it > 0.0) set.minor.push_back(-*it);
        }
        for (double v : half.minor) {
            if (v > 0.0) set.minor.push_back(v);
        }
        return set;
    }
    }
    return {};
}

} // namespace cplot
