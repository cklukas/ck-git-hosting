// libcworks — shared utilities for CK Office
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Deterministic formatting/parsing. Calendar arithmetic uses the
// proleptic-Gregorian civil algorithms (Howard Hinnant's construction)
// so no libc time function, locale, or timezone database is involved.
#include "cworks/format.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

#if !CWORKS_HAS_FP_FROM_CHARS
// Deterministic, locale-free double PARSING fallback for toolchains whose
// libc++ gates floating-point std::from_chars behind an OS version (Apple
// libc++ → macOS 26.0). strtod_l with a fixed "C" locale is correctly rounded
// (round-to-nearest) — identical to from_chars for the plain decimals this
// suite accepts. Confined to this translation unit and this build path.
#include <clocale>
#include <cstdlib>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#endif

namespace cworks {

namespace {

constexpr std::int64_t kMicrosPerSecond = 1'000'000;
constexpr std::int64_t kMicrosPerDay = 86'400 * kMicrosPerSecond;

/// Floor division/modulo for possibly negative dividends.
constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    return (a >= 0) ? a / b : -((-a + b - 1) / b);
}
constexpr std::int64_t floor_mod(std::int64_t a, std::int64_t b) noexcept {
    return a - floor_div(a, b) * b;
}

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

/// Parse exactly `digits` decimal digits from the front of `s` into
/// `out` and advance `s`. Returns false on any deviation.
bool take_fixed_uint(std::string_view& s, std::size_t digits, int& out) {
    if (s.size() < digits || !all_digits(s.substr(0, digits))) return false;
    int value = 0;
    for (std::size_t i = 0; i < digits; ++i) value = value * 10 + (s[i] - '0');
    out = value;
    s.remove_prefix(digits);
    return true;
}

bool take_char(std::string_view& s, char c) {
    if (s.empty() || s.front() != c) return false;
    s.remove_prefix(1);
    return true;
}

} // namespace

// -- numbers --------------------------------------------------------------

std::string format_double(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
    std::array<char, 64> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return std::string(buf.data(), res.ptr);
}

std::string format_double_fixed(double value, int max_decimals) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
    std::array<char, 512> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value,
                                   std::chars_format::fixed, max_decimals);
    std::string text(buf.data(), res.ptr);
    if (max_decimals > 0) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return text;
}

std::string format_int(std::int64_t value) {
    std::array<char, 24> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return std::string(buf.data(), res.ptr);
}

std::string format_compact(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
    if (value == 0.0) return "0"; // also catches -0.0

    const bool neg = value < 0.0;
    const double m = std::abs(value);

    // Round to three significant figures via to_chars scientific(2). The
    // standard guarantees correctly-rounded ties-to-even, byte-identical
    // across libstdc++/libc++/MSVC — unlike printf, whose halfway rounding
    // is implementation-defined (the 999.5 boundary the spec pins). Output
    // has the exact shape "D.DDe±XX": one leading digit, two fractional
    // digits, and a signed exponent of at least two digits.
    std::array<char, 32> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), m,
                                   std::chars_format::scientific, 2);
    const std::string s(buf.data(), res.ptr);
    const std::string digits = {s[0], s[2], s[3]}; // the three significant digits

    // Parse the exponent (skip the explicit leading '+' that from_chars rejects).
    const std::size_t epos = s.find('e');
    int exp = 0;
    std::from_chars(s.data() + epos + (s[epos + 1] == '+' ? 2 : 1), s.data() + s.size(), exp);

    static constexpr std::array<const char*, 9> kSuffix = {
        "", "k", "M", "G", "T", "P", "E", "Z", "Y"};

    std::string body;
    std::string suffix;
    if (exp >= 0) {
        const int p = std::min(exp / 3, 8); // thousands-group index, capped at "Y"
        const int es = exp - 3 * p;          // exponent within the group, 0..2 (>2 only when capped)
        const int integer_digits = es + 1;
        suffix = kSuffix[static_cast<std::size_t>(p)];
        if (integer_digits >= 3)
            body = digits + std::string(static_cast<std::size_t>(integer_digits - 3), '0');
        else
            body = digits.substr(0, static_cast<std::size_t>(integer_digits)) + "." +
                   digits.substr(static_cast<std::size_t>(integer_digits));
    } else {
        // |value| < 1: no small-side suffix, emit a plain 3-sig-fig decimal.
        body = "0." + std::string(static_cast<std::size_t>(-exp - 1), '0') + digits;
    }

    // Trim a trailing fractional zero run and any dangling point.
    if (body.find('.') != std::string::npos) {
        while (!body.empty() && body.back() == '0') body.pop_back();
        if (!body.empty() && body.back() == '.') body.pop_back();
    }

    return (neg && body != "0" ? "-" : "") + body + suffix;
}

