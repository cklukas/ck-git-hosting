// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "dag_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numeric>
#include <utility>
#include "cdiagram/error.hpp"
#include <cworks/app_error.hpp>

namespace cdiagram::detail {

namespace {

constexpr double kVirtualExtent = 8.0;  // breadth of a routing (virtual) node

double breadth_of(const DagNodeSize& s, FlowDir dir) {
    return (dir == FlowDir::Down || dir == FlowDir::Up) ? s.w : s.h;
}
double depth_of(const DagNodeSize& s, FlowDir dir) {
    return (dir == FlowDir::Down || dir == FlowDir::Up) ? s.h : s.w;
}

/// A cluster's screen-space margin resolved onto the layout's own axes:
/// `breadth` runs across a rank, `depth` from one rank to the next.
struct AxisInsets {
    double breadth_lo = 0.0;
    double breadth_hi = 0.0;
    double depth_lo = 0.0;
    double depth_hi = 0.0;
};

AxisInsets to_axes(const DagInsets& m, FlowDir dir) {
    switch (dir) {
    case FlowDir::Down: return {m.left, m.right, m.top, m.bottom};
    // Up and Left mirror their depth axis, so the frame's screen top is
    // the HIGH-depth side there.
    case FlowDir::Up: return {m.left, m.right, m.bottom, m.top};
    case FlowDir::Right: return {m.top, m.bottom, m.left, m.right};
    case FlowDir::Left: return {m.top, m.bottom, m.right, m.left};
    }
    return {};
}

/// An extent along both layout axes; `any` distinguishes "empty" from
/// "a zero-size box at the origin".
struct Span {
    double b_lo = 0.0;
    double b_hi = 0.0;
    double d_lo = 0.0;
    double d_hi = 0.0;
    bool any = false;

    void add(double blo, double bhi, double dlo, double dhi) {
        if (!any) {
            b_lo = blo; b_hi = bhi; d_lo = dlo; d_hi = dhi;
            any = true;
            return;
        }
        b_lo = std::min(b_lo, blo); b_hi = std::max(b_hi, bhi);
        d_lo = std::min(d_lo, dlo); d_hi = std::max(d_hi, dhi);
    }
    void unite(const Span& other) {
        if (other.any) add(other.b_lo, other.b_hi, other.d_lo, other.d_hi);
    }
};

/// The cluster forest resolved onto the layout axes: who nests in whom,
/// which nodes belong where, and how much room each frame needs.
struct ClusterTree {
    std::vector<int> parent;
    std::vector<std::vector<int>> children;
    std::vector<AxisInsets> inset;
    std::vector<double> min_breadth;
    std::vector<double> min_depth;
    /// Transitive membership (a node in a nested cluster belongs to every
    /// enclosing one), ascending by node index so ties break by input order.
    std::vector<std::vector<int>> members;

    int size() const { return static_cast<int>(parent.size()); }
    bool valid(int c) const { return c >= 0 && c < size(); }

