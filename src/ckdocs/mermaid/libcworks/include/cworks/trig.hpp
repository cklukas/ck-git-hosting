// libcworks — deterministic trigonometry
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Sine, cosine, tangent and the arc functions that give the SAME BITS on
// every platform — in DEGREES for everything that draws, and in RADIANS for
// the one kind of caller that genuinely works in them.
//
// WHY THIS EXISTS. The suite's central promise is that a document
// regenerates itself byte for byte from its sources, on every machine that
// builds it — which is why `-ffp-contract=off` is set suite-wide
// (`cmake/CWorksSettings.cmake`, "bit-identical floating point across"
// architectures). Ordinary IEEE arithmetic honours that promise: `+`, `-`,
// `*`, `/` and `sqrt` are exactly specified operations, so a fixed sequence
// of them yields a fixed result everywhere. libm's transcendentals do not:
// `sin`, `cos` and `atan2` are accurate to within an ulp but are **not
// bit-pinned** across platforms, versions, or even optimisation levels, and
// every figure with a non-right angle passes through them. Hoping that a
// downstream two-decimal coordinate formatter absorbs the difference is not
// a contract — it holds until a value lands on a rounding boundary, and
// "unlikely" is not the standard this suite sets for itself.
//
// So the suite owns the routine. It is built from IEEE double operations
// only, in a fixed order, and it never calls libm — therefore it is
// bit-identical everywhere **by construction** rather than by measurement.
//
// DEGREES FIRST, AND FOR EVERYTHING THAT DRAWS. A chart's sector sweep, a
// polar axis, a turtle's heading, a rotated label: every one of them is
// stated in degrees, and every one of them is better off staying there.
// `sin_deg(90)` is exactly 1, while `std::sin(M_PI/2)` cannot be, because
// π/2 is not a double — and a caller who writes `cos(89 * M_PI / 180)` has
// already lost about 25 ulp to the rounding of the argument before any
// library sees it, since the cosine is small there and its argument is not.
//
// **Converting degrees to radians in order to reach a radian entry point
// below is therefore a defect, not a style choice.** It throws away the one
// property the degree functions exist to have. The radian half of this
// header is for callers whose ARGUMENT is a radian value in the first place:
// a formula language whose `SIN` is specified in radians, an evaluator
// implementing somebody else's semantics. Those callers are real, and
// leaving them on the platform's libm — where the last place follows
// whichever library the host shipped — is what this half removes.
//
// WHAT THE TWO HALVES COST, because the difference is the whole of the
// design. Degrees fold modulo 360 EXACTLY, so a degree entry point performs
// NO argument reduction error whatsoever. Radians cannot: π/2 is
// irrational, "x modulo π/2" is a question about the binary expansion of
// 2/π, and answering it is what the radian half spends its time on
// (Payne-Hanek, against a committed 1536-bit expansion). The consequences
// are stated on each function below and they are not the same.
//
// HOW IT WORKS, and why each step keeps the result exact:
//
//   1. **Reduction modulo 360 is exact.** The argument is reduced by
//      repeatedly subtracting the largest power-of-two multiple of 360 that
//      fits. Each such subtraction satisfies Sterbenz's condition
//      (b ≤ a < 2b), so every step is exact and no argument reduction error
//      exists at all — the classic accuracy trap of a naive `sin` for large
//      arguments is structurally absent.
//   2. **Quadrant folding is exact**, on degrees: subtracting 90 from a
//      value below 360 is exact, and so is `90 - r` for r in [45, 90]
//      (Sterbenz again). The sign and swap rules are then pure bookkeeping.
//      This is what makes the axis directions exactly (±1, 0) and (0, ±1)
//      instead of 6.1e-17 off, so an axis-aligned figure lands on exact
//      coordinates and closes exactly.
//   3. **The kernel runs on [0°, 45°]**, where the degree→radian
//      conversion is carried as a double-double so the reduced argument is
//      good to ~2e-33, the leading terms of each series are evaluated in
//      double-double as well, and the series run far enough that the
//      truncation term is below 1e-19. Only the small tail correction is
//      left to plain double, so the final rounding is very nearly the only
//      error. Measured against a reference computed offline at 80 decimal
//      digits, the result is **faithfully rounded — worst case 1 ulp** —
//      and is in fact the *correctly* rounded double for every one of the
//      1736 arguments in `libcworks/tests/trig_corpus.inc` and for 99.94%
//      of a 120 000-argument randomized sweep. That is at or above the
//      accuracy of the libms this replaces, and unlike them it is the same
//      answer on every platform.
//
//      Working in degrees is worth accuracy as well as determinism: the
//      caller who writes `std::cos(89.0 * M_PI / 180.0)` has already lost
//      about 25 ulp to the rounding of the radian argument before libm sees
//      it, because cos is small there and its argument is not. `cos_deg(89)`
//      loses none.
//   4. **Exact values are returned exactly** where one exists: 0°, 30°,
//      45°, 60°, 90° and every reflection of them, so `sin_deg(30)` is
//      0.5 and not one ulp below it.
//
// NEGATIVE ZERO IS NORMALISED AWAY, in every result here and in the operands
// of both `atan2`s: `-0` differs from `0` in printed output while being the
// same number, which is exactly the kind of gratuitous difference a
// byte-exact golden must not have to tolerate. This is the one place these
// functions deliberately depart from Annex F, which would have
// `atan2(-0.0, -1.0)` be -π and `sin(-0.0)` be -0. It is a module-wide rule
// rather than a special case, and it is stated once here.
//
// WHERE THIS BELONGS. Here, not in a drawing library: it is pure numerics
// with no graphics types, it is needed by more than one component (the
// Charting Engine's sector, polar, radar and wind-rose geometry, the
// Turtle Graphics Engine's headings, and any formula language whose
// trigonometry is specified in radians), and it exists for the same reason
// `format_double` does — determinism the platform will not give us. It
// sits beside the other primitives that carry that burden.
#pragma once

