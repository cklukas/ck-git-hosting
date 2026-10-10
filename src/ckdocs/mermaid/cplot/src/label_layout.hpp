// ckplot — bounded, deterministic placement of labels around fixed points
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <vector>
#include "cplot/scene.hpp"
#include "cplot/text.hpp"

namespace cplot::detail {

struct LabelPlacement {
    TextItem text;
    RectF viewport;
    std::vector<Point> positions; // candidate text anchors, in preference order
};

/// Eight adjacent positions for an unrotated point label. Radius includes stroke.
LabelPlacement point_label(TextItem text, Point anchor, double radius,
                           RectF viewport, const TextMeasurer& measurer);

struct LabelObstacle {
    RectF bounds;
    double weight = 1.0;
    bool marker = false; // preserve data visibility before label separation
};

/// Chooses among supplied positions without changing text or font.
/// Measured padded boxes avoid labels, fixed obstacles, and the viewport.
/// Strictly improving ordered passes make the result deterministic and prevent
/// oscillation. A fixed comparison budget bounds work on pathological inputs;
/// when space or work runs out, all labels remain at the best positions found.
void place_labels(std::vector<LabelPlacement>& labels,
                        const std::vector<LabelObstacle>& obstacles,
                        const TextMeasurer& measurer);

} // namespace cplot::detail
