// ckplot — deterministic descriptive statistics and hypothesis tests
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cplot/stats.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

#include <cworks/app_error.hpp>
#include <cworks/math.hpp>

#include "cplot/figure.hpp" // cplot::Error

namespace cplot::stats {
namespace {

/// Collect the finite entries of a sample in index order.
std::vector<double> finite_of(std::span<const double> v) {
    std::vector<double> out;
    out.reserve(v.size());
    for (double x : v)
        if (std::isfinite(x)) out.push_back(x);
    return out;
}

/// Type-7 (linear interpolation) quantile of a sorted, non-empty sample —
/// the same definition the box-plot engine uses.
double quantile_sorted(const std::vector<double>& s, double p) {
    const double idx = p * static_cast<double>(s.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(idx));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
    const double frac = idx - static_cast<double>(lo);
    return s[lo] * (1.0 - frac) + s[hi] * frac;
}

// -- log-gamma, without libm ----------------------------------------------
//
// WHY THIS FILE OWNS ITS OWN. `std::lgamma` was the worst-behaved routine
// in the whole suite when it was measured: on the arguments these tests
// actually produce it is misrounded on 46% of them, with a worst case of
// three units in the last place — and IEEE-754 mandates correct rounding
// for `+ - * /` and `sqrt` and for nothing else, so a second C library is
// entitled to a different answer on every one of those. That would be
// harmless if a log-gamma ended up as a pixel coordinate, where the `%.2f`
// print grid absorbs fourteen orders of magnitude. It does not. It feeds
// `student_t_quantile_975`, whose two hundred bisection steps each turn a
// value into a single comparison bit, and it terminates in
// `significance_stars`, a threshold ladder at 0.05 — so a p-value that
// lands on the boundary prints "*" on one platform and "ns" on another.
// A word in the chart changes, not a last digit.
//
// (POSIX `lgamma` also writes the global `signgam` as a side effect, which
// is a data race in a threaded renderer and a reason of its own to be rid
// of it.)
//
// HOW IT IS BUILT. Half-integer arguments — which is every argument an
// integer degrees-of-freedom produces, i.e. all but Welch's fractional df —
// have a closed form with an EXACT rational in it:
//
//     Γ(n)     = (n−1)!                     for integer n
//     Γ(n+½)   = (2n−1)!! / 2^n · √π
//
// 22! and 29!! are the last of each that a double holds exactly, so up to
// a = 23 and a = 15.5 the rational is built by exact multiplication, scaled
// by a power of two (exact), and handed to ONE deterministic logarithm.
// Nothing else rounds. Above that, and for a fractional argument, Stirling's
// series is evaluated at a shifted argument, which is arithmetic plus one
// more deterministic logarithm. Either way every operation is either
// IEEE-mandated or `cworks::log`, so the answer is the same on every
// platform by construction.
//
// WHAT IT COSTS IN ACCURACY, measured offline against a 60-digit reference:
// at the half-integers, at most one ulp — better than the three the host
// libm was measured at. On 4500 random arguments over (0, 300) the worst
// absolute error was 1.5e-14 against libm's 4.5e-13; libm keeps more
// RELATIVE digits where lnΓ passes through zero near a = 1 and a = 2, which
// is the price of reaching those arguments by recurrence rather than by a
// dedicated minimax branch. Both are far tighter than a reported p-value
// can see, and only one of them is reproducible.

/// log(√π) and log(√2π), each the correctly rounded double of the exact
/// constant. Written out rather than derived at run time: `0.5 * log(2 * π)`
/// evaluates a logarithm of the *double* nearest 2π, which is a different
/// number, and lands one ulp below the right answer.
constexpr double kLogSqrtPi = 0x1.250d048e7a1bdp-1;
constexpr double kLogSqrt2Pi = 0x1.d67f1c864beb5p-1;

/// Where Stirling's series is entered. Its first omitted term at z = 20 is
/// 691/(360360·z^11) / z = 9.4e-18, below half an ulp of lnΓ(20) = 39.3, so
/// the truncation is not visible in a double; a smaller floor would need
/// more Bernoulli terms to say the same.
constexpr double kStirlingFloor = 20.0;

/// How far the closed form reaches, which is exactly how far its rational
/// stays an exact double. 29!! = 6190283353629375 is under 2^53 and 31!! is
/// not; 22! is the last exact factorial. Beyond either, the closed form
/// would have to round its own numerator — which is the one thing it exists
/// to avoid — so Stirling's series takes over instead.
constexpr int kMaxExactHalfStep = 15; ///< Γ(n+½) for n <= 15, i.e. a <= 15.5
constexpr int kMaxExactInteger = 23;  ///< Γ(n)   for n <= 23

/// lnΓ(z) from Stirling's asymptotic series, for z >= kStirlingFloor.
///
/// lnΓ(z) = (z−½)·ln z − z + ln√2π + Σ B_2k / (2k(2k−1) z^(2k−1)), evaluated
/// by Horner in 1/z² through the B_12 term. Every coefficient is a ratio of
/// two integer literals, so the compiler's constant folding rounds it once,
/// correctly, and identically everywhere.
double log_gamma_stirling(double z) {
    const double inv = 1.0 / z;
    const double sq = inv * inv;
    const double series =
        1.0 / 12.0 +
        sq * (-1.0 / 360.0 +
              sq * (1.0 / 1260.0 +
                    sq * (-1.0 / 1680.0 + sq * (1.0 / 1188.0 + sq * (-691.0 / 360360.0)))));
    return (z - 0.5) * cworks::log(z) - z + kLogSqrt2Pi + series * inv;
}

/// lnΓ(a) in closed form when `a` is a half-integer small enough for the
/// rational to be an exact double, and nothing otherwise.
///
/// The two exact cases are Γ(n) = (n−1)! and Γ(n+½) = (2n−1)!!·√π/2^n. The
/// half-integer branch adds the constant log√π rather than multiplying √π
/// into the argument: near a = 1.5 the logarithm is small while its argument
/// is not, so a rounding of √π inside the argument is worth three ulp of the
/// result and outside it is worth one. (Measured both ways at every
/// half-integer up to 31.5.)
std::optional<double> log_gamma_closed(double a) {
    if (!(a > 0.0) || a > static_cast<double>(kMaxExactInteger)) return std::nullopt;
    const double twice = 2.0 * a; // exact: scaling by a power of two
    const int steps = static_cast<int>(twice);
    if (static_cast<double>(steps) != twice) return std::nullopt; // not a half-integer
    if (steps % 2 == 0) {
        const int n = steps / 2;
        double factorial = 1.0; // (n-1)!, exact for n <= 23
        for (int k = 2; k < n; ++k) factorial *= static_cast<double>(k);
        return cworks::log(factorial); // log(1) is +0 exactly: lnΓ(1) = lnΓ(2) = 0
    }
    const int n = (steps - 1) / 2;
    if (n > kMaxExactHalfStep) return std::nullopt;
    double double_factorial = 1.0; // (2n-1)!!, exact for n <= 15
    for (int j = 1; j <= n; ++j) double_factorial *= static_cast<double>(2 * j - 1);
    return cworks::log(std::ldexp(double_factorial, -n)) + kLogSqrtPi;
}

// Regularized incomplete beta via the Lentz continued fraction (Numerical
// Recipes formulation). Deterministic: fixed iteration cap, index-order.
double beta_cf(double a, double b, double x) {
    const int max_it = 300;
    const double eps = 3.0e-12;
    const double fp_min = 1.0e-300;
    const double qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double c = 1.0;
    double d = 1.0 - qab * x / qap;
    if (std::abs(d) < fp_min) d = fp_min;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= max_it; ++m) {
        const double m2 = 2.0 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (std::abs(d) < fp_min) d = fp_min;
        c = 1.0 + aa / c;
        if (std::abs(c) < fp_min) c = fp_min;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (std::abs(d) < fp_min) d = fp_min;
        c = 1.0 + aa / c;
        if (std::abs(c) < fp_min) c = fp_min;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::abs(del - 1.0) <= eps) break;
    }
    return h;
}

