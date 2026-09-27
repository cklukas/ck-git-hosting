// ckdiagram — xychart / kanban / packet / requirement / gitGraph tests
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

TEST_CASE("new keywords are detected") {
    CHECK(detect_type("xychart-beta\n") == DiagramType::XyChart);
    CHECK(detect_type("kanban\n") == DiagramType::Kanban);
    CHECK(detect_type("packet-beta\n") == DiagramType::Packet);
    CHECK(detect_type("requirementDiagram\n") == DiagramType::Requirement);
    CHECK(detect_type("gitGraph\n") == DiagramType::Git);
}

TEST_CASE("xychart draws bars and a line") {
    const std::string src =
        "xychart-beta\n  title \"T\"\n  x-axis [A, B, C]\n"
        "  y-axis \"V\" 0 --> 10\n  bar [3, 6, 4]\n  line [2, 5, 3]\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::RectItem>(scene) >= 3u);      // 3 bars
    CHECK(count<cplot::PolylineItem>(scene) >= 1u);  // the line
    CHECK(count<cplot::CircleItem>(scene) == 3u);    // line dots
    check_deterministic(src);
}

TEST_CASE("xychart delegates strict categorical shape validation") {
    CHECK_THROWS_AS(
        render("xychart-beta\n  x-axis [A, B, C]\n  line [1, 2]\n"),
        cdiagram::Error);
}

TEST_CASE("kanban lays out columns and cards") {
    const std::string src =
        "kanban\n  a[To Do]\n    c1[Task 1]\n    c2[Task 2]\n  b[Done]\n    c3[Task 3]\n";
    const cplot::Scene scene = render(src);
    // 2 column backgrounds + 2 headers + 3 cards (rounded-rect paths)
    CHECK(count<cplot::PathItem>(scene) >= 5u);
    check_deterministic(src);
}

TEST_CASE("monochrome themes render kanban categories in grayscale") {
    const std::string src =
        "kanban\n  todo[To Do]\n    draft[Draft]\n  done[Done]\n    ship[Ship]\n";
    cdiagram::RenderOptions options;
    options.theme = cplot::Theme::monochrome();
    options.theme_name = "monochrome";
    options.mode = cdiagram::ThemeMode::Light;
    const std::string svg = cplot::SvgRenderer().render(render(src, options));

    // The named monochrome theme overrides an earlier Light mode rather than
    // leaving the familiar blue/orange series behind.
    CHECK(svg.find("#555555") != std::string::npos);
    CHECK(svg.find("#888888") != std::string::npos);
    CHECK(svg.find("#4C78A8") == std::string::npos);
    CHECK(svg.find("#F58518") == std::string::npos);
}

TEST_CASE("packet lays fields on a bit grid") {
    const std::string src =
        "packet-beta\n  0-15: \"A\"\n  16-31: \"B\"\n  32-63: \"C\"\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::RectItem>(scene) >= 3u);  // one cell per field
    check_deterministic(src);
}

TEST_CASE("requirement links a requirement and an element") {
    const std::string src =
        "requirementDiagram\n"
        "  requirement r { id: 1 }\n"
        "  element e { type: test }\n"
        "  e - satisfies -> r\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::RectItem>(scene) >= 2u);      // two boxes
    CHECK(count<cplot::PolylineItem>(scene) >= 1u);  // the relation
    check_deterministic(src);
}

TEST_CASE("gitGraph places commits on branch lanes") {
    const std::string src =
        "gitGraph\n  commit\n  branch dev\n  commit\n  checkout main\n  commit\n"
        "  merge dev\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::CircleItem>(scene) >= 4u);  // commit dots
    check_deterministic(src);
}

