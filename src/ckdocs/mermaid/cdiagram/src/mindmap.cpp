// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Mindmap (`mindmap`): an indentation-defined idea tree, laid out
// left-to-right with the layered engine and coloured per top-level
// branch. Deterministic.

#include <algorithm>
#include <string>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "canvas.hpp"
#include "cdiagram/error.hpp"
#include "dag_layout.hpp"
#include "diagnostics.hpp"
#include "links.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram::detail {

namespace {

enum class MShape { Round, Circle, Rect, Hexagon, Cloud, Bang };

struct MNode {
    std::string label;
    MShape shape = MShape::Round;
    int parent = -1;
    int depth = 0;
    int branch = -1;  // top-level subtree index (for colouring)
    DagNodeSize size;
};

/// Parsed mindmap: the idea tree plus the hyperlink table. A `click`
/// directive keys a link on the node's label text, looked up at draw time.
struct MModel {
    std::vector<MNode> nodes;
    LinkTable links;  ///< node label -> hyperlink (click …)
};

/// A node's label and shape. Mermaid mindmap nodes are written
/// `id((label))`, `id[label]`, `id(label)` or plain `label` — the shape
/// wrapper follows an optional id prefix and ends the string.
std::string node_label(const std::string& raw, MShape& shape) {
    const std::string s = cworks::trim(raw);
    const auto ends = [&](const char* c) {
        const std::string close = c;
        return s.size() >= close.size() &&
               s.compare(s.size() - close.size(), close.size(), close) == 0;
    };
    const auto between = [&](const char* open, std::size_t close_len) -> std::string {
        const std::size_t p = s.find(open);
        const std::size_t open_len = std::string(open).size();
        if (p == std::string::npos || p + open_len > s.size() - close_len) return s;
        return cworks::trim(s.substr(p + open_len, s.size() - close_len - p - open_len));
    };
    if (ends("))")) {
        shape = MShape::Circle;
        return between("((", 2);
    }
    if (ends("}}")) {
        shape = MShape::Hexagon;
        return between("{{", 2);
    }
    if (ends("((")) {  // bang: id))text((
        shape = MShape::Bang;
        return between("))", 2);
    }
    if (ends("]")) {
        shape = MShape::Rect;
        return between("[", 1);
    }
    if (ends(")")) {
        shape = MShape::Round;
        return between("(", 1);
    }
    if (ends("(")) {  // cloud: id)text(
        shape = MShape::Cloud;
        return between(")", 1);
    }
    shape = MShape::Round;
    return s;
}

MModel parse_mindmap(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> kKnown = {"::icon", ":::", "click", "mindmap"};
    std::vector<MNode> nodes;
    LinkTable links;
    std::vector<std::pair<std::size_t, int>> stack;  // (indent, node index)
    int branch_counter = 0;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const SourceLine& sl = lines[li];
        const std::size_t number = sl.number;
        // Interaction: `click <label> "url" ["tip"] [_target]` attaches a
        // hyperlink to the node whose text matches <label>. A JS callback
        // form carries no URL and cannot be exported, so it is reported, not
        // linked.
        if (is_click_statement(sl.text)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(sl.text, ref, link) && !link.callback) {
                if (!links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("mindmap", number,
                                     "'" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "mindmap", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        // A leading colon marks a mindmap decorator, not a node: `::icon(fa
        // fa-book)` attaches an icon to the preceding node and `:::className`
        // assigns it a CSS class. Neither is rendered here — but they must
        // NOT fabricate a bogus node box labelled with the raw decorator
        // text (the silent misparse this fix removes). Report and skip.
        if (sl.text.rfind("::icon", 0) == 0) {
            diagnose_unsupported(options, "mindmap", number, "::icon",
                                 "node icons are not rendered");
            continue;
        }
        if (sl.text.rfind(":::", 0) == 0) {
            diagnose_unsupported(options, "mindmap", number, ":::",
                                 "class assignments are not rendered");
            continue;
        }
        if (!sl.text.empty() && sl.text.front() == ':') {
            const std::string token = sl.text.substr(0, sl.text.find_first_of(" \t("));
            diagnose_unrecognized(options, "mindmap", number, token, kKnown);
            continue;
        }
        MNode node;
        std::string text = sl.text;
        // An inline `:::class` suffix is a Mermaid class assignment; the
        // class is not rendered (see the ::: guard above) and must never
        // leak into the node label.
        if (const std::size_t mark = text.find(":::"); mark != std::string::npos) {
            diagnose_unsupported(options, "mindmap", number, cworks::trim(text.substr(mark)),
                                 "class assignments are not rendered");
            text = cworks::trim(text.substr(0, mark));
        }
        node.label = node_label(text, node.shape);
        if (node.shape == MShape::Cloud || node.shape == MShape::Bang)
            diagnose_unsupported(options, "mindmap", number,
                                 node.shape == MShape::Cloud ? "cloud shape" : "bang shape",
                                 "drawn as a rounded node for now");
        while (!stack.empty() && stack.back().first >= sl.indent) stack.pop_back();
        node.parent = stack.empty() ? -1 : stack.back().second;
        node.depth = static_cast<int>(stack.size());
        if (node.depth == 0)
            node.branch = -1;  // root
        else if (node.depth == 1)
            node.branch = branch_counter++;
        else
            node.branch = nodes[static_cast<std::size_t>(node.parent)].branch;
        const int index = static_cast<int>(nodes.size());
        nodes.push_back(std::move(node));
        stack.push_back({sl.indent, index});
    }
    if (nodes.empty()) throw Error(cworks::validation_failed("mindmap: no nodes"));
    return MModel{std::move(nodes), std::move(links)};
}

} // namespace

cplot::Scene build_mindmap(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    MModel model = parse_mindmap(source, options);
    std::vector<MNode>& nodes = model.nodes;
    const Font font = theme.base_font();
    const double line_h = font.size * 1.35;

    std::vector<DagNodeSize> sizes(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const double w = text_width(nodes[i].label, font) + 24.0;
        const double h = line_h + 14.0;
        nodes[i].size = {w, h};
        sizes[i] = nodes[i].size;
    }
    std::vector<DagEdge> edges;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].parent >= 0) edges.push_back({nodes[i].parent, static_cast<int>(i)});