// Lower incomplete gamma P(a, x) via the power series (good for x < a + 1).
double gamma_series(double a, double x) {
    const int max_it = 400;
    const double eps = 3.0e-12;
    const double gln = log_gamma(a);
    double ap = a;
    double sum = 1.0 / a;
    double del = sum;
    for (int n = 1; n <= max_it; ++n) {
        ap += 1.0;
        del *= x / ap;
        sum += del;
        if (std::abs(del) < std::abs(sum) * eps) break;
    }
    return sum * cworks::exp(-x + a * cworks::log(x) - gln);
}

// Upper incomplete gamma Q(a, x) via the Lentz continued fraction
// (good for x >= a + 1).
double gamma_cf(double a, double x) {
    const int max_it = 400;
    const double eps = 3.0e-12;
    const double fp_min = 1.0e-300;
    const double gln = log_gamma(a);
    double b = x + 1.0 - a;
    double c = 1.0 / fp_min;
    double d = 1.0 / b;
    double h = d;
    for (int i = 1; i <= max_it; ++i) {
        const double an = -1.0 * i * (i - a);
        b += 2.0;
        d = an * d + b;
        if (std::abs(d) < fp_min) d = fp_min;
        c = b + an / c;
        if (std::abs(c) < fp_min) c = fp_min;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::abs(del - 1.0) <= eps) break;
    }
    return cworks::exp(-x + a * cworks::log(x) - gln) * h;
}