namespace cworks {

// All of these are pure: no state, no allocation, no locale, no errno, and
// nothing that can throw.

// -- degrees ------------------------------------------------------------------

/// Sine of `degrees`. Non-finite input yields NaN.
double sin_deg(double degrees) noexcept;

/// Cosine of `degrees`. Non-finite input yields NaN.
double cos_deg(double degrees) noexcept;

/// Both at once — one reduction, one kernel evaluation. Prefer this where
/// both are wanted (a direction vector, a rotation matrix): it is not only
/// faster, it guarantees the pair is consistent, computed from one and the
/// same reduced argument.
void sincos_deg(double degrees, double& sin_out, double& cos_out) noexcept;

/// The angle of the vector (`x`, `y`) in degrees, in (-180, 180], with the
/// argument order and the quadrant conventions of `atan2`. Exact on the
/// axes and the diagonals (0, ±45, ±90, ±135, 180). A NaN operand yields
/// NaN; infinities follow the IEEE limiting cases.
double atan2_deg(double y, double x) noexcept;

// -- radians ------------------------------------------------------------------
//
// DETERMINISTIC AND FAITHFUL — within one ulp of the true value, and the
// same one ulp on every platform — but NOT claimed to be correctly rounded,
// and versioned by `kMathKernelVersion` in `cworks/math.hpp` so that the day
// that changes is a decision with a paper trail rather than a golden that
// quietly moved. The degree entry points above make the same one-ulp
// contract and share the same series; what the two do not share is the
// argument reduction, and this is the half where reduction is the hard part.
//
// FULL ACCURACY AT EVERY FINITE ARGUMENT, WITH NO CUT-OFF AND NO DEGRADED
// RANGE. This is worth stating plainly because it is the property most
// implementations of radian trigonometry do not have, and because a caller
// has no way to notice its absence: a naive reduction returns a plausible
// number for `sin(1e300)` and it is wrong in every digit.
//
// The reduction is Payne and Hanek's — the argument's exact significand
// multiplied by a window of a committed 1536-bit expansion of 2/π, in
// integer arithmetic — so it never subtracts nearly equal quantities and
// never runs out of bits. The hardest argument a double can present loses
// 61 bits of the reduction to cancellation (it is near 2^797, where x·2/π
// falls within 2^-61 of an integer); 192 bits are computed, so 131 remain,
// and the reduced argument reaches the series with more than a
// double-double's worth of accuracy whatever the argument was. There is
// therefore no threshold above which these functions do something else, and
// no range outside which they should not be used. A non-finite argument is
// the only case they refuse, with a NaN.
//
// (For anything that draws, this accuracy is still not a reason to convert
// degrees to radians. `sin_deg(90)` is exactly 1 and `sin(x)` for any double
// x cannot be, because no double is π/2.)
//
// WHAT THE ONE-ULP CONTRACT ACTUALLY DELIVERS, measured rather than argued.
// Every value of the conformance corpus is the correctly rounded one — 979
// arguments each for `sin`, `cos` and `tan`, 561 for `atan`, 287 for
// `atan2`, 293 for `asin` and `acos`, all at zero ulp except two of the
// `asin` cases. On randomized sweeps run offline against the same reference,
// with arguments drawn across the WHOLE exponent range so that nearly all of
// them need the full reduction: `sin` 99.925% correctly rounded over 40 000
// arguments, `cos` 99.950%, `tan` 99.880%, `atan` 99.995% over 60 000,
// `asin` 99.982% and `acos` 99.990% over 40 000. Worst case in every one of
// them: ONE ulp, which is the contract.
//
// Over 40 000 arguments confined to [2^-40, 2^11] instead — where an
// ordinary caller lives — the same three read 99.980%, 99.990% and 99.970%.
// The gap between the two sweeps is the entire cost of reducing an enormous
// argument, and it is five hundredths of a percent. What is left in both is
// the price of a series whose last term is evaluated in plain double: 2^-61
// relative, an eighth of a thousandth of an ulp, which decides the last bit
// only when the true value is that close to a boundary.
//
// WHAT IT COSTS. Optimized, on a current desktop: `sin` and `cos` about
// 28 ns for an argument already inside [-π/4, π/4], where the reduction is
// the identity, and about 130 ns above it, which is what Payne-Hanek costs;
// `tan` about 58 ns and 150 ns on the same split; `atan` about 85 ns,
// `asin` and `acos` about 150 ns. Beside them the degree entry points are
// 41 ns for an ordinary angle and 75 ns for one that needs several folds.
// The platform's own are 3 to 25 ns. That is the price of an answer that
// does not depend on which library the host shipped, and it is the same
// order of magnitude the suite already pays on every drawn coordinate.

/// Sine of `radians`. Non-finite input yields NaN.
double sin(double radians) noexcept;

/// Cosine of `radians`. Non-finite input yields NaN.
double cos(double radians) noexcept;

/// Tangent of `radians`. Non-finite input yields NaN.
///
/// Formed as the quotient of the sine and cosine of ONE reduced argument,
/// each carried to twice the working precision, so the quotient is rounded
/// once rather than compounding two roundings that were already made. Near a
/// pole the result is large but finite: no double is π/2, so no argument
/// makes the cosine exactly zero.
double tan(double radians) noexcept;

/// The arctangent in radians, in [-π/2, π/2]. `atan(1)` is the correctly
/// rounded π/4 and `atan(±inf)` the correctly rounded ±π/2.
double atan(double x) noexcept;

/// The angle of the vector (`x`, `y`) in radians, in (-π, π], with the
/// argument order and the quadrant conventions of `atan2`. The axes and the
/// diagonals give the correctly rounded π, π/2 and π/4; a NaN operand yields
/// NaN; infinities follow the IEEE limiting cases.
double atan2(double y, double x) noexcept;

/// The arcsine in radians, in [-π/2, π/2]. NaN outside [-1, 1]. `asin(0)` is
/// exactly 0 and `asin(±1)` the correctly rounded ±π/2.
///
/// Derived, from `sqrt((1-x)(1+x))` and the arctangent, rather than given a
/// series of its own — and derived in DOUBLE-DOUBLE, which is what makes the
/// derivation free rather than expensive. The two forms are worth
/// distinguishing because the obvious one is neither:
///
///   * `sqrt(1 - x*x)` loses half its significant digits as |x| approaches
///     1, because `1 - x*x` cancels. The factored complement does not: both
///     differences are formed EXACTLY, so the cancellation happens in exact
///     arithmetic and nothing is lost.
///   * Computing the complement, the ratio and the angle in plain double
///     would spend a rounding on each, and cost about two ulp. Carrying all
///     three at twice the working precision costs 2^-104, which is a
///     thousand times below what the series already contributes — so the
///     measured accuracy of `asin` and `acos` is that of the arctangent
///     itself, not worse.
///
/// What is NOT claimed is monotonicity between adjacent arguments: a
/// function that is faithful rather than correctly rounded may return the
/// upper of two admissible answers at one argument and the lower at the
/// next, and a caller that needs a monotone inverse must not assume one.
double asin(double x) noexcept;

/// The arccosine in radians, in [0, π]. NaN outside [-1, 1]. `acos(1)` is
/// exactly 0, `acos(-1)` the correctly rounded π, `acos(0)` the correctly
/// rounded π/2. Derived like `asin`, and carrying the same cost.
double acos(double x) noexcept;

} // namespace cworks
