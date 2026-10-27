// ckdiagram — xychart / kanban / packet / requirement / gitGraph tests
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include <cworks/microtest.hpp>

#include <set>
#include <string>
#include <variant>
#include <vector>

#include <cplot/svg.hpp>
#include <cworks/app_error.hpp>

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

/// The data marks of a single-panel chart, without the plot background
/// under them: cplot nests each panel's marks as the one group inside it.
template <class Item>
std::vector<const Item*> marks(const cplot::Scene& scene) {
    std::vector<const Item*> out;
    for (const cplot::Node& panel : scene.root.children) {
        if (!panel.is_group()) continue;
        for (const cplot::Node& layer : panel.group().children) {
            if (!layer.is_group()) continue;
            for (const cplot::PlacedItem& placed : cplot::collect_items(layer.group()))
                if (const auto* item = std::get_if<Item>(placed.item)) out.push_back(item);
            return out;
        }
    }
    return out;
}

/// The structured refusal a frontend receives for `source`.
cworks::AppError refusal(const std::string& source) {
    try {
        (void)render(source);
    } catch (const cworks::Error& e) {
        return e.structured() != nullptr ? *e.structured() : cworks::AppError{};
    }
    return cworks::AppError{};
}

/// The source text a refusal's caret covers.
std::string caret_text(const std::string& source, const cworks::AppError& error) {
    if (!error.source_span) return {};
    return source.substr(error.source_span->offset, error.source_span->length);
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

TEST_CASE("xychart draws every bar and line statement as its own series") {
    const std::string src =
        "xychart-beta\n  x-axis [A, B, C]\n  bar [3, 6, 4]\n  bar [2, 5, 7]\n"
        "  line [1, 2, 3]\n  line [4, 4, 4]\n";
    const cplot::Scene scene = render(src);
    const std::vector<const cplot::RectItem*> bars = marks<cplot::RectItem>(scene);
    REQUIRE(bars.size() == 6u);
    // Grouped side by side: the second series' bar in each category stands
    // right of the first one's, at the same width, in its own colour.
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(bars[3 + i]->rect.x > bars[i]->rect.x);
        CHECK_APPROX(bars[3 + i]->rect.w, bars[i]->rect.w);
        CHECK(bars[3 + i]->rect.x + bars[3 + i]->rect.w <= bars[i]->rect.x + 2.0 * bars[i]->rect.w + 1e-9);
    }
    CHECK(bars[0]->style.fill != bars[3]->style.fill);
    CHECK_EQ(count<cplot::PolylineItem>(scene), 2u);
    CHECK_EQ(count<cplot::CircleItem>(scene), 6u);
    check_deterministic(src);
}

TEST_CASE("xychart horizontal lays the categories down the y axis") {
    const std::string src =
        "xychart-beta horizontal\n  x-axis \"Month\" [Jan, Feb, Mar]\n"
        "  y-axis \"Revenue\" 0 --> 10\n  bar [3, 6, 4]\n  line [2, 5, 3]\n";
    const cplot::Scene scene = render(src);
    const std::vector<const cplot::RectItem*> bars = marks<cplot::RectItem>(scene);
    const std::vector<const cplot::CircleItem*> dots = marks<cplot::CircleItem>(scene);
    REQUIRE(bars.size() == 3u);
    REQUIRE(dots.size() == 3u);
    // Bars grow along x from a shared baseline and share one thickness.
    CHECK_APPROX(bars[0]->rect.x, bars[1]->rect.x);
    CHECK_APPROX(bars[0]->rect.h, bars[1]->rect.h);
    CHECK(bars[1]->rect.w > bars[0]->rect.w);
    CHECK(bars[1]->rect.w > bars[2]->rect.w);
    // The line's points sit in the middle of their category's band.
    for (std::size_t i = 0; i < 3; ++i)
        CHECK_APPROX(dots[i]->center.y, bars[i]->rect.y + bars[i]->rect.h / 2.0);
    // The value axis title now reads along x, the category title along y.
    bool month = false, revenue = false;
    for (const cplot::PlacedItem& placed : cplot::collect_items(scene.root)) {
        if (const auto* text = std::get_if<cplot::TextItem>(placed.item)) {
            month = month || (text->text == "Month" && text->rotation != 0.0);
            revenue = revenue || (text->text == "Revenue" && text->rotation == 0.0);
        }
    }
    CHECK(month);
    CHECK(revenue);
    check_deterministic(src);

    // Mermaid's front-matter form says the same thing.
    const std::string configured =
        "---\nconfig:\n  xyChart:\n    chartOrientation: horizontal\n---\n"
        "xychart-beta\n  x-axis \"Month\" [Jan, Feb, Mar]\n"
        "  y-axis \"Revenue\" 0 --> 10\n  bar [3, 6, 4]\n  line [2, 5, 3]\n";
    CHECK_EQ(cplot::SvgRenderer().render(render(configured)),
             cplot::SvgRenderer().render(scene));
}

