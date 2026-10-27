// ckdiagram — shared boundary from Mermaid chart syntax to cplot
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <vector>
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
/// Each matched mark is paired with an explicit source-object ordinal. The
/// adapter must retain this identity when it sorts or omits data before drawing.
enum class ChartMark { Sector, Rect, Circle };

/// Source order for adapters that preserve every datum in its original order.
std::vector<std::size_t> chart_source_order(std::size_t count);

void report_chart_regions(const RenderOptions& options, const cplot::Scene& scene,
                          ChartMark mark, std::span<const std::size_t> source_order,
                          int role = 0);

cplot::Scene render_chart(const RenderOptions& options,
                          std::string_view diagram_name, double width,
                          double height,
                          const std::function<void(cplot::Figure&)>& configure);

} // namespace cdiagram::detail
