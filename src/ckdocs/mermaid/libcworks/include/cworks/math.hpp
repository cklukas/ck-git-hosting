// libcworks — deterministic elementary functions
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Powers of ten, decades, the hypotenuse, the exponential, the three
// logarithms, the general power, and the decimal-grid rounding rule built on
// the first two — each giving the SAME BITS on every platform.
//
// WHY THIS EXISTS. The suite's central promise is that a document
// regenerates itself byte for byte from its sources, on every machine that
// builds it — which is why `-ffp-contract=off` is set suite-wide
// (`cmake/CWorksSettings.cmake`, "bit-identical floating point across"
// architectures). The promise holds for ordinary arithmetic and it does not
// hold for libm:
//
//   **IEEE-754 requires correct rounding for `+`, `-`, `*`, `/` and `sqrt`,
//   and for nothing else.** Every other libm entry point — the
//   transcendentals, and `hypot` — is quality of implementation. glibc,
//   musl, Apple's libm and the Microsoft CRT each round differently, each
//   version of each of them may round differently again, and a value that
//   differs in its last bit is a coordinate that differs in its last bit,
//   a tick that lands on the other side of a rounding ladder, a p-value
//   that prints a star on one platform and none on another.
//
// So the suite owns these routines, as it already owns its degree
// trigonometry (`cworks/trig.hpp`). They are built from IEEE double
// operations in a fixed order and call no libm function that has any
// freedom in how it rounds — therefore they are bit-identical everywhere
// **by construction** rather than by measurement or by hope.
//
// DETERMINISM IS THE REQUIREMENT; CORRECT ROUNDING IS TAKEN WHERE IT IS
// CHEAP. A routine assembled from exactly specified operations is
// reproducible whatever its accuracy. Accuracy is a separate, second goal —
// worth having because it removes a class of future argument, and worth
// stating honestly rather than claiming loosely. What each function here
// actually guarantees is written on it below, and the distinction between
// "correctly rounded, by construction" and "correctly rounded on every
// argument yet measured" is never blurred.
//
// WHAT EACH ONE IS BUILT FROM:
//
//   * `pow10` and `floor_log10` READ A COMMITTED TABLE. Not one
//     floating-point operation stands between the argument and the answer —
//     a power of ten is fetched, and a decade is decided by comparing the
//     argument against the boundary, so no rounding decision exists to get
//     wrong or to disagree about.
//   * `hypot` decides its last bit with 192-bit INTEGER arithmetic on the
//     exact value of x^2 + y^2, so it is correctly rounded by construction —
//     including the exact ties that Pythagoras makes reachable, and
//     including the subnormal range, where it rounds once rather than
//     scaling a rounded value into a coarser grid. (Measured anyway, on
//     200 000 randomized pairs against an exact rational reference: zero
//     ulp, as construction says it must be.)
//   * `round_decimal` IS NOT A KERNEL — it is the one decimal-grid rounding
//     RULE the suite's languages share, built on the two above. Rounding to
//     a number of decimal places means rounding to a multiple of a power of
//     ten, and every way of getting that wrong is a way of getting a printed
//     number wrong. It lives here because more than one component needs the
//     identical rule and the rule needs no type either of them owns.
//   * `exp`, `log`, `log2` and `log10` reduce their argument with committed
//     tables and evaluate a double-double kernel accurate to about 2^-88
//     relative — 2^-35 of one ulp. They are FAITHFULLY ROUNDED with a very
//     large margin, and the correctly rounded value is what they actually
//     return: on all 3981 arguments of the conformance corpus, and on
//     1 200 000 randomized arguments measured offline against a 120-digit
//     reference (200 000 each over the full range of `exp`, over small
//     arguments of `exp`, over the full range of `log`, over `log` within
//     1e-3 of 1 where the cancellation is worst, and over `log2` and
//     `log10`), the worst distance observed was ZERO ulp.
//
//     They are nonetheless NOT PROVEN correctly rounded, and this header
//     will not claim it: the hardest arguments for these functions in
//     double precision sit closer to a rounding boundary than any
//     fixed-precision kernel can resolve, and settling the question would
//     need a multi-precision fallback this suite has no use for. The
//     contract is therefore one ulp. Determinism — the property the suite
//     actually needs — is unaffected either way: the answer is the same on
//     every platform whichever side of a boundary it falls.
//   * `pow` is the one function here that is only FAITHFUL, and it says so
//     rather than borrowing the measured record of the four above. It is
//     also the one that is VERSIONED — see `kMathKernelVersion` — because a
//     future accuracy change to it must be a decision with a paper trail
//     instead of a golden that quietly moved.
//
// WHAT IT COSTS, because a caller converting a hot path is entitled to know
// before measuring. Optimized, on a current desktop: `floor_log10` about
// 4 ns (a table read and a comparison, next to nothing), `hypot` about
// 32 ns, `exp` about 89 ns, `log` about 116 ns, `pow` about 310 ns — against
// 2 to 3 ns for the platform's own, which is what a routine hand-written for
// the chip in double precision costs and what a correctly rounded
// double-double kernel cannot approach. The comparison to draw is with
// `trig.hpp`'s 41 ns, which this suite already pays on every drawn
// coordinate: these are the same order of magnitude and the same trade. A
// per-point conversion on a very large series is still worth measuring
// first.
//
// `pow` is the expensive one because it is a logarithm and an exponential
// end to end — there is no shorter way to a general power that keeps the
// exponent accurate to 2^-80. Its exact paths are far cheaper (an integral
// exponent on a power-of-two base is a handful of integer operations), and a
// caller whose exponent is a fixed small integer or a decade should reach
// for `pow10` or a multiplication rather than for this.
//
// EXACT WHERE AN EXACT ANSWER EXISTS, which matters more than the last bit
// of accuracy because these are the arguments real documents contain:
// `pow10(3)` is 1000, `hypot(3, 4)` is 5, `exp(0)` is 1, `log(1)` is +0,
// `log2` of any power of two is that exponent, `log10` of any power of ten
// a double can hold is that exponent, `pow(2, 10)` is 1024 and `pow(3, 5)`
// is 243 — decided by integer arithmetic before any kernel runs.
//
// WHERE THIS BELONGS. Here, beside `trig.hpp` and `format_double`: pure
// numerics with no graphics and no document types, needed by more than one
// component, existing for the one reason those do — determinism the
// platform will not give us.
#pragma once