/// −log B(a, b) = lnΓ(a+b) − lnΓ(a) − lnΓ(b): the whole of the regularized
/// incomplete beta that does not depend on x, and therefore the whole of
/// what a bisection over x can hoist out of its loop. Three log-gammas is
/// most of the cost of one evaluation, and `student_t_quantile_975` used to
/// pay it two hundred times over for one answer.
double neg_log_beta(double a, double b) {
    return log_gamma(a + b) - log_gamma(a) - log_gamma(b);
}

/// I_x(a, b) with `-log B(a, b)` already in hand.
double reg_incomplete_beta_at(double a, double b, double x, double neg_lb) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    const double front = cworks::exp(neg_lb + a * cworks::log(x) + b * cworks::log(1.0 - x));
    if (x < (a + 1.0) / (a + b + 2.0)) return front * beta_cf(a, b, x) / a;
    return 1.0 - front * beta_cf(b, a, 1.0 - x) / b;
}

/// The Student-t CDF with `-log B(df/2, 1/2)` already in hand.
double student_t_cdf_at(double t, double df, double neg_lb) {
    const double x = df / (df + t * t);
    const double tail = 0.5 * reg_incomplete_beta_at(df / 2.0, 0.5, x, neg_lb);
    return t >= 0.0 ? 1.0 - tail : tail;
}

/// Q(a, x) = 1 − P(a, x), evaluated as the upper tail in its own right
/// rather than by subtracting the lower one, so the far tail keeps its
/// significant digits instead of cancelling them against 1.
double reg_incomplete_gamma_q(double a, double x) {
    if (a <= 0.0) return 0.0;
    if (x <= 0.0) return 1.0;
    return x < a + 1.0 ? 1.0 - gamma_series(a, x) : gamma_cf(a, x);
}

/// Ranks of the pooled sample with ties assigned their average rank (1-based).
/// `tie_term` accumulates Σ(t³ − t) over tie groups for tie corrections.
std::vector<double> average_ranks(const std::vector<double>& pooled, double& tie_term) {
    const std::size_t n = pooled.size();
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(),
              [&](std::size_t i, std::size_t j) { return pooled[i] < pooled[j]; });
    std::vector<double> ranks(n, 0.0);
    tie_term = 0.0;
    std::size_t i = 0;
    while (i < n) {
        std::size_t j = i + 1;
        while (j < n && pooled[order[j]] == pooled[order[i]]) ++j;
        const double avg = (static_cast<double>(i + 1) + static_cast<double>(j)) / 2.0;
        for (std::size_t k = i; k < j; ++k) ranks[order[k]] = avg;
        const double t = static_cast<double>(j - i);
        tie_term += t * t * t - t;
        i = j;
    }
    return ranks;
}

} // namespace

