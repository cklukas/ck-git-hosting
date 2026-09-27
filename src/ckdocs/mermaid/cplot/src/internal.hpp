// ckplot — internal helpers of the chart engine (not installed)
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Chart-engine internals only. Anything the four backends also need
// lives in scene_internal.hpp, which this header includes so a chart
// translation unit still gets one include for both.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cplot/colormap.hpp"
#include "cplot/figure.hpp"
#include "cplot/scene.hpp"
#include <cworks/app_error.hpp>
#include "scene_internal.hpp"

namespace cplot::detail {

/// Full pipeline: scales → ticks → layout → series geometry → scene.
Scene build_scene(const Figure& fig);
Scene build_scene(const Figure& fig, const SankeyChart& chart);
Scene build_scene(const Figure& fig, const TreemapChart& chart);
Scene build_scene(const Figure& fig, const QuadrantChart& chart);
Scene build_scene(const Figure& fig, const CalendarChart& chart);
Scene build_scene(const Figure& fig, const GaugeChart& chart);

/// The paint layers a chart panel is assembled from, and the rectangle its
/// data marks are cut to.
///
/// A chart is not drawn in the order it is computed. The frame has to be
/// measured before the grid can be placed, the marks have to exist before an
/// inside legend can find a corner clear of them, and the axes are drawn over
/// marks that were produced long before. Three named layers let every step
/// write where the finished picture needs it, and `build()` flattens them into
/// one group in paint order.
///
/// `clip` cuts `marks` and nothing else, which is why the marks are a layer
/// rather than merely a position: a data point sitting on the plot border must
/// be cut at the border, while the border itself and the tick label beside it
/// must not be. A producer that draws on the whole canvas leaves `clip` unset
/// and pays for no clipping at all.
struct PanelLayers {
    std::optional<RectF> clip;            ///< cuts `marks`; unset = no clipping
    std::vector<SceneItem> under;         ///< plot background, grid
    std::vector<SceneItem> marks;         ///< data marks, the series' own geometry
    std::vector<SceneItem> over;          ///< axes, tick labels, title, legend
    std::vector<LinkRegion> link_regions; ///< per-element hyperlinks
    std::vector<HitRegion> hit_regions;   ///< native hit targets, not exported

    /// The three layers as one group, in paint order: the panel, with its
    /// marks as a subgroup between the layers drawn under and over them.
    ///
    /// The marks stay a group of their own even when nothing clips them, and
    /// even when there are none. That is free — a group with no transform and
    /// no clip emits nothing in any backend — and it is what keeps the three
    /// layers legible in the finished scene: without it, "drawn under the
    /// marks" and "drawn over them" would be a fact about the producer rather
    /// than a fact about the tree, recoverable only by knowing the code.
    ///
    /// The clip, by contrast, is set only when there is something to cut. A
    /// `<clipPath>` that clips nothing is exactly the noise this migration
    /// removes; the layer is structure, the clip is output.
    Group build() && {
        Group data;
        if (!marks.empty()) data.clip = clip;
        data.add(std::move(marks));

        Group group;
        group.link_regions = std::move(link_regions);
        group.hit_regions = std::move(hit_regions);
        group.add(std::move(under));
        group.add(std::move(data));
        group.add(std::move(over));
        return group;
    }
};

/// Draws a colorbar legend strip (gradient + endpoint labels) beside `plot`,
/// sampling `scale` over [data_min, data_max], into the panel's `over` layer.
/// The one shared implementation behind every value-encoded series' colorbar
/// (heatmap, contour, color-by scatter/bubble — see layout.cpp) and behind the
/// treemap and calendar charts' own colorbars (structured_chart_layout.cpp).
void append_colorbar_scale(std::vector<SceneItem>& over, const ColorScale& scale,
                           double data_min, double data_max, const RectF& plot,
                           const Theme& theme);

/// Throws when `policy` requests missing-value interpolation on a series
/// type that has no notion of an interior gap to interpolate across — only
/// LineSeries, AreaSeries, and StepSeries do, through the resolve_runs()
/// helper in series.cpp. Every other series that carries a MissingPolicy
/// must reject Interpolate explicitly through this call rather than
/// silently falling back to Drop.
inline void reject_unsupported_interpolate(MissingPolicy policy, const char* series_kind) {
    if (policy == MissingPolicy::Interpolate) {
        throw Error(cworks::unsupported(std::string(series_kind) +
                                        " series does not support missing: interpolate (only line, area, "
                                        "and step series interpolate)"));
    }
}

} // namespace cplot::detail
