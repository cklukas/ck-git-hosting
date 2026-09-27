// ckdiagram — layered layout engine tests (dag_layout)
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// These exercise the engine directly rather than through a diagram type,
// because the C3 cluster contract is geometric: a frame must enclose
// exactly its members, nested frames must nest, and an edge that names a
// cluster must stop on that cluster's boundary. Asserting that on the
// numbers is far sharper than asserting it on rendered SVG text.
#include <cmath>
#include <vector>

#include <cworks/microtest.hpp>

#include "dag_layout.hpp"

using cdiagram::detail::DagCluster;
using cdiagram::detail::DagEnd;
using cdiagram::detail::DagGraph;
using cdiagram::detail::DagNodeSize;
using cdiagram::detail::DagParams;
using cdiagram::detail::DagResult;
using cdiagram::detail::FlowDir;
using cdiagram::detail::layout_dag;
using cplot::Point;
using cplot::RectF;

namespace {

constexpr double kEps = 1e-6;

/// A node's drawn box, the way every builder derives it from the layout.
RectF box_of(const DagResult& result, const DagGraph& graph, int node) {
    const std::size_t i = static_cast<std::size_t>(node);
    const Point c = result.centers[i];
    return {c.x - graph.nodes[i].w / 2.0, c.y - graph.nodes[i].h / 2.0, graph.nodes[i].w,
            graph.nodes[i].h};
}

bool contains(const RectF& outer, const RectF& inner) {
    return inner.x >= outer.x - kEps && inner.y >= outer.y - kEps &&
           inner.x + inner.w <= outer.x + outer.w + kEps &&
           inner.y + inner.h <= outer.y + outer.h + kEps;
}

bool overlaps(const RectF& a, const RectF& b) {
    return a.x < b.x + b.w - kEps && b.x < a.x + a.w - kEps && a.y < b.y + b.h - kEps &&
           b.y < a.y + a.h - kEps;
}

bool on_boundary(const RectF& r, Point p) {
    const bool within = p.x >= r.x - kEps && p.x <= r.x + r.w + kEps && p.y >= r.y - kEps &&
                        p.y <= r.y + r.h + kEps;
    const bool on_edge = std::abs(p.x - r.x) < kEps || std::abs(p.x - (r.x + r.w)) < kEps ||
                         std::abs(p.y - r.y) < kEps || std::abs(p.y - (r.y + r.h)) < kEps;
    return within && on_edge;
}

/// The padding + label band a flowchart subgraph frame asks for.
DagCluster framed(int parent = -1) {
    DagCluster cluster;
    cluster.parent = parent;
    cluster.margin = {12.0, 12.0, 30.0, 12.0};
    return cluster;
}

const DagNodeSize kBox{60.0, 30.0};

} // namespace

TEST_CASE("a cluster keeps its members together and shuts outsiders out (C3)") {
    // X fans out to A, M and B. A and B are one cluster, M is not, and the
    // three arrive in the rank interleaved (A, M, B) — the order a pure
    // barycenter sweep leaves them in, since all three see only X. The
    // constraint has to pull A and B together so the frame can enclose
    // them WITHOUT swallowing M.
    DagGraph graph;
    graph.nodes = {kBox, kBox, kBox, kBox};  // 0 X, 1 A, 2 M, 3 B
    graph.node_cluster = {-1, 0, -1, 0};
    graph.clusters = {framed()};
    graph.edges = {{0, 1}, {0, 2}, {0, 3}};

    const DagParams params;
    const DagResult result = layout_dag(graph, params);
    REQUIRE(result.cluster_rects.size() == 1u);
    const RectF frame = result.cluster_rects[0];
    CHECK(contains(frame, box_of(result, graph, 1)));
    CHECK(contains(frame, box_of(result, graph, 3)));
    CHECK(!overlaps(frame, box_of(result, graph, 2)));
    // The frame is RESERVED space, not a box drawn afterwards: the
    // outsider clears the frame by the full node gap (it does not merely
    // clear the member inside it), and the frame fits within the reported
    // size instead of spilling out of it.
    const RectF outsider = box_of(result, graph, 2);
    const double gap = std::max(outsider.x - (frame.x + frame.w), frame.x - (outsider.x + outsider.w));
    CHECK(gap >= params.node_sep - kEps);
    CHECK(frame.x >= -kEps);
    CHECK(frame.y >= -kEps);
    CHECK(frame.x + frame.w <= result.width + kEps);
    CHECK(frame.y + frame.h <= result.height + kEps);
}

