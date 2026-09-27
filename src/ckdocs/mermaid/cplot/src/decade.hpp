// ckplot — decade decisions, made by comparison instead of by logarithm
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// WHY THIS HEADER EXISTS. Two independent parts of the chart engine ask the
// same question — "which power of the base does this value sit in, and what
// is that power?" — and both used to answer it with `pow(10, floor(log10(x)))`.
// Tick generation needs it to pick a step (ticks.cpp) and to count a label's
// decimals; log-axis resolution needs it to snap a domain onto whole decades
// (layout.cpp).
//
// That idiom is the suite's single worst determinism defect, and not because
// its arithmetic is inaccurate. IEEE-754 mandates correct rounding for
// `+ - * /` and `sqrt` and for nothing else, so `log10` and `pow` may each
// round differently on a different C library — and the answer here is then
// fed straight into a `floor`, a `ceil` or a threshold ladder, which turns a
// last-bit difference into a different integer. A different integer is a
// different number of ticks, a different axis domain and a different label on
// every mark. The suite is elsewhere protected by a coarse output grid (SVG
// and PDF coordinates print through `%.2f`); a discrete decision has no grid
// in front of it at all.
//
// And the boundaries are struck exactly rather than occasionally, because
// chart data is round: a range of 30 across 10 ticks makes the normalized
// step exactly 3.0, which is exactly a ladder boundary.
//
// So the decade is decided here by comparing the value against a committed
// table of decade boundaries (`cworks::floor_log10`) and read back from a
// committed table of powers of ten (`cworks::pow10`). No logarithm is
// evaluated, so there is no rounding decision for a platform to disagree
// about — the defect is removed rather than made one ulp smaller. Bases other
// than ten reach `cworks`' deterministic logarithms and build their powers
// from multiplication alone, which IEEE-754 does specify exactly.
#pragma once

#include <cmath>

#include <cworks/math.hpp>

namespace cplot::detail {

/// Relative tolerance for "this value is a decade boundary that arithmetic
/// has left a hair off it". Domain bounds arrive from data reduction and
/// unit conversion, so 999.9999999999 must still count as three decades;
/// the old code spent the same tolerance in log space (`+ 1e-9` before a
/// `floor`), which is a factor of 10^1e-9, i.e. 2.3e-9 relative.
inline constexpr double kDecadeEps = 1e-9;

/// The base of a natural-log axis: e, correctly rounded to a double.
///
/// Written as a constant rather than obtained from `std::exp(1.0)` — the
/// argument is a literal, so there is nothing to compute, and asking libm
/// for it made a compile-time constant into a platform-dependent one.
inline constexpr double kEuler = 0x1.5bf0a8b145769p+1;

/// base^k for an integer k, from IEEE-mandated operations alone.
///
/// Base 10 is read from `cworks::pow10`'s committed table (exact for
/// 0 <= k <= 22, correctly rounded elsewhere) and base 2 from a power-of-two
/// scale, which is exact everywhere. Any other base is raised by squaring:
/// multiplication is correctly rounded by IEEE-754, so the sequence of
/// operations fixes the result bit for bit on every platform.
inline double decade_value(int k, double base = 10.0) noexcept {
    if (base == 10.0) return cworks::pow10(k);
    if (base == 2.0) return std::ldexp(1.0, k);
    double acc = 1.0;
    double factor = base;
    for (long long n = k < 0 ? -static_cast<long long>(k) : k; n != 0; n >>= 1) {
        if ((n & 1) != 0) acc *= factor;
        if ((n >> 1) != 0) factor *= factor;
    }
    return k < 0 ? 1.0 / acc : acc;
}

/// The decade of `value`: the largest integer k with base^k <= value, with a
/// value that sits within kDecadeEps *below* a boundary counted as being on
/// it. Replaces `floor(log(value) / log(base) + eps)`.
///
/// `value` must be finite and positive; 0 is returned otherwise, since a
/// decade is not defined there.
inline int decade_floor(double value, double base = 10.0) noexcept {
    if (!(value > 0.0) || !std::isfinite(value)) return 0;
    if (base == 10.0) {
        // Exact: cworks::floor_log10 compares against committed decade
        // boundaries and evaluates no logarithm at all.
        const int k = cworks::floor_log10(value);
        return value >= cworks::pow10(k + 1) * (1.0 - kDecadeEps) ? k + 1 : k;
    }
    const double lg = base == 2.0 ? cworks::log2(value)
                                  : cworks::log(value) / cworks::log(base);
    return static_cast<int>(std::floor(lg + kDecadeEps));
}

/// The smallest integer k with base^k >= value, with a value that sits
/// within kDecadeEps *above* a boundary counted as being on it. Replaces
/// `ceil(log(value) / log(base) - eps)`.
inline int decade_ceil(double value, double base = 10.0) noexcept {
    if (!(value > 0.0) || !std::isfinite(value)) return 0;
    if (base == 10.0) {
        const int k = cworks::floor_log10(value);
        return value <= cworks::pow10(k) * (1.0 + kDecadeEps) ? k : k + 1;
    }
    const double lg = base == 2.0 ? cworks::log2(value)
                                  : cworks::log(value) / cworks::log(base);
    return static_cast<int>(std::ceil(lg - kDecadeEps));
}

} // namespace cplot::detail
