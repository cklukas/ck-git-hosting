// ckdiagram — flowchart + layered-layout tests
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include <cworks/microtest.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <variant>

#include <cplot/svg.hpp>

#include "cdiagram/error.hpp"
#include "cdiagram/render.hpp"

using cdiagram::render;

namespace {

template <class Item>
std::size_t count(const cplot::Scene& scene) {
    std::size_t n = 0;
    for (const cplot::PlacedItem& placed : cplot::collect_items(scene.root))
        if (std::holds_alternative<Item>(*placed.item)) ++n;
    return n;
}

// A diagram bakes flat into one group with no transform (Canvas::bake), so
// an item's own coordinates are the scene's.

/// Where a label sits, by its text. NaN when the diagram has no such label.
cplot::Point label_pos(const cplot::Scene& scene, std::string_view text) {
    for (const cplot::PlacedItem& placed : cplot::collect_items(scene.root))
        if (const auto* item = std::get_if<cplot::TextItem>(&*placed.item))
            if (item->text == text) return item->pos;
    return {std::nan(""), std::nan("")};
}

/// The bounding box of a path. A rounded rectangle's corner curves stay
/// inside the box its straight edges span, so the segment endpoints alone
/// give the exact frame.
cplot::RectF path_bounds(const cplot::PathItem& path) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    const auto note = [&](cplot::Point p) {
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
    };
    for (const cplot::SubPath& sub : path.subpaths) {
        note(sub.start);
        for (const cplot::PathSegment& seg : sub.segments) note(seg.to);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

} // namespace

TEST_CASE("flowchart lays out nodes and directed edges") {
    const std::string src =
        "flowchart TD\n"
        "  A[Start] --> B{OK?}\n"
        "  B -->|yes| C[Go]\n"
        "  B -->|no| A\n";
    const cplot::Scene scene = render(src);
    REQUIRE(!scene.root.children.empty());
    CHECK(scene.width > 0.0);
    CHECK(scene.height > 0.0);
    // 3 edges → at least 3 polylines; a diamond + arrowheads are polygons.
    CHECK(count<cplot::PolylineItem>(scene) >= 3u);
    CHECK(count<cplot::PolygonItem>(scene) >= 3u);
    CHECK(count<cplot::RectItem>(scene) >= 2u);  // Start + Go rects
    // node labels + edge labels
    CHECK(count<cplot::TextItem>(scene) >= 5u);
}

TEST_CASE("flowchart output is deterministic") {
    const std::string src =
        "flowchart TD\n  A --> B\n  A --> C\n  B --> D\n  C --> D\n";
    const std::string a = cplot::SvgRenderer().render(render(src));
    const std::string b = cplot::SvgRenderer().render(render(src));
    CHECK_EQ(a, b);
    CHECK(a.find("<svg") != std::string::npos);
}

TEST_CASE("flowchart direction changes the aspect") {
    const std::string chain = "  A --> B --> C --> D\n";
    const cplot::Scene td = render("flowchart TD\n" + chain);
    const cplot::Scene lr = render("flowchart LR\n" + chain);
    // A vertical chain is taller than wide; a horizontal chain the reverse.
    CHECK(td.height > td.width);
    CHECK(lr.width > lr.height);
}

TEST_CASE("flowchart shapes map to primitives") {
    // circle (()), diamond {} -> a circle item and polygons.
    const cplot::Scene scene = render("flowchart TD\n  A((C)) --> B{D}\n");
    CHECK(count<cplot::CircleItem>(scene) >= 1u);
    CHECK(count<cplot::PolygonItem>(scene) >= 1u);
}

TEST_CASE("flowchart handles cycles without looping forever") {
    // A back edge must be broken by the layout, not hang.
    const cplot::Scene scene = render("flowchart TD\n  A --> B\n  B --> C\n  C --> A\n");
    CHECK(count<cplot::PolylineItem>(scene) >= 3u);
}

TEST_CASE("flowchart chained edges create one edge per hop") {
    const cplot::Scene one = render("flowchart LR\n  A --> B\n");
    const cplot::Scene three = render("flowchart LR\n  A --> B --> C --> D\n");
    // More hops → more polylines.
    CHECK(count<cplot::PolylineItem>(three) > count<cplot::PolylineItem>(one));
}

TEST_CASE("flowchart reports errors") {
    CHECK_THROWS_AS(render("flowchart TD\n  A[Start --> B\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("flowchart TD\n"), cdiagram::Error);  // no nodes
}

TEST_CASE("flowchart per-element styling recolors nodes (C2)") {
    const std::string src =
        "flowchart TD\n"
        "  classDef hot fill:#ff0000,stroke:#990000\n"
        "  A[Start] --> B[Stop]\n"
        "  class A hot\n"
        "  style B fill:#0000ff\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find("#FF0000") != std::string::npos); // classDef fill on A
    CHECK(svg.find("#0000FF") != std::string::npos); // direct style on B
    CHECK_EQ(svg, cplot::SvgRenderer().render(render(src)));
}

TEST_CASE("flowchart inline class and linkStyle apply (C2)") {
    const std::string src =
        "flowchart LR\n"
        "  classDef hot fill:#00ff00\n"
        "  A:::hot --> B\n"
        "  linkStyle 0 stroke:#ff8800\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find("#00FF00") != std::string::npos); // inline class fill
    CHECK(svg.find("#FF8800") != std::string::npos); // link recolor
}

TEST_CASE("flowchart subgraph draws a labeled boundary (C3)") {
    const cplot::Scene plain = render("flowchart TD\n  A --> B\n  B --> C\n");
    const std::string grouped =
        "flowchart TD\n"
        "  subgraph Group One\n"
        "    A --> B\n"
        "  end\n"
        "  B --> C\n";
    const cplot::Scene g = render(grouped);
    // The frame is a rounded-rect path behind the graph.
    CHECK(count<cplot::PathItem>(g) > count<cplot::PathItem>(plain));
    const std::string svg = cplot::SvgRenderer().render(g);
    CHECK(svg.find(">Group One<") != std::string::npos);
    CHECK_EQ(svg, cplot::SvgRenderer().render(render(grouped)));
}

TEST_CASE("flowchart subgraph groups its members in the layout, not after it (C3)") {
    // X fans out to three nodes; two of them are one subgraph. Without a
    // grouping CONSTRAINT they lay out interleaved and the frame — a
    // bounding box drawn afterwards — swallows the outsider. The frame's
    // fill covers exactly the members' span, so the outsider's label has
    // to sit beyond it.
    const std::string src =
        "flowchart TD\n"
        "  X --> A\n"
        "  X --> M\n"
        "  X --> B\n"
        "  subgraph Grouped\n"
        "    A\n"
        "    B\n"
        "  end\n";
    const cplot::Scene scene = render(src);
    // The three fan-out targets share a rank; A and B must be adjacent in
    // it, so M is either first or last — never between them.
    const double ax = label_pos(scene, "A").x;
    const double bx = label_pos(scene, "B").x;
    const double mx = label_pos(scene, "M").x;
    CHECK((mx < std::min(ax, bx) || mx > std::max(ax, bx)));
    // …and the frame that encloses A and B leaves M outside it.
    cplot::RectF frame{};
    for (const cplot::PlacedItem& placed : cplot::collect_items(scene.root))
        if (const auto* path = std::get_if<cplot::PathItem>(&*placed.item))
            frame = path_bounds(*path);
    REQUIRE(frame.w > 0.0);
    CHECK(ax > frame.x);
    CHECK(ax < frame.x + frame.w);
    CHECK(bx > frame.x);
    CHECK(bx < frame.x + frame.w);
    CHECK((mx < frame.x || mx > frame.x + frame.w));
    CHECK_EQ(cplot::SvgRenderer().render(scene), cplot::SvgRenderer().render(render(src)));
}

TEST_CASE("flowchart nested subgraphs render nested frames (C3)") {
    const std::string src =
        "flowchart TD\n"
        "  subgraph Outer\n"
        "    subgraph Inner\n"
        "      A --> B\n"
        "    end\n"
        "    B --> C\n"
        "  end\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find(">Outer<") != std::string::npos);
    CHECK(svg.find(">Inner<") != std::string::npos);
}

TEST_CASE("flowchart edge to a subgraph id lands on the frame, no phantom (C3/V1-04)") {
    const std::string src =
        "flowchart TD\n"
        "  subgraph db[Database Layer]\n"
        "    A[Query] --> B[Cache]\n"
        "  end\n"
        "  C[Client] --> db\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    // The subgraph frame title renders, but no spurious `db` node box beside it.
    CHECK(svg.find(">Database Layer<") != std::string::npos);
    CHECK(svg.find(">db<") == std::string::npos);
    CHECK(svg.find(">Client<") != std::string::npos);
    // The arrowhead's tip sits ON the frame's top edge — not on the border
    // of whichever member the layout routed through.
    const cplot::Scene scene = render(src);
    cplot::RectF frame{};
    cplot::Point tip{};
    for (const cplot::PlacedItem& placed : cplot::collect_items(scene.root)) {
        if (const auto* path = std::get_if<cplot::PathItem>(&*placed.item))
            frame = path_bounds(*path);
        // Canvas::arrow_head emits the tip as the triangle's first point.
        if (const auto* poly = std::get_if<cplot::PolygonItem>(&*placed.item))
            if (poly->points.size() == 3) tip = poly->points[0];
    }
    REQUIRE(frame.w > 0.0);
    CHECK(std::abs(tip.y - frame.y) < 1e-6);
    CHECK(tip.x > frame.x);
    CHECK(tip.x < frame.x + frame.w);
    CHECK(tip.y < label_pos(scene, "Query").y);
    // Attaching to a subgraph is supported behaviour now, not a fallback,
    // so it reports nothing.
    CHECK(cdiagram::check(src).empty());
    CHECK_EQ(svg, cplot::SvgRenderer().render(render(src)));  // deterministic
}

TEST_CASE("flowchart edges leave a subgraph id and forward references resolve (C3)") {
    // `api` is used as an endpoint BEFORE it is declared, and on both
    // sides of a link — both forms address the frame, and neither leaves
    // a phantom node behind.
    const std::string src =
        "flowchart LR\n"
        "  C[Client] --> api\n"
        "  subgraph api[API Tier]\n"
        "    R[Router] --> H[Handler]\n"
        "  end\n"
        "  api --> D[(Store)]\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find(">API Tier<") != std::string::npos);
    CHECK(svg.find(">api<") == std::string::npos);
    CHECK(svg.find(">Store<") != std::string::npos);
    CHECK(cdiagram::check(src).empty());
}

TEST_CASE("flowchart reports a subgraph endpoint it cannot attach to (C3)") {
    // An empty subgraph has nothing for the link to reach; the charter
    // forbids dropping it in silence.
    const cworks::Diagnostics d = cdiagram::check("flowchart TD\n"
                                                  "  subgraph hollow[Nothing]\n"
                                                  "  end\n"
                                                  "  A --> hollow\n");
    REQUIRE(!d.empty());
    CHECK(d[0].severity == cworks::Diagnostic::Severity::Warning);
    CHECK(d[0].message.find("subgraph") != std::string::npos);
}

TEST_CASE("check() reports unsupported and malformed flowchart lines (C1)") {
    // `click <id> "url"` now attaches a hyperlink to the node → no diagnostic.
    const cworks::Diagnostics d1 =
        cdiagram::check("flowchart TD\n  A --> B\n  click A \"http://x\"\n");
    CHECK(d1.empty());

    // A JavaScript callback form carries no URL and cannot be exported to a
    // static SVG/PDF → still a warning.
    const cworks::Diagnostics d1b =
        cdiagram::check("flowchart TD\n  A --> B\n  click A call cb()\n");
    REQUIRE(!d1b.empty());
    CHECK(d1b[0].severity == cworks::Diagnostic::Severity::Warning);
    CHECK(d1b[0].message.find("click") != std::string::npos);

    // An unparseable style value is diagnosed, not silently dropped.
    const cworks::Diagnostics d2 =
        cdiagram::check("flowchart TD\n  A --> B\n  style A fill:notacolor\n");
    REQUIRE(!d2.empty());
    CHECK(d2[0].message.find("colour") != std::string::npos);

    // A fully valid styled + subgraph diagram reports nothing.
    CHECK(cdiagram::check("flowchart TD\n"
                          "  subgraph S\n    A --> B\n  end\n"
                          "  classDef c fill:#f00\n  class A c\n")
              .empty());
}

TEST_CASE("flowchart click attaches a hyperlink to the node") {
    // The `click` directive (bare-URL and `href` forms), a tooltip, and a
    // target all become a live SVG anchor over the node's box.
    const std::string svg = cplot::SvgRenderer().render(cdiagram::render(
        "flowchart TD\n  A --> B\n"
        "  click A \"https://example.com/a\" \"Node A\" _blank\n"
        "  click B href \"https://example.com/b\"\n"));
    CHECK(svg.find("xmlns:xlink=\"http://www.w3.org/1999/xlink\"") != std::string::npos);
    CHECK(svg.find("<a xlink:href=\"https://example.com/a\" target=\"_blank\">") !=
          std::string::npos);
    CHECK(svg.find("<title>Node A</title>") != std::string::npos);
    CHECK(svg.find("<a xlink:href=\"https://example.com/b\">") != std::string::npos);

    // A link-free flowchart stays anchor-free and byte-compatible (no xlink).
    const std::string plain =
        cplot::SvgRenderer().render(cdiagram::render("flowchart TD\n  A --> B\n"));
    CHECK(plain.find("<a ") == std::string::npos);
    CHECK(plain.find("xlink") == std::string::npos);
}

TEST_CASE("front-matter link: makes the whole diagram a hyperlink") {
    const std::string svg = cplot::SvgRenderer().render(cdiagram::render(
        "---\nlink: https://example.com/all\nlinkTarget: _blank\n---\n"
        "flowchart LR\n  A --> B\n"));
    // A full-canvas anchor with the overall URL and its target.
    CHECK(svg.find("<a xlink:href=\"https://example.com/all\" target=\"_blank\">") !=
          std::string::npos);
}

TEST_CASE("flowchart renders the full classic shape set with clean labels (S0)") {
    const std::string src =
        "flowchart LR\n"
        "  A[/Lean right/] --> B[\\Lean left\\]\n"
        "  B --> C[/Trapezoid\\]\n"
        "  C --> D[\\Trapezoid alt/]\n"
        "  D --> E>Flag]\n"
        "  E --> F(((Double)))\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    // Labels must not keep their shape delimiters.
    CHECK(svg.find(">Lean right<") != std::string::npos);
    CHECK(svg.find(">Lean left<") != std::string::npos);
    CHECK(svg.find(">Trapezoid<") != std::string::npos);
    CHECK(svg.find(">Trapezoid alt<") != std::string::npos);
    CHECK(svg.find(">Flag<") != std::string::npos);
    CHECK(svg.find(">Double<") != std::string::npos);
    CHECK(svg.find("/Lean") == std::string::npos);
    // These shapes render diagnostic-free.
    CHECK(cdiagram::check(src).empty());
}

TEST_CASE("flowchart consumes legacy ';' terminators, reports unparsed tails (S0)") {
    CHECK(cdiagram::check("flowchart TD\n  A --> B;\n").empty());
    // `& C` node lists are recognised Mermaid the engine does not lay out.
    const cworks::Diagnostics tail = cdiagram::check("flowchart TD\n  A --> B & C\n");
    CHECK(!tail.empty());
}

TEST_CASE("flowchart styling core: default class, multi-:::, rgb(), subgraph style (S1)") {
    const std::string src =
        "flowchart TD\n"
        "  subgraph grp[Group]\n"
        "    A[One]:::hot:::wide --> B[Two]\n"
        "  end\n"
        "  classDef default stroke-width:2.5\n"
        "  classDef hot fill:rgb(255,153,102)\n"
        "  classDef wide stroke:#333\n"
        "  style grp fill:#eef,stroke:#88f\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find("#FF9966") != std::string::npos);  // rgb() classDef fill on A
    CHECK(svg.find("#EEEEFF") != std::string::npos);  // subgraph frame fill
    CHECK(svg.find("#333333") != std::string::npos);  // second ::: class applied
    CHECK(cdiagram::check(src).empty());
}

TEST_CASE("nested subgraph frames stay light enough to read their own titles") {
    // A frame paints over the frames enclosing it, so a per-frame tint
    // accumulates with depth. At the 32% this once used, three levels
    // composited to 69% black and the title's contrast fell to 1.2:1 — a
    // heading nobody can read. The tint is derived from depth and capped, and
    // this is what says so: whatever the nesting, the accumulated tint must
    // stay under the point where a muted title stops clearing 4.5:1.
    const std::string src =
        "flowchart TD\n"
        "  subgraph a[Outer]\n"
        "    subgraph b[Middle]\n"
        "      subgraph c[Inner]\n"
        "        N[Node]\n"
        "      end\n"
        "    end\n"
        "  end\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));

    // Every frame's own coat, in document order — outermost first, because
    // that is the order they are painted and therefore composited.
    std::vector<double> coats;
    for (std::size_t at = svg.find("fill-opacity=\""); at != std::string::npos;
         at = svg.find("fill-opacity=\"", at + 1)) {
        coats.push_back(std::stod(svg.substr(at + 15)));
    }
    REQUIRE(coats.size() == 3u);  // three frames, three tints

    // What a reader actually sees at the deepest frame: each coat over the
    // last. The bound is the engine's own cap, and the reason for the cap is
    // the contrast of the title drawn on top of it.
    double seen = 0.0;
    for (const double coat : coats) seen += coat * (1.0 - seen);
    CHECK(seen <= 0.19);

    // And nesting is still visible: a deeper frame is darker than its parent.
    CHECK(coats[0] > 0.0);
    CHECK(seen > coats[0]);
}

TEST_CASE("flowchart edge variants: invisible, bidirectional, circle and cross ends (S2)") {
    // `~~~` shapes the layout but draws nothing — nodes only, no connector.
    const cplot::Scene hidden = render("flowchart LR\n  A ~~~ B\n");
    CHECK(count<cplot::PolylineItem>(hidden) == 0u);
    // `<-->` draws an arrowhead triangle at both ends.
    const cplot::Scene bidir = render("flowchart LR\n  A <--> B\n");
    CHECK(count<cplot::PolygonItem>(bidir) == 2u);
    // `--o` ends in a filled circle, `x--x` in two X glyphs (two lines each).
    CHECK(count<cplot::CircleItem>(render("flowchart LR\n  A --o B\n")) == 1u);
    CHECK(count<cplot::LineItem>(render("flowchart LR\n  A x--x B\n")) == 4u);
    // All of these are supported syntax: no diagnostics.
    CHECK(cdiagram::check("flowchart LR\n  A ~~~ B\n  A <--> C\n  A --o D\n  A x--x E\n")
              .empty());
}

TEST_CASE("middle edge labels may contain link characters (review)") {
    // 'box' contains the link chars 'o'/'x'; it is label text, not a tail.
    const std::string svg = cplot::SvgRenderer().render(
        render("flowchart LR\n  A -- box --> B\n  C -- x-ray --> D\n"));
    CHECK(svg.find(">box<") != std::string::npos);
    CHECK(svg.find(">x-ray<") != std::string::npos);
    CHECK(svg.find(">ray<") == std::string::npos);  // no phantom node
    // Negative linkStyle selectors are ignored safely.
    CHECK(cplot::SvgRenderer()
              .render(render("flowchart LR\n  A --> B\n  linkStyle -1 stroke:#f00\n"))
              .find("<svg") != std::string::npos);
}

TEST_CASE("v11 @{ shape, label } node metadata maps onto the shape set (S6)") {
    const std::string svg = cplot::SvgRenderer().render(
        render("flowchart LR\n"
               "  A@{ shape: cyl, label: \"Store\" } --> B@{ shape: hex }\n"));
    CHECK(svg.find(">Store<") != std::string::npos);
    CHECK(cdiagram::check("flowchart TD\n  A@{ shape: diam } --> B\n").empty());
    // Unmapped shapes fall back to a rectangle WITH a diagnostic.
    const cworks::Diagnostics d =
        cdiagram::check("flowchart TD\n  A@{ shape: hourglass }\n");
    CHECK(!d.empty());
}