double log_gamma(double a) {
    if (!(a > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(a)) return a; // lnΓ(+inf) = +inf
    if (const std::optional<double> exact = log_gamma_closed(a)) return *exact;
    // Recurrence up to where Stirling's series is exact to a double, with
    // the shift accumulated as ONE product and ONE logarithm rather than a
    // logarithm per step. The product of at most twenty factors, each below
    // 20, cannot overflow; and its error is the product's relative rounding,
    // where a sum of logarithms accumulates an ulp of each TERM, which is
    // the larger of the two because the terms are larger than their sum's
    // last place. It is also nineteen deterministic logarithms cheaper.
    double shift = 1.0;
    double z = a;
    while (z < kStirlingFloor) {
        shift *= z;
        z += 1.0;
    }
    return log_gamma_stirling(z) - cworks::log(shift);
}

double reg_incomplete_beta(double a, double b, double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    return reg_incomplete_beta_at(a, b, x, neg_log_beta(a, b));
}

double reg_incomplete_gamma(double a, double x) {
    if (x <= 0.0 || a <= 0.0) return 0.0;
    if (x < a + 1.0) return gamma_series(a, x);
    return 1.0 - gamma_cf(a, x);
}

double normal_cdf(double z) {
    // Φ(z) = ½·erfc(−z/√2), and erfc IS the regularized upper incomplete
    // gamma at a = ½ — erfc(y) = Q(½, y²) for y ≥ 0 — so the error function
    // this file needs is one it already contains. `std::erfc` is one more
    // libm entry point with no correct-rounding guarantee, evaluated on the
    // path that ends at the significance-star ladder, and it is not needed
    // for anything. The argument is formed as z²/2 rather than (z/√2)²,
    // which is the same number with one rounding instead of two.
    if (std::isnan(z)) return z;
    if (std::isinf(z)) return z > 0.0 ? 1.0 : 0.0;
    const double half_tail = 0.5 * reg_incomplete_gamma_q(0.5, 0.5 * z * z);
    return z >= 0.0 ? 1.0 - half_tail : half_tail;
}

double student_t_cdf(double t, double df) {
    if (df <= 0.0) return 0.5;
    return student_t_cdf_at(t, df, neg_log_beta(df / 2.0, 0.5));
}

double f_cdf(double x, double d1, double d2) {
    if (x <= 0.0) return 0.0;
    const double y = d1 * x / (d1 * x + d2);
    return reg_incomplete_beta(d1 / 2.0, d2 / 2.0, y);
}

double chi_square_cdf(double x, double df) {
    if (x <= 0.0) return 0.0;
    return reg_incomplete_gamma(df / 2.0, x / 2.0);
}

double student_t_quantile_975(double df) {
    if (df <= 0.0) return 0.0;
    // Bisection on the CDF for F(t) = 0.975 (one-sided 0.025 upper tail).
    // Deterministic: fixed iteration count over a bracket wide enough for
    // df >= 1, where the 0.975 quantile is 12.706 and decreases with df.
    //
    // Every step evaluates the same I_x(df/2, 1/2); only x moves. Its three
    // log-gammas are therefore computed once here rather than six hundred
    // times inside the loop.
    const double neg_lb = neg_log_beta(df / 2.0, 0.5);
    double lo = 0.0, hi = 1000.0;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (student_t_cdf_at(mid, df, neg_lb) < 0.975)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

Summary summarize(std::span<const double> values) {
    Summary s;
    std::vector<double> v = finite_of(values);
    s.n = v.size();
    if (v.empty()) return s;

    double sum = 0.0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(s.n);

    if (s.n >= 2) {
        double ss = 0.0;
        for (double x : v) {
            const double d = x - s.mean;
            ss += d * d;
        }
        s.sd = std::sqrt(ss / static_cast<double>(s.n - 1));
        s.sem = s.sd / std::sqrt(static_cast<double>(s.n));
        s.ci95_half = student_t_quantile_975(static_cast<double>(s.n - 1)) * s.sem;
    }

    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    s.q1 = quantile_sorted(v, 0.25);
    s.median = quantile_sorted(v, 0.5);
    s.q3 = quantile_sorted(v, 0.75);
    return s;
}

Delta error_bounds(const Summary& s, ErrorMeaning meaning) {
    if (!s.valid()) return {};
    switch (meaning) {
    case ErrorMeaning::StdDev:
        return {s.sd, s.sd};
    case ErrorMeaning::StdError:
        return {s.sem, s.sem};
    case ErrorMeaning::CI95:
        return {s.ci95_half, s.ci95_half};
    case ErrorMeaning::Range:
        return {std::max(0.0, s.mean - s.min), std::max(0.0, s.max - s.mean)};
    case ErrorMeaning::IQR:
        return {std::max(0.0, s.mean - s.q1), std::max(0.0, s.q3 - s.mean)};
    case ErrorMeaning::Custom:
        break;
    }
    return {};
}

TestKind test_kind_from_name(const std::string& name) {
    std::string k;
    k.reserve(name.size());
    for (char c : name)
        if (c != '_' && c != '-' && c != ' ') k += static_cast<char>(std::tolower(c));
    if (k == "welch" || k == "welcht" || k == "t") return TestKind::WelchT;
    if (k == "student" || k == "studentt" || k == "ttest") return TestKind::StudentT;
    if (k == "paired" || k == "pairedt") return TestKind::PairedT;
    if (k == "mannwhitney" || k == "mann" || k == "u" || k == "ranksum" ||
        k == "wilcoxon")
        return TestKind::MannWhitney;
    if (k == "anova" || k == "f") return TestKind::ANOVA;
    if (k == "kruskal" || k == "kruskalwallis" || k == "h") return TestKind::KruskalWallis;
    throw Error(cworks::validation_failed(
        "unknown significance test '" + name +
        "' (welch, student, paired, mann_whitney, anova, kruskal)"));
}

TestResult t_test(std::span<const double> a, std::span<const double> b, bool welch) {
    const Summary sa = summarize(a);
    const Summary sb = summarize(b);
    TestResult r;
    if (sa.n < 2 || sb.n < 2 || (sa.sd == 0.0 && sb.sd == 0.0)) return r;
    const double na = static_cast<double>(sa.n), nb = static_cast<double>(sb.n);
    const double va = sa.sd * sa.sd, vb = sb.sd * sb.sd;
    double se, df;
    if (welch) {
        se = std::sqrt(va / na + vb / nb);
        const double num = (va / na + vb / nb) * (va / na + vb / nb);
        const double den = (va / na) * (va / na) / (na - 1.0) +
                           (vb / nb) * (vb / nb) / (nb - 1.0);
        df = den > 0.0 ? num / den : (na + nb - 2.0);
    } else {
        const double pooled = ((na - 1.0) * va + (nb - 1.0) * vb) / (na + nb - 2.0);
        se = std::sqrt(pooled * (1.0 / na + 1.0 / nb));
        df = na + nb - 2.0;
    }
    if (se <= 0.0) return r;
    r.statistic = (sa.mean - sb.mean) / se;
    r.df = df;
    r.p_value = 2.0 * (1.0 - student_t_cdf(std::abs(r.statistic), df));
    r.p_value = std::clamp(r.p_value, 0.0, 1.0);
    r.valid = true;
    return r;
}

TestResult paired_t_test(std::span<const double> a, std::span<const double> b) {
    TestResult r;
    std::vector<double> diff;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        if (std::isfinite(a[i]) && std::isfinite(b[i])) diff.push_back(a[i] - b[i]);
    if (diff.size() < 2) return r;
    const Summary sd = summarize(diff);
    if (sd.sem <= 0.0) return r;
    r.statistic = sd.mean / sd.sem;
    r.df = static_cast<double>(sd.n - 1);
    r.p_value = std::clamp(2.0 * (1.0 - student_t_cdf(std::abs(r.statistic), r.df)), 0.0, 1.0);
    r.valid = true;
    return r;
}

TestResult mann_whitney_u(std::span<const double> a, std::span<const double> b) {
    TestResult r;
    const std::vector<double> fa = finite_of(a);
    const std::vector<double> fb = finite_of(b);
    const double na = static_cast<double>(fa.size()), nb = static_cast<double>(fb.size());
    if (na < 1.0 || nb < 1.0) return r;
    std::vector<double> pooled = fa;
    pooled.insert(pooled.end(), fb.begin(), fb.end());
    double tie_term = 0.0;
    const std::vector<double> ranks = average_ranks(pooled, tie_term);
    double rank_sum_a = 0.0;
    for (std::size_t i = 0; i < fa.size(); ++i) rank_sum_a += ranks[i];
    const double u_a = rank_sum_a - na * (na + 1.0) / 2.0;
    const double u = std::min(u_a, na * nb - u_a);
    const double mean_u = na * nb / 2.0;
    const double nn = na + nb;
    const double var_u =
        (na * nb / 12.0) * ((nn + 1.0) - tie_term / (nn * (nn - 1.0)));
    if (var_u <= 0.0) return r;
    // Continuity-corrected normal approximation (two-sided).
    const double z = (std::abs(u - mean_u) - 0.5) / std::sqrt(var_u);
    r.statistic = u;
    r.p_value = std::clamp(2.0 * (1.0 - normal_cdf(std::max(0.0, z))), 0.0, 1.0);
    r.valid = true;
    return r;
}

TestResult one_way_anova(const std::vector<std::vector<double>>& groups) {
    TestResult r;
    std::vector<Summary> gs;
    double grand_sum = 0.0, total_n = 0.0;
    std::size_t k = 0;
    for (const auto& g : groups) {
        const Summary s = summarize(g);
        if (s.n == 0) continue;
        gs.push_back(s);
        grand_sum += s.mean * static_cast<double>(s.n);
        total_n += static_cast<double>(s.n);
        ++k;
    }
    if (k < 2 || total_n <= static_cast<double>(k)) return r;
    const double grand_mean = grand_sum / total_n;
    double ss_between = 0.0, ss_within = 0.0;
    for (const auto& s : gs) {
        const double n = static_cast<double>(s.n);
        ss_between += n * (s.mean - grand_mean) * (s.mean - grand_mean);
        ss_within += (n - 1.0) * s.sd * s.sd;
    }
    const double df1 = static_cast<double>(k) - 1.0;
    const double df2 = total_n - static_cast<double>(k);
    if (df2 <= 0.0 || ss_within <= 0.0) return r;
    const double ms_between = ss_between / df1;
    const double ms_within = ss_within / df2;
    r.statistic = ms_between / ms_within;
    r.df = df2;
    r.p_value = std::clamp(1.0 - f_cdf(r.statistic, df1, df2), 0.0, 1.0);
    r.valid = true;
    return r;
}

TestResult kruskal_wallis(const std::vector<std::vector<double>>& groups) {
    TestResult r;
    std::vector<std::vector<double>> finite_groups;
    std::vector<double> pooled;
    for (const auto& g : groups) {
        std::vector<double> fg = finite_of(g);
        if (fg.empty()) continue;
        pooled.insert(pooled.end(), fg.begin(), fg.end());
        finite_groups.push_back(std::move(fg));
    }
    const std::size_t k = finite_groups.size();
    const double nn = static_cast<double>(pooled.size());
    if (k < 2 || nn < 2.0) return r;
    double tie_term = 0.0;
    const std::vector<double> ranks = average_ranks(pooled, tie_term);
    double h = 0.0;
    std::size_t offset = 0;
    for (const auto& g : finite_groups) {
        double rank_sum = 0.0;
        for (std::size_t i = 0; i < g.size(); ++i) rank_sum += ranks[offset + i];
        offset += g.size();
        h += rank_sum * rank_sum / static_cast<double>(g.size());
    }
    h = 12.0 / (nn * (nn + 1.0)) * h - 3.0 * (nn + 1.0);
    const double correction = 1.0 - tie_term / (nn * nn * nn - nn);
    if (correction > 0.0) h /= correction;
    const double df = static_cast<double>(k) - 1.0;
    r.statistic = h;
    r.df = df;
    r.p_value = std::clamp(1.0 - chi_square_cdf(h, df), 0.0, 1.0);
    r.valid = true;
    return r;
}

TestResult run_pairwise(TestKind kind, std::span<const double> a, std::span<const double> b) {
    switch (kind) {
    case TestKind::StudentT:
        return t_test(a, b, /*welch=*/false);
    case TestKind::WelchT:
        return t_test(a, b, /*welch=*/true);
    case TestKind::PairedT:
        return paired_t_test(a, b);
    case TestKind::MannWhitney:
        return mann_whitney_u(a, b);
    case TestKind::ANOVA:
    case TestKind::KruskalWallis:
        break;
    }
    throw Error(cworks::validation_failed(
        "a significance bracket needs a pairwise test (welch, student, paired, "
        "mann_whitney); anova/kruskal are omnibus tests over all groups"));
}

std::string significance_stars(double p) {
    if (!(p >= 0.0)) return "ns"; // NaN / invalid
    if (p < 0.0001) return "****";
    if (p < 0.001) return "***";
    if (p < 0.01) return "**";
    if (p < 0.05) return "*";
    return "ns";
}

} // namespace cplot::stats
