// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "style.hpp"

namespace cdiagram::detail {

namespace {

std::vector<Color> light_palette() {
    return {Color::rgb(0x4C78A8), Color::rgb(0xF58518), Color::rgb(0x54A24B),
            Color::rgb(0xE45756), Color::rgb(0x72B7B2), Color::rgb(0xEECA3B),
            Color::rgb(0xB279A2), Color::rgb(0xFF9DA6), Color::rgb(0x9D755D),
            Color::rgb(0xBAB0AC)};
}

std::vector<Color> dark_palette() {
    return {Color::rgb(0x7FB3E8), Color::rgb(0xFFB56B), Color::rgb(0x8FD98A),
            Color::rgb(0xF08A89), Color::rgb(0x9FE0DB), Color::rgb(0xF5DE7A),
            Color::rgb(0xD3A6C8), Color::rgb(0xFFC2C8), Color::rgb(0xC7A48C),
            Color::rgb(0xCFC7C3)};
}

std::vector<Color> gray_palette() {
    return {Color::rgb(0x555555), Color::rgb(0x888888), Color::rgb(0x333333),
            Color::rgb(0xAAAAAA), Color::rgb(0x6E6E6E), Color::rgb(0x444444),
            Color::rgb(0x999999), Color::rgb(0x777777), Color::rgb(0xBBBBBB),
            Color::rgb(0x222222)};
}

std::vector<Color> high_contrast_light_palette() {
    return {Color::rgb(0x003B73), Color::rgb(0x8A2500), Color::rgb(0x145A20),
            Color::rgb(0x8B0015), Color::rgb(0x4B2582), Color::rgb(0x005C63),
            Color::rgb(0x694E00), Color::rgb(0x70205F)};
}

std::vector<Color> high_contrast_dark_palette() {
    return {Color::rgb(0x73B7FF), Color::rgb(0xFFAA7A), Color::rgb(0x77D98A),
            Color::rgb(0xFF8492), Color::rgb(0xC3A2FF), Color::rgb(0x6DDAE2),
            Color::rgb(0xFFE071), Color::rgb(0xF6A5DF)};
}

DiagramStyle with_increased_contrast(DiagramStyle style) {
    const bool dark = style.mode == ThemeMode::Dark;
    const Color foreground = dark ? cplot::colors::white : cplot::colors::black;
    const Color background = dark ? cplot::colors::black : cplot::colors::white;
    style.node_fill = background;
    style.node_stroke = foreground;
    style.text = foreground;
    style.muted = foreground;
    style.edge = foreground;
    style.note_fill = background;
    style.note_stroke = foreground;
    style.accent = foreground;
    style.label_mask = background;
    style.grid = foreground.with_alpha(0.65);
    style.entity_stroke = foreground;
    style.entity_fill = background;
    style.header_fill = dark ? Color::rgb(0x242424) : Color::rgb(0xE2E2E2);
    style.task_done = dark ? Color::rgb(0xBDBDBD) : Color::rgb(0x4A4A4A);
    style.task_active = dark ? Color::rgb(0x73B7FF) : Color::rgb(0x003B73);
    style.task_crit = dark ? Color::rgb(0xFF8492) : Color::rgb(0x8B0015);
    style.task_default = dark ? Color::rgb(0xC3A2FF) : Color::rgb(0x4B2582);
    style.c4_person = style.task_active;
    style.c4_element = dark ? Color::rgb(0x6DDAE2) : Color::rgb(0x005C63);
    style.score_scale = dark
        ? std::vector<Color>{Color::rgb(0xFF8492), Color::rgb(0xFFAA7A),
                             Color::rgb(0xFFE071), Color::rgb(0x77D98A),
                             Color::rgb(0x6DDAE2)}
        : std::vector<Color>{Color::rgb(0x8B0015), Color::rgb(0x8A2500),
                             Color::rgb(0x694E00), Color::rgb(0x145A20),
                             Color::rgb(0x005C63)};
    style.palette = dark ? high_contrast_dark_palette()
                         : high_contrast_light_palette();
    return style;
}

} // namespace

DiagramStyle style_for(ThemeMode mode) {
    DiagramStyle s;
    s.mode = mode;
    s.page = cplot::colors::transparent;
    switch (mode) {
    case ThemeMode::Dark:
        s.node_fill = Color::rgb(0x2E2E45);
        s.node_stroke = Color::rgb(0xB39DDB);
        s.text = Color::rgb(0xEBEBF2);
        s.muted = Color::rgb(0xAFAFC0);
        s.edge = Color::rgb(0xC8C8D6);
        s.note_fill = Color::rgb(0x4A4626);
        s.note_stroke = Color::rgb(0x9A9A54);
        s.accent = Color::rgb(0xB39DDB);
        s.label_mask = Color::rgb(0x1E1E28);
        s.grid = Color::rgb(0xFFFFFF, 0.30);
        s.entity_stroke = Color::rgb(0x9A9AC0);
        s.entity_fill = Color::rgb(0x24242E);
        s.header_fill = Color::rgb(0x33334A);
        s.task_done = Color::rgb(0x6E6E80);
        s.task_active = Color::rgb(0x5B8DBE);
        s.task_crit = Color::rgb(0xD9704C);
        s.task_default = Color::rgb(0x9F86D6);
        s.c4_person = Color::rgb(0x2E4E76);
        s.c4_element = Color::rgb(0x33608F);
        s.score_scale = {Color::rgb(0xD9704C), Color::rgb(0xDD9A4E), Color::rgb(0xD6C465),
                         Color::rgb(0x9CBF6A), Color::rgb(0x6FAF83)};
        s.palette = dark_palette();
        break;
    case ThemeMode::Grayscale:
        s.node_fill = Color::rgb(0xE8E8E8);
        s.node_stroke = Color::rgb(0x555555);
        s.text = Color::rgb(0x1A1A1A);
        s.muted = Color::rgb(0x5A5A5A);
        s.edge = Color::rgb(0x333333);
        s.note_fill = Color::rgb(0xF0F0F0);
        s.note_stroke = Color::rgb(0x999999);
        s.accent = Color::rgb(0x444444);
        s.label_mask = Color::rgb(0xFFFFFF);
        s.grid = Color::rgb(0x000000, 0.32);
        s.entity_stroke = Color::rgb(0x777777);
        s.entity_fill = Color::rgb(0xFFFFFF);
        s.header_fill = Color::rgb(0xE0E0E0);
        // Grayscale keeps the STATUS distinction as brightness steps.
        s.task_done = Color::rgb(0xC8C8C8);
        s.task_active = Color::rgb(0x8A8A8A);
        s.task_crit = Color::rgb(0x3A3A3A);
        s.task_default = Color::rgb(0xA8A8A8);
        s.c4_person = Color::rgb(0x4A4A4A);
        s.c4_element = Color::rgb(0x6A6A6A);
        s.score_scale = {Color::rgb(0x3A3A3A), Color::rgb(0x5E5E5E), Color::rgb(0x828282),
                         Color::rgb(0xA6A6A6), Color::rgb(0xCACACA)};
        s.palette = gray_palette();
        break;
    case ThemeMode::Light:
    default:
        s.node_fill = Color::rgb(0xECECFF);
        s.node_stroke = Color::rgb(0x9370DB);
        s.text = Color::rgb(0x262626);
        s.muted = Color::rgb(0x595959);
        s.edge = Color::rgb(0x33333A);
        s.note_fill = Color::rgb(0xFFF5AD);
        s.note_stroke = Color::rgb(0xAAAA33);
        s.accent = Color::rgb(0x9370DB);
        s.label_mask = Color::rgb(0xFFFFFF);
        s.grid = Color::rgb(0x000000, 0.32);
        s.entity_stroke = Color::rgb(0x66668A);
        s.entity_fill = Color::rgb(0xFFFFFF);
        s.header_fill = Color::rgb(0xECECFF);
        s.task_done = Color::rgb(0xB6B6C2);
        s.task_active = Color::rgb(0x4C78A8);
        s.task_crit = Color::rgb(0xE4572E);
        s.task_default = Color::rgb(0x9370DB);
        s.c4_person = Color::rgb(0x08427B);
        s.c4_element = Color::rgb(0x1168BD);
        s.score_scale = {Color::rgb(0xE4572E), Color::rgb(0xF3872F), Color::rgb(0xE8C547),
                         Color::rgb(0x8FB339), Color::rgb(0x4F9D69)};
        s.palette = light_palette();
        break;
    }
    return s;
}

DiagramStyle style_for(const RenderOptions& options) {
    // A named monochrome theme is a complete grayscale presentation, not a
    // coloured Light or Dark diagram with a grayscale palette only. Other
    // named themes retain the reader-selected semantic mode; their metrics
    // and chart-specific palette are carried by cplot as before.
    const ThemeMode mode = options.theme_name == "monochrome"
        ? ThemeMode::Grayscale
        : options.mode;
    DiagramStyle style = style_for(mode);
    if (options.theme.increased_contrast) return with_increased_contrast(style);
    return style;
}

cplot::Theme chart_theme_for(const RenderOptions& options) {
    const DiagramStyle style = style_for(options);
    cplot::Theme theme = options.theme;
    theme.page_background = style.page;
    theme.background = style.label_mask;
    theme.plot_background = style.entity_fill;
    theme.text_color = style.text;
    theme.muted_text_color = style.muted;
    theme.axis_color = style.edge;
    theme.grid_color = style.grid;
    theme.palette = style.palette;
    return theme;
}

} // namespace cdiagram::detail