TEST_CASE("a cluster reserves room between its ranks and the ones outside (C3)") {
    // The frame's label band and padding sit BETWEEN ranks, so the rank
    // above must move out of the way — a band that merely overlaps its
    // neighbour is not reserved space.
    DagGraph graph;
    graph.nodes = {kBox, kBox, kBox};  // 0 above, 1 in the cluster, 2 below
    graph.node_cluster = {-1, 0, -1};
    graph.clusters = {framed()};
    graph.edges = {{0, 1}, {1, 2}};

    const DagParams params;
    const DagResult result = layout_dag(graph, params);
    const RectF frame = result.cluster_rects[0];
    const RectF above = box_of(result, graph, 0);
    const RectF below = box_of(result, graph, 2);
    CHECK(contains(frame, box_of(result, graph, 1)));
    // Full rank separation OUTSIDE the frame at both ends, not inside it.
    CHECK(frame.y - (above.y + above.h) >= params.rank_sep - kEps);
    CHECK(below.y - (frame.y + frame.h) >= params.rank_sep - kEps);
}

TEST_CASE("cluster members stay together in every direction (C3)") {
    for (const FlowDir dir :
         {FlowDir::Down, FlowDir::Up, FlowDir::Right, FlowDir::Left}) {
        DagGraph graph;
        graph.nodes = {kBox, kBox, kBox, kBox};
        graph.node_cluster = {-1, 0, -1, 0};
        graph.clusters = {framed()};
        graph.edges = {{0, 1}, {0, 2}, {0, 3}};
        DagParams params;
        params.dir = dir;

        const DagResult result = layout_dag(graph, params);
        const RectF frame = result.cluster_rects[0];
        CHECK(contains(frame, box_of(result, graph, 1)));
        CHECK(contains(frame, box_of(result, graph, 3)));
        CHECK(!overlaps(frame, box_of(result, graph, 2)));
        CHECK(frame.x + frame.w <= result.width + kEps);
        CHECK(frame.y + frame.h <= result.height + kEps);
    }
}

TEST_CASE("an edge that names a cluster lands on the cluster frame (C3)") {
    // C --> (the cluster holding A --> B). The link must reach the
    // BOUNDARY, not some member the engine picked out of the group.
    DagGraph graph;
    graph.nodes = {kBox, kBox, kBox};  // 0 C, 1 A, 2 B
    graph.node_cluster = {-1, 0, 0};
    graph.clusters = {framed()};
    graph.edges = {{1, 2}, {0, DagEnd::on_cluster(0)}};

    const DagResult result = layout_dag(graph, DagParams{});
    const RectF frame = result.cluster_rects[0];
    REQUIRE(result.routes[1].size() >= 2u);
    CHECK(on_boundary(frame, result.routes[1].back()));
    // The link starts outside the frame and never reaches a member centre.
    CHECK(!overlaps(frame, box_of(result, graph, 0)));
    const Point tip = result.routes[1].back();
    CHECK(std::abs(tip.y - result.centers[1].y) > kEps);
    // Naming the cluster also ranks the WHOLE cluster below the source.
    CHECK(box_of(result, graph, 0).y + kBox.h <= frame.y + kEps);
}

TEST_CASE("an edge out of a cluster leaves from the cluster frame (C3)") {
    DagGraph graph;
    graph.nodes = {kBox, kBox, kBox};  // 0 A, 1 B, 2 D
    graph.node_cluster = {0, 0, -1};
    graph.clusters = {framed()};
    graph.edges = {{0, 1}, {DagEnd::on_cluster(0), 2}};

    const DagResult result = layout_dag(graph, DagParams{});
    const RectF frame = result.cluster_rects[0];
    REQUIRE(result.routes[1].size() >= 2u);
    CHECK(on_boundary(frame, result.routes[1].front()));
    CHECK(!overlaps(frame, box_of(result, graph, 2)));
    // Everything in the cluster ranks above the target.
    CHECK(frame.y + frame.h <= box_of(result, graph, 2).y + kEps);
}

TEST_CASE("nested clusters nest, and the outer frame holds the inner one (C3)") {
    DagGraph graph;
    graph.nodes = {kBox, kBox, kBox, kBox};  // 0 A, 1 B (inner), 2 C (outer), 3 D (loose)
    graph.node_cluster = {1, 1, 0, -1};
    graph.clusters = {framed(), framed(/*parent=*/0)};
    graph.edges = {{0, 1}, {1, 2}, {2, 3}};

    const DagResult result = layout_dag(graph, DagParams{});
    REQUIRE(result.cluster_rects.size() == 2u);
    const RectF outer = result.cluster_rects[0];
    const RectF inner = result.cluster_rects[1];
    CHECK(contains(inner, box_of(result, graph, 0)));
    CHECK(contains(inner, box_of(result, graph, 1)));
    CHECK(contains(outer, inner));
    CHECK(contains(outer, box_of(result, graph, 2)));
    CHECK(!overlaps(inner, box_of(result, graph, 2)));
    CHECK(!overlaps(outer, box_of(result, graph, 3)));
    // Both label bands got their own room: the inner frame starts below
    // the outer one's, it does not share the band.
    CHECK(inner.y >= outer.y + 30.0 - kEps);
}

