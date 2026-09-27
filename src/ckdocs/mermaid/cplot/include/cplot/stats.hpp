// ckplot — deterministic descriptive statistics and hypothesis tests
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// A small, self-contained statistics layer used to turn raw replicate
// observations into the summaries a scientific chart needs: means with
// standard-deviation / standard-error / confidence-interval error bars, and
// the significance tests that annotate the brackets between groups.
//
// Everything here is deterministic and byte-reproducible: sums accumulate in
// index order, the special functions (regularized incomplete beta and gamma)
// use fixed-iteration continued fractions, and no source of randomness is
// used. Non-finite observations (NaN) are ignored — null never counts as a
// value and never propagates as zero.
//
// Deterministic here also means NO libm TRANSCENDENTAL, which is a stronger
// statement than it sounds. IEEE-754 pins the rounding of `+ - * /` and
// `sqrt` and of nothing else, so `lgamma`, `erfc`, `exp` and `log` are free
// to differ in their last bit between two C libraries — and a p-value does
// not end on a coordinate grid that can absorb that. It ends at
// significance_stars(), a threshold ladder at 0.05, where one bit is the
// difference between printing "*" and printing "ns". So the log-gamma is
// this file's own (see log_gamma below), the error function is the
// incomplete gamma it already contained, and the exponential and logarithm
// are cworks::exp and cworks::log.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "series.hpp" // ErrorMeaning

namespace cplot::stats {

/// Descriptive summary of one sample. `n` counts finite observations only.
/// `sd` is the sample standard deviation (n − 1 denominator); `sem` is the
/// standard error of the mean; `ci95_half` is the half-width of the 95%
/// confidence interval of the mean (t-based, so correct for small n).
/// Quartiles use the type-7 (linear interpolation) definition, matching the
/// box-plot engine so a box and a bar-with-IQR-error agree on the same data.
struct Summary {
    std::size_t n = 0;
    double mean = 0.0;
    double sd = 0.0;
    double sem = 0.0;
    double min = 0.0;
    double max = 0.0;
    double q1 = 0.0;
    double median = 0.0;
    double q3 = 0.0;
    double ci95_half = 0.0;
    bool valid() const { return n > 0; }
};

/// Summarize a sample; non-finite entries are skipped.
Summary summarize(std::span<const double> values);

/// Lower/upper error deltas (both ≥ 0) measured from the mean, for the given
/// error meaning. Symmetric meanings (SD, SEM, CI95) return equal lower and
/// upper; Range and IQR are asymmetric about the mean. Custom returns {0,0}
/// (the caller supplies the magnitude directly).
struct Delta {
    double lower = 0.0;
    double upper = 0.0;
};
Delta error_bounds(const Summary& s, ErrorMeaning meaning);

// -- distribution functions ------------------------------------------------
// Regularized incomplete beta and gamma, and the CDFs built on them. All are
// deterministic; accuracy is ~1e-10 which is far tighter than any reported
// p-value needs.

/// Natural logarithm of the gamma function, lnΓ(a) for a > 0 (NaN below).
///
/// Not `std::lgamma`, and the difference is not academic: lnΓ is upstream of
/// every p-value here, a p-value ends at a threshold ladder, and a ladder
/// turns a last-bit difference between two C libraries into a different word
/// printed on the chart. This one is built from exact rationals and the
/// suite's own deterministic logarithm, so it gives the same bits
/// everywhere. It is EXACT in the sense that matters at a half-integer
/// argument — Γ(n) = (n−1)! and Γ(n+½) = (2n−1)!!·√π/2^n are formed without
/// rounding up to a = 23 and a = 15.5 — and it carries no global state, in
/// contrast to POSIX `lgamma`, which writes `signgam`.
double log_gamma(double a);

/// Regularized incomplete beta function I_x(a, b), x in [0, 1].
double reg_incomplete_beta(double a, double b, double x);
/// Regularized lower incomplete gamma P(a, x), x ≥ 0.
double reg_incomplete_gamma(double a, double x);

/// Standard-normal CDF Φ(z).
double normal_cdf(double z);
/// Student-t CDF with `df` degrees of freedom.
double student_t_cdf(double t, double df);
/// F-distribution CDF with (d1, d2) degrees of freedom.
double f_cdf(double x, double d1, double d2);
/// Chi-square CDF with `df` degrees of freedom.
double chi_square_cdf(double x, double df);
/// Two-sided upper 0.975 quantile of Student-t (the CI95 multiplier).
double student_t_quantile_975(double df);

// -- hypothesis tests ------------------------------------------------------

/// Which two-sample or omnibus test to run for a significance annotation.
enum class TestKind {
    StudentT,     ///< two-sample t-test, equal variances assumed
    WelchT,       ///< two-sample t-test, unequal variances (default, robust)
    PairedT,      ///< paired t-test (equal-length, matched samples)
    MannWhitney,  ///< Mann–Whitney U (rank-sum), normal approximation
    ANOVA,        ///< one-way ANOVA omnibus F-test (≥ 2 groups)
    KruskalWallis ///< Kruskal–Wallis omnibus H-test (≥ 2 groups)
};

/// Result of a test. `p_value` is two-sided for the pairwise tests and
/// upper-tail for the omnibus tests. `valid` is false when the inputs were
/// too small or degenerate to compute (e.g. zero variance, n < 2).
struct TestResult {
    double statistic = 0.0;
    double p_value = 1.0;
    double df = 0.0; ///< degrees of freedom (df2 for ANOVA)
    bool valid = false;
};

/// Parse a test name ("welch", "student", "t", "paired", "mann_whitney" /
/// "mannwhitney" / "u", "anova", "kruskal"). Throws cplot::Error on an
/// unknown name so the config layer can surface a clear diagnostic.
TestKind test_kind_from_name(const std::string& name);

/// Two-sample t-test; `welch` selects the unequal-variance (Welch) form.
TestResult t_test(std::span<const double> a, std::span<const double> b, bool welch);
/// Paired t-test over matched observations (non-finite pairs dropped).
TestResult paired_t_test(std::span<const double> a, std::span<const double> b);
/// Mann–Whitney U with tie correction, asymptotic (normal) p-value.
TestResult mann_whitney_u(std::span<const double> a, std::span<const double> b);
/// One-way ANOVA omnibus F-test.
TestResult one_way_anova(const std::vector<std::vector<double>>& groups);
/// Kruskal–Wallis omnibus H-test (tie-corrected, chi-square approximation).
TestResult kruskal_wallis(const std::vector<std::vector<double>>& groups);

/// Run a pairwise test selected by kind (throws on an omnibus kind).
TestResult run_pairwise(TestKind kind, std::span<const double> a, std::span<const double> b);

/// GraphPad-style star code for a two-sided p-value:
/// p < 0.0001 → "****", < 0.001 → "***", < 0.01 → "**", < 0.05 → "*",
/// otherwise "ns". Robust to tiny floating-point differences except exactly
/// on a threshold.
std::string significance_stars(double p);

} // namespace cplot::stats
