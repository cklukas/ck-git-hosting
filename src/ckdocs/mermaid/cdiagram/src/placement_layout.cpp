// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "placement_layout.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace cdiagram::detail {

namespace {

void report(cworks::Diagnostics* diagnostics, std::string message) {
    if (diagnostics == nullptr) return;
    diagnostics->push_back(
        {cworks::Diagnostic::Severity::Warning, "placement: " + std::move(message)});
}

bool same_point(Point a, Point b) {
    // The routes were built from these very values, so equality is a copy
    // comparison rather than a geometric one — but a tolerance costs nothing
    // and survives any future rounding in the layout.
    return std::abs(a.x - b.x) < 1e-9 && std::abs(a.y - b.y) < 1e-9;
}

} // namespace

void apply_placement(const Placement& placement, const std::vector<std::string>& keys,
                     const DagGraph& graph, DagResult& result,
                     cworks::Diagnostics* diagnostics) {
    if (placement.empty() || result.centers.empty()) return;

    std::map<std::string, std::size_t> index_of;
    for (std::size_t i = 0; i < keys.size() && i < result.centers.size(); ++i)
        index_of.emplace(keys[i], i);

    const std::vector<Point> original = result.centers;
    std::vector<char> moved(result.centers.size(), 0);

    // --- 1. Pins: an exact centre, applied first so alignments can read it.
    for (const auto& [key, entry] : placement.entries()) {
        if (!entry.pin) continue;
        const auto it = index_of.find(key);
        if (it == index_of.end()) {
            report(diagnostics, "'" + key + "' is not in this diagram, ignored");
            continue;
        }
        result.centers[it->second] = Point{entry.pin->x, entry.pin->y};
        moved[it->second] = 1;
    }

    // --- 2. Alignments, in dependency order. An alignment may point at
    // something that is itself aligned, so this resolves iteratively and stops
    // when a pass changes nothing — which is also how a cycle is caught.
    std::vector<const Placement::Entry*> pending;
    for (const auto& entry : placement.entries())
        if (entry.second.align) pending.push_back(&entry);

    std::vector<char> resolved(pending.size(), 0);
    for (std::size_t pass = 0; pass <= pending.size(); ++pass) {
        bool progressed = false;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (resolved[i]) continue;
            const std::string& key = pending[i]->first;
            const PlacementAlign& align = *pending[i]->second.align;

            const auto self = index_of.find(key);
            if (self == index_of.end()) {
                report(diagnostics, "'" + key + "' is not in this diagram, ignored");
                resolved[i] = 1;
                progressed = true;
                continue;
            }
            const auto target = index_of.find(align.to);
            if (target == index_of.end()) {
                report(diagnostics, "'" + key + "' is aligned to '" + align.to +
                                        "', which is not in this diagram, ignored");
                resolved[i] = 1;
                progressed = true;
                continue;
            }
            // Wait for the target when it is itself waiting on something.
            bool target_pending = false;
            for (std::size_t j = 0; j < pending.size(); ++j)
                if (!resolved[j] && pending[j]->first == align.to) target_pending = true;
            if (target_pending) continue;

            if (align.axis == PlacementAxis::X)
                result.centers[self->second].x = result.centers[target->second].x;
            else
                result.centers[self->second].y = result.centers[target->second].y;
            moved[self->second] = 1;
            resolved[i] = 1;
            progressed = true;
        }
        if (!progressed) break;
    }
    for (std::size_t i = 0; i < pending.size(); ++i) {
        if (resolved[i]) continue;
        report(diagnostics, "'" + pending[i]->first + "' is part of a circular alignment, ignored");
    }

    // --- 3. Edges follow their endpoints. A route runs from the source centre
    // through its routing points to the target centre, so moving a node means
    // moving the end of every route that started or finished on it.
    for (std::size_t k = 0; k < result.routes.size() && k < graph.edges.size(); ++k) {
        std::vector<Point>& route = result.routes[k];
        if (route.size() < 2) continue;
        const DagEdge& edge = graph.edges[k];
        if (!edge.from.is_cluster) {
            const auto from = static_cast<std::size_t>(edge.from.index);
            if (from < moved.size() && moved[from] && same_point(route.front(), original[from]))
                route.front() = result.centers[from];
        }
        if (!edge.to.is_cluster) {
            const auto to = static_cast<std::size_t>(edge.to.index);
            if (to < moved.size() && moved[to] && same_point(route.back(), original[to]))
                route.back() = result.centers[to];
        }
    }

    // --- 4. Cluster frames grow to keep their moved members inside. A frame
    // that no longer encloses its contents would be a drawing that contradicts
    // itself, so the frame yields rather than the author's position.
    for (std::size_t i = 0; i < graph.node_cluster.size() && i < moved.size(); ++i) {
        if (!moved[i]) continue;
        int cluster = graph.node_cluster[i];
        const double half_w = graph.nodes[i].w / 2.0;
        const double half_h = graph.nodes[i].h / 2.0;
        while (cluster >= 0 && static_cast<std::size_t>(cluster) < result.cluster_rects.size()) {
            RectF& rect = result.cluster_rects[static_cast<std::size_t>(cluster)];
            if (rect.w > 0.0 && rect.h > 0.0) {
                const double left = std::min(rect.x, result.centers[i].x - half_w);
                const double top = std::min(rect.y, result.centers[i].y - half_h);
                const double right = std::max(rect.x + rect.w, result.centers[i].x + half_w);
                const double bottom = std::max(rect.y + rect.h, result.centers[i].y + half_h);
                rect = RectF{left, top, right - left, bottom - top};
            }
            cluster = graph.clusters[static_cast<std::size_t>(cluster)].parent;
        }
    }

    // --- 5. Normalise: a pin may sit at a negative coordinate, which is a
    // legal thing for an author to ask for and an illegal thing to draw. Shift
    // everything so the drawing starts at the origin again, then grow the
    // canvas to hold it.
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = result.width;
    double max_y = result.height;
    for (std::size_t i = 0; i < result.centers.size(); ++i) {
        const double half_w = i < graph.nodes.size() ? graph.nodes[i].w / 2.0 : 0.0;
        const double half_h = i < graph.nodes.size() ? graph.nodes[i].h / 2.0 : 0.0;
        min_x = std::min(min_x, result.centers[i].x - half_w);
        min_y = std::min(min_y, result.centers[i].y - half_h);
        max_x = std::max(max_x, result.centers[i].x + half_w);
        max_y = std::max(max_y, result.centers[i].y + half_h);
    }
    for (const RectF& rect : result.cluster_rects) {
        if (rect.w <= 0.0 || rect.h <= 0.0) continue;
        min_x = std::min(min_x, rect.x);
        min_y = std::min(min_y, rect.y);
        max_x = std::max(max_x, rect.x + rect.w);
        max_y = std::max(max_y, rect.y + rect.h);
    }

    if (min_x < 0.0 || min_y < 0.0) {
        const double dx = -std::min(0.0, min_x);
        const double dy = -std::min(0.0, min_y);
        for (Point& centre : result.centers) {
            centre.x += dx;
            centre.y += dy;
        }
        for (std::vector<Point>& route : result.routes)
            for (Point& point : route) {
                point.x += dx;
                point.y += dy;
            }
        for (RectF& rect : result.cluster_rects) {
            rect.x += dx;
            rect.y += dy;
        }
        max_x += dx;
        max_y += dy;
    }
    result.width = std::max(result.width, max_x);
    result.height = std::max(result.height, max_y);
}

} // namespace cdiagram::detail