bool parse_int(std::string_view text, std::int64_t& out) {
    if (text.empty()) return false;
    std::int64_t value = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) return false;
    out = value;
    return true;
}

bool parse_double(std::string_view text, double& out) {
    if (text.empty()) return false;
    // Reject forms we consider non-numeric data even though strtod-style
    // parsers accept them: hex floats and textual infinities/NaNs are not
    // numbers in CK Office documents.
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        return false;
    for (char c : text) {
        const bool numeric_char = (c >= '0' && c <= '9') || c == '+' || c == '-' ||
                                  c == '.' || c == 'e' || c == 'E';
        if (!numeric_char) return false;
    }
    double value = 0;
#if CWORKS_HAS_FP_FROM_CHARS
    const auto res = std::from_chars(text.data(), text.data() + text.size(), value);
    if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) return false;
#else
    // Match std::from_chars(double) semantics exactly so parsing is byte-for-
    // byte identical on every platform (the suite's determinism invariant):
    //   * no leading '+' (from_chars rejects it; the char scan above already
    //     forbids leading whitespace),
    //   * the ENTIRE string must be consumed,
    //   * an out-of-range magnitude is a PARSE FAILURE, not a saturation to
    //     ±inf — from_chars reports result_out_of_range here, and strtod would
    //     otherwise return HUGE_VAL and "succeed", so "1e400" must fail on both
    //     paths, never become inf on this one.
    // Under the default round-to-nearest FP environment (the suite never
    // changes fegetround), strtod is correctly rounded and agrees with
    // from_chars bit-for-bit for every value both accept.
    if (text.front() == '+') return false;   // from_chars rejects a leading '+'
    static const ::locale_t kCLocale = ::newlocale(LC_ALL_MASK, "C", (::locale_t)0);
    if (!kCLocale) return false;             // newlocale("C") failed: refuse, don't UB
    const std::string buf(text);             // strtod needs a NUL terminator
    char* end = nullptr;
    value = ::strtod_l(buf.c_str(), &end, kCLocale);
    if (end != buf.c_str() + buf.size()) return false;
    // Overflow saturates to ±inf (nan is unreachable — the char scan forbids
    // 'n'/'a'); from_chars rejects such inputs, so reject them here too.
    if (!std::isfinite(value)) return false;
#endif
    out = value;
    return true;
}

// -- calendar time ----------------------------------------------------------

std::int64_t days_from_civil(int y, int m, int d) noexcept {
    // Howard Hinnant, "chrono-Compatible Low-Level Date Algorithms".
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
    const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5 +
                         static_cast<unsigned>(d) - 1;                      // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // [0, 146096]
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, int& year, int& month, int& day) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);           // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                                // [0, 11]
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;                        // [1, 31]
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;                           // [1, 12]
    year = static_cast<int>(y + (m <= 2));
    month = static_cast<int>(m);
    day = static_cast<int>(d);
}

bool is_valid_civil(int year, int month, int day) noexcept {
    if (month < 1 || month > 12 || day < 1) return false;
    static constexpr int lengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = lengths[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) max_day = 29;
    return day <= max_day;
}

