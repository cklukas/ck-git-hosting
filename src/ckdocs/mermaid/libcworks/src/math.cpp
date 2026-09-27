// libcworks — deterministic elementary functions
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The reasoning behind this module is in `cworks/math.hpp`. What follows is
// how each answer is arrived at, and why each step cannot be a source of
// disagreement between two platforms.
//
// THE ONLY LIBM FUNCTIONS CALLED HERE ARE THE ONES IEEE-754 PINS DOWN
// COMPLETELY, and they are called for three purposes. `std::sqrt` produces a
// starting guess for `hypot` that the integer arithmetic below then corrects,
// and IS the answer for `pow(x, 0.5)`; `std::round`, `std::trunc`,
// `std::floor` and `std::ceil` carry out the four directions of
// `round_decimal`, and `std::trunc` also decides whether an exponent is an
// integer and whether it is odd. Both exceptions are safe for a
// reason no transcendental can claim: IEEE-754 REQUIRES `sqrt` to be
// correctly rounded and specifies the roundToIntegral operations EXACTLY
// (they have no error at all), so each is the same value everywhere, in the
// same way `+` and `*` are. `rint` and `nearbyint` are NOT among them and
// never will be — they follow the ambient rounding mode, which is process
// state. `tests/test_math.cpp` scans this file for every name that has
// freedom in how it rounds and fails if one appears; the scan is plain text,
// so a banned name is banned in a COMMENT too, and the prose below therefore
// names those functions without their namespace qualification.
//
// Every constant is a HEX float literal. A decimal literal is only usually
// rounded correctly by a compiler — the standard permits the adjacent value
// — while a hex literal denotes one exact bit pattern and nothing else. The
// tables in `math_tables.inc` and the conformance corpus in
// `tests/math_corpus.inc` are produced together by `tools/gen_math.py` at
// 120 decimal digits, so a coefficient and the expectation that judges it
// cannot drift apart.
#include "cworks/math.hpp"

#include <bit>     // std::bit_width — the width of an exact integer power
#include <climits> // INT_MIN
#include <cmath>   // sqrt, the roundToIntegral four, and the classification
                   // predicates — nothing that has freedom in how it rounds
#include <cstdint>
#include <limits>

#include "binary64.hpp" // the exact integer view of a double
#include "dd.hpp"       // the exact transforms, shared with trig.cpp

