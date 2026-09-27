// ckdiagram — geometry diagram tests (gantt/timeline/journey/quadrant/mindmap)
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

TEST_CASE("quadrant chart plots points in a 2x2 matrix") {
    const std::string src =
        "quadrantChart\n  title Q\n  x-axis Low --> High\n  y-axis Low --> High\n"
        "  A: [0.2, 0.8]\n  B: [0.7, 0.3]\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::CircleItem>(scene) == 2u);  // the two points
    CHECK(count<cplot::RectItem>(scene) >= 5u);     // 4 quadrant fills + frame
    check_deterministic(src);
}

TEST_CASE("quadrant syntax adapter enforces normalized coordinates") {
    CHECK_THROWS_AS(render("quadrantChart\n  Outside: [1.2, 0.5]\n"), cdiagram::Error);
}

TEST_CASE("mindmap builds a tree from indentation") {
    const std::string src =
        "mindmap\n  root((Root))\n    A\n      A1\n    B\n";
    const cplot::Scene scene = render(src);
    // 4 nodes; edges connect children to parents.
    CHECK(count<cplot::PolylineItem>(scene) >= 3u);
    CHECK(count<cplot::TextItem>(scene) >= 4u);
    check_deterministic(src);
}

TEST_CASE("gantt resolves dates and after-dependencies") {
    const std::string src =
        "gantt\n  title P\n  section S\n  First :a1, 2024-01-01, 7d\n"
        "  Second :after a1, 3d\n";
    const cplot::Scene scene = render(src);
    // two task bars (rounded-rect paths) + section/labels
    CHECK(count<cplot::PathItem>(scene) >= 2u);
    CHECK(scene.width > 0.0);
    check_deterministic(src);
}

TEST_CASE("gantt handles a milestone") {
    const cplot::Scene scene = render(
        "gantt\n  title P\n  Kickoff :milestone, 2024-01-01, 0d\n  Work :2024-01-02, 5d\n");
    CHECK(scene.width > 0.0);  // milestone diamond + bar, no crash
}

TEST_CASE("timeline stacks events per period") {
    const std::string src =
        "timeline\n  title T\n  2019 : Founded\n  2021 : A : B\n  2023 : IPO\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::CircleItem>(scene) == 3u);  // one marker per period
    check_deterministic(src);
}

TEST_CASE("journey scores tasks in sections") {
    const std::string src =
        "journey\n  title J\n  section Do\n  Search: 4: Me\n  Buy: 2: Me, Cat\n";
    const cplot::Scene scene = render(src);
    CHECK(count<cplot::CircleItem>(scene) == 2u);  // score dots
    check_deterministic(src);
}

TEST_CASE("geometry types report errors on empty bodies") {
    CHECK_THROWS_AS(render("timeline\n  title T\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("journey\n  title J\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("gantt\n  title P\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("mindmap\n"), cdiagram::Error);
}

TEST_CASE("curved diagram shapes reach SVG as real Béziers") {
    // The Diagram Renderer used to approximate its own round corners and
    // cylinder ends with polygons — six chords per corner, 24 per ellipse
    // — because the Scene had no curve primitive. It has one now, so the
    // curve survives to the vector backends and a rounded node stays
    // round at any zoom instead of showing its chords.
    const std::string rounded = "flowchart LR\n  A(Rounded)\n";
    const std::string svg = cplot::SvgRenderer().render(render(rounded));
    CHECK(svg.find("<path d=\"M ") != std::string::npos);
    CHECK(svg.find(" C ") != std::string::npos);
    // Nothing pre-flattened the corner into a chord list on the way.
    CHECK(svg.find("<polygon") == std::string::npos);
    check_deterministic(rounded);

    // The cylinder's end cap is an ellipse: four cubics, one path.
    const std::string cylinder = "flowchart LR\n  A[(Store)]\n";
    const cplot::Scene scene = render(cylinder);
    CHECK(count<cplot::PathItem>(scene) == 1u);
    for (const cplot::PlacedItem& placed : cplot::collect_items(scene.root)) {
        if (const auto* path = std::get_if<cplot::PathItem>(placed.item)) {
            REQUIRE(path->subpaths.size() == 1u);
            REQUIRE(path->subpaths.front().segments.size() == 4u);
            for (const cplot::PathSegment& seg : path->subpaths.front().segments)
                CHECK(seg.curve);
        }
    }
    check_deterministic(cylinder);
}
