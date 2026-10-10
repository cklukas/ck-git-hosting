// ckplot — label placement and clipping regressions
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <cplot/figure.hpp>
#include <cplot/scene.hpp>
#include <cplot/structured_charts.hpp>
#include <cplot/text.hpp>
#include <cplot/svg.hpp>
#include "cworks/microtest.hpp"

using namespace cplot;

namespace {
double intersection(const RectF& a, const RectF& b) {
    return std::max(0.0, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x)) *
           std::max(0.0, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
}
std::map<std::string, RectF> text_boxes(const Scene& scene) {
    std::map<std::string, RectF> out;
    for (const auto& placed : collect_items(scene.root))
        if (const auto* t = std::get_if<TextItem>(placed.item))
            out[t->text] = text_bounds(*t, default_text_measurer());
    return out;
}
void inside(const RectF& a, const RectF& b) {
    CHECK(a.x >= b.x - 0.05);
    CHECK(a.y >= b.y - 0.05);
    CHECK(a.x + a.w <= b.x + b.w + 0.05);
    CHECK(a.y + a.h <= b.y + b.h + 0.05);
}
}

TEST_CASE("quadrant labels avoid clusters headings markers and plot edges") {
    QuadrantChart chart;
    chart.quadrants = {"Upper right", "Upper left", "Lower left", "Lower right"};
    chart.point("a", "Nearby label alpha", .25, .60);
    chart.point("b", "Nearby label beta", .28, .61);
    chart.point("c", "Edge label with a long name", .96, .25);
    chart.point("d", "Near heading", .23, .47);
    chart.point("e", "Bottom cluster one", .61, .08);
    chart.point("f", "Bottom cluster two", .62, .09);
    chart.point("g", "Large marker", .72, .70);
    chart.points.back().radius = 15.0;
    chart.points.back().stroke_width = 6.0;
    Figure fig;
    fig.size(800, 700).quadrant(chart);
    const Scene scene = fig.build_scene();
    const auto boxes = text_boxes(scene);
    RectF plot;
    std::vector<CircleItem> dots;
    for (const auto& item : collect_items(scene.root)) {
        if (const auto* r = std::get_if<RectItem>(item.item); r && r->style.stroke)
            plot = r->rect;
        if (const auto* c = std::get_if<CircleItem>(item.item)) dots.push_back(*c);
    }
    REQUIRE(dots.size() == chart.points.size());
    for (std::size_t i = 0; i < chart.points.size(); ++i) {
        const auto& p = chart.points[i];
        const RectF box = boxes.at(p.label);
        inside(box, plot);
        CHECK_APPROX(dots[i].center.x, plot.x + plot.w * p.x);
        CHECK_APPROX(dots[i].center.y, plot.y + plot.h * (1.0 - p.y));
        for (const auto& [name, other] : boxes)
            if (name != p.label) CHECK(intersection(box, other) < 0.001);
        for (const auto& dot : dots) {
            const double radius = dot.radius + (dot.style.stroke ? dot.style.stroke_width / 2 : 0);
            CHECK(intersection(box, {dot.center.x - radius, dot.center.y - radius,
                                     radius * 2, radius * 2}) < 0.001);
        }
    }
    // Headings are labels too: no text may cover any marker in this fixture.
    for (const auto& [name, box] : boxes) {
        (void)name;
        for (const auto& dot : dots) {
            const double radius = dot.radius + (dot.style.stroke ? dot.style.stroke_width / 2 : 0);
            CHECK(intersection(box, {dot.center.x - radius, dot.center.y - radius,
                                     radius * 2, radius * 2}) < 0.001);
        }
    }
    CHECK_EQ(SvgRenderer().render(fig.build_scene()), SvgRenderer().render(fig.build_scene()));
}

TEST_CASE("coincident and impossible quadrant labels remain intact and deterministic") {
    for (int count : {3, 200}) {
        QuadrantChart chart;
        for (int i = 0; i < count; ++i)
            chart.point(std::to_string(i), "Coincident point " + std::to_string(i), .5, .5);
        Figure fig;
        fig.size(560, 500).quadrant(chart);
        const Scene scene = fig.build_scene();
        const auto boxes = text_boxes(scene);
        CHECK_EQ(boxes.size(), static_cast<std::size_t>(count));
        for (const auto& [name, box] : boxes) {
            (void)name;
            CHECK(std::isfinite(box.x));
            CHECK(std::isfinite(box.y));
            inside(box, {0, 0, scene.width, scene.height});
        }
        if (count == 3)
            for (const auto& [a, box] : boxes)
                for (const auto& [b, other] : boxes)
                    if (a != b) CHECK(intersection(box, other) < 0.001);
        CHECK_EQ(SvgRenderer().render(fig.build_scene()), SvgRenderer().render(fig.build_scene()));
    }
}

TEST_CASE("rotated category tick bounds fit ordinary and reversed subplot cells") {
    const std::vector<std::string> labels{
        "A long first category label", "Second category", "Third category",
        "Fourth category", "Fifth category", "A long final category label"};
    for (bool reversed : {false, true}) {
        Figure fig;
        fig.size(1200, 480).subplots(1, 2);
        for (std::size_t panel = 0; panel < 2; ++panel) {
            auto& axes = fig.axes(0, panel);
            axes.bar(labels, {1, 2, 3, 4, 5, 6});
            axes.x_axis().reverse(reversed);
        }
        const Scene scene = fig.build_scene();
        std::size_t count = 0;
        for (const auto& placed : collect_items(scene.root)) {
            const auto* text = std::get_if<TextItem>(placed.item);
            if (!text || std::find(labels.begin(), labels.end(), text->text) == labels.end()) continue;
            CHECK(text->rotation < 0);
            const RectF box = text_bounds(*text, default_text_measurer());
            inside(box, {text->pos.x < 600 ? 0.0 : 600.0, 0, 600, 480});
            ++count;
        }
        CHECK_EQ(count, labels.size() * 2);
    }
}

TEST_CASE("unrotated long endpoint labels fit without unnecessary rotation") {
    Figure fig;
    fig.size(1000, 400);
    const std::vector<std::string> labels{"A fairly long first category", "Middle",
                                         "A fairly long last category"};
    fig.axes().bar(labels, {1, 2, 3});
    const Scene scene = fig.build_scene();
    for (const auto& placed : collect_items(scene.root)) {
        const auto* text = std::get_if<TextItem>(placed.item);
        if (!text || std::find(labels.begin(), labels.end(), text->text) == labels.end()) continue;
        CHECK_EQ(text->rotation, 0.0);
        inside(text_bounds(*text, default_text_measurer()), {0, 0, 1000, 400});
    }
}