namespace cworks {

namespace {

using namespace detail;

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// -- the committed tables -----------------------------------------------------

/// One entry of the logarithm's reduction table: `r` carries a bucket of
/// the significand onto 1, and `log_recip` is log(1/r) for that exact `r`.
struct LogBucket {
    double r;
    Two log_recip;
};

#include "math_tables.inc"

// -- exact wide integers ------------------------------------------------------
//
// 192 bits, three 64-bit limbs, least significant first. This is what makes
// `hypot` correctly rounded BY CONSTRUCTION rather than by an error bound:
// x^2 + y^2 is an integer once both operands are scaled, the square of a
// midpoint of the result is an integer too, and comparing two integers has
// no rounding in it at all. Nothing here is a general big-integer library;
// it is the four operations that one comparison needs.

struct UInt192 {
    std::uint64_t w[3];
};

/// The exact product of two 64-bit values, widened to the working type.
/// `mul_u64` itself is in `binary64.hpp`, shared with the radian argument
/// reduction, which decides its answer on integers for the same reason.
UInt192 product(std::uint64_t a, std::uint64_t b) noexcept {
    const UInt128 p = mul_u64(a, b);
    return {{p.lo, p.hi, 0}};
}

/// v << s, for 0 <= s < 64. Every shift here is bounded by 58; a shift of
/// 64 would be undefined and no caller needs one.
UInt192 shifted(UInt192 v, int s) noexcept {
    if (s == 0) return v;
    return {{v.w[0] << s,
             (v.w[1] << s) | (v.w[0] >> (64 - s)),
             (v.w[2] << s) | (v.w[1] >> (64 - s))}};
}

UInt192 sum(UInt192 a, UInt192 b) noexcept {
    UInt192 r{{0, 0, 0}};
    std::uint64_t carry = 0;
    for (int i = 0; i < 3; ++i) {
        const std::uint64_t s = a.w[i] + b.w[i];
        const std::uint64_t t = s + carry;
        carry = (s < a.w[i] ? 1u : 0u) + (t < s ? 1u : 0u);
        r.w[i] = t;
    }
    return r;
}

int compare(UInt192 a, UInt192 b) noexcept {
    for (int i = 2; i >= 0; --i)
        if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
    return 0;
}

// -- hypot --------------------------------------------------------------------

/// Four times the square of the midpoint between `r` and the next double
/// up, brought onto the common scale `shift` of the comparison it is made
/// for. The midpoint has 54 significant bits — `2 * significand + 1` — so
/// its square needs 108 and the shifted value up to 166: inside 192.
UInt192 upper_midpoint_squared(double r, int shift) noexcept {
    const std::uint64_t m = 2 * significand(r) + 1;
    return shifted(product(m, m), 2 * exponent_of(r) + shift);
}

double next_up(double v) noexcept { return from_bits(bits(v) + 1); }
double next_down(double v) noexcept { return from_bits(bits(v) - 1); }

/// Both operands below the smallest normal. There every double in play —
/// the operands AND the result — is an integer multiple of 2^-1074, and the
/// bit pattern of a subnormal IS that integer. So the rounding to be done
/// is rounding to a whole number, and it is done on whole numbers: the
/// result is the correctly rounded integer square root of ia^2 + ib^2.
///
/// This is not an optimisation. Scaling the operands up, computing there
/// and scaling back would round twice, and double rounding is exactly the
/// silent last-bit error this module exists to remove.
double hypot_subnormal(double a, double b) noexcept {
    const std::uint64_t ia = bits(a);
    const std::uint64_t ib = bits(b);
    const UInt192 four_n = shifted(sum(product(ia, ia), product(ib, ib)), 2);
    const auto odd_square = [](std::uint64_t k) { return product(2 * k + 1, 2 * k + 1); };

    // sqrt(n) <= k + 1/2  <=>  4n <= (2k+1)^2. Take the smallest such k;
    // then sqrt(n) > k - 1/2 follows from its minimality, so k is the
    // nearest integer. The first guess is within a unit or two.
    std::uint64_t k = static_cast<std::uint64_t>(
        std::sqrt(static_cast<double>(four_n.w[1]) * 0x1p64 + static_cast<double>(four_n.w[0])) *
        0.5);
    while (compare(four_n, odd_square(k)) > 0) ++k;
    while (k > 0 && compare(four_n, odd_square(k - 1)) <= 0) --k;
    // An exact half — a Pythagorean coincidence, not an impossibility —
    // goes to the even neighbour.
    if (compare(four_n, odd_square(k)) == 0 && (k & 1)) ++k;
    return from_bits(k);
}

// -- exp ----------------------------------------------------------------------

/// exp(r) - 1 for |r| <= ln(2)/128, as a double-double.
///
/// The subtraction of 1 is in the SHAPE of the routine rather than in its
/// arithmetic: returning exp(r) would round away the very digits the caller
/// needs, since exp(r) is within 1% of 1 over this range. `r * Q(r)` keeps
/// every one of them, and the caller adds the 1 back where it belongs — to
/// a table entry that is not close to cancelling with it.
Two expm1_kernel(Two r) noexcept {
    const Two tail{horner(kExpTail, r.hi), 0.0};
    return dd_mul(r, dd_horner(kExpHead, r, tail));
}

/// The double nearest to (v.hi + v.lo) * 2^n.
///
/// For a normal result this is `v.hi` scaled: `v.hi` is already the
/// double-double's value rounded to double, and multiplying by a power of
/// two rounds nothing — overflowing to +inf exactly when the true value
/// passes the overflow threshold, which is the correct rounding there.
///
/// A SUBNORMAL result cannot be reached that way, because scaling an
/// already-rounded significand into a coarser grid rounds a second time.
/// So that case counts the units of 2^-1074 directly and rounds once,
/// using the double-double's tail — which is not a refinement there but
/// half the answer, since the subnormal grid keeps as few as one bit.
double rounded_scaled(Two v, int n) noexcept {
    const int e = exponent_of(v.hi);
    if (e + n >= -1022) return scale2(v.hi, n);

    const int q = n + e + 1074; // the binary place of the last kept bit
    if (q < -1) return 0.0;     // below half of the smallest subnormal
    const double head = scale2(v.hi, -e) * pow2(q); // < 2^52, exact
    const double tail = scale2(v.lo, -e) * pow2(q);
    std::uint64_t whole = static_cast<std::uint64_t>(head);
    double fraction = (head - static_cast<double>(whole)) + tail;
    while (fraction > 0.5) {
        fraction -= 1.0;
        ++whole;
    }
    while (fraction < -0.5) {
        fraction += 1.0;
        --whole;
    }
    // A tie goes to the even neighbour. exp cannot actually produce one —
    // e^x is irrational for every non-zero rational x — but a rule that is
    // written down cannot be got wrong by the next kernel to come here.
    if (fraction == 0.5 && (whole & 1)) ++whole;
    if (fraction == -0.5 && (whole & 1)) --whole;
    return from_bits(whole);
}

// -- log ----------------------------------------------------------------------

/// log(1 + u) for |u| <= 2^-7, as a double-double, to full RELATIVE
/// accuracy — which is why it takes a double-double argument and returns
/// one: near x = 1 this is the whole of log(x), and a logarithm that lost
/// its leading digits there would be wrong in every digit for x = 1 + 2^-52.
Two log1p_kernel(Two u) noexcept {
    const Two tail{horner(kLogTail, u.hi), 0.0};
    return dd_mul(u, dd_horner(kLogHead, u, tail));
}

/// x = 2^exponent * m, with log(m) evaluated on the spot.
///
/// Two decisions make this exact where it has to be:
///
///   * The significand is folded into [0.75, 1.5) rather than [1, 2), so an
///     x on EITHER side of 1 keeps exponent 0. Without the fold, x slightly
///     below 1 would be written 2^-1 * (something near 2) and the answer
///     would be the difference of two nearly equal tabulated numbers —
///     catastrophic cancellation for the arguments closest to the one place
///     a logarithm is most sensitive.
///   * The two buckets that touch 1 have r == 1 exactly, so there the
///     reduction is the identity, `u = m - 1` is exact, and the tabulated
///     logarithm is exactly zero. log(1) is +0 by construction, not by
///     luck, and log(1 + 2^-52) keeps all its digits.
struct Split {
    int exponent;
    Two log_m;
};

Split split_log(double x) noexcept {
    int exponent = 0;
    if (x < kMinNormal) { // lift the subnormals; the scaling is exact
        x = scale2(x, 54);
        exponent = -54;
    }
    const std::uint64_t b = bits(x);
    exponent += static_cast<int>(b >> 52) - 1023;
    double m = from_bits((b & kMantissaMask) | (std::uint64_t{1023} << 52)); // [1, 2)
    if (m >= 1.5) {
        m *= 0.5; // exact
        ++exponent;
    }
    const LogBucket& bucket = kLogTable[static_cast<int>((m - 0.75) * 128.0)];
    // r * m is formed exactly, so the only error left in the reduced
    // argument is the one the series makes.
    const Two u = dd_add(two_product(bucket.r, m), Two{-1.0, 0.0});
    return {exponent, dd_add(bucket.log_recip, log1p_kernel(u))};
}

/// log2(x) as a double-double, for a finite x > 0. This is `log2` before its
/// final rounding, and `log2` is exactly its high word — the two cannot
/// disagree because there is one implementation.
Two log2_dd(double x) noexcept {
    const Split s = split_log(x);
    return dd_add(Two{static_cast<double>(s.exponent), 0.0}, dd_mul(s.log_m, kInvLn2));
}

/// 2^p, rounded once, for a double-double `p` with |p.hi| below about 1100.
///
/// The reduction is `exp`'s, read in the other direction: there the argument
/// is divided into sixty-fourths of ln(2), here it already IS a base-two
/// exponent, so the split into a table index and a remainder is exact
/// arithmetic on a power of two and only the remainder needs converting into
/// the natural base the shared series works in.
double exp2_dd(Two p) noexcept {
    // p * 64 is exact, and steps/64 is exact for the same reason, so the
    // remainder below carries every bit p had.
    const double scaled = p.hi * 64.0;
    const int steps = static_cast<int>(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
    const Two fraction = dd_add(p, Two{-static_cast<double>(steps) * 0x1p-6, 0.0});
    const Two r = dd_mul(fraction, kLn2); // |r| <= ln(2)/128: the kernel's domain

    const Two table = kExp2Table[steps & 63];
    return rounded_scaled(dd_add(table, dd_mul(table, expm1_kernel(r))), steps >> 6);
}

// -- pow ----------------------------------------------------------------------
//
// Every exact answer is settled before the kernel is reached, on integers.
// That is not an optimisation: `2^(y log2 x)` is accurate enough to round to
// 1024 for pow(2, 10) as things stand, but it would be accurate enough *by
// luck*, and the exactness of a power of two is not a property this module
// should have to re-earn every time the kernel changes.

/// What `y` is, as far as the special-value table and the sign rule care.
enum class Parity { NotInteger, Even, Odd };

Parity parity_of(double y) noexcept {
    const double a = magnitude(y);
    if (!(a < kInf)) return Parity::NotInteger; // infinities and NaN
    if (a == 0.0) return Parity::Even;
    // Above 2^53 the doubles are spaced two apart or wider, so every one of
    // them is an even integer and no bit test is needed to say so. An
    // implementation that looked for a bit that no longer exists would call
    // pow(-1, 1e300) a negative number.
    if (a >= 0x1p53) return Parity::Even;
    if (a != std::trunc(a)) return Parity::NotInteger;
    // a * 0.5 is exact — a power-of-two scaling of a value at least 1 — so
    // this asks the question about the true integer, not about a rounding.
    return std::trunc(a * 0.5) * 2.0 == a ? Parity::Even : Parity::Odd;
}

bool is_power_of_two(double a) noexcept { return a == pow2(exponent_of(a)); }

/// `a` written as an ODD integer times a power of two: a == m * 2^shift.
/// Every finite non-zero double has exactly one such form, and it is the
/// form in which an exact power can be recognised.
std::uint64_t odd_significand(double a, int& shift) noexcept {
    std::uint64_t m = 0;
    if (a < kMinNormal) {
        m = bits(a); // a subnormal IS this many units of 2^-1074
        shift = -1074;
    } else {
        m = significand(a);
        shift = exponent_of(a) - 52;
    }
    while ((m & 1) == 0) {
        m >>= 1;
        ++shift;
    }
    return m;
}

/// a^n for a finite a > 0 and an integer n >= 2, whenever the answer can be
/// decided on INTEGERS; `false` when it cannot, and the kernel answers
/// instead.
///
/// `a` is m * 2^k with m ODD, so a^n is m^n * 2^(kn) with m^n odd too. Two
/// consequences, and together they are the whole reason this exists:
///
///   * An odd integer's significant bits ARE its bit width. So a^n is
///     exactly representable precisely when m^n fits in 53 bits, which makes
///     the test an integer overflow check and nothing more: pow(3, 5) is
///     243, pow(10, 3) is 1000, pow(1.5, 4) is 5.0625.
///   * a^n is exactly HALFWAY between two doubles precisely when m^n has 54
///     bits — and an odd 54-bit integer fits in a machine word. So EVERY tie
///     a power can produce is settled here, ties to even, and none is left
///     to a kernel that cannot see a midpoint it is sitting on. pow(10, 23)
///     is that case and the only one the powers of ten reach: 10^23 is
///     5^23 * 2^23 and 5^23 has exactly 54 bits, so the exact value is the
///     midpoint of two doubles and the answer is the even one — which is
///     what `pow10(23)` returns from its table.
///
/// Above 54 bits m^n is odd and therefore CANNOT be a midpoint, which is
/// precisely what leaves the kernel's 2^-80 room enough to round correctly
/// on its own. Everything a word can hold is decided here anyway, since
/// deciding it costs one shift.
///
/// A subnormal or overflowing result is handed back to the kernel: rounding
/// a 53-bit significand onto the coarser subnormal grid would round twice,
/// which is the error this module exists to remove.
bool integer_power(double a, int n, double& result) noexcept {
    int shift = 0;
    const std::uint64_t m = odd_significand(a, shift);
    std::uint64_t accumulated = 1;
    for (int i = 0; i < n; ++i) {
        if (accumulated > ~std::uint64_t{0} / m) return false; // past a machine word
        accumulated *= m;
    }

    std::uint64_t kept = accumulated;
    int dropped = static_cast<int>(std::bit_width(accumulated)) - 53;
    if (dropped > 0) {
        kept = accumulated >> dropped;
        const std::uint64_t half = std::uint64_t{1} << (dropped - 1);
        const std::uint64_t rest = accumulated & ((std::uint64_t{1} << dropped) - 1);
        if (rest > half || (rest == half && (kept & 1))) ++kept; // ties to even
    } else {
        dropped = 0;
    }

    // The exponent of the answer, read after the rounding, which may have
    // carried a bit into it.
    const int exponent =
        shift * n + dropped + static_cast<int>(std::bit_width(kept)) - 1;
    if (exponent < -1022 || exponent > 1023) return false;
    result = scale2(static_cast<double>(kept), shift * n + dropped);
    return true;
}

} // namespace

// -- powers of ten and decades ------------------------------------------------

double pow10(int k) noexcept {
    if (k < kDecadeMin) return 0.0;
    if (k > kDecadeMax) return kInf;
    return kPow10[k - kDecadeMin];
}

int floor_log10(double x) noexcept {
    if (!(x > 0.0) || x == kInf) return INT_MIN; // NaN, zero, negative, infinite

    // floor(e * log10(2)) using 1292913987 / 2^32. Over the whole exponent
    // range that estimate drifts by at most 1.3e-7, while the closest any
    // e * log10(2) comes to an integer is 4.5e-4 (at e = 485) — three and a
    // half thousand times further — so the estimate IS the floor, and the
    // true decade is either it or one more.
    //
    // The two adjustments below make that a question of speed rather than
    // of correctness: whatever the estimate, the loops leave k with
    // 10^k <= x < 10^(k+1) decided by exact table comparison, which is the
    // only thing this function's answer ever rests on.
    long long estimate = (static_cast<long long>(exponent_of(x)) * 1292913987LL) >> 32;
    if (estimate < kDecadeMin - 1) estimate = kDecadeMin - 1;
    if (estimate > kDecadeMax) estimate = kDecadeMax;
    int k = static_cast<int>(estimate);
    while (k > kDecadeMin - 1 && x < kDecade[k - kDecadeMin]) --k;
    while (k < kDecadeMax && x >= kDecade[k + 1 - kDecadeMin]) ++k;
    return k;
}

// -- the decimal grid ---------------------------------------------------------
//
// Everything here exists so that the SCALE FACTOR is the same number on
// every platform, and so that it is the RIGHT number: always the positive
// decade from `pow10`'s table, multiplied in or divided out — never the
// platform's general power function raised to a decade, and never a
// reciprocal.

namespace {

/// The largest decade worth forming. 10^640 exceeds the ratio between the
/// largest and the smallest positive double, so scaling by any wider decade
/// gives the same answer as scaling by this one (zero one way, an infinity
/// the other); the cap exists to keep the conversion from the caller's
/// 64-bit `digits` in range, not to change any answer.
constexpr std::int64_t kDecadeCap = 640;

/// x * 10^k, k >= 0. Applied in steps of at most 10^308, because `pow10`
/// SATURATES beyond that and multiplying a zero by the resulting infinity
/// would give a NaN where the answer is plainly zero. Two steps carry the
/// widest decade there is, and the arithmetic below the cap needs at most
/// one — but "at most two" is a fact about the cap, not a bound worth
/// hard-coding, so the stepping is written as a loop.
double scale_up(double x, int k) noexcept {
    double scaled = x;
    while (k > 308) {
        scaled *= pow10(308);
        k -= 308;
    }
    return scaled * pow10(k);
}

/// x / 10^k, k >= 0 — the same stepping, and the reason a negative `digits`
/// divides by 10^|digits| instead of multiplying by an inexact 10^digits.
double scale_down(double x, int k) noexcept {
    double scaled = x;
    while (k > 308) {
        scaled /= pow10(308);
        k -= 308;
    }
    return scaled / pow10(k);
}

/// Rounds an already-scaled value to an integer in the requested
/// direction. Every operation here is IEEE-754's roundToIntegral*, exact
/// on every platform — the direction is the only thing that varies.
double to_integral(double scaled, DecimalRounding mode) noexcept {
    switch (mode) {
    case DecimalRounding::Nearest:
        // std::round, not nearbyint: nearbyint honours the ambient rounding
        // mode, which is process state a document's numbers must not depend
        // on. std::round is ties-AWAY-from-zero, which is the rule stated on
        // `DecimalRounding::Nearest`.
        return std::round(scaled);
    case DecimalRounding::TowardZero:
        return std::trunc(scaled);
    case DecimalRounding::AwayFromZero:
        return scaled < 0.0 ? std::floor(scaled) : std::ceil(scaled);
    case DecimalRounding::TowardNegative:
        return std::floor(scaled);
    case DecimalRounding::TowardPositive:
        return std::ceil(scaled);
    }
    return scaled; // unreachable: the switch is exhaustive
}

/// True when a scale that underflowed to zero must still move `x` to the
/// first grid step. The scaled value is zero but the true one is not — it
/// is an infinitesimal of x's sign — so the answer is the directed
/// rounding of that infinitesimal, which is one step for the mode that
/// grows away from it and zero for the other three.
bool grows_from_zero(bool negative, DecimalRounding mode) noexcept {
    if (mode == DecimalRounding::AwayFromZero) return true;
    return negative ? mode == DecimalRounding::TowardNegative
                    : mode == DecimalRounding::TowardPositive;
}

} // namespace

double round_decimal(double x, std::int64_t digits, DecimalRounding mode) noexcept {
    // Infinities, NaN and both zeros are their own answer at every number
    // of decimals, sign included.
    if (!std::isfinite(x) || x == 0.0) return x;

    // A grid finer than 10^-324 cannot move ANY double: its step is below
    // half the smallest ulp (2^-1074 ~ 4.94e-324), so x is already its own
    // nearest multiple. Stated on its own because it is the one case the
    // magnitude test below misses — a subnormal has no room left to grow.
    if (digits >= 324) return x;

    // 10^e <= |x| < 10^(e+1), by exact comparison against the committed
    // table of decade boundaries: no logarithm, so no rounding decision for
    // a platform to disagree about.
    const int e = floor_log10(magnitude(x));

    // Once the scaled magnitude reaches 10^16 the requested grid is finer
    // than the doubles are dense and x is its own answer; the reasoning is
    // on `round_decimal` in the header. It also bounds `digits` at 339,
    // which is what makes the cast below safe.
    if (digits >= static_cast<std::int64_t>(16) - e) return x;

    // The positive decade, saturating. Written without negating `digits`
    // when it is far out of range, because -digits is undefined at the
    // bottom of the 64-bit range and saturation is the answer there anyway.
    std::int64_t decade = digits;
    if (decade < 0) decade = decade < -kDecadeCap ? kDecadeCap : -decade;
    if (decade > kDecadeCap) decade = kDecadeCap;
    const int k = static_cast<int>(decade);

    const double scaled = digits >= 0 ? scale_up(x, k) : scale_down(x, k);
    double rounded = to_integral(scaled, mode);
    if (scaled == 0.0 && grows_from_zero(x < 0.0, mode)) rounded = x < 0.0 ? -1.0 : 1.0;
    return digits >= 0 ? scale_down(rounded, k) : scale_up(rounded, k);
}

// -- hypot --------------------------------------------------------------------

double hypot(double x, double y) noexcept {
    // Annex F, and it is not an accident of the algorithm: an infinite
    // operand wins over a NaN one, because the hypotenuse is infinite
    // whatever the other side turns out to be.
    if (std::isinf(x) || std::isinf(y)) return kInf;
    if (std::isnan(x) || std::isnan(y)) return x + y;

    double a = magnitude(x);
    double b = magnitude(y);
    if (a < b) {
        const double t = a;
        a = b;
        b = t;
    }
    if (b == 0.0) return a; // includes a == 0: hypot(0, 0) is +0

    const int ea = exponent_of(a);
    const int eb = exponent_of(b);
    // Beyond 28 binades the smaller side moves the result by less than half
    // an ulp, and the correctly rounded answer IS the larger side. This is
    // not a shortcut for speed: it is what keeps the exponent span of the
    // exact arithmetic below bounded, and therefore what lets 192 bits be
    // enough. (For subnormal operands the grid is absolute rather than
    // relative, and the same 28 binades are more than enough there too.)
    if (ea - eb >= 28) return a;
    if (a < kMinNormal) return hypot_subnormal(a, b);

    // Scale by an exact power of two so that a is in [1, 2) and b in
    // [2^-27, 2): no square can overflow, none can underflow, and the
    // scaling itself has changed nothing but the exponent.
    const int d = ea - eb;
    const double as = scale2(a, -ea);
    const double bs = scale2(b, -ea);
    const std::uint64_t sa = significand(as);
    const std::uint64_t sb = significand(bs);

    // a^2 + b^2 is exactly n * 2^(-104 - 2d) with n an integer below 2^161.
    // Multiplying both sides of every comparison by 2^(108 + 2d) leaves the
    // sum as n << 4 and the midpoint square as (2R+1)^2 << (2e + 2d + 2),
    // both non-negative shifts and both inside 192 bits.
    const UInt192 lhs = shifted(sum(shifted(product(sa, sa), 2 * d), product(sb, sb)), 4);
    const int shift = 2 * d + 2;

    // The starting guess is a correctly rounded sqrt of a rounded sum, so
    // it is within an ulp; the integers decide the last bit. Each loop runs
    // at most twice, and neither can be entered on a wrong premise: the
    // comparison it turns on is exact.
    double r = std::sqrt(as * as + bs * bs);
    while (compare(lhs, upper_midpoint_squared(r, shift)) > 0) r = next_up(r);
    while (compare(lhs, upper_midpoint_squared(next_down(r), shift)) <= 0) r = next_down(r);
    if (compare(lhs, upper_midpoint_squared(r, shift)) == 0 && (bits(r) & 1)) r = next_up(r);

    return scale2(r, ea);
}

// -- exp ----------------------------------------------------------------------

double exp(double x) noexcept {
    if (std::isnan(x)) return x + x;
    if (x >= 710.0) return kInf; // e^710 is past DBL_MAX; catches +inf
    if (x <= -746.0) return 0.0; // e^-746 is below half the smallest subnormal
    if (x == 0.0) return 1.0;    // exactly, for +0 and -0 alike

    // x = (64n + j) * ln(2)/64 + r with |r| <= ln(2)/128. The multiple is
    // subtracted in two pieces: `kLn2Over64Head` carries 21 bits, so its
    // product with the integer is exact and the subtraction that follows is
    // too, and `kLn2Over64Tail` supplies the other 106 bits of ln(2)/64 so
    // that nothing of the reduction is rounded away before the series that
    // needs it.
    const double t = x * k64OverLn2;
    const int steps = static_cast<int>(t >= 0.0 ? t + 0.5 : t - 0.5);
    const double count = static_cast<double>(steps);
    Two r = two_sum(x, -(count * kLn2Over64Head));
    r = dd_add(r, negated(dd_scale(kLn2Over64Tail, count)));

    // 2^(j/64) * (1 + (exp(r) - 1)). The table entry is between 1 and 2 and
    // the correction is below 1%, so the one full-magnitude addition here
    // is the only place a rounding of consequence happens before the final
    // one.
    const Two table = kExp2Table[steps & 63];
    return rounded_scaled(dd_add(table, dd_mul(table, expm1_kernel(r))), steps >> 6);
}

// -- the logarithms -----------------------------------------------------------

double log(double x) noexcept {
    if (std::isnan(x)) return x + x;
    if (x < 0.0) return kNaN;
    if (x == 0.0) return -kInf;
    if (x == kInf) return kInf;
    if (x == 1.0) return 0.0; // exactly +0, never -0

    const Split s = split_log(x);
    // e * ln(2) with the exponent multiplying an exactly representable head
    // — no part of the exponent's contribution is rounded before the sum
    // that needs it. Where the exponent is zero this term is exactly zero
    // and log(x) is the reduced logarithm alone, which is what gives x near
    // 1 its full relative accuracy.
    const double count = static_cast<double>(s.exponent);
    const Two scaled = dd_add(Two{count * kLn2Head, 0.0}, dd_scale(kLn2Tail, count));
    return dd_add(scaled, s.log_m).hi;
}

double log2(double x) noexcept {
    if (std::isnan(x)) return x + x;
    if (x < 0.0) return kNaN;
    if (x == 0.0) return -kInf;
    if (x == kInf) return kInf;

    // Evaluated in base two rather than divided out of log(x) at the end:
    // for a power of two the reduced logarithm is exactly zero and the
    // answer is the exponent itself, with no arithmetic between the two.
    // `pow` needs the same value before its final rounding, so both take it
    // from one place and cannot come to disagree.
    return log2_dd(x).hi;
}

double log10(double x) noexcept {
    if (std::isnan(x)) return x + x;
    if (x < 0.0) return kNaN;
    if (x == 0.0) return -kInf;
    if (x == kInf) return kInf;

    // The powers of ten a double can hold are answered from the table that
    // defines them. This is a structural guarantee rather than a
    // consequence of the kernel's accuracy: log10(1000) is 3 because 1000
    // IS the third power of ten, not because 3 is what the arithmetic came
    // closest to. (Above 10^22 no double is a power of ten, so there is
    // nothing here to be exact about — though the correctly rounded answer
    // for the double written 1e23 is still exactly 23.)
    const int k = floor_log10(x);
    if (k >= 0 && k <= 22 && x == kPow10[k - kDecadeMin]) return static_cast<double>(k);

    const Split s = split_log(x);
    const Two scaled = dd_scale(kLog10Of2, static_cast<double>(s.exponent));
    return dd_add(scaled, dd_mul(s.log_m, kLog10OfE)).hi;
}

// -- the general power ---------------------------------------------------------

double pow(double x, double y) noexcept {
    // Annex F, in the order the standard states it. The first two rules WIN
    // OVER A NaN OPERAND and therefore have to be asked before anything
    // else: one is the exponent that makes every base irrelevant, the other
    // the base that makes every exponent irrelevant.
    if (y == 0.0) return 1.0; // pow(x, ±0) — x may be a NaN
    if (x == 1.0) return 1.0; // pow(+1, y) — y may be a NaN
    if (std::isnan(x) || std::isnan(y)) return x + y;

    const Parity parity = parity_of(y);
    const double ax = magnitude(x);

    // An infinite exponent asks only whether the base is inside, on, or
    // outside the unit circle — an infinite base included, which is why this
    // comes first and covers pow(±inf, ±inf) as well. `pow(-1, ±inf)` is 1:
    // the sequence does not converge, and the standard picks the value the
    // even powers have.
    if (std::isinf(y)) {
        if (ax == 1.0) return 1.0;
        return (ax > 1.0) == (y > 0.0) ? kInf : 0.0;
    }

    // From here the sign of the answer is decided once, by the one rule that
    // decides it everywhere: a negative base survives only an odd integer
    // exponent. That rule is what makes pow(-0.0, -3) a NEGATIVE infinity
    // while pow(-0.0, -2) is a positive one.
    const bool odd = parity == Parity::Odd;
    const bool negated_result = std::signbit(x) && odd;

    if (x == 0.0) {
        if (y < 0.0) return negated_result ? -kInf : kInf;
        return negated_result ? -0.0 : 0.0;
    }
    if (ax == kInf) {
        if (y < 0.0) return negated_result ? -0.0 : 0.0;
        return negated_result ? -kInf : kInf;
    }
    // A negative base to a fractional power is not a real number.
    if (x < 0.0 && parity == Parity::NotInteger) return kNaN;

    // -- the exact answers, decided before the kernel ------------------------

    // The four exponents where an IEEE-754 operation IS the power, and is
    // therefore correctly rounded by mandate rather than by this kernel.
    // (`x` is strictly positive at 0.5: a negative base has already left.)
    if (y == 1.0) return x;
    if (y == 2.0) return x * x;
    if (y == -1.0) return 1.0 / x;
    if (y == 0.5) return std::sqrt(x);

    if (parity != Parity::NotInteger) {
        // A power of two raised to an integer is a power of two, and its
        // exponent is a product of two integers. Whenever that product can
        // matter it is also EXACT: a result inside the double range bounds
        // |y| by 1024, so both factors have eleven significant bits and the
        // product has at most twenty-two.
        if (is_power_of_two(ax)) {
            const double e = static_cast<double>(exponent_of(ax)) * y;
            if (e >= 1024.0) return negated_result ? -kInf : kInf;
            // 2^-1075 is exactly half the smallest subnormal, so it rounds
            // to zero — ties to even — and everything below it does too.
            if (e <= -1075.0) return negated_result ? -0.0 : 0.0;
            const double value = pow2(static_cast<int>(e));
            return negated_result ? -value : value;
        }
        // A power an integer can decide — every exactly representable one,
        // and every exact tie. Bounded at 63 because the odd part of any
        // base that reaches here is at least 3, whose 41st power already
        // overflows a word: the bound costs no case and makes the loop's
        // length obvious.
        double decided = 0.0;
        if (y >= 2.0 && y <= 63.0 && integer_power(ax, static_cast<int>(y), decided))
            return negated_result ? -decided : decided;
    }

    // -- the kernel ----------------------------------------------------------
    //
    // 2^(y * log2|x|), with the logarithm carried as a double-double so that
    // the exponent is formed to about 2^-80 absolute across its whole range.
    // The estimate below is not an optimisation either: it is what bounds
    // |y| before the exact product is formed, since a result that is neither
    // zero nor infinite forces |y| below 2^63 however extreme the operands
    // look.
    const Two logarithm = log2_dd(ax);
    const double estimate = y * logarithm.hi;
    if (estimate > 1030.0) return negated_result ? -kInf : kInf;
    if (estimate < -1080.0) return negated_result ? -0.0 : 0.0;
    const double value = exp2_dd(dd_scale(logarithm, y));
    return negated_result ? -value : value;
}

} // namespace cworks
