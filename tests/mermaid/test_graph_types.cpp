// ckdiagram — class / state / ER diagram tests (dagre-cluster types)
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include <cworks/microtest.hpp>

#include <string>
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

void check_deterministic(const std::string& src) {
    const std::string a = cplot::SvgRenderer().render(render(src));
    const std::string b = cplot::SvgRenderer().render(render(src));
    CHECK_EQ(a, b);
    CHECK(a.find("<svg") != std::string::npos);
}

} // namespace

TEST_CASE("state diagram: states, transitions and pseudo-states") {
    const std::string src =
        "stateDiagram-v2\n"
        "  [*] --> Idle\n"
        "  Idle --> Running : start\n"
        "  Running --> [*]\n";
    const cplot::Scene scene = render(src);
    CHECK(scene.width > 0.0);
    // start (filled circle) + end (ring + dot) = circles present
    CHECK(count<cplot::CircleItem>(scene) >= 2u);
    CHECK(count<cplot::PolylineItem>(scene) >= 3u);   // 3 transitions
    CHECK(count<cplot::PolygonItem>(scene) >= 3u);    // arrowheads
    check_deterministic(src);
}

TEST_CASE("state bidirectional pair: arrowheads stay visible, regions hug the curve") {
    // Opposite transitions between the same two states bow into smoothed
    // curves whose raw routes run centre to centre, so the samples next to
    // each end sit inside the state boxes. Those samples must be dropped
    // before the endpoints are clipped: an arrowhead aimed from an inner
    // sample is built on the far side of the border and painted over by the
    // state, and the edge's reported region balloons from the visible curve
    // to the node centres.
    const std::string src =
        "stateDiagram-v2\n"
        "  [*] --> Idle\n"
        "  Idle --> Running : start\n"
        "  Running --> Idle : stop\n"
        "  Running --> [*] : shutdown\n";
    std::vector<cdiagram::DrawnRegion> regions;
    cdiagram::RenderOptions options;
    options.regions = &regions;
    const cplot::Scene scene = render(src, options);

    std::vector<cplot::RectF> boxes;  // the two state rectangles
    std::vector<cplot::RectF> edges;  // one region per transition
    for (const cdiagram::DrawnRegion& region : regions)
        (region.role == 0 ? boxes : edges).push_back(region.bounds);
    REQUIRE(boxes.size() == 2u);
    REQUIRE(edges.size() == 4u);

    // Every arrowhead (the filled polygons) lies outside both states: a
    // marker under a state box is a marker nobody sees. The tip touches the
    // border, so "inside" leaves a one-pixel belt.
    const auto strictly_inside = [](cplot::Point p, const cplot::RectF& r) {
        return p.x > r.x + 1.0 && p.x < r.x + r.w - 1.0 &&
               p.y > r.y + 1.0 && p.y < r.y + r.h - 1.0;
    };
    std::size_t arrows = 0;
    for (const cplot::PlacedItem& placed : cplot::collect_items(scene.root)) {
        const auto* polygon = std::get_if<cplot::PolygonItem>(placed.item);
        if (polygon == nullptr) continue;
        ++arrows;
        for (const cplot::Point p : polygon->points)
            for (const cplot::RectF& box : boxes)
                CHECK(!strictly_inside(placed.ctm.apply(p), box));
    }
    CHECK_EQ(arrows, 4u);  // one per transition

    // A transition's selection region spans the visible curve, not the
    // centre-to-centre route: it never swallows a state's centre.
    for (const cplot::RectF& edge : edges)
        for (const cplot::RectF& box : boxes) {
            const double cx = box.x + box.w / 2.0, cy = box.y + box.h / 2.0;
            CHECK(!(cx >= edge.x && cx <= edge.x + edge.w &&
                    cy >= edge.y && cy <= edge.y + edge.h));
        }
}

TEST_CASE("er diagram: entities, attributes and cardinality") {
    const std::string src =
        "erDiagram\n"
        "  CUSTOMER ||--o{ ORDER : places\n"
        "  CUSTOMER {\n    string name\n    int id\n  }\n";
    const cplot::Scene scene = render(src);
    // entity boxes + header bands are rects; attribute rows are text.
    CHECK(count<cplot::RectItem>(scene) >= 3u);
    CHECK(count<cplot::TextItem>(scene) >= 4u);
    check_deterministic(src);
}

TEST_CASE("er diagram: PK and FK key constraints render as a column") {
    const std::string src =
        "erDiagram\n"
        "  ORDERS }o--|| CUSTOMER : \"customer_id\"\n"
        "  CUSTOMER {\n    INTEGER id PK\n  }\n"
        "  ORDERS {\n    INTEGER id PK\n    INTEGER customer_id FK\n  }\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find(">PK<") != std::string::npos);
    CHECK(svg.find(">FK<") != std::string::npos);
    // A quoted comment in the key slot is NOT a key constraint.
    const std::string commented =
        "erDiagram\n  T {\n    int id \"the identifier\"\n  }\n";
    const std::string svg2 = cplot::SvgRenderer().render(render(commented));
    CHECK(svg2.find(">PK<") == std::string::npos);
    check_deterministic(src);
}

