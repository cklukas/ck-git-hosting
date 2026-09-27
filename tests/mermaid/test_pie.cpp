// ckdiagram — pie diagram + detection + dispatch tests
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

} // namespace

TEST_CASE("detect_type classifies by leading keyword") {
    CHECK(detect_type("pie\n\"A\" : 1") == DiagramType::Pie);
    CHECK(detect_type("flowchart TD\nA-->B") == DiagramType::Flowchart);
    CHECK(detect_type("graph LR") == DiagramType::Flowchart);
    CHECK(detect_type("sequenceDiagram") == DiagramType::Sequence);
    CHECK(detect_type("stateDiagram-v2") == DiagramType::State);
    CHECK(detect_type("erDiagram") == DiagramType::Er);
    CHECK(detect_type("nonsense here") == DiagramType::Unknown);
    // A leading YAML front-matter block and %% comments are skipped.
    CHECK(detect_type("---\ntitle: x\n---\n%% a comment\npie\n\"A\" : 1") == DiagramType::Pie);
}

TEST_CASE("pie renders a scene with one sector per slice") {
    const std::string src = "pie title Pets\n  \"Dogs\" : 3\n  \"Cats\" : 1\n";
    const cplot::Scene scene = render(src);
    CHECK(scene.width > 0.0);
    CHECK(scene.height > 0.0);
    REQUIRE(scene.root.children.size() == 1u); // one chart panel
    CHECK_EQ(count<cplot::SectorItem>(scene), 2u);
    // title + 2 legend labels (+ on-slice percentages) = several texts
    CHECK(count<cplot::TextItem>(scene) >= 3u);
    // At least two legend swatches; cplot also owns the panel and legend
    // backgrounds.
    CHECK(count<cplot::RectItem>(scene) >= 2u);
}

TEST_CASE("pie output is deterministic and standalone SVG") {
    const std::string src = "pie\n  \"A\" : 40\n  \"B\" : 35\n  \"C\" : 25\n";
    const std::string svg1 = cplot::SvgRenderer().render(render(src));
    const std::string svg2 = cplot::SvgRenderer().render(render(src));
    CHECK_EQ(svg1, svg2);
    CHECK(svg1.find("<svg") != std::string::npos);
    CHECK(svg1.find("</svg>") != std::string::npos);
}

TEST_CASE("pie showData reaches the legend") {
    const cplot::Scene withdata = render("pie showData\n  \"A\" : 40\n  \"B\" : 60\n");
    const std::string svg = cplot::SvgRenderer().render(withdata);
    CHECK(svg.find("A [40]") != std::string::npos);
}

TEST_CASE("pie reports syntax errors") {
    CHECK_THROWS_AS(render("pie\n  Dogs : 3"), cdiagram::Error);      // no quotes
    CHECK_THROWS_AS(render("pie\n  \"Dogs\" 3"), cdiagram::Error);    // no colon
    CHECK_THROWS_AS(render("pie\n  \"Dogs\" : abc"), cdiagram::Error);  // not a number
    CHECK_THROWS_AS(render("pie\n  \"Dogs\" : -1"), cdiagram::Error);   // negative
    CHECK_THROWS_AS(render("pie"), cdiagram::Error);                    // no data
}

TEST_CASE("unknown and unsupported diagram types throw") {
    CHECK_THROWS_AS(render("bogusDiagram\n"), cdiagram::Error);
    CHECK_THROWS_AS(render(""), cdiagram::Error);
    // recognised but not yet laid out (gantt is on the roadmap)
    CHECK_THROWS_AS(render("gantt\n  title Plan"), cdiagram::Error);
}

TEST_CASE("check() collects errors without throwing") {
    CHECK(cdiagram::check("pie\n  \"A\" : 1\n").empty());
    CHECK(!cdiagram::check("pie\n  broken").empty());
}
