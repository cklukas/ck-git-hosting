// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Two-tier render limits (suite charter): exceeding a soft bound
// produces a warning diagnostic, exceeding a hard bound is an error.
// The defaults are generous — they exist to catch runaway inputs
// (a million-point line, a hundred-gigabyte raster), not to
// constrain real charts.
//
// Resolution order, later wins:
//   1. built-in defaults (below)
//   2. central config (`~/.config/ckoffice/config.yaml`, `ckplot:
//      limits:` section — see RenderLimits::from_central_config)
//   3. the chart config's top-level `limits:` block
//   4. Figure::limits() set by the host program
#pragma once

#include <cworks/limits.hpp>

namespace cplot {

/// The hard bound on the pixels of one decoded picture — the default of
/// RenderLimits::picture_pixels and of every decoder's `max_pixels`.
inline constexpr std::uint64_t kMaxPicturePixels = 100'000'000ULL;

/// Resource bounds enforced while building and rendering a figure.
struct RenderLimits {
    /// Data points in a single series. Soft: a chart past this is
    /// unreadable and SVG output huge — downsampling is the fix
    /// (LineSeries::downsample). Hard: runaway input protection.
    cworks::Limits::Bound series_points{1'000'000ULL, 50'000'000ULL};

    /// Data points across all series of the figure — catches "many
    /// series, each just under the limit".
    cworks::Limits::Bound figure_points{5'000'000ULL, 100'000'000ULL};

    /// Output pixels (width × height) of a raster render. Memory is
    /// roughly 40 bytes per output pixel (RGBA plus the 3×3
    /// supersampled canvas): the soft default (25 MP ≈ a 5000×5000
    /// PNG) warns near 1 GB, the hard default (400 MP) stops
    /// renders that would need ~16 GB.
    cworks::Limits::Bound raster_pixels{25'000'000ULL, 400'000'000ULL};

    /// Pixels (width × height) of one picture a document points at — a PNG
    /// or JPEG on a slide or in a Writer file — checked from the picture's
    /// header before it is decoded. A decoded picture costs 4 bytes per
    /// pixel and a JPEG up to about 12 more while it decodes, so the default
    /// refuses a picture that would need well over a gigabyte. Only the hard
    /// tier applies: a picture is read whole or refused, and there is no
    /// partial result a warning could accompany, so the soft tier defaults
    /// to the same value.
    cworks::Limits::Bound picture_pixels{kMaxPicturePixels, kMaxPicturePixels};

    static RenderLimits defaults() { return {}; }

    /// Read overrides from a `limits:` mapping onto `base`:
    ///   limits:
    ///     series_points: {soft: 200000, hard: 1000000}
    ///     raster_pixels: {hard: 100000000}   # partial override is fine
    /// Unknown keys are errors (suite convention). A Null node
    /// returns `base` unchanged.
    static RenderLimits from_yaml(const cworks::YamlNode& node);
    static RenderLimits from_yaml(const cworks::YamlNode& node, const RenderLimits& base);

    /// Limits from the central CK Office configuration.
    /// (cworks::load_central_config), read from the component
    /// section:
    ///   ckplot:
    ///     limits:
    ///       series_points: {soft: 200000}
    /// Returns the defaults when no config or no section exists.
    static RenderLimits from_central_config();
};

} // namespace cplot