TEST_CASE("class diagram: compartments and relationships") {
    const std::string src =
        "classDiagram\n"
        "  Animal <|-- Dog\n"
        "  Animal : +String name\n"
        "  Animal : +makeSound() void\n"
        "  Dog : +fetch() void\n";
    const cplot::Scene scene = render(src);
    // class boxes are rects; the inheritance triangle is a polygon.
    CHECK(count<cplot::RectItem>(scene) >= 2u);
    CHECK(count<cplot::PolygonItem>(scene) >= 1u);
    // name + members
    CHECK(count<cplot::TextItem>(scene) >= 5u);
    check_deterministic(src);
}

TEST_CASE("class relations classify markers") {
    // composition (filled diamond), aggregation, association arrow.
    const cplot::Scene scene = render(
        "classDiagram\n  A *-- B\n  A o-- C\n  A --> D\n");
    CHECK(count<cplot::PolygonItem>(scene) >= 2u);  // two diamonds + arrow
    check_deterministic("classDiagram\n  A *-- B\n  A o-- C\n  A --> D\n");
}

TEST_CASE("graph types report errors") {
    CHECK_THROWS_AS(render("stateDiagram-v2\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("erDiagram\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("classDiagram\n"), cdiagram::Error);
}

TEST_CASE("state ::: annotations never corrupt ids or labels (S0)") {
    const std::string svg = cplot::SvgRenderer().render(
        render("stateDiagram-v2\n  Idle --> Error:::bad : fail\n  Error --> Idle : reset\n"));
    CHECK(svg.find(">fail<") != std::string::npos);
    CHECK(svg.find(">Error<") != std::string::npos);
    CHECK(svg.find(":::") == std::string::npos);
}

TEST_CASE("class ::: annotations never leak into the class name (S0)") {
    const std::string svg = cplot::SvgRenderer().render(
        render("classDiagram\n  class Foo:::highlight\n  Foo <|-- Bar\n"));
    CHECK(svg.find(">Foo<") != std::string::npos);
    CHECK(svg.find(":::") == std::string::npos);
}

TEST_CASE("er entity aliases display the label and keep the identity (S0)") {
    const std::string svg = cplot::SvgRenderer().render(
        render("erDiagram\n"
               "    p[Person] ||--o{ o[Order] : places\n"
               "    p {\n        string name\n    }\n"));
    CHECK(svg.find(">Person<") != std::string::npos);
    CHECK(svg.find(">Order<") != std::string::npos);
    CHECK(svg.find(">p<") == std::string::npos);
}

TEST_CASE("class/state/er styling directives colour the elements (S3)") {
    const std::string cls = cplot::SvgRenderer().render(
        render("classDiagram\n  class Foo:::hot\n  Foo <|-- Bar\n"
               "  classDef hot fill:#f96\n  style Bar stroke:rgb(0,128,0)\n"));
    CHECK(cls.find("#FF9966") != std::string::npos);
    CHECK(cls.find("#008000") != std::string::npos);
    const std::string state = cplot::SvgRenderer().render(
        render("stateDiagram-v2\n  classDef bad fill:#f00,color:white\n"
               "  Idle --> Error:::bad : fail\n"));
    CHECK(state.find("#FF0000") != std::string::npos);
    CHECK(state.find(">fail<") != std::string::npos);
    const std::string er = cplot::SvgRenderer().render(
        render("erDiagram\n  CUSTOMER ||--o{ ORDER : places\n"
               "  style CUSTOMER fill:#bbf,stroke:#333\n"));
    CHECK(er.find("#BBBBFF") != std::string::npos);
    CHECK(cdiagram::check("classDiagram\n  class A:::x\n  classDef x fill:#f96\n").empty());
}

TEST_CASE("bare styling keywords are diagnosed, never abort (review)") {
    // A lone `style` / `class` keyword used to throw std::out_of_range.
    const char* sources[] = {
        "classDiagram\n  class A\n  style\n",
        "stateDiagram-v2\n  [*] --> A\n  class\n",
        "erDiagram\n  A ||--o{ B : x\n  style\n",
        "requirementDiagram\nrequirement r {\nid: 1\n}\nclass\n",
        "block-beta\n  columns 1\n  A\n  style\n",
    };
    for (const char* source : sources) CHECK(!cdiagram::check(source).empty());
}

TEST_CASE("er ::: annotations and quoted names survive relationship lines (review)") {
    const std::string styled = cplot::SvgRenderer().render(
        render("erDiagram\n  c:::big ||--o{ o2 : places\n  classDef big fill:#ff0000\n"));
    CHECK(styled.find("#FF0000") != std::string::npos);
    CHECK(styled.find(">places<") != std::string::npos);
    const std::string quoted = cplot::SvgRenderer().render(
        render("erDiagram\n  \"Customer Account\" ||--o{ ORDER : places\n"
               "  c[\"Big Client\"]\n"));
    CHECK(quoted.find("Customer Account") != std::string::npos);
    CHECK(quoted.find("Big Client") != std::string::npos);
}
