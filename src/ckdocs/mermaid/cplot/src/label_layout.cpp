// ckplot — bounded, deterministic placement of labels around fixed points
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "label_layout.hpp"

#include <algorithm>
#include <array>
#include <tuple>

namespace cplot::detail {
namespace {

double overlap(const RectF& a, const RectF& b) {
    return std::max(0.0, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x)) *
           std::max(0.0, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
}

struct Candidate {
    Point position;
    RectF bounds;
};

} // namespace

LabelPlacement point_label(TextItem text, Point anchor, double radius,
                           RectF viewport, const TextMeasurer& measurer) {
    LabelPlacement label{std::move(text), viewport, {}};
    // Padding tolerates small font-metric differences; gap preserves the dot.
    const RectF relative = text_bounds(label.text, measurer, 2.0);
    const std::array<Point, 8> directions{{{1, 0}, {-1, 0}, {0, -1}, {0, 1},
                                         {1, -1}, {-1, -1}, {1, 1}, {-1, 1}}};
    for (const Point d : directions) {
        const double clearance = radius + 4.0;
        const double x = anchor.x + d.x * (clearance + relative.w / 2.0) - relative.w / 2.0;
        const double y = anchor.y + d.y * (clearance + relative.h / 2.0) - relative.h / 2.0;
        label.positions.push_back({label.text.pos.x + x - relative.x,
                                   label.text.pos.y + y - relative.y});
    }
    return label;
}

void place_labels(std::vector<LabelPlacement>& labels,
                  const std::vector<LabelObstacle>& obstacles,
                  const TextMeasurer& measurer) {
    // Measure once, then only rectangle arithmetic. No chart-wide font shrinking.
    std::vector<std::vector<Candidate>> candidates(labels.size());
    std::vector<std::size_t> chosen(labels.size(), 0);
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const auto& label = labels[i];
        const RectF relative = text_bounds(label.text, measurer, 2.0);
        const auto append = [&](Point p) {
            candidates[i].push_back({p, {relative.x + p.x - label.text.pos.x,
                                         relative.y + p.y - label.text.pos.y,
                                         relative.w, relative.h}});
        };
        for (Point p : label.positions) append(p);
        if (candidates[i].empty()) append(label.text.pos);
    }

    // Lexicographic cost: prefer fitting text on the canvas, then minimizing
    // covered markers, other covered area, then the direction preference. Pair costs are
    // symmetric, so each accepted move lowers the global objective.
    using Cost = std::tuple<double, double, double, std::size_t>;
    std::size_t remaining = 4000000;
    bool exhausted = false;
    const auto cost = [&](std::size_t i, std::size_t j) {
        const RectF& box = candidates[i][j].bounds;
        const double clipped = std::max(0.0, box.w * box.h - overlap(box, labels[i].viewport));
        const double outside = clipped < 1e-7 ? 0.0 : clipped;
        double covered = 0.0, markers = 0.0;
        for (const LabelObstacle& obstacle : obstacles) {
            if (remaining == 0) { exhausted = true; break; }
            --remaining;
            const double area = obstacle.weight * overlap(box, obstacle.bounds);
            if (obstacle.marker) markers += area;
            else covered += area;
        }
        for (std::size_t k = 0; k < labels.size() && !exhausted; ++k) {
            if (i == k) continue;
            if (remaining == 0) { exhausted = true; break; }
            --remaining;
            covered += overlap(box, candidates[k][chosen[k]].bounds);
        }
        return Cost{outside, markers, covered, j};
    };
    for (int pass = 0; pass < 12 && !exhausted; ++pass) {
        bool moved = false;
        for (std::size_t i = 0; i < labels.size() && !exhausted; ++i) {
            std::size_t best = chosen[i];
            Cost best_cost = cost(i, best);
            for (std::size_t j = 0; j < candidates[i].size() && !exhausted; ++j) {
                if (j == chosen[i]) continue;
                const Cost next = cost(i, j);
                if (!exhausted && next < best_cost) {
                    best_cost = next;
                    best = j;
                }
            }
            if (best != chosen[i]) { chosen[i] = best; moved = true; }
        }
        if (!moved) break;
    }
    for (std::size_t i = 0; i < labels.size(); ++i)
        labels[i].text.pos = candidates[i][chosen[i]].position;
}

} // namespace cplot::detail