bool parse_datetime(std::string_view text, std::int64_t& micros_utc) {
    std::string_view s = text;
    bool negative_year = false;
    if (!s.empty() && s.front() == '-') {
        negative_year = true;
        s.remove_prefix(1);
    }
    int year = 0, month = 0, day = 0;
    if (!take_fixed_uint(s, 4, year)) return false;
    if (negative_year) year = -year;
    if (!take_char(s, '-') || !take_fixed_uint(s, 2, month)) return false;
    if (!take_char(s, '-') || !take_fixed_uint(s, 2, day)) return false;
    if (!is_valid_civil(year, month, day)) return false;

    int hour = 0, minute = 0, second = 0;
    std::int64_t fraction_micros = 0;
    if (!s.empty() && (s.front() == ' ' || s.front() == 'T')) {
        s.remove_prefix(1);
        if (!take_fixed_uint(s, 2, hour) || !take_char(s, ':') ||
            !take_fixed_uint(s, 2, minute))
            return false;
        if (hour > 23 || minute > 59) return false;
        if (!s.empty() && s.front() == ':') {
            s.remove_prefix(1);
            if (!take_fixed_uint(s, 2, second) || second > 59) return false;
            if (!s.empty() && s.front() == '.') {
                s.remove_prefix(1);
                std::size_t digits = 0;
                while (digits < s.size() && s[digits] >= '0' && s[digits] <= '9') ++digits;
                if (digits < 1 || digits > 6) return false;
                std::int64_t frac = 0;
                for (std::size_t i = 0; i < digits; ++i) frac = frac * 10 + (s[i] - '0');
                for (std::size_t i = digits; i < 6; ++i) frac *= 10;
                fraction_micros = frac;
                s.remove_prefix(digits);
            }
        }
    }

    // Optional timezone designator, normalized to UTC.
    std::int64_t offset_micros = 0;
    if (!s.empty()) {
        if (s.front() == 'Z') {
            s.remove_prefix(1);
        } else if (s.front() == '+' || s.front() == '-') {
            const bool negative = s.front() == '-';
            s.remove_prefix(1);
            int oh = 0, om = 0;
            if (!take_fixed_uint(s, 2, oh) || !take_char(s, ':') ||
                !take_fixed_uint(s, 2, om))
                return false;
            if (oh > 23 || om > 59) return false;
            offset_micros = (static_cast<std::int64_t>(oh) * 3600 + om * 60) *
                            kMicrosPerSecond;
            if (negative) offset_micros = -offset_micros;
        }
    }
    if (!s.empty()) return false; // strict full-string match

    const std::int64_t days = days_from_civil(year, month, day);
    micros_utc = days * kMicrosPerDay +
                 (static_cast<std::int64_t>(hour) * 3600 + minute * 60 + second) *
                     kMicrosPerSecond +
                 fraction_micros - offset_micros;
    return true;
}

std::string format_datetime(std::int64_t micros_utc) {
    const std::int64_t days = floor_div(micros_utc, kMicrosPerDay);
    std::int64_t rem = floor_mod(micros_utc, kMicrosPerDay);
    int year = 0, month = 0, day = 0;
    civil_from_days(days, year, month, day);

    char buf[64];
    if (rem == 0) {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
        return buf;
    }
    const int hour = static_cast<int>(rem / (3600 * kMicrosPerSecond));
    rem %= 3600 * kMicrosPerSecond;
    const int minute = static_cast<int>(rem / (60 * kMicrosPerSecond));
    rem %= 60 * kMicrosPerSecond;
    const int second = static_cast<int>(rem / kMicrosPerSecond);
    std::int64_t frac = rem % kMicrosPerSecond;

    if (frac == 0) {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day,
                      hour, minute, second);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06lld", year, month,
                  day, hour, minute, second, static_cast<long long>(frac));
    std::string out = buf;
    while (out.back() == '0') out.pop_back(); // fraction is nonzero: no dot removal risk
    return out;
}

// -- durations ---------------------------------------------------------------

std::string format_duration(std::int64_t micros) {
    const bool negative = micros < 0;
    // Careful with INT64_MIN: work in unsigned magnitude.
    std::uint64_t mag = negative
                            ? (static_cast<std::uint64_t>(-(micros + 1)) + 1ULL)
                            : static_cast<std::uint64_t>(micros);
    const std::uint64_t whole = mag / kMicrosPerSecond;
    std::uint64_t frac = mag % kMicrosPerSecond;

    std::string out;
    if (negative) out += '-';
    out += std::to_string(whole);
    if (frac != 0) {
        char fbuf[8];
        std::snprintf(fbuf, sizeof(fbuf), "%06llu", static_cast<unsigned long long>(frac));
        std::string f = fbuf;
        while (f.back() == '0') f.pop_back();
        out += '.';
        out += f;
    }
    out += 's';
    return out;
}

bool parse_duration(std::string_view text, std::int64_t& micros) {
    std::string_view s = text;
    if (s.size() < 2 || s.back() != 's') return false;
    s.remove_suffix(1);
    bool negative = false;
    if (!s.empty() && s.front() == '-') {
        negative = true;
        s.remove_prefix(1);
    }
    const std::size_t dot = s.find('.');
    std::string_view whole = dot == std::string_view::npos ? s : s.substr(0, dot);
    std::string_view frac = dot == std::string_view::npos ? std::string_view{}
                                                          : s.substr(dot + 1);
    if (!all_digits(whole)) return false;
    if (dot != std::string_view::npos && (frac.empty() || frac.size() > 6 || !all_digits(frac)))
        return false;

    std::int64_t seconds = 0;
    if (!parse_int(whole, seconds)) return false;
    std::int64_t fraction = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        fraction *= 10;
        if (i < frac.size()) fraction += frac[i] - '0';
    }
    if (seconds > (std::numeric_limits<std::int64_t>::max() - fraction) / kMicrosPerSecond)
        return false; // overflow
    micros = seconds * kMicrosPerSecond + fraction;
    if (negative) micros = -micros;
    return true;
}

} // namespace cworks
