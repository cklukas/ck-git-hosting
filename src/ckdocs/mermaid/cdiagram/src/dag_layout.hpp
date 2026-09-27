// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Internal: a deterministic layered ("Sugiyama") graph layout — the
// engine mermaid draws flowcharts with (dagre). Nodes are assigned to
// ranks (longest-path), ordered within ranks to reduce edge crossings
// (barycenter sweeps), then given coordinates; long edges route through
// virtual nodes. Shared by flowchart, class, ER, state, requirement,
// C4 and mindmap.
//
// Clusters (mermaid's `subgraph`) are a layout constraint, not a frame
// painted afterwards: members stay contiguous in every rank's ordering,
// the coordinate pass reserves the frame's margin between a member and
// its neighbours, and the result carries the frame rectangle the layout
// actually made room for. An edge may name a cluster instead of a node;
// it is then ranked and ordered against the cluster's whole membership
// and its route stops on the cluster frame.
//
// Determinism is a hard requirement: every tie-break is by input index,
// iteration counts are fixed, and no map is iterated in hash order — so
// the same graph always lays out to the same coordinates.
#pragma once

#include <cstddef>
#include <vector>

#include <cplot/scene.hpp>

namespace cdiagram::detail {

using cplot::Point;
using cplot::RectF;

/// Reading direction of the layout (Mermaid's TD/TB, BT, LR, RL).
enum class FlowDir { Down, Up, Right, Left };

struct DagNodeSize {
    double w = 0.0;
    double h = 0.0;
};

/// Space reserved outside a cluster's members, in screen axes — the
/// caller draws its frame there. The label band is a `top` inset
/// whichever way the graph flows, because the band is horizontal.
struct DagInsets {
    double left = 0.0;
    double right = 0.0;
    double top = 0.0;
    double bottom = 0.0;
};

/// One `subgraph … end` boundary. Clusters must be listed parents
/// before children (`parent` < own index); a forward or self reference
/// is treated as top level rather than followed into a loop.
struct DagCluster {
    int parent = -1;      ///< enclosing cluster, -1 at top level
    DagInsets margin;     ///< frame space reserved around the members
    double min_width = 0.0;   ///< smallest frame the caller can draw…
    double min_height = 0.0;  ///< …e.g. one that fits a title band
};

/// One end of an edge. Implicitly a node index, so plain graphs read as
/// before; `DagEnd::on_cluster(c)` addresses a cluster's frame instead.
struct DagEnd {
    int index = 0;
    bool is_cluster = false;

    constexpr DagEnd(int node_index = 0) : index(node_index) {}  // NOLINT: node ends read as indices
    static constexpr DagEnd on_cluster(int cluster_index) {
        DagEnd end;
        end.index = cluster_index;
        end.is_cluster = true;
        return end;
    }
    constexpr bool operator==(const DagEnd& other) const {
        return index == other.index && is_cluster == other.is_cluster;
    }
};

struct DagEdge {
    DagEnd from;
    DagEnd to;
};

/// The graph to lay out. `node_cluster` is parallel to `nodes` and names
/// each node's INNERMOST cluster (-1 for none); it may be left empty
/// when `clusters` is, which is the plain layered case.
struct DagGraph {
    std::vector<DagNodeSize> nodes;
    std::vector<DagEdge> edges;
    std::vector<DagCluster> clusters;
    std::vector<int> node_cluster;
};

struct DagParams {
    FlowDir dir = FlowDir::Down;
    double node_sep = 36.0;  ///< gap between nodes within a rank
    double rank_sep = 54.0;  ///< gap between ranks
};

struct DagResult {
    std::vector<Point> centers;              ///< one per input node
    std::vector<std::vector<Point>> routes;  ///< one polyline per input edge
    /// One frame per cluster, in the same coordinates as `centers`. A
    /// cluster with no members gets a zero rectangle and is not drawn.
    std::vector<RectF> cluster_rects;
    double width = 0.0;
    double height = 0.0;
};

/// Lay out `graph`. Returns node centres, an edge spine per edge, the
/// cluster frames, and the overall size.
///
/// An edge spine runs from the source centre through its routing points
/// to the target centre; the caller clips those ends to the node
/// outlines it drew. An end that names a cluster is already clipped —
/// the spine stops exactly on that cluster's frame — so the caller must
/// not clip it again. Degenerate edges (both ends the same node or the
/// same cluster, or an end naming a cluster with no members) get an
/// empty route and are the caller's to draw or drop.
DagResult layout_dag(const DagGraph& graph, const DagParams& params);

} // namespace cdiagram::detail