    /// Is `c` on the ancestor chain of (and including) `innermost`?
    bool covers(int c, int innermost) const {
        for (int x = innermost; x >= 0; x = parent[static_cast<std::size_t>(x)])
            if (x == c) return true;
        return false;
    }
    /// The cluster on `innermost`'s chain whose parent is `ancestor`, or
    /// -1 when the item sits directly in `ancestor`.
    int child_of(int ancestor, int innermost) const {
        for (int x = innermost; x >= 0; x = parent[static_cast<std::size_t>(x)]) {
            if (x == ancestor) return -1;
            if (parent[static_cast<std::size_t>(x)] == ancestor) return x;
        }
        return -1;
    }
    /// The innermost cluster enclosing both `a` and `b` (-1 = top level).
    int common(int a, int b) const {
        for (int x = a; x >= 0; x = parent[static_cast<std::size_t>(x)])
            if (covers(x, b)) return x;
        return -1;
    }
};

ClusterTree build_cluster_tree(const DagGraph& graph, FlowDir dir) {
    ClusterTree tree;
    const int cn = static_cast<int>(graph.clusters.size());
    const bool swapped = dir == FlowDir::Right || dir == FlowDir::Left;
    tree.parent.assign(static_cast<std::size_t>(cn), -1);
    tree.children.resize(static_cast<std::size_t>(cn));
    tree.inset.resize(static_cast<std::size_t>(cn));
    tree.min_breadth.assign(static_cast<std::size_t>(cn), 0.0);
    tree.min_depth.assign(static_cast<std::size_t>(cn), 0.0);
    tree.members.resize(static_cast<std::size_t>(cn));
    for (int c = 0; c < cn; ++c) {
        const DagCluster& in = graph.clusters[static_cast<std::size_t>(c)];
        const std::size_t u = static_cast<std::size_t>(c);
        // Parents precede children by contract; anything else would let the
        // ancestor walks below loop forever, so it is read as top level.
        tree.parent[u] = (in.parent >= 0 && in.parent < c) ? in.parent : -1;
        if (tree.parent[u] >= 0)
            tree.children[static_cast<std::size_t>(tree.parent[u])].push_back(c);
        tree.inset[u] = to_axes(in.margin, dir);
        tree.min_breadth[u] = swapped ? in.min_height : in.min_width;
        tree.min_depth[u] = swapped ? in.min_width : in.min_height;
    }
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        const int innermost = i < graph.node_cluster.size() ? graph.node_cluster[i] : -1;
        if (!tree.valid(innermost)) continue;
        for (int c = innermost; c >= 0; c = tree.parent[static_cast<std::size_t>(c)])
            tree.members[static_cast<std::size_t>(c)].push_back(static_cast<int>(i));
    }
    return tree;
}

/// Reorder one rank so that every cluster's members form a single
/// contiguous run at every nesting level, disturbing the barycenter order
/// the sweep just produced as little as the constraint allows (Forster's
/// constrained crossing reduction). `items` is that barycenter order, so
/// an item's position in it IS its key — integer, hence exact.
std::vector<int> arrange(const ClusterTree& tree, const std::vector<int>& innermost,
                         const std::vector<int>& items, int cluster) {
    if (items.size() < 2) return items;
    struct Group {
        int cluster = -1;  ///< -1 marks a bare item that sits directly here
        // 64-bit on every platform: the comparison below multiplies these,
        // and a 32-bit `long` (Windows) would wrap on a large rank and pick
        // a different order there than on unix.
        long long key_sum = 0;
        long long key_count = 0;
        std::size_t first = 0;
        std::vector<int> items;
    };
    std::vector<Group> groups;
    std::map<int, std::size_t> by_cluster;  // ordered: never iterated in hash order
    for (std::size_t i = 0; i < items.size(); ++i) {
        const int item = items[i];
        const int child =
            tree.child_of(cluster, innermost[static_cast<std::size_t>(item)]);
        std::size_t g = groups.size();
        if (child < 0) {
            groups.push_back(Group{-1, 0, 0, i, {}});
        } else if (const auto [it, fresh] = by_cluster.emplace(child, groups.size()); fresh) {
            groups.push_back(Group{child, 0, 0, i, {}});
        } else {
            g = it->second;
        }
        groups[g].key_sum += static_cast<long long>(i);
        ++groups[g].key_count;
        groups[g].items.push_back(item);
    }
    std::vector<int> order(groups.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Group& ga = groups[static_cast<std::size_t>(a)];
        const Group& gb = groups[static_cast<std::size_t>(b)];
        // Mean position, compared without division so the ordering carries
        // no rounding of its own.
        const long long lhs = ga.key_sum * gb.key_count;
        const long long rhs = gb.key_sum * ga.key_count;
        if (lhs != rhs) return lhs < rhs;
        return ga.first < gb.first;  // unique, so this is a total order
    });
    std::vector<int> out;
    out.reserve(items.size());
    for (const int g : order) {
        const Group& group = groups[static_cast<std::size_t>(g)];
        if (group.cluster < 0) {
            out.push_back(group.items.front());
        } else {
            const std::vector<int> inner =
                arrange(tree, innermost, group.items, group.cluster);
            out.insert(out.end(), inner.begin(), inner.end());
        }
    }
    return out;
}

bool inside(Point p, const RectF& r) {
    return p.x >= r.x && p.x <= r.x + r.w && p.y >= r.y && p.y <= r.y + r.h;
}

/// Trim `pts`, walked from its front, so it stops where it first meets
/// `rect` — how an edge that names a cluster lands on that cluster's
/// frame instead of on some member inside it. Returned unchanged when the
/// route never enters, or already starts inside (an edge from a node to
/// the very cluster that contains it).
std::vector<Point> clip_before_rect(std::vector<Point> pts, const RectF& rect) {
    if (pts.size() < 2 || rect.w <= 0.0 || rect.h <= 0.0) return pts;
    if (inside(pts.front(), rect)) return pts;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        if (!inside(pts[i + 1], rect)) continue;
        // Liang-Barsky: the largest entry parameter over the four slabs.
        const Point p0 = pts[i], p1 = pts[i + 1];
        const double dx = p1.x - p0.x, dy = p1.y - p0.y;
        const double denom[4] = {-dx, dx, -dy, dy};
        const double dist[4] = {p0.x - rect.x, rect.x + rect.w - p0.x, p0.y - rect.y,
                                rect.y + rect.h - p0.y};
        double t = 0.0;
        for (int e = 0; e < 4; ++e)
            if (denom[e] < 0.0) t = std::max(t, dist[e] / denom[e]);
        t = std::clamp(t, 0.0, 1.0);
        std::vector<Point> cut(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        cut.push_back({p0.x + dx * t, p0.y + dy * t});
        return cut;
    }
    return pts;
}

/// A Catmull-Rom spline through `pts`, sampled at a fixed rate so bends
/// (long edges, parallel-edge offsets) draw as smooth curves. Endpoints
/// are preserved. Fewer than three points pass through unchanged.
std::vector<Point> smooth(const std::vector<Point>& pts) {
    if (pts.size() < 3) return pts;
    constexpr int kPerSegment = 8;
    std::vector<Point> out;
    out.push_back(pts.front());
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const Point p0 = i == 0 ? pts[i] : pts[i - 1];
        const Point p1 = pts[i];
        const Point p2 = pts[i + 1];
        const Point p3 = i + 2 < pts.size() ? pts[i + 2] : pts[i + 1];
        for (int s = 1; s <= kPerSegment; ++s) {
            const double t = static_cast<double>(s) / kPerSegment;
            const double t2 = t * t, t3 = t2 * t;
            out.push_back({0.5 * ((2.0 * p1.x) + (-p0.x + p2.x) * t +
                                  (2.0 * p0.x - 5.0 * p1.x + 4.0 * p2.x - p3.x) * t2 +
                                  (-p0.x + 3.0 * p1.x - 3.0 * p2.x + p3.x) * t3),
                           0.5 * ((2.0 * p1.y) + (-p0.y + p2.y) * t +
                                  (2.0 * p0.y - 5.0 * p1.y + 4.0 * p2.y - p3.y) * t2 +
                                  (-p0.y + 3.0 * p1.y - 3.0 * p2.y + p3.y) * t3)});
        }
    }
    return out;
}