    DagParams params;
    params.dir = FlowDir::Right;
    params.node_sep = 16.0;
    const DagResult layout = layout_dag(DagGraph{sizes, edges, {}, {}}, params);

    const double margin = 22.0;
    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = layout.width + 2.0 * margin;
    canvas.height = layout.height + 2.0 * margin;
    const auto shift = [&](Point p) { return Point{p.x + margin, p.y + margin}; };

    const auto branch_color = [&](int branch) {
        return branch < 0 ? sty.node_stroke
                          : sty.series(static_cast<std::size_t>(branch));
    };

    // Land a branch exactly on each node's outline (root circle, rounded
    // pill, or rectangle) so the connector touches the bubble precisely —
    // the bounding box would poke into round nodes on one axis and fall
    // short on the other.
    const auto mnode_border = [&](const MNode& node, Point center, DagNodeSize s,
                                  Point toward) -> Point {
        switch (node.shape) {
        case MShape::Circle:
            return circle_border(center, std::max(s.w, s.h) / 2.0, toward);
        case MShape::Rect:
            return box_border(center, s.w, s.h, toward);
        default:
            return stadium_border(center, s.w, s.h, toward);
        }
    };

    // Branches (behind nodes): a line from each child to its parent.
    for (std::size_t k = 0; k < edges.size(); ++k) {
        std::vector<Point> route = layout.routes[k];
        if (route.size() < 2) continue;
        for (Point& p : route) p = shift(p);
        const int parent = edges[k].from.index;
        const int child = edges[k].to.index;
        route.front() = mnode_border(nodes[static_cast<std::size_t>(parent)],
                                     shift(layout.centers[static_cast<std::size_t>(parent)]),
                                     sizes[static_cast<std::size_t>(parent)], route[1]);
        route.back() = mnode_border(nodes[static_cast<std::size_t>(child)],
                                    shift(layout.centers[static_cast<std::size_t>(child)]),
                                    sizes[static_cast<std::size_t>(child)],
                                    route[route.size() - 2]);
        ShapeStyle ls;
        ls.stroke = branch_color(nodes[static_cast<std::size_t>(child)].branch);
        ls.stroke_width = 1.6;
        ls.cap = cplot::LineCap::Round;
        ls.join = cplot::LineJoin::Round;
        canvas.polyline(route, ls);
    }

    // Nodes.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const MNode& node = nodes[i];
        const Point c = shift(layout.centers[i]);
        const Color col = branch_color(node.branch);
        ShapeStyle s;
        s.fill = col.with_alpha(node.depth == 0 ? 0.28 : 0.14);
        s.stroke = col;
        s.stroke_width = node.depth == 0 ? 1.8 : 1.2;
        const RectF box{c.x - node.size.w / 2.0, c.y - node.size.h / 2.0, node.size.w,
                        node.size.h};
        // Where this node was drawn, so it can be selected. The box is the
        // hit area for every shape, including the circle: a reader aiming at
        // a node means the node, not the difference between its silhouette
        // and the rectangle around it.
        if (options.regions != nullptr)
            options.regions->push_back({"#" + std::to_string(i), 0 /* HitRole::Node */, box});
        if (node.shape == MShape::Circle)
            canvas.circle(c, std::max(node.size.w, node.size.h) / 2.0, s);
        else if (node.shape == MShape::Rect)
            canvas.rect(box, s);
        else if (node.shape == MShape::Hexagon) {
            const double k = std::min(12.0, box.w / 4.0);
            canvas.polygon({{box.x + k, box.y}, {box.x + box.w - k, box.y},
                            {box.x + box.w, c.y}, {box.x + box.w - k, box.y + box.h},
                            {box.x + k, box.y + box.h}, {box.x, c.y}},
                           s);
        } else
            // Cloud and bang render as rounded pills for now (their labels
            // parse correctly; the shape difference is reported below).
            canvas.rounded_rect(box, node.size.h / 2.0, s);
        canvas.text(c, node.label,
                    node.depth == 0 ? font.with_weight(cplot::FontWeight::Bold) : font,
                    sty.text, HAlign::Center, VAlign::Middle);
        if (const auto it = model.links.find(node.label); it != model.links.end())
            canvas.add_link(box, it->second.href, it->second.title, it->second.target);
    }

    return canvas.bake();
}

} // namespace cdiagram::detail
