// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cplot/theme.hpp"

namespace cplot {

Theme& Theme::base_font_size(double size) {
    const double scale = size / base_font_size_;
    base_font_size_ = size;
    title_font_size_ *= scale;
    subtitle_font_size_ *= scale;
    axis_label_font_size_ *= scale;
    tick_font_size_ *= scale;
    legend_font_size_ *= scale;
    return *this;
}

namespace {

// Muted, print-friendly qualitative palette (colorblind-aware ordering).
std::vector<Color> muted_palette() {
    return {
        Color::rgb(0x4C72B0), // blue
        Color::rgb(0xDD8452), // orange
        Color::rgb(0x55A868), // green
        Color::rgb(0xC44E52), // red
        Color::rgb(0x8172B3), // purple
        Color::rgb(0x937860), // brown
        Color::rgb(0xDA8BC3), // pink
        Color::rgb(0x8C8C8C), // gray
        Color::rgb(0xCCB974), // olive
        Color::rgb(0x64B5CD), // cyan
    };
}

std::vector<Color> bright_palette() {
    return {
        Color::rgb(0x5B9BD5), Color::rgb(0xED7D31), Color::rgb(0x70AD47),
        Color::rgb(0xE15759), Color::rgb(0xB07AA1), Color::rgb(0xEDC948),
        Color::rgb(0x76B7B2), Color::rgb(0xFF9DA7),
    };
}

std::vector<Color> gray_palette() {
    return {
        Color::rgb(0x1A1A1A), Color::rgb(0x595959), Color::rgb(0x8C8C8C),
        Color::rgb(0xB3B3B3), Color::rgb(0x404040), Color::rgb(0x737373),
    };
}

} // namespace

Theme Theme::print() {
    Theme t;
    t.palette = muted_palette();
    return t;
}

Theme Theme::paper() {
    Theme t;
    t.palette = muted_palette();
    t.font_family_ = "Georgia, 'Times New Roman', serif";
    t.base_font_size_ = 11.0;
    t.title_font_size_ = 13.0;
    t.subtitle_font_size_ = 12.0;
    t.axis_label_font_size_ = 11.0;
    t.tick_font_size_ = 9.5;
    t.legend_font_size_ = 10.0;
    t.grid_color = Color::rgb(0x000000, 0.10);
    t.series_stroke_width = 1.3;
    return t;
}

Theme Theme::dark() {
    Theme t;
    // A dark scheme fills the whole canvas; the panel interior shows it
    // through (transparent plot_background), unlike the light schemes.
    t.page_background = Color::rgb(0x16181D);
    t.background = Color::rgb(0x16181D);
    t.plot_background = colors::transparent;
    // Titles and axis/tick labels render pure white so they stay crisp on
    // a dark page (both use text_color; see build_panel in layout.cpp).
    t.text_color = colors::white;
    t.muted_text_color = Color::rgb(0xB8B8B8);
    t.axis_color = Color::rgb(0xC4C4C4);
    t.grid_color = Color::rgb(0xFFFFFF, 0.10);
    t.minor_grid_color = Color::rgb(0xFFFFFF, 0.04);
    t.legend_background = Color::rgb(0x16181D, 0.85);
    t.legend_border = Color::rgb(0xFFFFFF, 0.18);
    // Midway between the dark page and the white ink, as the light schemes'
    // grey sits midway between white and black.
    t.undetermined_fill = Color::rgb(0x8C8C8C);
    t.palette = {
        Color::rgb(0x6E9BD8), Color::rgb(0xE8985E), Color::rgb(0x6FBF82),
        Color::rgb(0xD96A6E), Color::rgb(0x9C8FD0), Color::rgb(0xB0906E),
        Color::rgb(0xE0A3CE), Color::rgb(0xA6A6A6), Color::rgb(0xD8C888),
        Color::rgb(0x7CC5DD),
    };
    return t;
}

Theme Theme::presentation() {
    Theme t;
    t.palette = bright_palette();
    t.base_font_size_ = 15.0;
    t.title_font_size_ = 20.0;
    t.subtitle_font_size_ = 17.0;
    t.axis_label_font_size_ = 15.0;
    t.tick_font_size_ = 13.0;
    t.legend_font_size_ = 14.0;
    t.series_stroke_width = 2.4;
    t.axis_stroke_width = 1.4;
    t.padding = 16.0;
    return t;
}

Theme Theme::monochrome() {
    Theme t;
    t.palette = gray_palette();
    t.grid_color = Color::rgb(0x000000, 0.15);
    // Without hue, an undetermined area is told apart by lightness alone.
    // Every tint of the grey palette at the opacity a region fills with
    // (0.32) is lighter than #B6B6B6, and the page is white, so a 50 % grey
    // stands at least 54 levels from both. A pattern could not do this job:
    // an area one device pixel across has no room for one.
    t.undetermined_fill = Color::rgb(0x808080);
    return t;
}

Theme Theme::high_contrast() {
    Theme t;
    t.page_background = colors::white;
    t.background = colors::white;
    t.plot_background = colors::white;
    t.text_color = colors::black;
    t.muted_text_color = colors::black;
    t.axis_color = colors::black;
    t.grid_color = Color::rgb(0x000000, 0.42);
    t.minor_grid_color = Color::rgb(0x000000, 0.24);
    t.legend_background = colors::white;
    t.legend_border = colors::black;
    // The lightest grey with 4.5:1 contrast against white.
    t.undetermined_fill = Color::rgb(0x767676);
    t.palette = {
        Color::rgb(0x003B73), Color::rgb(0x8A2500), Color::rgb(0x145A20),
        Color::rgb(0x8B0015), Color::rgb(0x4B2582), Color::rgb(0x005C63),
        Color::rgb(0x694E00), Color::rgb(0x70205F),
    };
    t.series_stroke_width = 2.4;
    t.axis_stroke_width = 1.6;
    t.grid_stroke_width = 1.3;
    t.draw_axis_frame = true;
    t.increased_contrast = true;
    return t;
}

Theme Theme::terminal() {
    Theme t = dark();
    t.page_background = Color::rgb(0x000000);
    t.background = Color::rgb(0x000000);
    t.text_color = Color::rgb(0xF0F0F0);
    t.axis_color = Color::rgb(0xD0D0D0);
    t.grid_color = Color::rgb(0xFFFFFF, 0.18);
    t.font_family_ = "monospace";
    t.series_stroke_width = 2.0;
    t.palette = {
        Color::rgb(0x5FAFFF), Color::rgb(0xFFAF5F), Color::rgb(0x5FD75F),
        Color::rgb(0xFF5F5F), Color::rgb(0xAF87FF), Color::rgb(0x5FD7D7),
        Color::rgb(0xFFD75F), Color::rgb(0xFF87D7),
    };
    return t;
}

} // namespace cplot