TEST_CASE("new types report errors on empty bodies") {
    CHECK_THROWS_AS(render("xychart-beta\n  title \"x\"\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("kanban\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("packet-beta\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("requirementDiagram\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("gitGraph\n"), cdiagram::Error);
}

TEST_CASE("quadrant per-point styling never costs the data point (S0)") {
    const cplot::Scene scene = render(
        "quadrantChart\n"
        "  title T\n"
        "  A: [0.2, 0.4] radius: 12\n"
        "  B: [0.7, 0.6]\n");
    CHECK(count<cplot::CircleItem>(scene) == 2u);  // both points survive
}

TEST_CASE("block and quadrant styling directives apply (S3)") {
    const std::string block = cplot::SvgRenderer().render(
        render("block-beta\n  columns 2\n  A[\"Store\"] B[\"Query\"]\n"
               "  style A fill:#f96,stroke:#333\n"));
    CHECK(block.find("#FF9966") != std::string::npos);
    const cplot::Scene quadrant = render(
        "quadrantChart\n  title T\n  classDef big radius: 12\n"
        "  A:::big: [0.2, 0.4]\n  B: [0.7, 0.6] color: #ff0000\n");
    // A's radius comes from the classDef, B's colour from the inline tail.
    std::size_t big = 0, red = 0;
    for (const cplot::PlacedItem& placed : cplot::collect_items(quadrant.root)) {
        if (const auto* circle = std::get_if<cplot::CircleItem>(placed.item)) {
            if (circle->radius == 12.0) ++big;
            if (circle->style.fill && circle->style.fill->hex() == "#FF0000") ++red;
        }
    }
    CHECK(big == 1u);
    CHECK(red == 1u);
}

TEST_CASE("quadrant rgb() classDefs and lone stroke-width apply (review)") {
    const cplot::Scene scene = render(
        "quadrantChart\n  classDef hot color: rgb(255,0,0)\n"
        "  A:::hot: [0.2, 0.4]\n  B: [0.7, 0.6] stroke-width: 3\n");
    std::size_t red = 0, stroked = 0;
    for (const cplot::PlacedItem& placed : cplot::collect_items(scene.root)) {
        if (const auto* circle = std::get_if<cplot::CircleItem>(placed.item)) {
            if (circle->style.fill && circle->style.fill->hex() == "#FF0000") ++red;
            if (circle->style.stroke && circle->style.stroke_width == 3.0) ++stroked;
        }
    }
    CHECK(red == 1u);
    CHECK(stroked == 1u);
}

TEST_CASE("gitgraph commit types, ids, and cherry-picks render (S5)") {
    const std::string src =
        "gitGraph\n"
        "  commit id: \"base\"\n"
        "  commit type: HIGHLIGHT tag: \"v1\"\n"
        "  branch feature\n"
        "  commit type: REVERSE\n"
        "  checkout main\n"
        "  cherry-pick id: \"base\"\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::RectItem>(scene) >= 1u);   // the HIGHLIGHT square
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find(">base<") != std::string::npos);  // id label under the dot
    CHECK(svg.find("stroke-dasharray") != std::string::npos);  // cherry-pick link
    CHECK(cdiagram::check(src).empty());
}

TEST_CASE("timeline sections group and colour their periods (S5)") {
    const std::string src =
        "timeline\n"
        "  title History\n"
        "  section Ancient\n"
        "  3000 BC : writing\n"
        "  500 BC : philosophy\n"
        "  section Modern\n"
        "  1900 : flight\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find(">Ancient<") != std::string::npos);
    CHECK(svg.find(">Modern<") != std::string::npos);
    CHECK(cdiagram::check(src).empty());
}

TEST_CASE("gantt and journey colours follow the render mode (S5)") {
    const std::string src =
        "gantt\n  title T\n  section S\n  Task A : done, 2026-01-01, 3d\n"
        "  Task B : active, 2026-01-04, 2d\n";
    cdiagram::RenderOptions dark;
    dark.mode = cdiagram::ThemeMode::Dark;
    const std::string svg = cplot::SvgRenderer().render(cdiagram::render(src, dark));
    CHECK(svg.find("#4C78A8") == std::string::npos);  // light-mode active blue
    CHECK(svg.find("#5B8DBE") != std::string::npos);  // dark-mode active blue
}

TEST_CASE("engine high contrast theme strengthens semantic diagram colours") {
    const std::string source =
        "gantt\n  title T\n  section S\n"
        "  Task A : active, 2026-01-01, 3d\n";
    cdiagram::RenderOptions light;
    light.theme = cplot::Theme::high_contrast();
    const std::string light_svg =
        cplot::SvgRenderer().render(cdiagram::render(source, light));
    CHECK(light_svg.find("#003B73") != std::string::npos);
    CHECK(light_svg.find("#4C78A8") == std::string::npos);

    cdiagram::RenderOptions dark = light;
    dark.mode = cdiagram::ThemeMode::Dark;
    const std::string dark_svg =
        cplot::SvgRenderer().render(cdiagram::render(source, dark));
    CHECK(dark_svg.find("#73B7FF") != std::string::npos);
    CHECK(dark_svg.find("#5B8DBE") == std::string::npos);
}
