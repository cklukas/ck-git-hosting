// libcworks — deterministic trigonometry, in degrees and in radians
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Every constant below is written as a HEX float literal. A decimal literal
// is only *usually* rounded correctly by a compiler — the standard permits
// the adjacent value — while a hex literal denotes one exact bit pattern and
// nothing else. In a file whose entire purpose is bit-identical output that
// distinction is the difference between a claim and a proof. The decimal
// form and the exact rational each constant approximates are in the comment
// beside it; every one of them is pinned to the last bit by the conformance
// corpus, which `tools/gen_trig.py corpus` computes at 80 decimal digits
// from the same series. The RADIAN tables are wider than a comment can carry
// and are generated instead, into `trig_tables.inc`.
//
// ONE KERNEL, TWO UNITS. `sincos_kernel` takes a double-double argument in
// RADIANS and is the whole of the mathematics; the two entry conventions
// differ only in how they get an argument into its domain, and that
// difference is the interesting part:
//
//   * DEGREES fold modulo 360 EXACTLY (Sterbenz), so the reduction
//     introduces no error at all and the cardinal angles come back exact.
//   * RADIANS cannot: pi/2 is irrational, so "x modulo pi/2" is a question
//     about the binary expansion of 2/pi and nothing shorter will answer it.
//     `reduce_radians` is Payne and Hanek's method — the argument's
//     significand multiplied by a window of that expansion in exact integer
//     arithmetic — which is why the radian entry points are as accurate at
//     1e300 as at 1.
//
// The exact transforms the kernel is built on live in `dd.hpp`, and the
// integer view of a double in `binary64.hpp`, both shared with `math.cpp`:
// one implementation of each for the two files in the suite that owe their
// callers the same bits everywhere.
//
// That arithmetic is exact only if the compiler does not fuse a multiply
// and an add behind our back: `two_product` splits its operands on the
// assumption that `a * b` was rounded. `cworks_strict()` compiles every
// first-party target with `-ffp-contract=off` (and MSVC's default
// `/fp:precise` forbids contraction outright), which is the same flag the
// suite already relies on for byte-exact goldens — this file simply makes
// the dependency explicit. `tests/test_trig.cpp` verifies the exactness of
// both primitives directly, so a build that lost the flag fails loudly
// instead of silently producing different bytes.
//
// THE ONLY LIBM FUNCTION CALLED HERE IS `std::sqrt`, which IEEE-754 REQUIRES
// to be correctly rounded — the same value on every platform, in the same
// way `+` and `*` are, and the reason it is not on the banned list the test
// scans this file with. It is the start of the double-double square root
// that `asin` and `acos` turn into an angle, and it appears nowhere else.
#include "cworks/trig.hpp"

#include <cmath>   // isfinite and sqrt; no transcendental is called here
#include <cstddef> // std::size_t
#include <cstdint> // the limbs of the 2/pi expansion
#include <limits>

#include "binary64.hpp" // the integer view of a double, shared with math.cpp
#include "dd.hpp"       // the exact transforms, shared with math.cpp

