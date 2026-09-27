// ckdiagram — radar / treemap / block / C4 / sankey tests
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include <cworks/microtest.hpp>

#include <string>
#include <variant>

#include <cplot/svg.hpp>

#include "cdiagram/diagram.hpp"
#include "cdiagram/error.hpp"
#include "cdiagram/render.hpp"

using cdiagram::detect_type;
using cdiagram::DiagramType;
using cdiagram::render;

namespace {

template <class Item>
std::size_t count(const cplot::Scene& scene) {
    std::size_t n = 0;
    for (const cplot::PlacedItem& placed : cplot::collect_items(scene.root))
        if (std::holds_alternative<Item>(*placed.item)) ++n;
    return n;
}

void check_deterministic(const std::string& src) {
    const std::string a = cplot::SvgRenderer().render(render(src));
    const std::string b = cplot::SvgRenderer().render(render(src));
    CHECK_EQ(a, b);
    CHECK(a.find("<svg") != std::string::npos);
}

} // namespace

TEST_CASE("exotic keywords are detected") {
    CHECK(detect_type("radar-beta\n") == DiagramType::Radar);
    CHECK(detect_type("treemap-beta\n") == DiagramType::Treemap);
    CHECK(detect_type("block-beta\n") == DiagramType::Block);
    CHECK(detect_type("C4Context\n") == DiagramType::C4);
    CHECK(detect_type("C4Container\n") == DiagramType::C4);
    CHECK(detect_type("sankey-beta\n") == DiagramType::Sankey);
}

TEST_CASE("radar draws a polygon per curve over axes") {
    const std::string src =
        "radar-beta\n  title R\n  axis a[\"A\"], b[\"B\"], c[\"C\"]\n"
        "  curve x[\"X\"]{3, 5, 4}\n  curve y[\"Y\"]{5, 2, 4}\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::PolygonItem>(scene) >= 2u);  // two curves
    // axis labels have their quotes stripped
    const std::string svg = cplot::SvgRenderer().render(scene);
    CHECK(svg.find(">A<") != std::string::npos);
    CHECK(svg.find("\"A\"<") == std::string::npos);
    check_deterministic(src);
}

TEST_CASE("treemap packs leaves sized by value") {
    const std::string src =
        "treemap-beta\n\"Root\"\n    \"A\": 30\n    \"B\": 10\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::RectItem>(scene) >= 2u);  // two leaves
    check_deterministic(src);
}

TEST_CASE("block flows blocks into a column grid with spans") {
    const std::string src =
        "block-beta\n  columns 3\n  a[\"A\"] b[\"B\"] c[\"C\"]\n  d[\"D\"]:2 e[\"E\"]\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::PathItem>(scene) >= 5u);  // 5 blocks (rounded-rect paths)
    check_deterministic(src);
}

TEST_CASE("C4 lays out people and systems with relations") {
    const std::string src =
        "C4Context\n  Person(u, \"User\")\n  System(s, \"System\")\n"
        "  Rel(u, s, \"uses\")\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::PathItem>(scene) >= 2u);      // two rounded boxes
    CHECK(count<cplot::PolylineItem>(scene) >= 1u);  // the relation
    check_deterministic(src);
}

TEST_CASE("sankey draws proportional flow bands") {
    const std::string src =
        "sankey-beta\nA,B,10\nA,C,5\nB,D,10\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::PolygonItem>(scene) >= 3u);  // three link bands
    CHECK(count<cplot::RectItem>(scene) >= 4u);      // four node bars
    check_deterministic(src);
}

TEST_CASE("sankey honours the nodeAlignment front matter") {
    // C is a stage-1 sink: justify (the default) pins its bar to the last
    // column, while nodeAlignment: left keeps it in B's column.
    const std::string body = "sankey-beta\nA,B,10\nA,C,5\nB,D,10\n";
    const std::string config = "---\nconfig:\n  sankey:\n    nodeAlignment: left\n---\n";
    const auto node_x = [](const cplot::Scene& scene) {
        std::vector<double> xs;  // node bars in node order: A, B, C, D
        for (const cplot::PlacedItem& placed : cplot::collect_items(scene.root))
            if (const auto* rect = std::get_if<cplot::RectItem>(placed.item))
                xs.push_back(rect->rect.x);
        return xs;
    };
    const std::vector<double> justify = node_x(render(body));
    CHECK_EQ(justify[2], justify[3]);
    const std::vector<double> left = node_x(render(config + body));
    CHECK_EQ(left[1], left[2]);
    CHECK(left[2] < left[3]);
    check_deterministic(config + body);
}

TEST_CASE("sankey syntax adapter surfaces shared graph validation") {
    CHECK_THROWS_AS(render("sankey-beta\nA,B,1\nB,A,1\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("sankey-beta\nA,B,-1\n"), cdiagram::Error);
}

TEST_CASE("exotic types report errors on empty bodies") {
    CHECK_THROWS_AS(render("radar-beta\n  title R\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("treemap-beta\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("block-beta\n  columns 2\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("C4Context\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("sankey-beta\n"), cdiagram::Error);
}

TEST_CASE("c4 Update*Style directives colour elements and relations (S5)") {
    const std::string src =
        "C4Context\n"
        "  Person(user, \"User\")\n"
        "  System(web, \"Web\")\n"
        "  Rel(user, web, \"uses\")\n"
        "  UpdateElementStyle(web, $bgColor=\"#2E8B57\", $fontColor=\"#ffffff\")\n"
        "  UpdateRelStyle(user, web, $lineColor=\"#ff6347\")\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find("#2E8B57") != std::string::npos);
    CHECK(svg.find("#FF6347") != std::string::npos);
    CHECK(cdiagram::check(src).empty());
}
