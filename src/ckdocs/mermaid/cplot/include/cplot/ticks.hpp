// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace cplot {

/// One axis tick: position in data space plus rendered label.
struct Tick {
    double value = 0.0;
    std::string label;
    bool minor = false;
};

/// A resolved tick set for an axis.
struct TickSet {
    std::vector<Tick> ticks;       ///< major ticks with labels
    std::vector<double> minor;     ///< minor tick positions (no labels)
    double step = 0.0;             ///< major step (linear scales)
};

namespace detail {

/// "Nice" rounded interval covering [lo, hi].
struct NiceRange {
    double lo = 0.0;
    double hi = 1.0;
    double step = 1.0;
};

/// Choose a nice step for the given raw interval targeting ~target_count ticks.
double nice_step(double range, int target_count);

/// Expand [lo, hi] to nice tick boundaries.
NiceRange nice_range(double lo, double hi, int target_count);

/// Generate linear major/minor ticks covering [lo, hi] (inclusive, with tolerance).
/// If format is non-empty it is used as a printf-style format for labels.
TickSet linear_ticks(double lo, double hi, int target_count, const std::string& format,
                     bool minor_ticks);

/// Generate log ticks for [lo, hi] in data space (lo > 0) at the given
/// base (10 for log10, 2 for log2, e for natural log). Base-10 keeps its
/// decimal 2·/5· sub-labels and 2..9 minor ticks; other bases place ticks
/// only at integer powers of the base.
TickSet log_ticks(double lo, double hi, const std::string& format, bool minor_ticks,
                  double base = 10.0);

/// Generate symmetric-log ticks for [lo, hi] with linear threshold
/// `linthresh`: zero, ±linthresh, and base-10 decades beyond, so a signed
/// range crossing zero gets legible, deterministic ticks.
TickSet symlog_ticks(double lo, double hi, double linthresh, const std::string& format,
                     bool minor_ticks);

/// Format a tick value with the number of decimals implied by step.
std::string format_tick_value(double value, double step);

/// Format using printf-style format string (e.g. "%.1f"); falls back on error.
std::string format_with(const std::string& format, double value);

/// Generate date/time ticks for [lo, hi] given as Unix timestamps (UTC).
/// Chooses a calendar-aware step (seconds → years). If format is non-empty
/// it is used as an strftime format for labels.
TickSet datetime_ticks(double lo, double hi, int target_count, const std::string& format);

/// Format a Unix timestamp with an strftime format (UTC).
std::string format_datetime(double timestamp, const std::string& format);

} // namespace detail

} // namespace cplot