#include <cstdint>

namespace cworks {

// All of these are pure: no state, no allocation, no locale, no errno, no
// rounding-mode changes, and nothing that can throw. Round-to-nearest,
// ties-to-even is assumed, as it is everywhere else in the suite.

/// 10^k. EXACT for 0 <= k <= 22 — the whole range in which a power of ten
/// is a double — and the correctly rounded double for every other k, read
/// from a committed table. Saturates: +0 below 10^-323, +inf above 10^308.
///
/// (Powers of ten with a negative exponent are NOT exact and cannot be:
/// 0.1 is not a double. The value returned is the nearest one.)
double pow10(int k) noexcept;

/// The unique integer k with 10^k <= x < 10^(k+1).
///
/// Decided by exact comparison against a committed table of decade
/// boundaries — no logarithm is evaluated, so there is no rounding decision
/// to get wrong and nothing for a platform to disagree about. The
/// boundaries that are not themselves representable are handled by the same
/// comparison: `floor_log10(1e23)` is 22, because the double written `1e23`
/// is slightly BELOW 10^23 and the answer to the question asked is 22.
///
/// `INT_MIN` for x <= 0 and for a non-finite x — a decade is not defined
/// there, and a caller who ignores the check gets an obviously wrong number
/// rather than a plausible one.
int floor_log10(double x) noexcept;

/// Which way `round_decimal` sends the part below the last kept digit.
///
/// The five are genuinely different rules, not spellings of one: the two
/// MAGNITUDE directions treat -1.5 and 1.5 alike, the two SIGN directions
/// do not, and a caller that confuses them changes answers for negative
/// numbers only — the failure mode that survives a test suite. Each is
/// named for what it does to the value, never for a function that happens
/// to implement it, because `floor` means one thing in a spreadsheet and
/// the other in a programming language.
enum class DecimalRounding {
    Nearest,       ///< nearest multiple; a TIE GOES AWAY FROM ZERO
    TowardZero,    ///< the magnitude shrinks: 2.7 -> 2, -2.7 -> -2
    AwayFromZero,  ///< the magnitude grows: 2.1 -> 3, -2.1 -> -3
    TowardNegative,///< toward -infinity: 2.7 -> 2, -2.1 -> -3
    TowardPositive,///< toward +infinity: 2.1 -> 3, -2.7 -> -2
};

/// `x` rounded to `digits` decimal places — to a multiple of 10^-digits,
/// with a NEGATIVE `digits` rounding to tens, hundreds and beyond.
///
/// The suite's one decimal-rounding rule. The spreadsheet's ROUND /
/// ROUNDUP / ROUNDDOWN / TRUNC and the table expression language's
/// round / floor / ceil and fixed-decimal formatting are all this
/// function under different names, differing only in which
/// `DecimalRounding` they select and in what range of `digits` their own
/// grammar accepts. That is the whole of the difference, and keeping it a
/// parameter is what stops two languages drifting apart in the last
/// place.
///
/// DETERMINISTIC, which the obvious spelling is not. The scale is
/// `pow10`, read from a committed table rather than obtained from
/// `std::pow(10.0, digits)`, which IEEE-754 leaves free to miss by an ulp
/// and which therefore makes ROUND(2.5, 0) print 3.0000000000000004 on a
/// libm that does. It is always the POSITIVE decade, multiplied in for
/// `digits >= 0` and divided out below it: 10^-5 is not a double, so
/// multiplying by the reciprocal both rounds twice and starts from the
/// wrong number — that is what made ROUNDUP(1, -5) come back as
/// 99999.999999999985. The decade of `x` likewise comes from
/// `floor_log10`, an exact table comparison, so no logarithm is evaluated
/// anywhere in this function.
///
/// The cases that decide whether two implementations are the same rule:
///
///   * TIES go away from zero, in `Nearest` only. That is the rule
///     spreadsheets state, and it is a rule about the SCALED value: 2.675
///     is not a tie in binary, but 2.675 * 100 is exactly 267.5, so
///     rounding it to two places gives 2.68.
///   * TOO FINE A GRID returns `x` UNCHANGED. Once |x|*10^digits reaches
///     10^16 the requested step is below 0.91 ulp(x): for `Nearest` the
///     nearest multiple is then within 0.46 ulp and `x` IS the correctly
///     rounded answer, and for the four directed modes the exact answer
///     lies strictly between `x` and its neighbour, where no double
///     expresses "one step". Returning `x` is not a shortcut for speed —
///     scaling is what introduces the error, and the round trip through
///     10^16 is what used to turn ROUND(100000.28571428571, 11) into
///     ...72. It is also what keeps a rounding of 1e300 from scaling a
///     finite value into an infinity.
///   * TOO COARSE A GRID for the value to survive is answered exactly:
///     when the scaled magnitude underflows to zero the true value is
///     still a nonzero infinitesimal of x's sign, so `AwayFromZero` (and
///     `TowardPositive` above zero, `TowardNegative` below it) returns the
///     first grid step rather than zero.
///   * ANY `digits` is accepted. Beyond the range where a decade can
///     change the answer it saturates, so there is no undefined corner and
///     no overflow in forming the scale; a language that wants a narrower
///     domain rejects the argument itself rather than relying on this.
///   * NON-FINITE and ZERO are their own answer at every `digits`, sign
///     included: -0.0 stays -0.0, NaN stays NaN, an infinity stays itself.
///   * The result may be an INFINITY when rounding a finite number away
///     from zero leaves the double range — ROUNDUP(1e308, -309) is 10^309.
///     The caller decides what that means in its language.
double round_decimal(double x, std::int64_t digits, DecimalRounding mode) noexcept;

/// sqrt(x^2 + y^2), without the intermediate overflow and underflow that
/// writing it that way would bring: the answer is ordinary long after the
/// squares have stopped being representable. Correctly rounded, and
/// therefore exact whenever the true value is a double: `hypot(3, 4)` is 5.
///
/// Annex F to the letter: `hypot(±inf, y)` is +inf for EVERY y, including a
/// NaN; otherwise a NaN operand yields NaN. The result is never negative
/// and never a negative zero.
double hypot(double x, double y) noexcept;

/// e^x. `exp(0)` is exactly 1. Overflows to +inf and underflows through the
/// subnormal range to +0, both with the last bit rounded as carefully as
/// the normal range's.
double exp(double x) noexcept;

/// The natural logarithm. `log(1)` is exactly +0; `log(0)` is -inf; a
/// negative argument yields NaN.
double log(double x) noexcept;

/// The binary logarithm. EXACT on every power of two, down to the subnormal
/// ones: `log2(1024)` is 10 and `log2(0x1p-1074)` is -1074. That is a
/// stronger property than dividing `log(x)` by ln(2) can offer, and it is
/// the one a caller who is reasoning about magnitudes actually wants.
double log2(double x) noexcept;

/// The decimal logarithm. EXACT on every power of ten a double can hold:
/// `log10(1000)` is 3, for k in [0, 22]. Elsewhere correctly rounded on
/// everything measured — note that this makes `log10(1e23)` exactly 23,
/// because the true logarithm of the double written `1e23` is within a
/// thousandth of an ulp of 23.
double log10(double x) noexcept;

/// x raised to the power y. DETERMINISTIC and FAITHFUL — within one ulp of
/// the true value, and the same one ulp on every platform. NOT claimed to be
/// correctly rounded, and versioned (`kMathKernelVersion`) so that the day
/// it becomes so is a decision rather than an accident.
///
/// WHAT THAT MEANS IN PRACTICE, since a one-ulp contract sounds weaker than
/// it is here. Away from the exact cases the answer is `2^(y * log2|x|)`,
/// and the exponent is formed to an absolute accuracy of about 2^-80 across
/// its whole range — a hundred million times finer than the half ulp that
/// decides the rounding. So the returned double is the correctly rounded one
/// unless the true value lies within about 2^-26 of an ulp of the midpoint
/// between two doubles.
///
/// Such arguments exist and the conformance corpus contains one, which is
/// the reason this paragraph is here rather than a claim of correct
/// rounding: `pow(1 - 2^-53, 1.5)` is 2^-107 above a midpoint, and the
/// double-double the answer is reassembled in resolves 2^-106. That is the
/// shape of the exception — a true value astronomically close to a rounding
/// boundary — and it is why the contract is one ulp and the kernel is
/// versioned. It is also rare: on 250 000 randomized arguments measured
/// offline against the same 120-digit reference (200 000 across the whole
/// range of both operands, and 50 000 with a base within 1e-3 of 1 and an
/// exponent up to a million, where the logarithm's accuracy near 1 is the
/// whole of the answer), the worst distance observed was ZERO ulp.
///
/// EXACT WHERE AN EXACT ANSWER EXISTS, and decided ALGEBRAICALLY before the
/// kernel is reached, so the exactness is a property of the code rather than
/// of the accuracy:
///
///   * an INTEGRAL `y` with `x` a power of two is answered from the
///     exponents alone: `pow(2, 10)` is 1024, `pow(0.5, -3)` is 8,
///     `pow(2, 5000)` is +inf and `pow(2, -5000)` is +0.
///   * an INTEGRAL `y` whose exact power is representable is answered by
///     INTEGER exponentiation of the odd part of the significand:
///     `pow(3, 5)` is 243, `pow(10, 3)` is 1000, `pow(1.5, 4)` is 5.0625.
///   * an INTEGRAL `y` whose exact power sits exactly HALFWAY between two
///     doubles is settled there too, ties to even, which is the one case no
///     approximation of any width could settle. That is why `pow(10, k)`
///     equals `pow10(k)` for every decade a double holds — including
///     `pow(10, 23)`, which is such a tie.
///   * `y` of 1, 2, -1 and 0.5 are `x`, `x*x`, `1/x` and the square root —
///     IEEE-754 operations, correctly rounded by mandate.
///
/// ANNEX F TO THE LETTER, including the corners that are easy to get subtly
/// wrong: `pow(1, y)` is 1 for EVERY y, a NaN included; `pow(x, ±0)` is 1
/// for every x, a NaN included; `pow(-1, ±inf)` is 1; a negative `x` with a
/// non-integral `y` is a NaN; and the sign of a zero or an infinity survives
/// exactly when `y` is an odd integer, so `pow(-0.0, -3)` is -inf while
/// `pow(-0.0, -2)` is +inf.
double pow(double x, double y) noexcept;

/// The version of the FAITHFUL kernels in libcworks — `pow` here, and the
/// radian trigonometry of `cworks/trig.hpp`.
///
/// The correctly rounded functions need no such number: there is only one
/// correctly rounded answer, so their results can never move again. A
/// faithful one has two admissible answers wherever the true value is within
/// an ulp of a boundary, and a better kernel may legitimately pick the other
/// one — which changes bytes in every document that reached it. That is a
/// decision, not a defect, and this constant is where it is recorded.
///
/// A consumer that must reproduce an older document bit for bit stores this
/// number with it and compares. Bump it in the same change that alters what
/// any faithful kernel returns, and never for anything else.
inline constexpr int kMathKernelVersion = 1;

} // namespace cworks
