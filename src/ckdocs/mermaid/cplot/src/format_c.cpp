// ckplot — locale-independent number formatting
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "format_c.hpp"

#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#if defined(__APPLE__)
#include <xlocale.h> // strtod_l / newlocale / uselocale on macOS
#endif

namespace cplot::detail {

namespace {

// A process-lifetime "C" locale handle, created once on first use. newlocale
// is thread-safe and the returned handle is immutable, so it is shared across
// threads without a lock. Deliberately never freed — it lives for the whole
// process (a single static handle, not a per-call allocation).
::locale_t c_locale() {
    static ::locale_t loc = ::newlocale(LC_ALL_MASK, "C", static_cast<::locale_t>(0));
    return loc;
}

} // namespace

std::string format_c(const char* fmt, ...) {
    const ::locale_t c = c_locale();
    // Swap THIS thread's locale to "C" for the duration of the format, so
    // vsnprintf renders '.' as the decimal separator. uselocale is per-thread,
    // so this is safe under cplot's parallel raster/text rendering. If
    // newlocale failed we format in the ambient locale rather than risk UB.
    const ::locale_t prev = c ? ::uselocale(c) : static_cast<::locale_t>(0);

    char buf[256];
    std::va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::string out;
    if (n >= 0 && static_cast<std::size_t>(n) < sizeof(buf)) {
        out.assign(buf, static_cast<std::size_t>(n));
    } else if (n >= static_cast<int>(sizeof(buf))) {
        // Value needed a larger buffer (never reached by the suite's numbers,
        // but handled so this is a total drop-in for snprintf). Still in the
        // "C" thread locale here, so the retry stays locale-independent.
        out.resize(static_cast<std::size_t>(n));
        va_start(ap, fmt);
        std::vsnprintf(out.data(), out.size() + 1, fmt, ap);
        va_end(ap);
    }

    if (prev) ::uselocale(prev);
    return out;
}

double strtod_c(const char* str, char** end) {
    const ::locale_t c = c_locale();
    if (!c) return std::strtod(str, end); // newlocale failed: ambient fallback
    return ::strtod_l(str, end, c);
}

} // namespace cplot::detail
