// libcworks — shared utilities for CK Office
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Deterministic, locale-independent formatting and parsing. These are
// the canonical textual forms of the suite's scalar types: every
// component that turns a number, timestamp, or duration into text (CSV
// writers, casts, `inspect` output, error messages) uses these so that
// output is byte-identical across components and platforms.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cworks {

// -- numbers --------------------------------------------------------------

/// Shortest round-trip decimal representation (std::to_chars): "1",
/// "0.5", "1e+100". NaN → "nan", infinities → "inf"/"-inf".
/// Deterministic on every platform.
std::string format_double(double value);

/// Fixed-notation decimal for reader-facing labels: never scientific,
/// at most `max_decimals` fractional digits, trailing zeros trimmed —
/// 100000 stays "100000" (format_double would say "1e+05") and 124.729
/// becomes "124.73" at two decimals. Not a round-trip form; use
/// format_double wherever the text is data.
std::string format_double_fixed(double value, int max_decimals);

/// Compact reader-facing form: three significant figures with fixed
/// English SI-style large-magnitude suffixes (k, M, G, T, P, E, Z, Y) —
/// 1200 → "1.2k", 3.4e6 → "3.4M", 5.6e9 → "5.6G". Rounding is
/// half-even and the promotion boundary is pinned (999.5 → "1k",
/// 999.49… → "999"). Intentionally lossy — a display form for dense
/// axes and dashboard readouts, never round-trip data; use
/// format_double wherever the text is data. Values below 1 render as a
/// plain three-significant-figure decimal (0.5 → "0.5"); there are no
/// small-magnitude (milli/micro) suffixes. NaN → "nan", infinities →
/// "inf"/"-inf". Deterministic and locale-free on every platform.
std::string format_compact(double value);

/// Canonical decimal representation of a 64-bit integer.
std::string format_int(std::int64_t value);

/// Strict full-string parses. Leading/trailing whitespace is NOT
/// accepted; callers trim first. Return false without touching `out`
/// on any deviation (empty, overflow, trailing garbage, "1,5", hex).
bool parse_int(std::string_view text, std::int64_t& out);
bool parse_double(std::string_view text, double& out);

// -- calendar time ----------------------------------------------------------
//
// The suite stores instants as microseconds since the Unix epoch, UTC
// (`DateTime` in ctable's Value). Conversion uses proleptic-Gregorian
// civil-calendar arithmetic (no libc time functions, no timezone
// database, no environment dependence).

/// Days since 1970-01-01 for a civil date (proleptic Gregorian).
std::int64_t days_from_civil(int year, int month, int day) noexcept;

/// Inverse of days_from_civil.
void civil_from_days(std::int64_t days, int& year, int& month, int& day) noexcept;

/// True when the civil date exists (month 1..12, day valid incl. leap years).
bool is_valid_civil(int year, int month, int day) noexcept;

/// Parse an ISO-8601-style datetime into UTC microseconds.
/// Accepted forms (the suite's documented set):
///   YYYY-MM-DD
///   YYYY-MM-DD HH:MM        /  YYYY-MM-DDTHH:MM
///   YYYY-MM-DD HH:MM:SS     /  ...THH:MM:SS
///   YYYY-MM-DD HH:MM:SS.ffffff   (1..6 fractional digits)
/// each optionally followed by 'Z' or a ±HH:MM offset, which is
/// normalized to UTC. Strict: full-string match, valid calendar date,
/// hours 0..23, minutes/seconds 0..59.
bool parse_datetime(std::string_view text, std::int64_t& micros_utc);

/// Canonical rendering of a UTC instant:
///   exactly midnight        → "YYYY-MM-DD"
///   whole seconds           → "YYYY-MM-DD HH:MM:SS"
///   with fraction           → "YYYY-MM-DD HH:MM:SS.ffffff" (trailing
///                              zeros of the fraction removed)
/// Negative years are rendered with a leading '-'.
std::string format_datetime(std::int64_t micros_utc);

// -- durations ---------------------------------------------------------------

/// Canonical duration form: decimal seconds with 's' suffix, computed
/// exactly in the integer domain ("90s", "-0.25s", "1.5s").
std::string format_duration(std::int64_t micros);

/// Parse the canonical duration form (strict full-string match,
/// at most 6 fractional digits).
bool parse_duration(std::string_view text, std::int64_t& micros);

} // namespace cworks
