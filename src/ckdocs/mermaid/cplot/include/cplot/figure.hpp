// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <cworks/error.hpp>

#include "axes.hpp"
#include "error.hpp"
#include "extent_policy.hpp"
#include "limits.hpp"
#include "scene.hpp"
#include "structured_charts.hpp"
#include "theme.hpp"
#include "units.hpp"

namespace cplot {

namespace detail {

/// A per-object once-flag that is safe to test-and-set from concurrent
/// const renders of the same Figure. Moving copies the current value,
/// which keeps the enclosing type's defaulted move operations
/// (std::atomic itself is neither copyable nor movable).
class AtomicOnceFlag {
public:
    AtomicOnceFlag() = default;
    AtomicOnceFlag(AtomicOnceFlag&& other) noexcept : set_(other.set_.load()) {}
    AtomicOnceFlag& operator=(AtomicOnceFlag&& other) noexcept {
        set_.store(other.set_.load());
        return *this;
    }

    /// Sets the flag; returns the previous value (std::atomic_flag
    /// semantics), so exactly one caller observes false.
    bool test_and_set() noexcept { return set_.exchange(true); }

private:
    std::atomic<bool> set_{false};
};

} // namespace detail

/// Accessibility and provenance metadata embedded in exported files.
struct Metadata {
    std::string title;
    std::string description;
    std::string alt_text;
    std::vector<std::pair<std::string, std::string>> extra;
};

/// The full drawing: size, theme, one plotting area (subplots come later).
///
/// Thread contract: build and mutate a Figure on one owning thread.
/// The const render calls (build_scene, to_svg, to_pdf, save_*) may
/// then run concurrently from any number of threads — on this figure
/// or on independent ones — and produce byte-identical output (see
/// the thread model in <cplot/cplot.hpp>).
class Figure {
public:
    Figure();
    ~Figure();
    Figure(const Figure&) = delete;
    Figure& operator=(const Figure&) = delete;
    Figure(Figure&&) noexcept;
    Figure& operator=(Figure&&) noexcept;

    Figure& size(double width, double height);
    /// Physical size, e.g. fig.size(85_mm, 55_mm) with using namespace
    /// cplot::units. Layout uses CSS pixels (96/in); SVG keeps the
    /// physical dimensions, raster output uses dpi().
    Figure& size(Length width, Length height);
    /// Raster export resolution for physical sizes (default 96).
    /// save_png() then produces physical_size_in_inches × dpi pixels.
    Figure& dpi(double dpi);
    Figure& theme(const Theme& theme);
    /// Apply a publication preset (size + theme + fonts), e.g.
    /// "paper-single-column". See cplot::presets() for the full list.
    Figure& preset(std::string_view name);
    /// Record a compatibility profile ("cplot-0.3"); it is embedded in
    /// SVG metadata so output provenance is traceable.
    Figure& compatibility(std::string profile);
    /// Render-resource bounds (see RenderLimits). The library default
    /// is RenderLimits::defaults(); the config engine (build_figure)
    /// additionally honors the central config and the chart config's
    /// `limits:` block.
    Figure& limits(const RenderLimits& limits);
    const RenderLimits& render_limits() const { return limits_; }

    // Explicit-style aliases.
    Figure& set_size(double w, double h) { return size(w, h); }
    Figure& set_theme(const Theme& t) { return theme(t); }

    // -- metadata ------------------------------------------------------------
    Figure& title(std::string text);
    Figure& description(std::string text);
    Figure& alt_text(std::string text);
    Figure& metadata(std::string key, std::string value);
    const Metadata& metadata() const { return meta_; }

    Axes& axes();
    const Axes& axes() const;

    /// Select a whole-canvas structured chart. Structured charts and axes
    /// grids are mutually exclusive because their layout models differ.
    Figure& sankey(SankeyChart chart);
    Figure& treemap(TreemapChart chart);
    Figure& quadrant(QuadrantChart chart);
    Figure& calendar(CalendarChart chart);
    Figure& gauge(GaugeChart chart);
    bool has_structured_chart() const noexcept {
        return !std::holds_alternative<std::monostate>(structured_chart_);
    }

    /// Split the figure into a rows × cols grid of independent axes.
    /// Must be called before adding series; replaces existing axes.
    Figure& subplots(int rows, int cols);

    /// How the subplot panels resolve their x extent (cplot/extent_policy.hpp).
    ///
    /// `Independent` is the default and gives every panel its own x domain.
    /// The other two give the grid ONE domain — `Union` the union of the
    /// panels' ranges, with categories merged in first-seen order, and
    /// `Pinned` the range this call states — and a grid with one domain draws
    /// tick labels and the axis label on the bottom row only and
    /// column-aligns the plot areas, because a shared axis whose panels do
    /// not line up is a shared axis nobody can read across.
    ///
    /// Requires Cartesian panels with matching x scale kinds; a pie, radar or
    /// polar panel has no axis to share and is refused when the scene is
    /// built. Throws cworks::Error now for a range without `Pinned` or a
    /// `Pinned` without a range, and for a structured chart, which has no
    /// panels.
    Figure& x_extent(ExtentPolicy policy,
                     std::optional<std::pair<double, double>> range = std::nullopt);
    /// How the subplot panels resolve their y extent: as x above, with tick
    /// labels and the axis label on the leftmost column only. (The secondary
    /// y2 axis is never shared.)
    Figure& y_extent(ExtentPolicy policy,
                     std::optional<std::pair<double, double>> range = std::nullopt);
    const AxisExtent& x_extent() const noexcept { return x_extent_; }
    const AxisExtent& y_extent() const noexcept { return y_extent_; }
    /// Access the axes at a grid cell (row 0 is the top row).
    Axes& axes(int row, int col);
    const Axes& axes(int row, int col) const;
    int subplot_rows() const { return rows_; }
    int subplot_cols() const { return cols_; }

    double width() const { return width_; }
    double height() const { return height_; }
    double dpi_value() const { return dpi_; }
    const std::optional<std::pair<Length, Length>>& physical_size() const {
        return physical_size_;
    }
    const std::string& compatibility_profile() const { return compatibility_; }
    const Theme& current_theme() const { return theme_; }

    /// Run the full pipeline (scales → ticks → layout → geometry) and
    /// return the backend-independent scene. Also used by tests.
    /// Point-count limits are enforced first: exceeding a hard bound
    /// throws cplot::Error before any geometry is built; soft-bound
    /// warnings land in Scene::diagnostics and are echoed to stderr
    /// once per figure.
    Scene build_scene() const;

private:
    double width_ = 800.0;
    double height_ = 480.0;
    double dpi_ = 96.0;
    std::optional<std::pair<Length, Length>> physical_size_;
    std::string compatibility_;
    int rows_ = 1;
    int cols_ = 1;
    AxisExtent x_extent_;
    AxisExtent y_extent_;
    Theme theme_;
    Metadata meta_;
    RenderLimits limits_;
    mutable detail::AtomicOnceFlag limit_warnings_printed_;
    std::vector<std::unique_ptr<Axes>> cells_;
    std::variant<std::monostate, SankeyChart, TreemapChart, QuadrantChart, CalendarChart,
                GaugeChart>
        structured_chart_;
};

} // namespace cplot