TEST_CASE("a cluster frame is never smaller than the caller can draw (C3)") {
    DagGraph graph;
    graph.nodes = {kBox, kBox};
    graph.node_cluster = {0, -1};
    DagCluster cluster = framed();
    cluster.min_width = 260.0;  // a title band far wider than the one member
    graph.clusters = {cluster};
    graph.edges = {{0, 1}};

    const DagResult result = layout_dag(graph, DagParams{});
    const RectF frame = result.cluster_rects[0];
    CHECK(frame.w >= 260.0 - kEps);
    CHECK(contains(frame, box_of(result, graph, 0)));
    // The room was RESERVED, so the widened frame still fits the canvas
    // and still keeps clear of the node outside it.
    CHECK(frame.x >= -kEps);
    CHECK(frame.x + frame.w <= result.width + kEps);
    CHECK(!overlaps(frame, box_of(result, graph, 1)));
}

TEST_CASE("a cluster with no members frames nothing and carries no edge (C3)") {
    DagGraph graph;
    graph.nodes = {kBox, kBox};
    graph.node_cluster = {-1, -1};
    graph.clusters = {framed()};
    graph.edges = {{0, 1}, {0, DagEnd::on_cluster(0)}};

    const DagResult result = layout_dag(graph, DagParams{});
    CHECK(result.cluster_rects[0].w == 0.0);
    CHECK(result.cluster_rects[0].h == 0.0);
    CHECK(result.routes[0].size() == 2u);
    CHECK(result.routes[1].empty());  // nothing to attach to: the caller reports it
}

TEST_CASE("a cluster edge is broken like any other cycle (C3)") {
    // The cluster border vertices must not turn a cyclic graph into a hang
    // or a ranking that never terminates.
    DagGraph graph;
    graph.nodes = {kBox, kBox, kBox};
    graph.node_cluster = {0, 0, -1};
    graph.clusters = {framed()};
    graph.edges = {{0, 1}, {DagEnd::on_cluster(0), 2}, {2, DagEnd::on_cluster(0)}};

    const DagResult result = layout_dag(graph, DagParams{});
    CHECK(result.width > 0.0);
    CHECK(result.height > 0.0);
    CHECK(contains(result.cluster_rects[0], box_of(result, graph, 0)));
    CHECK(contains(result.cluster_rects[0], box_of(result, graph, 1)));
}

TEST_CASE("clustered layout is bit-for-bit reproducible (C3)") {
    DagGraph graph;
    graph.nodes = {kBox, kBox, kBox, kBox, kBox, kBox};
    graph.node_cluster = {-1, 1, 1, 0, -1, -1};
    graph.clusters = {framed(), framed(/*parent=*/0)};
    graph.edges = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {0, 5},
                   {5, DagEnd::on_cluster(1)}, {DagEnd::on_cluster(0), 4}};

    const DagResult a = layout_dag(graph, DagParams{});
    const DagResult b = layout_dag(graph, DagParams{});
    REQUIRE(a.centers.size() == b.centers.size());
    for (std::size_t i = 0; i < a.centers.size(); ++i) {
        CHECK(a.centers[i].x == b.centers[i].x);
        CHECK(a.centers[i].y == b.centers[i].y);
    }
    REQUIRE(a.routes.size() == b.routes.size());
    for (std::size_t k = 0; k < a.routes.size(); ++k) {
        REQUIRE(a.routes[k].size() == b.routes[k].size());
        for (std::size_t p = 0; p < a.routes[k].size(); ++p) {
            CHECK(a.routes[k][p].x == b.routes[k][p].x);
            CHECK(a.routes[k][p].y == b.routes[k][p].y);
        }
    }
    for (std::size_t c = 0; c < a.cluster_rects.size(); ++c) {
        CHECK(a.cluster_rects[c].x == b.cluster_rects[c].x);
        CHECK(a.cluster_rects[c].y == b.cluster_rects[c].y);
        CHECK(a.cluster_rects[c].w == b.cluster_rects[c].w);
        CHECK(a.cluster_rects[c].h == b.cluster_rects[c].h);
    }
    CHECK(a.width == b.width);
    CHECK(a.height == b.height);
}

TEST_CASE("a graph without clusters is untouched by the cluster machinery") {
    DagGraph plain;
    plain.nodes = {kBox, kBox, kBox};
    plain.edges = {{0, 1}, {1, 2}, {0, 2}};

    DagGraph declared = plain;  // one cluster, but nothing in it
    declared.clusters = {framed()};
    declared.node_cluster = {-1, -1, -1};

    const DagResult a = layout_dag(plain, DagParams{});
    const DagResult b = layout_dag(declared, DagParams{});
    REQUIRE(a.centers.size() == b.centers.size());
    for (std::size_t i = 0; i < a.centers.size(); ++i) {
        CHECK(a.centers[i].x == b.centers[i].x);
        CHECK(a.centers[i].y == b.centers[i].y);
    }
    CHECK(a.width == b.width);
    CHECK(a.height == b.height);
}
