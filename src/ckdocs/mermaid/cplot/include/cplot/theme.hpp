// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include "color.hpp"

namespace cplot {

enum class FontWeight { Normal, Bold };
enum class FontStyle { Normal, Italic };

/// Where a Font may resolve its faces from.
///
/// `Any` is the catalog as a whole: the fonts compiled into the library
/// first, then the machine's own, then the `CKPLOT_FONT` override — the
/// right answer for a chart, which wants text on every machine. `Bundled`
/// is the fonts compiled into the library and nothing else: no platform
/// font, no environment override, no silent substitution — the right
/// answer for output that must be byte-identical everywhere. A code point
/// no bundled face covers then renders as the primary face's `.notdef`
/// box, which is visible evidence rather than a platform-dependent glyph.
enum class FontSources { Any, Bundled };

struct Font {
    std::string family = "Helvetica, Arial, sans-serif";
    double size = 12.0;
    FontWeight weight = FontWeight::Normal;
    FontStyle style = FontStyle::Normal;
    /// The text's language as a BCP 47 tag (`ja`, `zh-Hans`, `zh-Hant`,
    /// `ko`, `ru`), empty when unknown. It selects the locale-specific
    /// forms a face carries and, for the East Asian scripts whose
    /// ideographs differ by region, which face of a collection the
    /// glyph-coverage fallback reaches for first.
    std::string language = {};
    FontSources sources = FontSources::Any;

    Font with_size(double s) const {
        Font f = *this;
        f.size = s;
        return f;
    }
    Font with_weight(FontWeight w) const {
        Font f = *this;
        f.weight = w;
        return f;
    }
};

/// Visual style of a figure: colors, fonts, line widths, spacing.
/// Built-in themes are deliberately calm and print-friendly.
class Theme {
public:
    // -- colors ----------------------------------------------------------
    // Three distinct backdrops:
    //   page_background — the full SVG/PDF canvas. Transparent by default so
    //     exported charts float on whatever hosts them (a doc page, a slide);
    //     the raster (PNG/SIXEL) path composites transparency onto white.
    //   plot_background — the panel interior, inside the x/y axes. Filled
    //     white in the light schemes so the data sits on a clean rectangle.
    //   background — the semantic backdrop color used to knock labels and
    //     mark separators out against a series (kept opaque; see series.cpp).
    Color page_background = colors::transparent;
    Color background = colors::white;
    Color plot_background = colors::white;
    Color text_color = Color::rgb(0x262626);
    Color muted_text_color = Color::rgb(0x595959);
    Color axis_color = Color::rgb(0x404040);
    Color grid_color = Color::rgb(0x000000, 0.12);
    Color minor_grid_color = Color::rgb(0x000000, 0.05);
    Color legend_background = Color::rgb(0xFFFFFF, 0.85);
    Color legend_border = Color::rgb(0x000000, 0.15);
    /// The fill of an area whose classification could not be determined —
    /// a certified region's undecided cells, for instance, which a proof
    /// could not settle as inside or outside. An opaque neutral midway in
    /// lightness between the page and the ink: no hue, so it never reads as
    /// a data series, and far enough from the background that even an area
    /// one device pixel across stays visibly different from an empty one.
    /// Opaque, so it looks the same on any backdrop and covers whatever a
    /// neighbouring mark's stroke spills into it.
    Color undetermined_fill = Color::rgb(0x808080);
    std::vector<Color> palette;

    // -- fonts -----------------------------------------------------------
    std::string font_family_ = "Helvetica, Arial, sans-serif";
    double base_font_size_ = 12.0;
    double title_font_size_ = 15.0;
    double subtitle_font_size_ = 13.0;
    double axis_label_font_size_ = 12.0;
    double tick_font_size_ = 10.5;
    double legend_font_size_ = 11.0;

    // -- lines and spacing -------------------------------------------------
    double series_stroke_width = 1.6;
    double axis_stroke_width = 1.0;
    double grid_stroke_width = 1.0;
    double tick_length = 4.0;
    double padding = 12.0; ///< outer figure padding in px

    bool draw_axis_frame = false; ///< full frame vs. left/bottom spines only
    /// Semantic renderers may use this to strengthen their own role colours.
    /// Keeping the request in the engine theme avoids platform-specific
    /// recolouring after a scene has already been built.
    bool increased_contrast = false;

    /// Themes are values. Exact equality is useful to caches that may retain
    /// completed layout only while every styling and metric input matches.
    bool operator==(const Theme&) const = default;

    // -- fluent setters ----------------------------------------------------
    Theme& font_family(std::string family) {
        font_family_ = std::move(family);
        return *this;
    }
    Theme& base_font_size(double size);
    Theme& title_font_size(double size) {
        title_font_size_ = size;
        return *this;
    }
    Theme& subtitle_font_size(double size) {
        subtitle_font_size_ = size;
        return *this;
    }
    Theme& tick_font_size(double size) {
        tick_font_size_ = size;
        return *this;
    }
    Theme& grid_alpha(double alpha) {
        grid_color.a = alpha;
        return *this;
    }

    Font base_font() const { return Font{font_family_, base_font_size_}; }
    Font title_font() const { return Font{font_family_, title_font_size_, FontWeight::Bold}; }
    /// The subtitle sits below the title: Normal weight (the hierarchy is
    /// carried by the smaller size and the muted color it is drawn in).
    Font subtitle_font() const { return Font{font_family_, subtitle_font_size_}; }
    Font axis_label_font() const { return Font{font_family_, axis_label_font_size_}; }
    Font tick_font() const { return Font{font_family_, tick_font_size_}; }
    Font legend_font() const { return Font{font_family_, legend_font_size_}; }

    Color series_color(std::size_t index) const {
        if (palette.empty()) return colors::black;
        return palette[index % palette.size()];
    }

    // -- built-in themes -----------------------------------------------------
    static Theme print();        ///< default: white background, muted palette
    static Theme paper();        ///< serif fonts, subtle grid, for publications
    static Theme dark();         ///< dark background
    static Theme presentation(); ///< larger fonts, stronger lines
    static Theme monochrome();   ///< grayscale only
    static Theme high_contrast(); ///< maximum figure/semantic-role contrast
    static Theme terminal();     ///< high-contrast, for SIXEL/terminal output
};

} // namespace cplot
