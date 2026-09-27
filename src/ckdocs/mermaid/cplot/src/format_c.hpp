// ckplot — locale-independent number formatting (internal, not installed)
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The suite's determinism invariant requires byte-identical output on every
// host. Chart text and SVG/PDF coordinates are rendered with printf
// `%f`/`%g`/`%e` conversions, which honour LC_NUMERIC for the decimal
// separator. A process that adopts the user's locale with setlocale(LC_ALL,
// "") under a comma-radix locale (e.g. de_DE.UTF-8) then makes a plain
// snprintf("%.2f", 0.5) yield "0,5" — corrupting label
// text AND the SVG geometry itself (points="12,5,3,5"). These helpers pin the
// "C" locale so output is identical in every locale.  libcworks solves the
// same problem for its own scalar formatting via std::to_chars / strtod_l
// (libcworks/src/format.cpp); this is the printf-shaped equivalent for the
// chart backends, which need the exact `%f`/`%g` rounding they already emit.
#pragma once

#include <string>

namespace cplot::detail {

/// snprintf-into-string that ALWAYS formats with the "C" locale, so the
/// decimal separator is '.' regardless of the process LC_NUMERIC. A drop-in
/// for `std::snprintf(buf, n, fmt, ...)` followed by `std::string(buf)` — the
/// format string and rounding are unchanged, only the radix is pinned.
/// Thread-safe: the locale swap is per-thread, so cplot's parallel raster /
/// text rendering stays correct.
std::string format_c(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/// strtod pinned to the "C" locale: parses a leading double from `str` and,
/// like std::strtod, advances `*end` past the consumed text. Immune to
/// LC_NUMERIC so "1.5" parses to 1.5 in every locale.
double strtod_c(const char* str, char** end);

} // namespace cplot::detail
