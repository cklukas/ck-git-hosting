// ckdiagram — Mermaid front-matter configuration tests
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include <cworks/microtest.hpp>

#include <string>

#include <cplot/svg.hpp>

#include "cdiagram/render.hpp"

using cdiagram::render;

namespace {

std::string svg(const std::string& source) {
    return cplot::SvgRenderer().render(render(source));
}

} // namespace

TEST_CASE("front matter title draws a band above any diagram") {
    const std::string flow = "flowchart TD\n  A[Go] --> B[Stop]\n";
    const cplot::Scene plain = render(flow);
    const cplot::Scene titled = render("---\ntitle: Front matter title\n---\n" + flow);
    CHECK(titled.height > plain.height);
    CHECK_EQ(titled.meta_title, "Front matter title");
    CHECK(svg("---\ntitle: Front matter title\n---\n" + flow).find("Front matter title") !=
          std::string::npos);
    // A builder-drawn title wins; the band is not added twice.
    const cplot::Scene both =
        render("---\ntitle: Outer\n---\npie title Pets\n  \"Dogs\" : 5\n");
    CHECK(svg("---\ntitle: Outer\n---\npie title Pets\n  \"Dogs\" : 5\n").find("Outer") ==
          std::string::npos);
    CHECK_EQ(both.meta_title, "Pets");
}

TEST_CASE("front matter sizes the chart-family canvas") {
    const cplot::Scene sized = render(
        "---\nconfig:\n  sankey:\n    width: 1200\n    height: 600\n---\n"
        "sankey-beta\nA,B,10\n");
    CHECK_EQ(sized.width, 1200.0);
    CHECK_EQ(sized.height, 600.0);
    // Unusable values fall back to the defaults instead of failing.
    const cplot::Scene guarded = render(
        "---\nconfig:\n  sankey:\n    width: 5\n---\nsankey-beta\nA,B,10\n");
    CHECK_EQ(guarded.width, 800.0);
}

TEST_CASE("sankey shows node totals by default, honours prefix/suffix and showValues") {
    const std::string source = "sankey-beta\nA,B,1500\n";
    CHECK(svg(source).find("A 1500") != std::string::npos);
    CHECK(svg("---\nconfig:\n  sankey:\n    prefix: \"$\"\n    suffix: M\n---\n" + source)
              .find("A $1500M") != std::string::npos);
    CHECK(svg("---\nconfig:\n  sankey:\n    showValues: false\n---\n" + source)
              .find("A 1500") == std::string::npos);
}

TEST_CASE("packet and flowchart spacing keys reshape the layout") {
    const std::string packet = "packet-beta\n0-31: \"Word\"\n";
    CHECK(render("---\nconfig:\n  packet:\n    bitsPerRow: 8\n---\n" + packet).width <
          render(packet).width);
    CHECK(svg("---\nconfig:\n  packet:\n    showBits: false\n---\n" + packet).find(">31<") ==
          std::string::npos);
    const std::string branchy = "flowchart TD\n  A --> B\n  A --> C\n";
    CHECK(render("---\nconfig:\n  flowchart:\n    nodeSpacing: 140\n---\n" + branchy).width >
          render(branchy).width);
}

TEST_CASE("gitgraph and timeline front-matter options") {
    const std::string git = "gitGraph\n  commit\n  commit\n";
    CHECK(svg("---\nconfig:\n  gitGraph:\n    mainBranchName: trunk\n---\n" + git)
              .find("trunk") != std::string::npos);
    CHECK(svg("---\nconfig:\n  gitGraph:\n    showBranches: false\n---\n" + git)
              .find("main") == std::string::npos);
    const std::string timeline = "timeline\n  2024 : one\n  2025 : two\n";
    const std::string mono =
        svg("---\nconfig:\n  timeline:\n    disableMulticolor: true\n---\n" + timeline);
    CHECK(mono.find("<svg") != std::string::npos);
}