/// Inversions in `seq` — the crossing count contribution of one layer.
/// 64-bit on every platform: a dense layer has O(k^2) inversions, which a
/// 32-bit `long` (Windows) would wrap, and the sweep would then keep a
/// different candidate ordering there than on unix.
long long inversions(const std::vector<int>& seq) {
    long long crossings = 0;
    for (std::size_t i = 0; i < seq.size(); ++i)
        for (std::size_t j = i + 1; j < seq.size(); ++j)
            if (seq[i] > seq[j]) ++crossings;
    return crossings;
}

/// One arc of the ranking graph. `edge` names the input edge it came
/// from, or -1 for a cluster border arc.
struct Arc {
    int u = 0;
    int v = 0;
    int len = 0;
    int edge = -1;
};

} // namespace

DagResult layout_dag(const DagGraph& graph, const DagParams& params) {
    if (graph.nodes.size() > 1024 || graph.edges.size() > 4096 || graph.clusters.size() > 128)
        throw Error(cworks::validation_failed("graph exceeds 1024 nodes, 4096 edges, or 128 clusters"));
    DagResult result;
    const int n = static_cast<int>(graph.nodes.size());
    if (n == 0) return result;
    const int m = static_cast<int>(graph.edges.size());
    const FlowDir dir = params.dir;
    const ClusterTree tree = build_cluster_tree(graph, dir);
    const int cn = tree.size();
    result.cluster_rects.assign(static_cast<std::size_t>(cn), RectF{});

    // --- 1. Ranking graph: the real nodes, plus a pair of border vertices
    // per cluster. `enter(c)` sits at or above every member and `exit(c)`
    // at or below, joined to the members by ZERO-length arcs — so an edge
    // that names a cluster constrains the whole cluster (dagre's border
    // nodes) at O(members) instead of wiring every member pair. ---
    const auto enter_v = [&](int c) { return n + 2 * c; };
    const auto exit_v = [&](int c) { return n + 2 * c + 1; };
    const int vertices = n + 2 * cn;
    std::vector<Arc> arcs;
    for (int c = 0; c < cn; ++c)
        for (const int member : tree.members[static_cast<std::size_t>(c)]) {
            arcs.push_back({enter_v(c), member, 0, -1});
            arcs.push_back({member, exit_v(c), 0, -1});
        }
    const auto usable = [&](DagEnd end) {
        if (!end.is_cluster) return end.index >= 0 && end.index < n;
        return tree.valid(end.index) &&
               !tree.members[static_cast<std::size_t>(end.index)].empty();
    };
    const auto vertex_of = [&](DagEnd end, bool leaving) {
        return end.is_cluster ? (leaving ? exit_v(end.index) : enter_v(end.index))
                              : end.index;
    };
    std::vector<int> arc_of_edge(static_cast<std::size_t>(m), -1);
    for (int k = 0; k < m; ++k) {
        const DagEdge& e = graph.edges[static_cast<std::size_t>(k)];
        // A degenerate edge (a self-loop, or an end naming an empty
        // cluster) gets no arc and no route; the caller draws or drops it.
        if (e.from == e.to || !usable(e.from) || !usable(e.to)) continue;
        arc_of_edge[static_cast<std::size_t>(k)] = static_cast<int>(arcs.size());
        arcs.push_back({vertex_of(e.from, true), vertex_of(e.to, false), 1, k});
    }

    // --- 2. Break cycles: a DFS marks back arcs. A real edge is reversed
    // for the acyclic phases (it still DRAWS the way it was written); a
    // border arc is relaxed instead, since "reversed containment" would
    // mean nothing. Iterative, so a long chain cannot overflow the stack.
    std::vector<std::vector<int>> adj(static_cast<std::size_t>(vertices));
    for (std::size_t a = 0; a < arcs.size(); ++a)
        adj[static_cast<std::size_t>(arcs[a].u)].push_back(static_cast<int>(a));
    std::vector<char> color(static_cast<std::size_t>(vertices), 0);  // 0 white 1 gray 2 black
    std::vector<char> reversed(arcs.size(), 0);
    std::vector<char> relaxed(arcs.size(), 0);
    std::vector<std::pair<int, std::size_t>> stack;
    for (int s = 0; s < vertices; ++s) {
        if (color[static_cast<std::size_t>(s)] != 0) continue;
        color[static_cast<std::size_t>(s)] = 1;
        stack.push_back({s, 0});
        while (!stack.empty()) {
            const int u = stack.back().first;
            std::vector<int>& out = adj[static_cast<std::size_t>(u)];
            if (stack.back().second < out.size()) {
                const int a = out[stack.back().second++];
                const int v = arcs[static_cast<std::size_t>(a)].v;
                if (color[static_cast<std::size_t>(v)] == 1) {
                    if (arcs[static_cast<std::size_t>(a)].edge >= 0)
                        reversed[static_cast<std::size_t>(a)] = 1;
                    else
                        relaxed[static_cast<std::size_t>(a)] = 1;
                } else if (color[static_cast<std::size_t>(v)] == 0) {
                    color[static_cast<std::size_t>(v)] = 1;
                    stack.push_back({v, 0});
                }
            } else {
                color[static_cast<std::size_t>(u)] = 2;
                stack.pop_back();
            }
        }
    }

    // --- 3. Longest-path ranking on the (now acyclic) forward arcs. ---
    std::vector<std::vector<std::pair<int, int>>> fout(static_cast<std::size_t>(vertices));
    std::vector<int> indeg(static_cast<std::size_t>(vertices), 0);
    for (std::size_t a = 0; a < arcs.size(); ++a) {
        if (relaxed[a]) continue;
        const int u = reversed[a] ? arcs[a].v : arcs[a].u;
        const int w = reversed[a] ? arcs[a].u : arcs[a].v;
        fout[static_cast<std::size_t>(u)].push_back({w, arcs[a].len});
        ++indeg[static_cast<std::size_t>(w)];
    }
    std::vector<int> vrank(static_cast<std::size_t>(vertices), 0);
    std::vector<int> queue;
    for (int i = 0; i < vertices; ++i)
        if (indeg[static_cast<std::size_t>(i)] == 0) queue.push_back(i);
    std::vector<int> pending = indeg;
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const int u = queue[head];
        for (const auto& [w, len] : fout[static_cast<std::size_t>(u)]) {
            vrank[static_cast<std::size_t>(w)] = std::max(
                vrank[static_cast<std::size_t>(w)], vrank[static_cast<std::size_t>(u)] + len);
            if (--pending[static_cast<std::size_t>(w)] == 0) queue.push_back(w);
        }
    }
    const std::vector<int> rank(vrank.begin(), vrank.begin() + n);
    int max_rank = 0;
    for (int i = 0; i < n; ++i) max_rank = std::max(max_rank, rank[static_cast<std::size_t>(i)]);

    // --- 4. Layout graph: real nodes + virtual nodes on long edges. An
    // end that names a cluster routes to that cluster's rank-extreme
    // member — deepest into the flow when the cluster is the source,
    // shallowest when it is the target — and the route is cut back to the
    // frame in step 10. ---
    const auto rank_extreme = [&](int c, bool deepest) {
        const std::vector<int>& members = tree.members[static_cast<std::size_t>(c)];
        int best = members.front();  // members are non-empty here, ascending
        for (const int candidate : members)
            if (deepest ? rank[static_cast<std::size_t>(candidate)] >
                              rank[static_cast<std::size_t>(best)]
                        : rank[static_cast<std::size_t>(candidate)] <
                              rank[static_cast<std::size_t>(best)])
                best = candidate;
        return best;
    };
    // The cluster an END lives in: a node's own innermost one, but the
    // PARENT of a cluster end — an edge to a frame runs outside it.
    const auto enclosing = [&](DagEnd end) {
        if (end.is_cluster) return tree.parent[static_cast<std::size_t>(end.index)];
        const std::size_t i = static_cast<std::size_t>(end.index);
        const int innermost = i < graph.node_cluster.size() ? graph.node_cluster[i] : -1;
        return tree.valid(innermost) ? innermost : -1;
    };

    std::vector<int> lrank(rank.begin(), rank.end());  // rank per layout node
    std::vector<int> lcluster(static_cast<std::size_t>(n), -1);
    for (int i = 0; i < n; ++i) lcluster[static_cast<std::size_t>(i)] = enclosing(DagEnd(i));
    std::vector<std::vector<int>> chain(static_cast<std::size_t>(m));
    std::vector<char> drawn(static_cast<std::size_t>(m), 0);
    for (int k = 0; k < m; ++k) {
        const int arc = arc_of_edge[static_cast<std::size_t>(k)];
        if (arc < 0) continue;  // degenerate: no chain, empty route
        const DagEdge& e = graph.edges[static_cast<std::size_t>(k)];
        const bool flipped = reversed[static_cast<std::size_t>(arc)] != 0;
        const DagEnd tail = flipped ? e.to : e.from;
        const DagEnd head = flipped ? e.from : e.to;
        const int a = tail.is_cluster ? rank_extreme(tail.index, /*deepest=*/true) : tail.index;
        const int b = head.is_cluster ? rank_extreme(head.index, /*deepest=*/false) : head.index;
        if (a == b) continue;  // a one-member cluster edged to its own member
        drawn[static_cast<std::size_t>(k)] = 1;
        // Routing points belong to the innermost cluster enclosing BOTH
        // ends, so a long link inside a subgraph stays inside its frame
        // and one leaving it does not drag the frame along.
        const int through = tree.common(enclosing(tail), enclosing(head));
        std::vector<int>& c = chain[static_cast<std::size_t>(k)];
        c.push_back(a);
        for (int r = rank[static_cast<std::size_t>(a)] + 1;
             r < rank[static_cast<std::size_t>(b)]; ++r) {
            if (lrank.size() >= 16384)
                throw Error(cworks::validation_failed("graph exceeds 16384 routing points"));
            c.push_back(static_cast<int>(lrank.size()));
            lrank.push_back(r);
            lcluster.push_back(through);
        }
        c.push_back(b);
    }
    const int N = static_cast<int>(lrank.size());

    // Adjacency between consecutive ranks (for ordering + coordinates).
    std::vector<std::vector<int>> up(static_cast<std::size_t>(N)),
        down(static_cast<std::size_t>(N));
    for (int k = 0; k < m; ++k)
        for (std::size_t i = 0; i + 1 < chain[static_cast<std::size_t>(k)].size(); ++i) {
            const int a = chain[static_cast<std::size_t>(k)][i];
            const int b = chain[static_cast<std::size_t>(k)][i + 1];
            down[static_cast<std::size_t>(a)].push_back(b);
            up[static_cast<std::size_t>(b)].push_back(a);
        }

    // Rank buckets, initial order = creation order.
    std::vector<std::vector<int>> layers(static_cast<std::size_t>(max_rank + 1));
    for (int i = 0; i < N; ++i)
        layers[static_cast<std::size_t>(lrank[static_cast<std::size_t>(i)])].push_back(i);

    // --- 5. Ordering: barycenter sweeps, keeping the fewest crossings.
    // Every candidate order is projected back onto the cluster constraint
    // before it is scored, so the winner is always one a frame can
    // actually enclose. ---
    std::vector<int> order(static_cast<std::size_t>(N), 0);
    const auto reindex = [&] {
        for (const auto& layer : layers)
            for (std::size_t i = 0; i < layer.size(); ++i)
                order[static_cast<std::size_t>(layer[i])] = static_cast<int>(i);
    };
    const auto group_clusters = [&] {
        if (cn == 0) return;
        for (std::vector<int>& layer : layers) layer = arrange(tree, lcluster, layer, -1);
    };
    group_clusters();
    reindex();
    const auto crossings = [&]() -> long long {
        long long total = 0;
        for (int r = 0; r + 1 <= max_rank; ++r) {
            std::vector<std::pair<int, int>> es;
            for (const int u : layers[static_cast<std::size_t>(r)])
                for (const int v : down[static_cast<std::size_t>(u)])
                    es.push_back({order[static_cast<std::size_t>(u)],
                                  order[static_cast<std::size_t>(v)]});
            std::sort(es.begin(), es.end());
            std::vector<int> seq;
            for (const auto& e : es) seq.push_back(e.second);
            total += inversions(seq);
        }
        return total;
    };
    const auto sort_layer = [&](int r, bool use_up) {
        std::vector<int>& layer = layers[static_cast<std::size_t>(r)];
        std::vector<std::pair<double, int>> keyed;
        for (const int node : layer) {
            const std::vector<int>& nb = use_up ? up[static_cast<std::size_t>(node)]
                                                : down[static_cast<std::size_t>(node)];
            double key = order[static_cast<std::size_t>(node)];  // no neighbors → keep place
            if (!nb.empty()) {
                double sum = 0.0;
                for (const int x : nb) sum += order[static_cast<std::size_t>(x)];
                key = sum / static_cast<double>(nb.size());
            }
            keyed.push_back({key, node});
        }
        std::stable_sort(keyed.begin(), keyed.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < layer.size(); ++i) layer[i] = keyed[i].second;
    };
    std::vector<std::vector<int>> best = layers;
    long long best_cross = crossings();
    for (int iter = 0; iter < 8; ++iter) {
        if (iter % 2 == 0)
            for (int r = 1; r <= max_rank; ++r) sort_layer(r, /*use_up=*/true);
        else
            for (int r = max_rank - 1; r >= 0; --r) sort_layer(r, /*use_up=*/false);
        group_clusters();
        reindex();
        const long long c = crossings();
        if (c < best_cross) {
            best_cross = c;
            best = layers;
        }
    }
    layers = best;
    reindex();

    // --- 6. Coordinates. Both axes reserve each cluster's frame margin:
    // across a rank as extra separation wherever a frame closes or opens
    // between two neighbours, and along the ranks as an extra gap where a
    // cluster's first or last rank meets the rank outside it. ---
    std::vector<double> half(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
        half[static_cast<std::size_t>(i)] =
            (i < n ? breadth_of(graph.nodes[static_cast<std::size_t>(i)], dir) : kVirtualExtent) /
            2.0;
    std::vector<int> first_rank(static_cast<std::size_t>(cn), -1);
    std::vector<int> last_rank(static_cast<std::size_t>(cn), -1);
    for (int c = 0; c < cn; ++c)
        for (const int member : tree.members[static_cast<std::size_t>(c)]) {
            const int r = rank[static_cast<std::size_t>(member)];
            const std::size_t u = static_cast<std::size_t>(c);
            first_rank[u] = first_rank[u] < 0 ? r : std::min(first_rank[u], r);
            last_rank[u] = last_rank[u] < 0 ? r : std::max(last_rank[u], r);
        }

    const auto assign_breadth = [&](const std::vector<AxisInsets>& inset) {
        // Separation between rank neighbours: the base gap, plus every
        // frame that has to close after the left one and every frame that
        // has to open before the right one.
        std::vector<std::vector<double>> sep(static_cast<std::size_t>(max_rank + 1));
        for (int r = 0; r <= max_rank; ++r) {
            const std::vector<int>& layer = layers[static_cast<std::size_t>(r)];
            std::vector<double>& row = sep[static_cast<std::size_t>(r)];
            row.assign(layer.size(), params.node_sep);
            for (std::size_t i = 1; i < layer.size(); ++i) {
                const int lo = lcluster[static_cast<std::size_t>(layer[i - 1])];
                const int hi = lcluster[static_cast<std::size_t>(layer[i])];
                if (lo == hi) continue;
                for (int c = 0; c < cn; ++c) {
                    const bool in_lo = tree.covers(c, lo);
                    const bool in_hi = tree.covers(c, hi);
                    if (in_lo && !in_hi) row[i] += inset[static_cast<std::size_t>(c)].breadth_hi;
                    else if (in_hi && !in_lo) row[i] += inset[static_cast<std::size_t>(c)].breadth_lo;
                }
            }
        }
        std::vector<double> pos(static_cast<std::size_t>(N), 0.0);
        for (int r = 0; r <= max_rank; ++r) {
            const std::vector<int>& layer = layers[static_cast<std::size_t>(r)];
            double x = 0.0;
            for (std::size_t i = 0; i < layer.size(); ++i) {
                const int node = layer[i];
                if (i == 0)
                    x = half[static_cast<std::size_t>(node)];
                else
                    x += sep[static_cast<std::size_t>(r)][i] + half[static_cast<std::size_t>(node)];
                pos[static_cast<std::size_t>(node)] = x;
                x += half[static_cast<std::size_t>(node)];
            }
        }
        // Relaxation: pull each node toward its neighbours' mean, clamped so
        // it keeps order and separation from its rank siblings. Deterministic.
        for (int iter = 0; iter < 6; ++iter) {
            for (int r = 0; r <= max_rank; ++r) {
                const std::vector<int>& layer = layers[static_cast<std::size_t>(r)];
                const std::vector<double>& row = sep[static_cast<std::size_t>(r)];
                for (std::size_t i = 0; i < layer.size(); ++i) {
                    const int node = layer[i];
                    double sum = 0.0;
                    int count = 0;
                    for (const int x : up[static_cast<std::size_t>(node)]) {
                        sum += pos[static_cast<std::size_t>(x)];
                        ++count;
                    }
                    for (const int x : down[static_cast<std::size_t>(node)]) {
                        sum += pos[static_cast<std::size_t>(x)];
                        ++count;
                    }
                    if (count == 0) continue;
                    const double desired = sum / count;
                    const double lo =
                        i == 0 ? -std::numeric_limits<double>::infinity()
                               : pos[static_cast<std::size_t>(layer[i - 1])] +
                                     half[static_cast<std::size_t>(layer[i - 1])] + row[i] +
                                     half[static_cast<std::size_t>(node)];
                    const double hi =
                        i + 1 == layer.size()
                            ? std::numeric_limits<double>::infinity()
                            : pos[static_cast<std::size_t>(layer[i + 1])] -
                                  half[static_cast<std::size_t>(layer[i + 1])] - row[i + 1] -
                                  half[static_cast<std::size_t>(node)];
                    if (lo <= hi) pos[static_cast<std::size_t>(node)] = std::clamp(desired, lo, hi);
                }
            }
        }
        return pos;
    };

    // Per-rank thickness (real nodes only — routing points are not boxes).
    std::vector<double> thick(static_cast<std::size_t>(max_rank + 1), 0.0);
    for (int i = 0; i < n; ++i) {
        const std::size_t r = static_cast<std::size_t>(rank[static_cast<std::size_t>(i)]);
        thick[r] = std::max(thick[r], depth_of(graph.nodes[static_cast<std::size_t>(i)], dir));
    }
    const auto assign_depth = [&](const std::vector<AxisInsets>& inset) {
        // A frame that opens where its parent opens needs room for both
        // bands, so the reservation accumulates down the nesting chain.
        std::vector<double> opens(static_cast<std::size_t>(cn), 0.0);
        std::vector<double> closes(static_cast<std::size_t>(cn), 0.0);
        std::vector<double> open_at(static_cast<std::size_t>(max_rank + 1), 0.0);
        std::vector<double> close_at(static_cast<std::size_t>(max_rank + 1), 0.0);
        for (int c = 0; c < cn; ++c) {
            const std::size_t u = static_cast<std::size_t>(c);
            if (first_rank[u] < 0) continue;
            const int p = tree.parent[u];
            const std::size_t pu = static_cast<std::size_t>(p);
            opens[u] = inset[u].depth_lo +
                       (p >= 0 && first_rank[pu] == first_rank[u] ? opens[pu] : 0.0);
            closes[u] = inset[u].depth_hi +
                        (p >= 0 && last_rank[pu] == last_rank[u] ? closes[pu] : 0.0);
            open_at[static_cast<std::size_t>(first_rank[u])] =
                std::max(open_at[static_cast<std::size_t>(first_rank[u])], opens[u]);
            close_at[static_cast<std::size_t>(last_rank[u])] =
                std::max(close_at[static_cast<std::size_t>(last_rank[u])], closes[u]);
        }
        std::vector<double> depth(static_cast<std::size_t>(max_rank + 1), 0.0);
        depth[0] = thick[0] / 2.0;
        for (int r = 1; r <= max_rank; ++r) {
            const std::size_t u = static_cast<std::size_t>(r);
            const double extra = close_at[u - 1] + open_at[u];
            depth[u] = depth[u - 1] + thick[u - 1] / 2.0 + params.rank_sep + extra + thick[u] / 2.0;
        }
        return depth;
    };

    // --- 7. Cluster extents: the members, the routing points that belong
    // to the cluster, and the nested frames, grown by the reserved margin.
    // Children carry higher indices than their parents, so one descending
    // pass has every child finished before its parent needs it. ---
    std::vector<std::vector<int>> route_nodes(static_cast<std::size_t>(cn));
    for (int i = n; i < N; ++i)
        for (int c = lcluster[static_cast<std::size_t>(i)]; c >= 0;
             c = tree.parent[static_cast<std::size_t>(c)])
            route_nodes[static_cast<std::size_t>(c)].push_back(i);
    const auto cluster_spans = [&](const std::vector<double>& breadth_pos,
                                   const std::vector<double>& rank_depth,
                                   const std::vector<AxisInsets>& inset,
                                   std::vector<double>* breadth_short,
                                   std::vector<double>* depth_short) {
        std::vector<Span> spans(static_cast<std::size_t>(cn));
        for (int c = cn - 1; c >= 0; --c) {
            const std::size_t u = static_cast<std::size_t>(c);
            Span span;
            for (const int i : tree.members[u]) {
                const std::size_t v = static_cast<std::size_t>(i);
                const double d = rank_depth[static_cast<std::size_t>(rank[v])];
                const double dh = depth_of(graph.nodes[v], dir) / 2.0;
                span.add(breadth_pos[v] - half[v], breadth_pos[v] + half[v], d - dh, d + dh);
            }
            for (const int i : route_nodes[u]) {
                const std::size_t v = static_cast<std::size_t>(i);
                const double d = rank_depth[static_cast<std::size_t>(lrank[v])];
                span.add(breadth_pos[v] - half[v], breadth_pos[v] + half[v], d, d);
            }
            for (const int child : tree.children[u]) span.unite(spans[static_cast<std::size_t>(child)]);
            if (!span.any) continue;
            span.b_lo -= inset[u].breadth_lo;
            span.b_hi += inset[u].breadth_hi;
            span.d_lo -= inset[u].depth_lo;
            span.d_hi += inset[u].depth_hi;
            // How far this frame falls short of the size the caller needs.
            // The reservation pass turns that into margin and lays out
            // again, which is what makes the bigger frame fit its
            // neighbours instead of growing over them.
            if (breadth_short)
                (*breadth_short)[u] = std::max(0.0, tree.min_breadth[u] - (span.b_hi - span.b_lo));
            if (depth_short)
                (*depth_short)[u] = std::max(0.0, tree.min_depth[u] - (span.d_hi - span.d_lo));
            spans[u] = span;
        }
        return spans;
    };

    std::vector<AxisInsets> inset = tree.inset;
    std::vector<double> pos = assign_breadth(inset);
    std::vector<double> depth = assign_depth(inset);
    std::vector<Span> spans;
    if (cn == 0) {
        spans = cluster_spans(pos, depth, inset, nullptr, nullptr);
    } else {
        // A first pass measures how far each frame falls short of the size
        // the caller needs; the second reserves that shortfall as margin,
        // so the frame grows WITH room around it rather than over its
        // neighbours. Separations only ever grow, so one retry suffices.
        std::vector<double> breadth_short(static_cast<std::size_t>(cn), 0.0);
        std::vector<double> depth_short(static_cast<std::size_t>(cn), 0.0);
        spans = cluster_spans(pos, depth, inset, &breadth_short, &depth_short);
        bool grew = false;
        for (int c = 0; c < cn; ++c) {
            const std::size_t u = static_cast<std::size_t>(c);
            if (breadth_short[u] > 0.0) {
                inset[u].breadth_lo += breadth_short[u] / 2.0;
                inset[u].breadth_hi += breadth_short[u] / 2.0;
                grew = true;
            }
            if (depth_short[u] > 0.0) {
                inset[u].depth_lo += depth_short[u] / 2.0;
                inset[u].depth_hi += depth_short[u] / 2.0;
                grew = true;
            }
        }
        if (grew) {
            pos = assign_breadth(inset);
            depth = assign_depth(inset);
            spans = cluster_spans(pos, depth, inset, nullptr, nullptr);
        }
    }

    // Normalise breadth so it starts at the margin origin (0).
    double min_b = std::numeric_limits<double>::infinity();
    double max_b = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < N; ++i) {
        min_b = std::min(min_b, pos[static_cast<std::size_t>(i)] - half[static_cast<std::size_t>(i)]);
        max_b = std::max(max_b, pos[static_cast<std::size_t>(i)] + half[static_cast<std::size_t>(i)]);
    }
    const double breadth_span = max_b - min_b;
    const double depth_span =
        depth[static_cast<std::size_t>(max_rank)] + thick[static_cast<std::size_t>(max_rank)] / 2.0;

    // --- 8. Map (breadth, depth) to (x, y) per direction. ---
    const auto to_screen = [&](double b, double d) -> Point {
        switch (dir) {
        case FlowDir::Down: return {b, d};
        case FlowDir::Up: return {b, depth_span - d};
        case FlowDir::Right: return {d, b};
        case FlowDir::Left: return {depth_span - d, b};
        }
        return {};
    };
    std::vector<Point> lc(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
        lc[static_cast<std::size_t>(i)] =
            to_screen(pos[static_cast<std::size_t>(i)] - min_b,
                      depth[static_cast<std::size_t>(lrank[static_cast<std::size_t>(i)])]);
    for (int c = 0; c < cn; ++c) {
        const Span& span = spans[static_cast<std::size_t>(c)];
        if (!span.any) continue;
        const Point p = to_screen(span.b_lo - min_b, span.d_lo);
        const Point q = to_screen(span.b_hi - min_b, span.d_hi);
        result.cluster_rects[static_cast<std::size_t>(c)] = {
            std::min(p.x, q.x), std::min(p.y, q.y), std::abs(q.x - p.x), std::abs(q.y - p.y)};
    }

    result.centers.assign(lc.begin(), lc.begin() + n);
    result.routes.resize(static_cast<std::size_t>(m));
    for (int k = 0; k < m; ++k) {
        if (!drawn[static_cast<std::size_t>(k)]) continue;
        std::vector<Point> pts;
        for (const int node : chain[static_cast<std::size_t>(k)])
            pts.push_back(lc[static_cast<std::size_t>(node)]);
        // Back to the direction it was written in (step 2 may have flipped it).
        if (reversed[static_cast<std::size_t>(arc_of_edge[static_cast<std::size_t>(k)])])
            std::reverse(pts.begin(), pts.end());
        result.routes[static_cast<std::size_t>(k)] = std::move(pts);
    }

    // --- 9. Separate parallel edges (the same end pair, either direction)
    // so bidirectional links don't draw on top of each other: bend each one
    // to a distinct perpendicular offset. Simple single edges stay
    // straight — only genuinely bent routes (parallel pairs, long edges
    // through virtual nodes) become curves. ---
    const auto end_key = [](DagEnd end) {
        return end.is_cluster ? -(end.index + 1) : end.index;
    };
    const auto end_point = [&](int key) -> Point {
        if (key >= 0) return lc[static_cast<std::size_t>(key)];
        const RectF& r = result.cluster_rects[static_cast<std::size_t>(-key - 1)];
        return {r.x + r.w / 2.0, r.y + r.h / 2.0};
    };
    std::map<std::pair<int, int>, std::vector<int>> parallel;
    for (int k = 0; k < m; ++k) {
        if (!drawn[static_cast<std::size_t>(k)]) continue;
        const int a = end_key(graph.edges[static_cast<std::size_t>(k)].from);
        const int b = end_key(graph.edges[static_cast<std::size_t>(k)].to);
        parallel[{std::min(a, b), std::max(a, b)}].push_back(k);
    }
    for (const auto& [pair, group] : parallel) {
        if (group.size() < 2) continue;
        const double sep = 26.0;
        // One canonical perpendicular for the whole end pair, so an edge
        // and its reverse (a bidirectional pair) land on OPPOSITE sides —
        // not the same side, which happened when each used its own
        // (reversed) direction.
        const Point ca = end_point(pair.first), cb = end_point(pair.second);
        const double cdx = cb.x - ca.x, cdy = cb.y - ca.y;
        const double clen = std::hypot(cdx, cdy);
        if (clen < 1e-9) continue;
        const double px = -cdy / clen, py = cdx / clen;
        for (std::size_t j = 0; j < group.size(); ++j) {
            std::vector<Point>& route = result.routes[static_cast<std::size_t>(group[j])];
            if (route.size() < 2) continue;
            const double off =
                (static_cast<double>(j) - static_cast<double>(group.size() - 1) / 2.0) * sep;
            if (off == 0.0) continue;
            // Bow the edge to its own side with a single mid control point,
            // so a bidirectional pair draws as two clearly separated,
            // elegant curves. Endpoints stay on the node-centre line so
            // each still lands cleanly on the node border; smooth() below
            // turns the 3-point route into an arc.
            const Point s = route.front(), e = route.back();
            const Point mid{(s.x + e.x) / 2.0 + px * off, (s.y + e.y) / 2.0 + py * off};
            route = {s, mid, e};
        }
    }

    // Only genuinely routed edges (through virtual nodes) curve; adjacent
    // and parallel edges stay straight.
    for (std::vector<Point>& route : result.routes)
        if (route.size() >= 3) route = smooth(route);

    // --- 10. Fit the drawing around the frames, then land the edges that
    // name a cluster on the frame instead of on the member they routed to. ---
    double min_x = 0.0, min_y = 0.0;
    double max_x = dir == FlowDir::Down || dir == FlowDir::Up ? breadth_span : depth_span;
    double max_y = dir == FlowDir::Down || dir == FlowDir::Up ? depth_span : breadth_span;
    for (const RectF& r : result.cluster_rects) {
        if (r.w <= 0.0 && r.h <= 0.0) continue;
        min_x = std::min(min_x, r.x);
        min_y = std::min(min_y, r.y);
        max_x = std::max(max_x, r.x + r.w);
        max_y = std::max(max_y, r.y + r.h);
    }
    if (min_x != 0.0 || min_y != 0.0) {
        for (Point& p : result.centers) { p.x -= min_x; p.y -= min_y; }
        for (std::vector<Point>& route : result.routes)
            for (Point& p : route) { p.x -= min_x; p.y -= min_y; }
        for (RectF& r : result.cluster_rects) { r.x -= min_x; r.y -= min_y; }
    }
    result.width = max_x - min_x;
    result.height = max_y - min_y;

    for (int k = 0; k < m; ++k) {
        const DagEdge& e = graph.edges[static_cast<std::size_t>(k)];
        if (!drawn[static_cast<std::size_t>(k)] || (!e.from.is_cluster && !e.to.is_cluster))
            continue;
        std::vector<Point>& route = result.routes[static_cast<std::size_t>(k)];
        if (e.to.is_cluster)
            route = clip_before_rect(std::move(route),
                                     result.cluster_rects[static_cast<std::size_t>(e.to.index)]);
        if (e.from.is_cluster) {
            std::reverse(route.begin(), route.end());
            route = clip_before_rect(std::move(route),
                                     result.cluster_rects[static_cast<std::size_t>(e.from.index)]);
            std::reverse(route.begin(), route.end());
        }
    }
    return result;
}

} // namespace cdiagram::detail
