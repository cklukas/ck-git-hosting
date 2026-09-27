// libcworks — the exact transforms the deterministic kernels stand on
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Knuth's two-sum and Dekker's two-product, and the compound operations
// built on them. Both primitives use nothing but IEEE `+`, `-` and `*`, so
// they are as portable as the arithmetic itself: a fused multiply-add would
// be shorter, but reaching it means a library call whose correctness on an
// exotic platform is one more thing to trust, and splitting needs no such
// trust. (The source scans in `test_trig.cpp` and `test_math.cpp` read this
// file for the names of library functions that may round, which is also why
// none is spelled out in the prose here.)
//
// A `Two` denotes the real number `hi + lo` with `hi == fl(hi + lo)`, so it
// carries about 106 significant bits, and `hi` alone is already the value
// rounded to double. Nothing here is a general-purpose extended-precision
// library: these routines are used where a single rounding in the wrong
// place would cost a tenth of an ulp of a final answer, and nowhere else.
//
// This header is PRIVATE to libcworks — it is the shared floor under
// `trig.cpp` and `math.cpp`, which are the two places in the suite that owe
// their callers bit-identical results on every platform. It is not
// installed, and it deliberately names no domain concept: it is arithmetic,
// not geometry and not elementary functions.
//
// The arithmetic below is exact only if the compiler does not fuse a
// multiply and an add behind our back: `two_product` splits its operands on
// the assumption that `a * b` was rounded. `cworks_strict()` compiles every
// first-party target with `-ffp-contract=off` (and MSVC's default
// `/fp:precise` forbids contraction outright), which is the same flag the
// suite already relies on for byte-exact goldens — this header simply makes
// the dependency explicit. `tests/test_trig.cpp` verifies the exactness of
// both primitives directly, so a build that lost the flag fails loudly
// instead of silently producing different bytes.
#pragma once

#include <cstddef> // std::size_t

namespace cworks::detail {

struct Two {
    double hi = 0.0;
    double lo = 0.0;
};

/// Exact sum for operands already in magnitude order (|a| >= |b|).
inline Two quick_two_sum(double a, double b) noexcept {
    const double s = a + b;
    return {s, b - (s - a)};
}

/// Exact sum: hi + lo == a + b, with hi == fl(a + b).
inline Two two_sum(double a, double b) noexcept {
    const double s = a + b;
    const double bb = s - a;
    const double err = (a - (s - bb)) + (b - bb);
    return {s, err};
}

/// Exact product: hi + lo == a * b, with hi == fl(a * b). Requires that
/// neither operand exceed about 1e292, so that the splitting below cannot
/// overflow; every caller works well inside that, scaling its operands into
/// range first where the argument range does not already guarantee it.
inline Two two_product(double a, double b) noexcept {
    constexpr double kSplitter = 134217729.0; // 2^27 + 1
    const double p = a * b;
    const double as = kSplitter * a;
    const double ah = as - (as - a);
    const double al = a - ah;
    const double bs = kSplitter * b;
    const double bh = bs - (bs - b);
    const double bl = b - bh;
    const double err = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    return {p, err};
}

inline Two negated(Two a) noexcept { return {-a.hi, -a.lo}; }

inline Two dd_add(Two a, Two b) noexcept {
    Two s = two_sum(a.hi, b.hi);
    const Two t = two_sum(a.lo, b.lo);
    s = quick_two_sum(s.hi, s.lo + t.hi);
    return quick_two_sum(s.hi, s.lo + t.lo);
}

/// Sum for operands that cannot cancel — |b| must be well below |a|, as it
/// is when a series coefficient is added to the smaller terms after it.
/// Half the work of `dd_add`, which spends its extra operations entirely on
/// the cancelling case. Using it where the operands CAN cancel loses the
/// tail, so the precondition is the caller's to keep.
inline Two dd_add_small(Two a, Two b) noexcept {
    Two s = two_sum(a.hi, b.hi);
    s.lo += a.lo + b.lo;
    return quick_two_sum(s.hi, s.lo);
}

/// `a` times a plain double.
inline Two dd_scale(Two a, double b) noexcept {
    Two p = two_product(a.hi, b);
    p.lo += a.lo * b;
    return quick_two_sum(p.hi, p.lo);
}

inline Two dd_mul(Two a, Two b) noexcept {
    Two p = two_product(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return quick_two_sum(p.hi, p.lo);
}

/// Newton-corrected quotient: three double divisions, each one correcting
/// the residue the last left behind.
inline Two dd_div(Two a, Two b) noexcept {
    const double q1 = a.hi / b.hi;
    Two r = dd_add(a, negated(dd_scale(b, q1)));
    const double q2 = r.hi / b.hi;
    r = dd_add(r, negated(dd_scale(b, q2)));
    const double q3 = r.hi / b.hi;
    const Two q = quick_two_sum(q1, q2);
    return quick_two_sum(q.hi, q.lo + q3);
}

/// Horner's rule in plain double, in a FIXED order — no reassociation, no
/// vectorised pairwise sum, nothing the platform gets to choose.
template <std::size_t N>
inline double horner(const double (&coefficients)[N], double t) noexcept {
    double acc = coefficients[N - 1];
    for (std::size_t i = N - 1; i > 0; --i) acc = acc * t + coefficients[i - 1];
    return acc;
}

/// Horner's rule in double-double, from a double-double argument, for the
/// leading terms of a series — where a rounded coefficient or a rounded
/// partial sum would cost more than the final rounding does. The trailing
/// terms arrive already summed in `tail`, evaluated in plain double where
/// they are small enough that their rounding cannot reach the result.
///
/// The additions are the non-cancelling kind by construction: each
/// coefficient dominates everything multiplied by the argument after it,
/// because the argument has been reduced to well below one.
template <std::size_t N>
inline Two dd_horner(const Two (&coefficients)[N], Two t, Two tail) noexcept {
    Two acc = tail;
    for (std::size_t i = N; i > 0; --i) acc = dd_add_small(coefficients[i - 1], dd_mul(acc, t));
    return acc;
}

} // namespace cworks::detail
