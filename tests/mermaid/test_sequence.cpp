// ckdiagram — sequence diagram tests
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

} // namespace

TEST_CASE("sequence renders participants, lifelines and messages") {
    const std::string src =
        "sequenceDiagram\n"
        "  participant A as Alice\n"
        "  participant B as Bob\n"
        "  A->>B: Hello Bob\n"
        "  B-->>A: Hi Alice\n";
    const cplot::Scene scene = render(src);
    REQUIRE(!scene.root.children.empty());
    // two participant boxes (rounded-rect paths) + lifelines
    CHECK(count<cplot::LineItem>(scene) >= 2u);   // 2 lifelines + message lines
    CHECK(count<cplot::PathItem>(scene) >= 2u);     // the boxes
    CHECK(count<cplot::PolygonItem>(scene) >= 1u);  // the arrowheads (still flat)
    CHECK(count<cplot::TextItem>(scene) >= 4u);   // 2 names + 2 message labels
    CHECK(scene.width > 0.0);
    CHECK(scene.height > 0.0);
}

TEST_CASE("sequence auto-creates undeclared participants in first-seen order") {
    const cplot::Scene scene = render("sequenceDiagram\n  Alice->>John: hi\n");
    // Two boxes (each a rounded-rect path) even without declarations.
    CHECK(count<cplot::PathItem>(scene) >= 2u);
}

TEST_CASE("sequence handles notes and self-messages") {
    const std::string src =
        "sequenceDiagram\n"
        "  A->>A: think\n"
        "  Note over A: a private thought\n"
        "  Note left of A: aside\n";
    const cplot::Scene scene = render(src);
    // note boxes are RectItems
    CHECK(count<cplot::RectItem>(scene) >= 2u);
}

TEST_CASE("sequence output is deterministic") {
    const std::string src =
        "sequenceDiagram\n  participant A\n  participant B\n  A->>B: ping\n  B->>A: pong\n";
    const std::string a = cplot::SvgRenderer().render(render(src));
    const std::string b = cplot::SvgRenderer().render(render(src));
    CHECK_EQ(a, b);
    CHECK(a.find("<svg") != std::string::npos);
}

TEST_CASE("sequence widens gaps to fit long labels (no fixed width)") {
    const cplot::Scene narrow = render("sequenceDiagram\n  A->>B: hi\n");
    const cplot::Scene wide = render(
        "sequenceDiagram\n  A->>B: a very long message label that needs room\n");
    CHECK(wide.width > narrow.width);
}

TEST_CASE("sequence reports syntax errors") {
    CHECK_THROWS_AS(render("sequenceDiagram\n  A oops B\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("sequenceDiagram\n  Note over A a thought\n"), cdiagram::Error);
    CHECK_THROWS_AS(render("sequenceDiagram\n"), cdiagram::Error);  // no participants
}

TEST_CASE("sequence alt/else block renders a labeled frame (C8)") {
    const std::string src =
        "sequenceDiagram\n"
        "  A->>B: request\n"
        "  alt ok\n"
        "    B-->>A: yes\n"
        "  else fail\n"
        "    B-->>A: no\n"
        "  end\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find(">alt<") != std::string::npos);
    CHECK(svg.find(">[ok]<") != std::string::npos);   // condition label
    CHECK(svg.find(">[fail]<") != std::string::npos); // else compartment
    CHECK_EQ(svg, cplot::SvgRenderer().render(render(src)));
}

TEST_CASE("sequence loop and opt blocks render (C8)") {
    const std::string loop =
        cplot::SvgRenderer().render(render("sequenceDiagram\n  A->>B: x\n"
                                           "  loop every day\n    B-->>A: y\n  end\n"));
    CHECK(loop.find(">loop<") != std::string::npos);
    const std::string opt = cplot::SvgRenderer().render(
        render("sequenceDiagram\n  opt maybe\n    A->>B: x\n  end\n"));
    CHECK(opt.find(">opt<") != std::string::npos);
}

TEST_CASE("sequence activation bars render for activate/deactivate (C8)") {
    const cplot::Scene plain = render("sequenceDiagram\n  A->>B: x\n  B-->>A: y\n");
    const cplot::Scene activ = render(
        "sequenceDiagram\n  A->>B: x\n  activate B\n  B-->>A: y\n  deactivate B\n");
    // The activation bar is an extra rectangle on B's lifeline.
    CHECK(count<cplot::RectItem>(activ) > count<cplot::RectItem>(plain));
}

TEST_CASE("sequence activation shorthand +/- renders bars (C8)") {
    const cplot::Scene plain = render("sequenceDiagram\n  A->>B: x\n  B-->>A: y\n");
    const cplot::Scene activ = render("sequenceDiagram\n  A->>+B: x\n  B-->>-A: y\n");
    CHECK(count<cplot::RectItem>(activ) > count<cplot::RectItem>(plain));
}

TEST_CASE("check() reports unsupported sequence directives (C1)") {
    const cworks::Diagnostics d =
        cdiagram::check("sequenceDiagram\n  A->>B: x\n  create participant D\n");
    REQUIRE(!d.empty());
    CHECK(d[0].severity == cworks::Diagnostic::Severity::Warning);
    CHECK(d[0].message.find("create") != std::string::npos);
    // A valid alt + activation diagram reports nothing.
    CHECK(cdiagram::check("sequenceDiagram\n  A->>+B: x\n"
                          "  alt ok\n    B-->>-A: y\n  end\n")
              .empty());
}

TEST_CASE("sequence box groups, rect bands, and autonumber render (S4)") {
    const std::string src =
        "sequenceDiagram\n"
        "    box rgb(200,220,255) Team A\n"
        "    participant Alice\n"
        "    participant Bob\n"
        "    end\n"
        "    autonumber\n"
        "    rect rgb(255,230,200)\n"
        "    Alice->>Bob: inside band\n"
        "    end\n"
        "    Bob-->>Alice: reply\n";
    const std::string svg = cplot::SvgRenderer().render(render(src));
    CHECK(svg.find(">Team A<") != std::string::npos);
    CHECK(svg.find("1. inside band") != std::string::npos);
    CHECK(svg.find("2. reply") != std::string::npos);
    CHECK(svg.find("#FFE6C8") != std::string::npos);  // the band tint
    CHECK(cdiagram::check(src).empty());
    // critical/break blocks share the frame machinery.
    CHECK(cdiagram::check("sequenceDiagram\n  critical fetch\n    A->>B: q\n"
                          "  option timeout\n    B-->>A: late\n  end\n"
                          "  break failure\n    A->>B: stop\n  end\n")
              .empty());
}

TEST_CASE("rect transparent draws no tint; bad autonumber args are diagnosed (review)") {
    const std::string svg = cplot::SvgRenderer().render(
        render("sequenceDiagram\n  rect transparent\n  A->>B: hi\n  end\n"));
    CHECK(svg.find("hi") != std::string::npos);
    CHECK(cdiagram::check("sequenceDiagram\n  rect transparent\n  A->>B: hi\n  end\n").empty());
    const cworks::Diagnostics d =
        cdiagram::check("sequenceDiagram\n  autonumber nope\n  A->>B: x\n");
    CHECK(!d.empty());
}
