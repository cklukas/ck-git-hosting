// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cplot/ticks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "cworks/format.hpp"
#include "decade.hpp"
#include "format_c.hpp"

namespace cplot::detail {

namespace {

constexpr double kEps = 1e-9;

/// Snap a value that is within floating-point noise of a step multiple.
double snap(double value, double step) {
    const double snapped = std::round(value / step) * step;
    if (std::abs(snapped - value) < step * 1e-6) return snapped;
    return value;
}

/// Decimals needed to print a value that lives on a grid of `step`.
///
/// This is `ceil(-log10(step))` with the logarithm taken out: the answer is
/// the negated decade of the step, and asking libm for a decade that a
/// `ceil` then turns into an integer is exactly the pattern decade.hpp
/// exists to remove — one ulp either way relabels every tick on the axis.
int decimals_for_step(double step) {
    if (!(step > 0) || !std::isfinite(step)) return 0;
    return std::clamp(-decade_floor(step), 0, 12);
}

std::string trim_number(std::string s) {
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

} // namespace

double nice_step(double range, int target_count) {
    if (target_count < 2) target_count = 2;
    if (!(range > 0) || !std::isfinite(range)) return 1.0;
    const double raw = range / target_count;
    if (!(raw > 0)) return 1.0; // the quotient underflowed; no decade exists
    // THE decade decision of the chart engine, and the reason decade.hpp
    // exists: the ladder below amplifies one ulp in `mag` into a different
    // factor, hence a different step, tick count, axis domain and label set.
    // Real ranges land on its boundaries exactly — 30 across 10 ticks gives
    // raw == 3.0 — so `mag` must be the same double everywhere by
    // construction, not by the local libm's choice of rounding.
    const double mag = cworks::pow10(cworks::floor_log10(raw));
    const double norm = raw / mag; // in [1, 10)
    double factor;
    if (norm < 1.5)
        factor = 1.0;
    else if (norm < 3.0)
        factor = 2.0;
    else if (norm < 7.0)
        factor = 5.0;
    else
        factor = 10.0;
    return factor * mag;
}

NiceRange nice_range(double lo, double hi, int target_count) {
    if (!std::isfinite(lo) || !std::isfinite(hi)) return {0.0, 1.0, 0.2};
    if (lo > hi) std::swap(lo, hi);
    if (hi - lo < kEps * std::max(1.0, std::abs(lo))) {
        // Degenerate range: expand around the value.
        const double pad = (std::abs(lo) > kEps) ? std::abs(lo) * 0.1 : 1.0;
        lo -= pad;
        hi += pad;
    }
    const double step = nice_step(hi - lo, target_count);
    NiceRange r;
    r.step = step;
    r.lo = std::floor(lo / step + kEps) * step;
    r.hi = std::ceil(hi / step - kEps) * step;
    return r;
}

std::string format_tick_value(double value, double step) {
    value = snap(value, step > 0 ? step : 1.0);
    if (value == 0.0) return "0";
    const double av = std::abs(value);
    if (av >= 1e6 || av < 1e-4) {
        return format_c("%g", value); // C locale: '.' radix (see format_c.hpp)
    }
    const int dec = decimals_for_step(step);
    return trim_number(format_c("%.*f", dec, value));
}

std::string format_with(const std::string& format, double value) {
    // Reserved keyword: SI-suffixed compact labels (1.2k, 3.4M). Checked
    // before any printf/brace parsing so it never falls through to format_c.
    if (format == "compact") return cworks::format_compact(value);
    // Accept both printf-style ("%.1f") and brace-style ("{:.1f}") formats.
    std::string fmt = format;
    if (fmt.size() >= 4 && fmt.front() == '{' && fmt.back() == '}') {
        // "{:.1f}" -> "%.1f"
        const auto colon = fmt.find(':');
        if (colon != std::string::npos) {
            fmt = "%" + fmt.substr(colon + 1, fmt.size() - colon - 2);
        }
    }
    // Only allow numeric conversions.
    const char last = fmt.empty() ? '\0' : fmt.back();
    if (fmt.empty() || fmt.front() != '%' ||
        (last != 'f' && last != 'g' && last != 'e' && last != 'E' && last != 'G')) {
        return format_tick_value(value, 0.0);
    }
    // fmt is validated above to be a single numeric conversion; C locale so
    // the tick label radix is '.' regardless of LC_NUMERIC (see format_c.hpp).
    return format_c(fmt.c_str(), value);
}

TickSet linear_ticks(double lo, double hi, int target_count, const std::string& format,
                     bool minor_ticks) {
    TickSet set;
    if (lo > hi) std::swap(lo, hi);
    const double step = nice_step(hi - lo, target_count);
    set.step = step;
    const double first = std::ceil(lo / step - kEps) * step;
    const double tol = step * 1e-6;
    for (double v = first; v <= hi + tol; v += step) {
        const double value = snap(v, step);
        Tick t;
        t.value = value;
        t.label = format.empty() ? format_tick_value(value, step) : format_with(format, value);
        set.ticks.push_back(std::move(t));
        if (set.ticks.size() > 1000) break; // safety
    }
    if (minor_ticks && !set.ticks.empty()) {
        const double minor_step = step / 5.0;
        const double mfirst = std::ceil(lo / minor_step - kEps) * minor_step;
        for (double v = mfirst; v <= hi + tol; v += minor_step) {
            const double value = snap(v, minor_step);
            // Skip positions that coincide with major ticks.
            const double ratio = value / step;
            if (std::abs(ratio - std::round(ratio)) < 1e-6) continue;
            set.minor.push_back(value);
            if (set.minor.size() > 5000) break;
        }
    }
    return set;
}

TickSet log_ticks(double lo, double hi, const std::string& format, bool minor_ticks,
                  double base) {
    TickSet set;
    if (lo > hi) std::swap(lo, hi);
    if (!(lo > 0)) lo = 1e-12;
    if (!(hi > 0)) hi = 1.0;
    if (!(base > 1.0)) base = 10.0;
    const bool decimal = base == 10.0; // keep the base-10 look exactly
    // How many decades the axis spans is a discrete decision — it fixes the
    // number of ticks — so it is settled by comparison against the decade
    // boundaries rather than by flooring a logarithm (see decade.hpp).
    const int e_lo = decade_floor(lo, base);
    const int e_hi = decade_ceil(hi, base);
    const int decades = e_hi - e_lo;

    auto fmt = [&](double v) {
        if (!format.empty()) return format_with(format, v);
        if (v >= 1e-4 && v < 1e6) return format_tick_value(v, v);
        return format_c("%g", v); // C locale: '.' radix (see format_c.hpp)
    };

    const double tol_lo = lo * (1.0 - 1e-9);
    const double tol_hi = hi * (1.0 + 1e-9);
    for (int e = e_lo; e <= e_hi; ++e) {
        const double decade = decade_value(e, base);
        if (decade >= tol_lo && decade <= tol_hi) {
            set.ticks.push_back({decade, fmt(decade), false});
        }
        if (decimal && decades <= 1) {
            // Few decades: label 2 and 5 as well.
            for (double m : {2.0, 5.0}) {
                const double v = m * decade;
                if (v >= tol_lo && v <= tol_hi) set.ticks.push_back({v, fmt(v), false});
            }
        }
        if (decimal && (minor_ticks || decades <= 6)) {
            for (int m = 2; m <= 9; ++m) {
                const double v = m * decade;
                if (v >= tol_lo && v <= tol_hi) set.minor.push_back(v);
            }
        } else if (!decimal && minor_ticks) {
            // Non-decimal bases: minor ticks at the integer multiples that
            // fall below the next power (e.g. 3·, 5·, 7· for base 2).
            for (int m = 2; static_cast<double>(m) < base + 0.5; ++m) {
                const double v = m * decade;
                if (v >= tol_lo && v <= tol_hi) set.minor.push_back(v);
            }
        }
    }
    std::sort(set.ticks.begin(), set.ticks.end(),
              [](const Tick& a, const Tick& b) { return a.value < b.value; });
    std::sort(set.minor.begin(), set.minor.end());
    // Remove minor positions that duplicate major ticks.
    set.minor.erase(std::remove_if(set.minor.begin(), set.minor.end(),
                                   [&](double v) {
                                       for (const auto& t : set.ticks)
                                           if (std::abs(v - t.value) <= v * 1e-9) return true;
                                       return false;
                                   }),
                    set.minor.end());
    return set;
}

TickSet symlog_ticks(double lo, double hi, double linthresh, const std::string& format,
                     bool minor_ticks) {
    TickSet set;
    if (lo > hi) std::swap(lo, hi);
    if (!(linthresh > 0)) linthresh = 1.0;

    auto fmt = [&](double v) {
        if (!format.empty()) return format_with(format, v);
        if (std::abs(v) >= 1e-4 && std::abs(v) < 1e6) return format_tick_value(v, v);
        return format_c("%g", v); // C locale: '.' radix (see format_c.hpp)
    };
    const double tol_lo = lo - std::abs(lo) * 1e-9 - 1e-12;
    const double tol_hi = hi + std::abs(hi) * 1e-9 + 1e-12;
    auto add_major = [&](double v) {
        if (v >= tol_lo && v <= tol_hi) set.ticks.push_back({v, fmt(v), false});
    };

    add_major(0.0);
    add_major(linthresh);
    add_major(-linthresh);
    // Base-10 decades on the positive and negative log-compressed wings.
    for (double d = linthresh * 10.0; d <= tol_hi; d *= 10.0) {
        add_major(d);
        if (minor_ticks)
            for (int m = 2; m <= 9; ++m) {
                const double v = m * (d / 10.0);
                if (v > linthresh && v >= tol_lo && v <= tol_hi) set.minor.push_back(v);
            }
        if (set.ticks.size() > 1000) break; // safety
    }
    for (double d = -linthresh * 10.0; d >= tol_lo; d *= 10.0) {
        add_major(d);
        if (minor_ticks)
            for (int m = 2; m <= 9; ++m) {
                const double v = m * (d / 10.0);
                if (v < -linthresh && v >= tol_lo && v <= tol_hi) set.minor.push_back(v);
            }
        if (set.ticks.size() > 2000) break; // safety
    }
    std::sort(set.ticks.begin(), set.ticks.end(),
              [](const Tick& a, const Tick& b) { return a.value < b.value; });
    std::sort(set.minor.begin(), set.minor.end());
    return set;
}

// -- date/time ticks -----------------------------------------------------------

namespace {

std::time_t to_utc_time(std::tm& tm) {
#if defined(_WIN32)
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

std::tm utc_tm(double timestamp) {
    const std::time_t t = static_cast<std::time_t>(std::floor(timestamp));
    std::tm out{};
#if defined(_WIN32)
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

} // namespace

std::string format_datetime(double timestamp, const std::string& format) {
    std::tm tm = utc_tm(timestamp);
    char buf[128];
    if (std::strftime(buf, sizeof(buf), format.c_str(), &tm) == 0) return {};
    return buf;
}

TickSet datetime_ticks(double lo, double hi, int target_count, const std::string& format) {
    TickSet set;
    if (lo > hi) std::swap(lo, hi);
    const double span = std::max(1.0, hi - lo);
    if (target_count < 2) target_count = 2;

    // Fixed-length units in seconds, with allowed step multiples.
    struct Unit {
        double seconds;
        std::vector<int> steps;
        const char* fmt;
    };
    static const Unit units[] = {
        {1.0, {1, 2, 5, 10, 15, 30}, "%H:%M:%S"},
        {60.0, {1, 2, 5, 10, 15, 30}, "%H:%M"},
        {3600.0, {1, 2, 3, 6, 12}, "%H:%M"},
        {86400.0, {1, 2, 7, 14}, "%Y-%m-%d"},
    };

    const double target_step = span / target_count;

    // Months / years need calendar stepping.
    if (target_step >= 86400.0 * 20.0) {
        const bool years = target_step >= 86400.0 * 365.0 * 0.8;
        std::tm start = utc_tm(lo);
        start.tm_sec = start.tm_min = start.tm_hour = 0;
        start.tm_mday = 1;
        const char* fmt = years ? "%Y" : "%Y-%m";
        int step;
        if (years) {
            start.tm_mon = 0;
            const double yspan = span / (86400.0 * 365.25);
            step = std::max(1, static_cast<int>(nice_step(yspan, target_count)));
        } else {
            const double mspan = span / (86400.0 * 30.44);
            const int msteps[] = {1, 2, 3, 6};
            step = 6;
            for (int s : msteps) {
                if (mspan / s <= target_count + 1) {
                    step = s;
                    break;
                }
            }
            start.tm_mon = (start.tm_mon / step) * step;
        }
        std::tm cur = start;
        for (int guard = 0; guard < 1000; ++guard) {
            std::tm tmp = cur;
            const std::time_t t = to_utc_time(tmp);
            const double v = static_cast<double>(t);
            if (v > hi + 1.0) break;
            if (v >= lo - 1.0) {
                set.ticks.push_back(
                    {v, format_datetime(v, format.empty() ? fmt : format), false});
            }
            if (years)
                cur.tm_year += step;
            else
                cur.tm_mon += step;
        }
        return set;
    }

    // Fixed-length units.
    const Unit* unit = &units[0];
    int step = unit->steps.back();
    bool found = false;
    for (const Unit& u : units) {
        for (int s : u.steps) {
            if (target_step <= u.seconds * s) {
                unit = &u;
                step = s;
                found = true;
                break;
            }
        }
        if (found) break;
    }
    if (!found) {
        unit = &units[3];
        step = unit->steps.back();
    }

    const double step_seconds = unit->seconds * step;
    // Align the first tick to a unit boundary.
    double first;
    if (unit->seconds >= 86400.0) {
        std::tm tm = utc_tm(lo);
        tm.tm_sec = tm.tm_min = tm.tm_hour = 0;
        first = static_cast<double>(to_utc_time(tm));
        while (first < lo - 1e-9) first += step_seconds;
    } else {
        first = std::ceil(lo / step_seconds) * step_seconds;
    }

    // If days span multiple months the label needs the date; short spans
    // crossing midnight need day context too.
    std::string fmt = format.empty() ? unit->fmt : format;
    if (format.empty() && unit->seconds < 86400.0 && span > 86400.0) fmt = "%m-%d %H:%M";

    for (double v = first; v <= hi + step_seconds * 1e-6; v += step_seconds) {
        set.ticks.push_back({v, format_datetime(v, fmt), false});
        if (set.ticks.size() > 1000) break;
    }
    set.step = step_seconds;
    return set;
}

} // namespace cplot::detail