TEST_CASE("xychart refuses a series whose length differs from the categories") {
    const std::string src = "xychart-beta\n  x-axis [A, B, C]\n  bar [3, 6, 4]\n  line [1, 2]\n";
    const cworks::AppError error = refusal(src);
    CHECK(error.is(cworks::ErrorCode::ValidationFailed));
    CHECK(error.summary.find("(line 4)") != std::string::npos);
    CHECK(error.summary.find("line series has 2 values but the x axis has 3 categories") !=
          std::string::npos);
    CHECK_EQ(caret_text(src, error), std::string("[1, 2]"));

    // Without categories the first series sets the count.
    const std::string numeric = "xychart-beta\n  line [1, 2, 3]\n  line [1, 2]\n";
    CHECK(refusal(numeric).summary.find("the first series has 3") != std::string::npos);
}

TEST_CASE("xychart refusals point at the offending text") {
    const std::string bad_value = "xychart-beta\r\n  x-axis [A, B]\r\n  bar [3, x7]\r\n";
    const cworks::AppError value = refusal(bad_value);
    CHECK(value.summary.find("'x7' is not a finite number") != std::string::npos);
    CHECK_EQ(caret_text(bad_value, value), std::string("x7"));

    const std::string trailing = "xychart-beta\n  bar [3, 7] [1, 1]\n";
    CHECK_EQ(caret_text(trailing, refusal(trailing)), std::string("[1, 1]"));

    const std::string unclosed = "xychart-beta\n  bar [3, 7\n";
    CHECK_EQ(caret_text(unclosed, refusal(unclosed)), std::string("[3, 7"));
}

TEST_CASE("xychart bars on a numeric x axis cannot group or turn") {
    const std::string several = "xychart-beta\n  x-axis 0 --> 10\n  bar [1, 2]\n  bar [3, 4]\n";
    const cworks::AppError grouped = refusal(several);
    CHECK(grouped.summary.find("several bar series need categories") != std::string::npos);
    CHECK_EQ(caret_text(several, grouped), std::string("bar [3, 4]"));

    const std::string turned = "xychart-beta horizontal\n  x-axis 0 --> 10\n  bar [1, 2]\n";
    CHECK(refusal(turned).summary.find("horizontal bars need categories") != std::string::npos);

    // Lines on a numeric axis need no band, so both forms draw.
    CHECK_EQ(count<cplot::PolylineItem>(render(
                 "xychart-beta horizontal\n  x-axis 0 --> 10\n  line [1, 2]\n  line [2, 1]\n")),
             2u);
}

TEST_CASE("xychart reports an orientation it does not know") {
    const cworks::Diagnostics diagnostics =
        cdiagram::check("xychart-beta horizontl\n  x-axis [A]\n  bar [1]\n");
    bool found = false;
    for (const cworks::Diagnostic& d : diagnostics)
        found = found || d.message.find("did you mean 'horizontal'") != std::string::npos;
    CHECK(found);
}

TEST_CASE("xychart regions name the series statement that drew each mark") {
    // This copy has no editor scene map; the regions a renderer reports are
    // the same identities upstream resolves to series objects.
    const std::string src =
        "xychart-beta\n  x-axis [A, B, C]\n  bar [3, 6, 4]\n  line [1, 2, 3]\n"
        "  bar [2, 5, 7]\n";
    std::vector<cdiagram::DrawnRegion> regions;
    cdiagram::RenderOptions options;
    options.regions = &regions;
    const cplot::Scene scene = render(src, options);
    REQUIRE(regions.size() == 9u);
    // Paint order: the first bar series, the line's points, the second bars.
    const char* expected[] = {"#0", "#0", "#0", "#1", "#1", "#1", "#2", "#2", "#2"};
    for (std::size_t i = 0; i < regions.size(); ++i) CHECK_EQ(regions[i].key, std::string(expected[i]));
    // Each bar region is exactly a drawn bar, never the plot background.
    const auto bars = marks<cplot::RectItem>(scene);
    REQUIRE(bars.size() == 6u);
    CHECK_APPROX(regions[0].bounds.x, bars[0]->rect.x);
    CHECK_APPROX(regions[0].bounds.h, bars[0]->rect.h);
    CHECK_APPROX(regions[6].bounds.x, bars[3]->rect.x);
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
