// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "scale.hpp"

namespace cplot {

/// Configuration of a single axis (x or y).
class Axis {
public:
    Axis& label(std::string text) {
        label_ = std::move(text);
        return *this;
    }
    /// Unit appended to the label as "Label [unit]".
    Axis& unit(std::string unit) {
        unit_ = std::move(unit);
        return *this;
    }
    Axis& range(double lo, double hi) {
        range_ = std::pair<double, double>{lo, hi};
        return *this;
    }
    Axis& automatic_range() {
        range_.reset();
        return *this;
    }
    Axis& log10() {
        kind_ = ScaleKind::Log10;
        return *this;
    }
    /// Base-2 log axis (fold-change, doublings).
    Axis& log2() {
        kind_ = ScaleKind::Log2;
        return *this;
    }
    /// Natural-log (base e) axis.
    Axis& ln() {
        kind_ = ScaleKind::Ln;
        return *this;
    }
    /// Symmetric-log axis: linear within [-linthresh, linthresh], log
    /// beyond, so signed data crossing zero stays legible.
    Axis& symlog() {
        kind_ = ScaleKind::Symlog;
        return *this;
    }
    /// The linear threshold for a symlog axis (default 1).
    Axis& linthresh(double value) {
        symlog_linthresh_ = value;
        return *this;
    }
    Axis& linear() {
        kind_ = ScaleKind::Linear;
        return *this;
    }
    /// Treat values as Unix timestamps (seconds, UTC).
    /// tick_format() then takes an strftime format like "%Y-%m-%d".
    Axis& datetime() {
        kind_ = ScaleKind::DateTime;
        return *this;
    }
    /// Use a custom scale from the plugin registry
    /// (cplot::register_scale); the name is resolved at layout time.
    Axis& custom_scale(std::string name) {
        kind_ = ScaleKind::Custom;
        custom_scale_ = std::move(name);
        return *this;
    }
    /// Reverse the axis direction: the domain maps from the far edge back
    /// to the near edge (rank charts with 1 at the top, descending time).
    /// Data, ticks, gridlines, and — for a category axis — the category
    /// order all flip together, since every value maps through one pixel
    /// range. Works under shared axes: the union domain is still computed
    /// forward; only the mapping flips.
    Axis& reverse(bool enabled = true) {
        reversed_ = enabled;
        return *this;
    }
    Axis& grid(bool enabled) {
        grid_ = enabled;
        return *this;
    }
    Axis& minor_ticks(bool enabled) {
        minor_ticks_ = enabled;
        return *this;
    }
    /// printf-style tick label format, e.g. "%.1f".
    Axis& tick_format(std::string format) {
        tick_format_ = std::move(format);
        return *this;
    }
    /// Show tick labels as magnitudes: a tick at −108 reads "108".
    /// Ticks, gridlines, and data stay at their true signed positions;
    /// only the label text drops the sign. Diverging charts (population
    /// pyramids, tornado charts) show positive values on both sides of
    /// zero this way.
    Axis& absolute_labels(bool enabled = true) {
        absolute_labels_ = enabled;
        return *this;
    }
    /// Fixed tick positions (empty = automatic).
    Axis& ticks(std::vector<double> positions) {
        fixed_ticks_ = std::move(positions);
        return *this;
    }
    Axis& target_tick_count(int count) {
        target_tick_count_ = count;
        return *this;
    }

    // Explicit-style aliases.
    Axis& set_label(std::string text) { return label(std::move(text)); }
    Axis& set_range(double lo, double hi) { return range(lo, hi); }
    Axis& set_tick_format(std::string f) { return tick_format(std::move(f)); }

    // -- getters -----------------------------------------------------------
    const std::string& label_text() const { return label_; }
    const std::string& unit_text() const { return unit_; }
    /// "Label [unit]" or just the label when no unit is set.
    std::string display_label() const {
        if (label_.empty()) return {};
        if (unit_.empty()) return label_;
        return label_ + " [" + unit_ + "]";
    }
    const std::optional<std::pair<double, double>>& explicit_range() const { return range_; }
    ScaleKind scale_kind() const { return kind_; }
    /// Registry name for ScaleKind::Custom, empty otherwise.
    const std::string& custom_scale_name() const { return custom_scale_; }
    /// Linear threshold for a symlog axis.
    double symlog_linthresh() const { return symlog_linthresh_; }
    bool reversed() const { return reversed_; }
    bool grid_enabled() const { return grid_; }
    bool minor_ticks_enabled() const { return minor_ticks_; }
    bool absolute_labels_enabled() const { return absolute_labels_; }
    const std::string& tick_format_text() const { return tick_format_; }
    const std::vector<double>& fixed_ticks() const { return fixed_ticks_; }
    int tick_count_target() const { return target_tick_count_; }

private:
    std::string label_;
    std::string unit_;
    std::optional<std::pair<double, double>> range_;
    ScaleKind kind_ = ScaleKind::Linear;
    std::string custom_scale_;
    double symlog_linthresh_ = 1.0;
    bool reversed_ = false;
    bool grid_ = true;
    bool minor_ticks_ = false;
    bool absolute_labels_ = false;
    std::string tick_format_;
    std::vector<double> fixed_ticks_;
    int target_tick_count_ = 6;
};

} // namespace cplot
