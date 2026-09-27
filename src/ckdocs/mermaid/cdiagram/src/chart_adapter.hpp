// ckdiagram — shared boundary from Mermaid chart syntax to cplot
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <functional>
#include <string_view>

#include <cplot/figure.hpp>

#include "cdiagram/render.hpp"

namespace cdiagram::detail {

/// Build a cplot figure for a Mermaid chart adapter and stamp the resulting
/// scene consistently. Mermaid parsers own syntax only; cplot owns chart
/// models, validation, layout, and scene construction.
///
/// A cplot refusal is re-thrown as a cdiagram::Error that keeps cplot's own
/// structured code and subject and adds only the diagram framing: cplot has
/// already decided what went wrong, and this layer knows strictly less about
/// it than cplot does. Flattening it to a message would leave a frontend
/// offering the wrong recovery for every chart error in the engine.
/// Report where a chart drew each of its data marks, by walking the finished
/// scene and pairing marks with objects in order.
///
/// cplot lays these charts out, so their geometry exists only in the scene it
/// hands back — a slice's wedge, a box's rect. Re-deriving it here would be a
/// second implementation of cplot's layout, so this reads what was actually
/// drawn instead.
///
/// The join is POSITIONAL: the n-th mark of the given kind is the n-th object.
/// That holds because a series emits one mark per datum in datum order, and it
/// is the same idiom the animation player already uses to join a token table
/// to a diagram. `mark` selects which leaf type counts as a datum, so a chart
/// says whether its data are sectors, rectangles or markers.
enum class ChartMark { Sector, Rect, Circle };

void report_chart_regions(const RenderOptions& options, const cplot::Scene& scene,
                          ChartMark mark, std::size_t count, int role = 0);

cplot::Scene render_chart(const RenderOptions& options,
                          std::string_view diagram_name, double width,
                          double height,
                          const std::function<void(cplot::Figure&)>& configure);

} // namespace cdiagram::detail
