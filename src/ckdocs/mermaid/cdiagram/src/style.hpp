// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Internal: the semantic colour scheme every diagram type draws with,
// resolved from the render mode. The overall page background is always
// transparent (`page`); every other colour switches with the mode so a
// diagram reads well on a light page, a dark page, or in grayscale.
#pragma once

#include <cstddef>
#include <vector>

#include <cplot/color.hpp>
#include <cplot/theme.hpp>

#include "cdiagram/render.hpp"

namespace cdiagram::detail {

using cplot::Color;

struct DiagramStyle {
    Color page;          ///< overall scene background (transparent)
    Color node_fill;     ///< default node fill (flowchart/state boxes)
    Color node_stroke;   ///< default node stroke
    Color text;          ///< primary label text
    Color muted;         ///< secondary text (edge labels, attributes)
    Color edge;          ///< edges/arrows
    Color note_fill;     ///< note/tag background
    Color note_stroke;
    Color accent;        ///< start/end markers, git tags
    Color label_mask;    ///< backdrop behind edge/message labels (masked_text draws it at 75% opacity)
    Color grid;          ///< gridlines/axes
    Color entity_stroke; ///< er/class/c4 box stroke + relations
    Color entity_fill;   ///< er/class/c4 box body (a plain panel)
    Color header_fill;   ///< er entity / table header band
    Color task_done;     ///< gantt: completed task bar
    Color task_active;   ///< gantt: active task bar
    Color task_crit;     ///< gantt: critical task bar
    Color task_default;  ///< gantt: ordinary task bar
    Color c4_person;     ///< c4: person/actor fill
    Color c4_element;    ///< c4: system/container/component fill
    std::vector<Color> score_scale;  ///< journey: 1..5 score colours (bad → good)
    std::vector<Color> palette;  ///< categorical (slices, cards, series)
    ThemeMode mode = ThemeMode::Light;

    /// A categorical colour for index `i` (wraps).
    Color series(std::size_t i) const {
        return palette.empty() ? node_stroke : palette[i % palette.size()];
    }
};

/// The style for a render mode.
DiagramStyle style_for(ThemeMode mode);

/// The style for one complete render request. An increased-contrast cplot
/// theme strengthens cdiagram's own semantic role colours before scene
/// construction; frontends never recolour finished vector output.
DiagramStyle style_for(const RenderOptions& options);

/// Adapt cdiagram's semantic mode to a cplot theme for chart families whose
/// data model and layout live in cplot.
cplot::Theme chart_theme_for(const RenderOptions& options);

} // namespace cdiagram::detail
