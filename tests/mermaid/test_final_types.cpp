// ckdiagram — architecture / zenuml tests + parallel-edge separation
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

TEST_CASE("architecture and zenuml keywords are detected") {
    CHECK(detect_type("architecture-beta\n") == DiagramType::Architecture);
    CHECK(detect_type("zenuml\n") == DiagramType::ZenUml);
}

TEST_CASE("architecture groups services and draws edges") {
    const std::string src =
        "architecture-beta\n"
        "  group g(cloud)[Cluster]\n"
        "  service a(server)[A] in g\n"
        "  service b(database)[B] in g\n"
        "  service c(internet)[C]\n"
        "  a:R -- L:b\n"
        "  c:T --> B:a\n";
    const cplot::Scene scene = render(src);
    // 3 service boxes + 1 group box (rounded-rect paths)
    CHECK(count<cplot::PathItem>(scene) >= 4u);
    CHECK(scene.width > 0.0);
    check_deterministic(src);
}

TEST_CASE("zenuml renders method-call interactions as a sequence") {
    const std::string src =
        "zenuml\n  User->Web: open\n  Web->API.load()\n  API->Web: data\n";
    const cplot::Scene scene = render(src);
    // 3 participants -> lifelines; messages -> lines + arrowheads
    CHECK(count<cplot::LineItem>(scene) >= 3u);
    CHECK(count<cplot::PathItem>(scene) >= 3u);     // participant boxes (rounded-rect paths)
    // the method name reaches the label
    CHECK(cplot::SvgRenderer().render(scene).find("load()") != std::string::npos);
    check_deterministic(src);
}

TEST_CASE("parallel edges are separated, not overlapping") {
    // A bidirectional pair must produce distinct (bent) routes.
    const std::string src = "flowchart LR\n  A --> B\n  B --> A\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find("<svg") != std::string::npos);
    // Two arrowheads (one per direction) survive.
    std::size_t polys = 0, pos = 0;
    while ((pos = svg.find("<polygon", pos)) != std::string::npos) {
        ++polys;
        pos += 8;
    }
    CHECK(polys >= 2u);
}

TEST_CASE("final types report errors on empty bodies") {
    CHECK_THROWS_AS(render("architecture-beta\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("zenuml\n"), cdiagram::Error);
}
