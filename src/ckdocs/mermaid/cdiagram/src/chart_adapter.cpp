// ckdiagram — shared boundary from Mermaid chart syntax to cplot
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "chart_adapter.hpp"
#include <vector>
#include <variant>
#include <algorithm>

#include <string>
#include <utility>

#include <cworks/app_error.hpp>

#include "cdiagram/error.hpp"
#include "style.hpp"

namespace cdiagram::detail {

cplot::Scene render_chart(const RenderOptions& options,
                          std::string_view diagram_name, double width,
                          double height,
                          const std::function<void(cplot::Figure&)>& configure) {
    try {
        cplot::Figure figure;
        figure.size(width, height).theme(chart_theme_for(options));
        configure(figure);
        cplot::Scene scene = figure.build_scene();
        scene.meta_generator = "cdiagram";
        return scene;
    } catch (const cplot::Error& error) {
        // cplot already decided what went wrong — an unknown theme is not a
        // malformed diagram — and re-deciding it here would tell a frontend to
        // offer the wrong recovery. Keep the cause's code and subject; add only
        // the diagram framing the author needs in order to place the message.
        // Rebuilding this message-only is what stranded cplot's codes before
        // they existed to strand.
        std::string framed = std::string(diagram_name) + ": " + error.what();
        cworks::AppError e = error.structured() != nullptr ? *error.structured()
                                                           : cworks::validation_failed({});
        e.summary = std::move(framed);
        throw Error(std::move(e));
    }
}

void report_chart_regions(const RenderOptions& options, const cplot::Scene& scene,
                          ChartMark mark, std::size_t count, int role) {
    if (options.regions == nullptr || count == 0) return;

    std::size_t ordinal = 0;
    for (const cplot::PlacedItem& placed : cplot::collect_items(scene.root)) {
        if (ordinal >= count) break;
        std::vector<cplot::Point> shape;
        cplot::RectF bounds{};

        if (mark == ChartMark::Sector) {
            const auto* sector = std::get_if<cplot::SectorItem>(placed.item);
            if (sector == nullptr) continue;
            // The engine's own tessellation, so the hit area is the wedge
            // that was drawn rather than an approximation of it.
            shape = cplot::tessellate_sector(*sector);
        } else if (mark == ChartMark::Rect) {
            const auto* rect = std::get_if<cplot::RectItem>(placed.item);
            if (rect == nullptr) continue;
            bounds = rect->rect;
        } else {
            const auto* circle = std::get_if<cplot::CircleItem>(placed.item);
            if (circle == nullptr) continue;
            bounds = cplot::RectF{circle->center.x - circle->radius,
                                  circle->center.y - circle->radius, circle->radius * 2.0,
                                  circle->radius * 2.0};
        }

        for (cplot::Point& p : shape) p = placed.ctm.apply(p);
        if (!shape.empty()) {
            double lo_x = shape.front().x, hi_x = lo_x, lo_y = shape.front().y, hi_y = lo_y;
            for (const cplot::Point& p : shape) {
                lo_x = std::min(lo_x, p.x); hi_x = std::max(hi_x, p.x);
                lo_y = std::min(lo_y, p.y); hi_y = std::max(hi_y, p.y);
            }
            bounds = cplot::RectF{lo_x, lo_y, hi_x - lo_x, hi_y - lo_y};
        } else {
            bounds = placed.ctm.apply_bounds(bounds);
        }

        DrawnRegion region;
        region.key = "#" + std::to_string(ordinal);
        region.role = role;
        region.bounds = bounds;
        region.shape = std::move(shape);
        options.regions->push_back(std::move(region));
        ++ordinal;
    }
}

} // namespace cdiagram::detail
