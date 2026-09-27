// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ticks.hpp"

namespace cplot {

enum class ScaleKind { Linear, Log10, Log2, Ln, Symlog, Category, DateTime, Custom, Mirrored };

/// Maps data values to pixel coordinates.
///
/// A Scale owns a data domain and a pixel range. For category scales the
/// domain is [0, n] where each category occupies one unit band.
class Scale {
public:
    Scale() = default;

    static Scale linear(double lo, double hi);
    static Scale log10(double lo, double hi);
    static Scale log2(double lo, double hi);
    /// Natural-log (base e) scale.
    static Scale ln(double lo, double hi);
    /// Symmetric-log scale: linear within [-linthresh, linthresh] and
    /// log-compressed beyond, so signed data crossing zero is legible.
    static Scale symlog(double lo, double hi, double linthresh);
    static Scale category(std::vector<std::string> categories);
    /// Domain values are Unix timestamps (seconds, UTC).
    static Scale datetime(double lo, double hi);
    /// Custom monotone scale from the plugin registry
    /// (cplot::register_scale): pixel positions interpolate linearly
    /// in the transform's forward() space; ticks are chosen in data
    /// space. Throws cplot::Error when `name` is not registered.
    static Scale transform(const std::string& name, double lo, double hi);
    /// Mirrored magnitude scale for diverging charts (population
    /// pyramids): the domain [-magnitude, +magnitude] maps piecewise
    /// onto two pixel bands separated by a centre gap, negative values
    /// into the near band and positive values into the far band. Ticks
    /// mirror the [0, magnitude] ticks onto both sides — labelled as
    /// magnitudes, zero excluded (the panel draws one zero per inner
    /// edge). The gap defaults to 0 px; layout sets it once the label
    /// column is measured.
    static Scale mirrored(double magnitude);

    ScaleKind kind() const { return kind_; }
    /// Registry name of a Custom scale, empty otherwise.
    const std::string& transform_name() const { return transform_name_; }

    double domain_lo() const { return lo_; }
    double domain_hi() const { return hi_; }
    /// The range over which ticks are chosen — the mapping domain unless a
    /// separate tick domain was set (see set_tick_domain).
    double tick_domain_lo() const { return has_tick_domain_ ? tick_lo_ : lo_; }
    double tick_domain_hi() const { return has_tick_domain_ ? tick_hi_ : hi_; }
    /// Linear threshold of a Symlog scale (unused for other kinds).
    double linthresh() const { return linthresh_; }

    /// Set pixel range. lo may be greater than hi (inverted y axis).
    void set_pixel_range(double px_lo, double px_hi);
    double pixel_lo() const { return px_lo_; }
    double pixel_hi() const { return px_hi_; }

    /// Map a data value to a pixel coordinate.
    double map(double value) const;

    /// Map a pixel coordinate back to a data value: the inverse of map()
    /// for every kind but Category (a pixel lands in a band, not a single
    /// point — callers recover the index with floor/clamp themselves) and
    /// Mirrored's centre gap (no data value lives there; returns 0.0).
    ///
    /// Built on the suite's deterministic `cworks::exp`/`cworks::pow`
    /// rather than forward_value()'s libm, unlike map() itself. That
    /// asymmetry is deliberate, not an oversight — see forward_value()'s
    /// own comment for why map() may use libm and unmap() may not: an
    /// unmapped value is data a caller can persist or branch on exactly,
    /// which is precisely the condition that comment names as reversing
    /// the choice.
    double unmap(double pixel) const;

    /// Width of the centre gap in pixels (Mirrored only; 0 otherwise).
    void set_center_gap(double px);
    double center_gap() const { return center_gap_; }
    /// Inner pixel edges of the two halves: {negative-side zero edge,
    /// positive-side zero edge}. Both are map(0) for every other kind,
    /// so bar-pair geometry can use one code path for joined and
    /// gap-separated layouts.
    std::pair<double, double> center_edges() const;

    /// Center pixel position of category band i.
    double map_category(std::size_t index) const;

    /// Pixel width of one category band (categories only).
    double band_width() const;

    const std::vector<std::string>& categories() const { return categories_; }

    /// Generate ticks for this scale.
    TickSet ticks(int target_count, const std::string& format, bool minor) const;

    /// Choose tick positions over [lo, hi] instead of the mapping domain.
    /// Used to give a linear axis a small frame margin — the frame (mapping
    /// domain) is widened so extremes are not drawn flush on the plot border,
    /// while the tick values stay on the nice bounds. Linear/Custom only.
    void set_tick_domain(double lo, double hi);

private:
    /// The scale's forward transform: the space in which pixel positions
    /// interpolate linearly. The identity for linear, datetime, category and
    /// mirrored scales; a logarithm, the symlog compression, or the
    /// registered plugin function for the rest.
    double forward_value(double value) const;
    /// Evaluate forward_value() on the domain endpoints and remember the
    /// result. A Scale's domain is fixed by the factory that builds it and
    /// never changes afterwards, so map() reads these instead of
    /// transforming both endpoints again for every point it maps.
    void cache_domain();

    ScaleKind kind_ = ScaleKind::Linear;
    double lo_ = 0.0;
    double hi_ = 1.0;
    double forward_lo_ = 0.0;                   ///< forward_value(lo_), see cache_domain()
    double forward_span_ = 1.0;                 ///< forward_value(hi_) - forward_lo_
    double px_lo_ = 0.0;
    double px_hi_ = 1.0;
    double tick_lo_ = 0.0;                       ///< Tick domain (if set)
    double tick_hi_ = 1.0;                       ///< Tick domain (if set)
    bool has_tick_domain_ = false;               ///< tick_lo_/hi_ override lo_/hi_ for ticks
    double linthresh_ = 1.0;                    ///< Symlog only
    double center_gap_ = 0.0;                   ///< Mirrored only
    std::vector<std::string> categories_;
    std::string transform_name_;                ///< Custom scales only
    std::function<double(double)> forward_;     ///< Custom scales only
    std::function<double(double)> inverse_;     ///< Custom scales only
};

} // namespace cplot
