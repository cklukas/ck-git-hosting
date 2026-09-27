// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include <cworks/microtest.hpp>
#include <cdiagram/diagram.hpp>
#include <cdiagram/render.hpp>
#include <cplot/svg.hpp>
#include "mermaid.hpp"

#include <set>
#include <string>

TEST_CASE("ckdocs renders all 23 types in both colour schemes") {
    CHECK_EQ(cdiagram::catalogue().size(), 23u);
    std::set<cdiagram::DiagramType> types;
    for (const auto& entry : cdiagram::catalogue()) {
        types.insert(entry.type);
        for (const auto& source : {entry.example, entry.minimal}) {
            const auto result = ckgit::renderMermaid(source);
            CHECK(result.warnings.empty());
            CHECK(!result.alternative_text.empty());
            CHECK(result.light_svg.find("<svg") != std::string::npos);
            CHECK(result.dark_svg.find("<svg") != std::string::npos);
            CHECK(result.light_svg != result.dark_svg);
            CHECK_EQ(result.light_svg, cplot::SvgRenderer().render(cdiagram::render(source)));
            CHECK_EQ(result.dark_svg, ckgit::renderMermaid(source).dark_svg);
        }
    }
    CHECK_EQ(types.size(), 23u);
}

TEST_CASE("ckdocs preserves parser diagnostics and enforces input bounds") {
    CHECK(!ckgit::renderMermaid("packet-beta\n0-7: \"A\"\ntitle ignored\n").warnings.empty());
    CHECK_THROWS_AS(ckgit::renderMermaid("unknownDiagram\n"), std::exception);
    CHECK(cdiagram::detect_type("simflow\n") == cdiagram::DiagramType::Unknown);
    CHECK_THROWS_AS(ckgit::renderMermaid("simflow\n"), std::exception);
    CHECK_THROWS_AS(ckgit::renderMermaid(std::string(64 * 1024 + 1, 'x')), std::exception);
    CHECK_THROWS_AS(ckgit::renderMermaid(std::string(1025, '\n')), std::exception);
    CHECK_THROWS_AS(ckgit::renderMermaid(std::string("flowchart TD\nA\0B", 16)), std::exception);
}

TEST_CASE("ckdocs refuses numeric ranges that would expand tiny inputs excessively") {
    CHECK_THROWS_AS(ckgit::renderMermaid("packet-beta\n0-2147483647: \"A\"\n"), std::exception);
    CHECK_THROWS_AS(ckgit::renderMermaid("---\nconfig:\n  packet:\n    bitsPerRow: 1e30\n---\npacket-beta\n0-7: \"A\"\n"), std::exception);
    CHECK_THROWS_AS(ckgit::renderMermaid("block-beta\ncolumns 2147483648\nA\n"), std::exception);
    CHECK_THROWS_AS(ckgit::renderMermaid("gantt\nBig :2026-01-01, 100000000000d\n"), std::exception);
}

TEST_CASE("ckdocs escapes labels and titles without introducing active markup") {
    const auto result = ckgit::renderMermaid("---\ntitle: '<script>alert(1)</script>'\n---\nflowchart LR\nA[<img src=x onerror=bad>] --> B[Safe]\n");
    CHECK(result.light_svg.find("<script>") == std::string::npos);
    CHECK(result.light_svg.find("<img ") == std::string::npos);
    CHECK(result.light_svg.find("&lt;script&gt;") != std::string::npos);
    CHECK_THROWS_AS(ckgit::renderMermaid("flowchart LR\nA --> B\nclick A \"javascript:alert(1)\"\n"), std::exception);
    CHECK_THROWS_AS(ckgit::renderMermaid("---\nlink: 'data:text/html,hello'\n---\nflowchart LR\nA --> B\n"), std::exception);
}
