// libcworks — the exact integer view of an IEEE double
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// A double IS its 64 bits, and reading them costs nothing and rounds nothing.
// Everything here that looks like a floating-point trick — extracting an
// exponent, scaling by a power of two, taking the significand as an integer —
// is exact integer bookkeeping wearing a floating-point coat, which is why
// none of it can differ between two platforms.
//
// The wide product at the bottom belongs with them for the same reason: the
// kernels that have to DECIDE a last bit rather than approximate it —
// `hypot`'s midpoint comparison, the radian argument reduction — do that
// decision on integers, and both need the exact 128-bit product of two 64-bit
// values.
//
// This header is PRIVATE to libcworks, like `dd.hpp` beside it, and names no
// domain concept: it is the representation, not the mathematics. It is not
// installed. The source scans in `test_math.cpp` and `test_trig.cpp` read it
// for the names of library functions that may round, which is why the prose
// here names none of them.
#pragma once

#include <cstdint>
#include <cstring> // std::memcpy — the bit view

namespace cworks::detail {

inline constexpr std::uint64_t kMagnitudeMask = 0x7fffffffffffffffull;
inline constexpr std::uint64_t kMantissaMask = 0x000fffffffffffffull;
inline constexpr std::uint64_t kImplicitBit = std::uint64_t{1} << 52;
inline constexpr double kMinNormal = 0x1p-1022;

inline std::uint64_t bits(double v) noexcept {
    std::uint64_t b = 0;
    std::memcpy(&b, &v, sizeof b);
    return b;
}

inline double from_bits(std::uint64_t b) noexcept {
    double v = 0.0;
    std::memcpy(&v, &b, sizeof v);
    return v;
}

inline double magnitude(double v) noexcept { return from_bits(bits(v) & kMagnitudeMask); }

/// floor(log2(|x|)) for a finite, non-zero `x` — subnormals included, which
/// is why this is not simply the biased exponent field.
inline int exponent_of(double x) noexcept {
    const std::uint64_t b = bits(x) & kMagnitudeMask;
    const int biased = static_cast<int>(b >> 52);
    if (biased != 0) return biased - 1023;
    // Subnormal: lift it into the normal range by an exact power of two and
    // read the exponent there.
    return static_cast<int>(bits(from_bits(b) * 0x1p54) >> 52) - 1023 - 54;
}

/// 2^n exactly, for -1074 <= n <= 1023 (subnormal powers included).
inline double pow2(int n) noexcept {
    if (n >= -1022) return from_bits(static_cast<std::uint64_t>(n + 1023) << 52);
    return from_bits(std::uint64_t{1} << (n + 1074));
}

/// v * 2^n. Multiplying by a power of two rounds nothing, so this is EXACT
/// whenever the result is a normal number — which is how it is used, except
/// where overflowing to +inf is precisely the correctly rounded answer. The
/// split keeps every factor representable.
inline double scale2(double v, int n) noexcept {
    while (n > 1023) {
        v *= 0x1p1023;
        n -= 1023;
    }
    while (n < -1022) {
        v *= 0x1p-1022;
        n += 1022;
    }
    return v * pow2(n);
}

/// The 53-bit significand of a NORMAL double as an integer:
/// x == significand(x) * 2^(exponent_of(x) - 52), exactly.
inline std::uint64_t significand(double x) noexcept {
    return (bits(x) & kMantissaMask) | kImplicitBit;
}

struct UInt128 {
    std::uint64_t lo;
    std::uint64_t hi;
};

/// The exact 128-bit product of two 64-bit values, by 32-bit halves: the same
/// instructions on every compiler, with no reliance on __int128 or on a
/// high-multiply intrinsic.
inline UInt128 mul_u64(std::uint64_t a, std::uint64_t b) noexcept {
    constexpr std::uint64_t kLow32 = 0xffffffffull;
    const std::uint64_t a0 = a & kLow32;
    const std::uint64_t a1 = a >> 32;
    const std::uint64_t b0 = b & kLow32;
    const std::uint64_t b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0;
    const std::uint64_t p01 = a0 * b1;
    const std::uint64_t p10 = a1 * b0;
    const std::uint64_t p11 = a1 * b1;
    const std::uint64_t mid = (p00 >> 32) + (p01 & kLow32) + (p10 & kLow32);
    return {(p00 & kLow32) | (mid << 32), p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32)};
}

} // namespace cworks::detail