namespace cworks {

namespace {

using namespace detail;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// -- constants ---------------------------------------------------------------

constexpr Two kRadiansPerDegree{0x1.1df46a2529d39p-6, 0x1.5c1d8becdd291p-62};  // pi/180
constexpr Two kDegreesPerRadian{0x1.ca5dc1a63c1f8p+5, -0x1.1e7ab456405f9p-49}; // 180/pi
constexpr Two kSixth{0x1.5555555555555p-3, 0x1.5555555555555p-57};             // 1/3!
constexpr Two kTwentyFourth{0x1.5555555555555p-5, 0x1.5555555555555p-59};      // 1/4!

constexpr double kCos30 = 0x1.bb67ae8584caap-1; // 0.8660254037844386  = sqrt(3)/2
constexpr double kSin45 = 0x1.6a09e667f3bcdp-1; // 0.7071067811865476  = sqrt(2)/2

/// sin(x) = x - x^3/6 + x^5 * S(x^2). The x^3/6 term is a tenth of the
/// result at 45 degrees and is therefore carried in double-double; what is
/// left is below 2.5e-3, so a plain double evaluation of it costs less than
/// a hundredth of an ulp. Coefficients are (-1)^k/(2k+5)!, carried to 1/17!
/// so the first omitted term contributes below 1e-19.
constexpr double kSinTail[] = {
    0x1.1111111111111p-7,   // +1/5!
    -0x1.a01a01a01a01ap-13, // -1/7!
    0x1.71de3a556c734p-19,  // +1/9!
    -0x1.ae64567f544e4p-26, // -1/11!
    0x1.6124613a86d09p-33,  // +1/13!
    -0x1.ae7f3e733b81fp-41, // -1/15!
    0x1.952c77030ad4ap-49,  // +1/17!
};

/// cos(x) = 1 - x^2/2 + x^4/24 + x^6 * C(x^2), the first three terms in
/// double-double for the same reason. Coefficients are (-1)^(k+1)/(2k+6)!
/// to 1/16!; the first omitted term contributes below 1e-20.
constexpr double kCosTail[] = {
    -0x1.6c16c16c16c17p-10, // -1/6!
    0x1.a01a01a01a01ap-16,  // +1/8!
    -0x1.27e4fb7789f5cp-22, // -1/10!
    0x1.1eed8eff8d898p-29,  // +1/12!
    -0x1.93974a8c07c9dp-37, // -1/14!
    0x1.ae7f3e733b81fp-45,  // +1/16!
};

/// atan(u) = u + u^3 * A(u^2), coefficients (-1)^(k+1)/(2k+3). The argument
/// is reduced below 1/8, so u^2 < 0.0157 and the first omitted term is
/// below 1e-17 of a term that is itself a thousandth of the result.
constexpr double kAtanTail[] = {
    -0x1.5555555555555p-2, // -1/3
    0x1.999999999999ap-3,  // +1/5
    -0x1.2492492492492p-3, // -1/7
    0x1.c71c71c71c71cp-4,  // +1/9
    -0x1.745d1745d1746p-4, // -1/11
    0x1.3b13b13b13b14p-4,  // +1/13
    -0x1.1111111111111p-4, // -1/15
    0x1.e1e1e1e1e1e1ep-5,  // +1/17
    -0x1.af286bca1af28p-5, // -1/19
    0x1.8618618618618p-5,  // +1/21
};

/// The arctangent reduction centres, and atan(centre) in degrees. The
/// centres are quarters — EXACTLY representable — which is the whole point:
/// a centre of tan(22.5 deg) would have to be rounded, and the tabulated
/// angle would then be the arctangent of a number a little to one side of
/// the intended one, biasing every result in that branch by a sixth of an
/// ulp. With quarters the table is exact and the bias is zero, and
/// atan(1) = 45 degrees exactly makes the diagonals come back exact.
constexpr double kAtanCentre[] = {0.0, 0.25, 0.5, 0.75, 1.0};
constexpr Two kAtanCentreDegrees[] = {
    {0x0p+0, 0x0p+0},                               // 0
    {0x1.c128e80fae02ep+3, -0x1.0fc10e257c651p-53}, // 14.036243467926479
    {0x1.a90a731a61dc4p+4, -0x1.80b27b26e182bp-51}, // 26.565051177077990
    {0x1.26f58ce59e23cp+5, 0x1.80b27b26e182bp-50},  // 36.869897645844021
    {0x1.68p+5, 0x0p+0},                            // 45, exactly
};

// The circle constants and the 2/pi expansion the radian half needs — too
// wide to read beside the code, so generated rather than written out.
#include "trig_tables.inc"

// -- reduction ---------------------------------------------------------------

/// `a` (finite, non-negative) reduced into [0, 360), EXACTLY.
///
/// The largest power-of-two multiple of 360 not exceeding `a` is subtracted,
/// then the next, and so on. At each step b <= a < 2b holds, which is
/// Sterbenz's condition, so every subtraction is exact and the reduced angle
/// carries no error whatsoever — not a rounding, not an ulp. That is what
/// makes an angle of 3600 degrees give the same bits as one of 0 degrees.
/// Reducing modulo an irrational pi/2 could make no such claim; this is the
/// dividend degrees pay.
double reduce_degrees(double a) noexcept {
    if (a < 360.0) return a;
    double b = 360.0;
    while (b * 2.0 <= a) b *= 2.0;
    while (b >= 360.0) {
        if (a >= b) a -= b;
        b *= 0.5;
    }
    return a;
}

/// Sine and cosine of the RADIAN double-double `x`, |x| <= pi/4, each as a
/// double-double. This is the whole of the mathematics; both units reach it,
/// and neither reaches anything else.
///
/// It returns pairs rather than doubles for one caller and one reason:
/// `tan` is their quotient, and a quotient of two already-rounded doubles
/// carries both of their roundings into a third. The high word of each pair
/// is nevertheless the value rounded to double exactly as a direct
/// evaluation would give it — `quick_two_sum(a, b).hi` IS `a + b` — so the
/// degree entry points are bit for bit what they always were.
void sincos_kernel(Two x, Two& s, Two& c) noexcept {
    // x^2, to twice the working precision.
    Two square = two_product(x.hi, x.hi);
    square = quick_two_sum(square.hi, square.lo + 2.0 * (x.hi * x.lo));
    const double u = square.hi; // fl(x^2), the argument of both series

    // sin(x) = (x - x^3/6) + x^5*S(u). Everything before the tail is exact
    // to ~1e-32, so the single rounding of the sum below is the only error
    // that survives into the result.
    Two cube = two_product(x.hi, u); // x^3
    cube = quick_two_sum(cube.hi, cube.lo + (x.lo * u + x.hi * square.lo));
    const Two sine_lead = dd_mul(cube, kSixth);
    const Two sine_head = two_sum(x.hi, -sine_lead.hi);
    s = quick_two_sum(sine_head.hi, ((sine_head.lo + x.lo) - sine_lead.lo) +
                                        cube.hi * u * horner(kSinTail, u));

    // cos(x) = (1 - x^2/2 + x^4/24) + x^6*C(u). Splitting the leading
    // cancellation off into double-double is the one place where plain
    // double arithmetic would give away half an ulp for nothing.
    Two quartic = two_product(u, u); // x^4
    quartic = quick_two_sum(quartic.hi, quartic.lo + 2.0 * (u * square.lo));
    const Two cosine_head =
        dd_add(dd_add(Two{1.0, 0.0}, Two{-0.5 * square.hi, -0.5 * square.lo}),
               dd_mul(quartic, kTwentyFourth));
    c = quick_two_sum(cosine_head.hi,
                      cosine_head.lo + quartic.hi * u * horner(kCosTail, u));
}

/// Sine and cosine for `d` in [0, 45] degrees.
void kernel(double d, double& s, double& c) noexcept {
    // The three arguments in this range whose sine and cosine are exactly
    // representable. Returning the exact value is not a shortcut but the
    // more accurate answer, and it is what makes a hexagonal figure close on
    // exact half-integers.
    if (d == 0.0) {
        s = 0.0;
        c = 1.0;
        return;
    }
    if (d == 30.0) {
        s = 0.5;
        c = kCos30;
        return;
    }
    if (d == 45.0) {
        s = kSin45;
        c = kSin45;
        return;
    }

    // x = d * (pi/180) as a double-double: xh carries the rounded product
    // and xl the part of it that a single double cannot hold, plus the tail
    // of the conversion constant. Without this the conversion alone costs
    // most of an ulp before the series has run at all. Note that THIS is
    // the reduction a caller throws away by converting degrees to radians
    // before the call: d is exact, and so is the product formed here.
    Two x = two_product(d, kRadiansPerDegree.hi);
    x = quick_two_sum(x.hi, x.lo + d * kRadiansPerDegree.lo);

    Two sine{0.0, 0.0};
    Two cosine{0.0, 0.0};
    sincos_kernel(x, sine, cosine);
    s = sine.hi;
    c = cosine.hi;
}

/// -0.0 is the same number as 0.0 and a different four bytes of output.
double unsign_zero(double v) noexcept { return v == 0.0 ? 0.0 : v; }

/// `numerator / denominator` to twice the working precision, for finite
/// positive operands with numerator <= denominator.
///
/// A plain quotient is correct to half an ulp, and that half ulp is passed
/// straight through to the angle — the arctangent of a number is only as
/// good as the number. The residue below recovers it. The scaling loops are
/// there so `two_product`'s splitting cannot overflow on an operand near the
/// top of the range; scaling both sides by the same power of two is exact
/// and leaves the quotient untouched.
Two unit_ratio(double numerator, double denominator) noexcept {
    const double quotient = numerator / denominator;
    if (!(quotient > 0.0)) return {0.0, 0.0};
    double n = numerator;
    double d = denominator;
    while (d > 0x1p+512) {
        n *= 0x1p-512;
        d *= 0x1p-512;
    }
    while (d < 0x1p-512) {
        n *= 0x1p+512;
        d *= 0x1p+512;
    }
    const Two product = two_product(quotient, d);
    return quick_two_sum(quotient, ((n - product.hi) - product.lo) / d);
}

/// `atan(t)` for t in [0, 1], as a branch index and the angle the series
/// contributes: the answer is `centre_angle[index] + radians`, in whichever
/// unit the caller's table of centre angles is written.
///
/// Splitting it this way is what keeps the two units from drifting apart
/// while letting each stay exact where it can: 45 degrees IS a double and
/// pi/4 is not, so the degree branch adds an exact centre and converts only
/// the series, while the radian branch adds a correctly rounded one and
/// converts nothing.
struct AtanBranch {
    std::size_t index;
    Two radians;
};

AtanBranch atan_reduce(Two t) noexcept {
    // Reduce with the tangent difference identity onto |u| <= 1/8, around
    // the nearest quarter. Two double-double operations buy the reduced
    // argument its full precision; computing it in plain double would leave
    // a quarter of an ulp of the final angle on the table, which is most of
    // the error budget for a function whose series contributes none.
    std::size_t index = 0;
    if (t.hi >= 0.875) index = 4;
    else if (t.hi >= 0.625) index = 3;
    else if (t.hi >= 0.375) index = 2;
    else if (t.hi >= 0.125) index = 1;

    Two u = t;
    if (index != 0) {
        const double centre = kAtanCentre[index];
        u = dd_div(dd_add(t, Two{-centre, 0.0}),
                   dd_add(Two{1.0, 0.0}, dd_scale(t, centre)));
    }
    const double v = u.hi * u.hi;
    return {index, dd_add(u, Two{u.hi * v * horner(kAtanTail, v), 0.0})};
}

/// atan(t) in DEGREES, for t in [0, 1].
Two atan_unit_degrees(Two t) noexcept {
    const AtanBranch branch = atan_reduce(t);
    return dd_add(kAtanCentreDegrees[branch.index],
                  dd_mul(branch.radians, kDegreesPerRadian));
}

/// atan(t) in RADIANS, for t in [0, 1] (and a shade beyond: a ratio that
/// rounds just above 1 lands in the last branch, where the reduced argument
/// is small and the series is at its most accurate).
Two atan_unit_radians(Two t) noexcept {
    const AtanBranch branch = atan_reduce(t);
    return dd_add(kAtanCentreRadians[branch.index], branch.radians);
}

// -- the radian argument reduction --------------------------------------------
//
// Payne and Hanek's method, and the one place where radians cost what
// degrees do not. Folding modulo 360 is exact because 360 is a double;
// folding modulo pi/2 cannot be, because pi/2 is irrational, and a reduction
// that subtracts a rounded multiple of a double-double pi/2 runs out of bits
// long before the arguments do — near 2^797 the true remainder is 2^-61 of
// the argument, so 61 of the reduction's bits cancel and a 106-bit constant
// has 45 left.
//
// So the reduction is done on the EXACT value of the argument, against a
// 1536-bit expansion of 2/pi, in integer arithmetic. The result is the
// fractional part of x * 2/pi to 192 bits and the integer part modulo four,
// which is the quadrant. Nothing here rounds until the reduced argument is
// converted to a double-double at the end, so a sine at 1e300 is as accurate
// as a sine at 1.

/// Where an argument sits relative to the quadrant boundaries:
/// x == quadrant * pi/2 + radians, modulo 2 pi, with |radians| <= pi/4.
struct Quadrant {
    int quadrant;
    Two radians;
};

/// Add `value` into the 384-bit `p` at word `index`, carrying upward.
/// `p[0]` is the most significant word, so the carry travels toward 0.
void add_at(std::uint64_t (&p)[6], int index, std::uint64_t value) noexcept {
    for (int i = index; i >= 0 && value != 0; --i) {
        p[i] += value;
        value = p[i] < value ? 1 : 0;
    }
}

/// `a` reduced modulo pi/2, for a finite `a` above pi/4.
///
/// `a` is m * 2^e with m a 53-bit integer. Then a * (2/pi) is m times the
/// expansion of 2/pi shifted by e, and two observations bound how much of
/// that expansion has to be read:
///
///   * a bit of 2/pi at position 2^-j contributes m * 2^(e-j) to the
///     product, which is an integer multiple of FOUR whenever j <= e-2 —
///     invisible in a quadrant. So nothing above bit e-2 is needed, which is
///     what makes the window start under the argument's own exponent.
///   * five 64-bit limbs below that leave the truncated tail below 2^-201,
///     which is 140 bits past the worst cancellation a double argument can
///     produce.
Quadrant reduce_radians(double a) noexcept {
    const int e = exponent_of(a) - 52; // a == significand(a) * 2^e, exactly
    const std::uint64_t m = significand(a);
    const int first = e >= 2 ? (e - 2) / 64 : 0; // the limb the window starts at
    // The largest e a double has is 971, which puts `first` at 15 and the
    // last limb read at 19. Stated to the compiler so that shortening the
    // table is a build failure rather than a silent read past its end.
    static_assert(sizeof(kTwoOverPi) / sizeof(kTwoOverPi[0]) >= 20,
                  "the widest double reads limb 19 of the 2/pi expansion");

    // p = m * (five limbs of 2/pi), exactly: 384 bits, most significant
    // word first. Every partial product is the exact 128-bit product of two
    // machine words, so this is integer arithmetic throughout.
    std::uint64_t p[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 4; i >= 0; --i) {
        const UInt128 term = mul_u64(m, kTwoOverPi[first + i]);
        add_at(p, i + 1, term.lo);
        add_at(p, i, term.hi);
    }

    // p represents a * (2/pi) modulo four, scaled up by 2^drop. The bounds
    // on `first` put drop between 255 and 373, so the integer part is read
    // well inside the 384 bits and the fraction has at least 255 below it.
    const int drop = 64 * (first + 5) - e;
    const auto bit_at = [&p](int i) { return (p[5 - i / 64] >> (i % 64)) & 1; };
    const auto word_at = [&p](int i) { // the 64 bits at positions i .. i+63
        const int limb = 5 - i / 64;
        const int offset = i % 64;
        const std::uint64_t low = p[limb];
        const std::uint64_t high = limb > 0 ? p[limb - 1] : 0;
        return offset == 0 ? low : (low >> offset) | (high << (64 - offset));
    };

    int quadrant = static_cast<int>(bit_at(drop) | (bit_at(drop + 1) << 1));
    std::uint64_t fraction[3] = {word_at(drop - 64), word_at(drop - 128),
                                 word_at(drop - 192)};

    // A fraction above a half belongs to the NEXT quadrant, with a negative
    // remainder: |r| <= pi/4 is what the kernel's series is built for, and
    // reaching it costs one two's complement.
    const bool beyond_half = (fraction[0] >> 63) != 0;
    if (beyond_half) {
        std::uint64_t carry = 1;
        for (int i = 2; i >= 0; --i) {
            const std::uint64_t complemented = ~fraction[i] + carry;
            carry = (carry == 1 && complemented == 0) ? 1 : 0;
            fraction[i] = complemented;
        }
        quadrant = (quadrant + 1) & 3;
    }

    // The 192-bit fraction as a double-double, summed from its smallest
    // piece so that nothing a double-double can hold is lost on the way.
    // Each piece is 32 bits, so its conversion is exact, and each scale is a
    // power of two, so its multiplication is exact too.
    constexpr double kPieceScale[6] = {0x1p-32,  0x1p-64,  0x1p-96,
                                       0x1p-128, 0x1p-160, 0x1p-192};
    Two value{0.0, 0.0};
    for (int i = 5; i >= 0; --i) {
        const std::uint64_t word = fraction[i / 2];
        const std::uint64_t piece =
            (i % 2 == 0) ? (word >> 32) : (word & 0xffffffffull);
        value = dd_add(value, Two{static_cast<double>(piece) * kPieceScale[i], 0.0});
    }
    if (beyond_half) value = negated(value);

    return {quadrant, dd_mul(value, kHalfPi)};
}

/// The largest argument reduced by the identity rather than by the machinery
/// above: at or below this the reduced argument IS the argument, exactly,
/// and a reduction could only make it worse — sin(x) would stop being x for
/// a subnormal x, which is the one place the answer is trivially perfect.
///
/// It is the nearest double to pi/4, which happens to fall BELOW pi/4, so
/// every argument taking the shortcut genuinely is inside the kernel's
/// domain and no comparison has to be argued about.
constexpr double kQuarterPiBelow = 0x1.921fb54442d18p-1;

/// Sine and cosine of `radians` as a consistent double-double pair, for a
/// finite argument.
///
/// The sign is carried by the odd/even symmetry rather than folded into the
/// reduction, so sin(-x) == -sin(x) and cos(-x) == cos(x) hold to the last
/// bit rather than to within a rounding.
void sincos_radians(double radians, Two& s, Two& c) noexcept {
    const double a = magnitude(radians);
    const Quadrant reduced =
        a > kQuarterPiBelow ? reduce_radians(a) : Quadrant{0, {a, 0.0}};

    Two sine{0.0, 0.0};
    Two cosine{0.0, 0.0};
    sincos_kernel(reduced.radians, sine, cosine);
    switch (reduced.quadrant) {
    case 0:
        break;
    case 1: { // sin(pi/2 + r) = cos r, cos(pi/2 + r) = -sin r
        const Two t = sine;
        sine = cosine;
        cosine = negated(t);
        break;
    }
    case 2:
        sine = negated(sine);
        cosine = negated(cosine);
        break;
    default: { // 3 pi/2 + r
        const Two t = sine;
        sine = negated(cosine);
        cosine = t;
        break;
    }
    }
    s = radians < 0.0 ? negated(sine) : sine;
    c = cosine;
}

// -- the arc functions ---------------------------------------------------------

/// The square root of a non-negative double-double, by one Newton step on
/// the hardware root. `sqrt` is correctly rounded by IEEE-754 mandate, so
/// the starting point is already good to half an ulp and the correction —
/// whose residue is formed EXACTLY — carries the result to about 106 bits.
Two dd_sqrt(Two w) noexcept {
    if (!(w.hi > 0.0)) return {0.0, 0.0};
    const double root = std::sqrt(w.hi);
    const Two residue = dd_add(w, negated(two_product(root, root)));
    return quick_two_sum(root, (residue.hi + residue.lo) / (2.0 * root));
}

/// sqrt((1 - x)(1 + x)) as a double-double, for |x| <= 1 — the other leg of
/// the unit right triangle whose angles are asin(x) and acos(x).
///
/// The FACTORED form is the whole point, and it is why these two functions
/// can be derived at all. Both differences are formed EXACTLY (Sterbenz's
/// condition holds for 1 - x above a half, and `two_sum` recovers the
/// residue everywhere else), so the cancellation that destroys `1 - x*x` as
/// |x| approaches 1 happens here in exact arithmetic instead: at
/// x = 1 - 2^-53 the naive form has already lost half its digits and this
/// one has lost none.
Two arc_complement(double x) noexcept {
    return dd_sqrt(dd_mul(two_sum(1.0, -x), two_sum(1.0, x)));
}

/// atan(y / x) in RADIANS for double-double operands in the first quadrant.
/// Only the arc functions need it, and they need it in double-double
/// because the ratio they form is the whole of their accuracy.
Two atan_quadrant(Two y, Two x) noexcept {
    if (y.hi == 0.0) return {0.0, 0.0};
    if (x.hi == 0.0) return kHalfPi;
    if (y.hi < x.hi) return atan_unit_radians(dd_div(y, x));
    return dd_add(kHalfPi, negated(atan_unit_radians(dd_div(x, y))));
}

} // namespace

void sincos_deg(double degrees, double& sin_out, double& cos_out) noexcept {
    if (!std::isfinite(degrees)) {
        sin_out = cos_out = degrees - degrees; // NaN, without naming a macro
        return;
    }

    // The sign is handled by the odd/even symmetry rather than by folding it
    // into the reduction, so sin(-x) == -sin(x) holds to the last bit.
    const bool negative = degrees < 0.0;
    double d = reduce_degrees(negative ? -degrees : degrees);

    int quadrant = 0;
    while (d >= 90.0) {
        d -= 90.0;
        ++quadrant;
    }
    bool swapped = false;
    if (d > 45.0) {
        d = 90.0 - d;
        swapped = true;
    }

    double s = 0.0;
    double c = 0.0;
    kernel(d, s, c);
    if (swapped) {
        const double t = s;
        s = c;
        c = t;
    }
    switch (quadrant) {
    case 0:
        break;
    case 1: { // sin(90+r) = cos r, cos(90+r) = -sin r
        const double t = s;
        s = c;
        c = -t;
        break;
    }
    case 2:
        s = -s;
        c = -c;
        break;
    default: { // 270 + r
        const double t = s;
        s = -c;
        c = t;
        break;
    }
    }
    if (negative) s = -s;
    sin_out = unsign_zero(s);
    cos_out = unsign_zero(c);
}

double sin_deg(double degrees) noexcept {
    double s = 0.0;
    double c = 0.0;
    sincos_deg(degrees, s, c);
    return s;
}

double cos_deg(double degrees) noexcept {
    double s = 0.0;
    double c = 0.0;
    sincos_deg(degrees, s, c);
    return c;
}

double atan2_deg(double y, double x) noexcept {
    if (y != y || x != x) return y - y + (x - x); // NaN in, NaN out

    const bool finite_y = std::isfinite(y);
    const bool finite_x = std::isfinite(x);
    Two angle{45.0, 0.0}; // the |y| == |x| answer, including inf/inf
    if (finite_y || finite_x) {
        const double ay = magnitude(y);
        const double ax = magnitude(x);
        if (ay == 0.0)
            angle = {0.0, 0.0};
        else if (ax == 0.0 || !finite_y)
            angle = {90.0, 0.0};
        else if (!finite_x)
            angle = {0.0, 0.0};
        else if (ay == ax)
            angle = {45.0, 0.0};
        else if (ay < ax)
            angle = atan_unit_degrees(unit_ratio(ay, ax));
        else
            angle = dd_add(Two{90.0, 0.0},
                           negated(atan_unit_degrees(unit_ratio(ax, ay))));
    }

    // Quadrant. Both reflections stay in double-double so that the angle is
    // rounded to a double exactly once, at the return.
    const Two result = x < 0.0 ? dd_add(Two{180.0, 0.0}, negated(angle)) : angle;
    return unsign_zero(y < 0.0 ? -result.hi : result.hi);
}

// -- the radian entry points ---------------------------------------------------

double sin(double radians) noexcept {
    if (!std::isfinite(radians)) return radians - radians; // NaN, without a macro
    Two s{0.0, 0.0};
    Two c{0.0, 0.0};
    sincos_radians(radians, s, c);
    return unsign_zero(s.hi);
}

double cos(double radians) noexcept {
    if (!std::isfinite(radians)) return radians - radians;
    Two s{0.0, 0.0};
    Two c{0.0, 0.0};
    sincos_radians(radians, s, c);
    return unsign_zero(c.hi);
}

double tan(double radians) noexcept {
    if (!std::isfinite(radians)) return radians - radians;
    // The quotient of the double-double PAIR, not of two rounded doubles:
    // dividing two values that have each already been rounded would carry
    // both roundings into a third, and the reason the kernel returns pairs
    // at all is to keep that from happening here.
    Two s{0.0, 0.0};
    Two c{0.0, 0.0};
    sincos_radians(radians, s, c);
    return unsign_zero(dd_div(s, c).hi);
}

double atan(double x) noexcept {
    if (x != x) return x - x;
    const double a = magnitude(x);
    Two angle{0.0, 0.0};
    if (!std::isfinite(a))
        angle = kHalfPi;
    else if (a > 0.0)
        angle = a <= 1.0 ? atan_unit_radians(Two{a, 0.0})
                         : dd_add(kHalfPi, negated(atan_unit_radians(unit_ratio(1.0, a))));
    return unsign_zero(x < 0.0 ? -angle.hi : angle.hi);
}

double atan2(double y, double x) noexcept {
    if (y != y || x != x) return y - y + (x - x); // NaN in, NaN out

    const bool finite_y = std::isfinite(y);
    const bool finite_x = std::isfinite(x);
    Two angle = kQuarterPi; // the |y| == |x| answer, including inf/inf
    if (finite_y || finite_x) {
        const double ay = magnitude(y);
        const double ax = magnitude(x);
        if (ay == 0.0)
            angle = {0.0, 0.0};
        else if (ax == 0.0 || !finite_y)
            angle = kHalfPi;
        else if (!finite_x)
            angle = {0.0, 0.0};
        else if (ay == ax)
            angle = kQuarterPi;
        else if (ay < ax)
            angle = atan_unit_radians(unit_ratio(ay, ax));
        else
            angle = dd_add(kHalfPi, negated(atan_unit_radians(unit_ratio(ax, ay))));
    }

    const Two result = x < 0.0 ? dd_add(kPi, negated(angle)) : angle;
    return unsign_zero(y < 0.0 ? -result.hi : result.hi);
}

double asin(double x) noexcept {
    if (x != x) return x - x;
    const double a = magnitude(x);
    if (a > 1.0) return kNaN; // outside the domain, not an angle
    const Two angle = atan_quadrant(Two{a, 0.0}, arc_complement(a));
    return unsign_zero(x < 0.0 ? -angle.hi : angle.hi);
}

double acos(double x) noexcept {
    if (x != x) return x - x;
    const double a = magnitude(x);
    if (a > 1.0) return kNaN;
    // The magnitude decides the acute angle and the sign reflects it, which
    // is what makes acos(-1) exactly the correctly rounded pi rather than a
    // difference of two roundings.
    const Two acute = atan_quadrant(arc_complement(a), Two{a, 0.0});
    const Two angle = x < 0.0 ? dd_add(kPi, negated(acute)) : acute;
    return unsign_zero(angle.hi);
}

} // namespace cworks
