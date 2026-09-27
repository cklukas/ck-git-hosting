// ckdiagram — V1-03 diagnostics: every parser reports, never drops in silence
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The suite charter forbids silently coercing or dropping input. Before
// this, only flowchart and sequence routed unrecognized lines through the
// diagnostics channel; the other 21 parsers silently skipped — or worse,
// fabricated bogus nodes/edges from directives and typos (e.g. classDiagram
// turning `classDef red fill:#f00` into a class box). Each case here feeds a
// valid header plus one unrecognized / recognized-but-unrendered / misparse-
// prone line and asserts a diagnostic naming it. The companion guarantee —
// that VALID syntax stays diagnostic-free — is enforced for every diagram
// type by test_catalogue.cpp ("check(example).empty()").
#include <string>
#include <vector>

#include <cworks/microtest.hpp>

#include "cdiagram/render.hpp"

namespace {

struct Case {
    const char* name;    ///< diagram type
    const char* source;  ///< a diagram with one offending line
    const char* needle;  ///< text the diagnostic must name
};

const std::vector<Case>& cases() {
    static const std::vector<Case> all = {
        {"architecture",
         "architecture-beta\nservice db(database)[DB]\ngrop api\n", "grop"},
        {"block",
         "block-beta\n  columns 2\n  A[\"S\"]\n  A --> B\n", "edges"},
        {"c4",
         "C4Context\nPerson(user, \"User\")\nPrson(bad, \"Typo\")\n", "Prson"},
        {"class",
         "classDiagram\n  Animal <|-- Dog\n  note \"free text\"\n", "note"},
        {"er",
         "erDiagram\n  CUSTOMER ||--o{ ORDER : places\n  @@@\n", "@@@"},
        {"gantt",
         "gantt\n  title X\n  section S\n  bogusline\n", "bogusline"},
        {"gitgraph",
         "gitGraph\n  commit\n  comit\n", "comit"},
        {"journey",
         "journey\n  title My day\n  Wake: 5: Me\n  accTitle: X\n", "accTitle"},
        {"kanban",
         "kanban\n  col[To Do]\n  t1[Task]@{ assigned: bob }\n", "@{"},
        {"mindmap",
         "mindmap\n  root\n    ::icon(fa fa-book)\n", "::icon"},
        {"packet",
         "packet-beta\n  0-7: A\n  7-0: B\n", "7-0"},
        {"pie",
         "pie title Pets\n  \"A\" : 1\n  foobar\n", "foobar"},
        {"quadrant",
         "quadrantChart\n  title X\n  style p fill:#f00 [0.5, 0.5]\n", "style"},
        {"radar",
         "radar-beta\n  title X\n  axis a, b, c\n  titled Q1\n", "titled"},
        {"requirement",
         "requirementDiagram\nrequirement test_req {\nid: 1\ntext: t\n}\nzzz garble\n", "zzz"},
        {"sankey",
         "sankey-beta\nA,B,5\nbad,row\n", "bad"},
        {"state",
         "stateDiagram-v2\n  [*] --> A\n  note left of A : x\n", "note"},
        {"timeline",
         "timeline\n  title X\n  section S\n  classDef bad fill:#f00\n", "classDef"},
        {"treemap",
         "treemap-beta\n\"Root\"\n  \"Child\": 5\nclassDef big fill:#f00\n", "classDef"},
        {"xychart",
         "xychart-beta\n  title X\n  x-axis [a, b]\n  bar [1, 2]\n  lines [3, 4]\n", "lines"},
        {"zenuml",
         "zenuml\n  A->B\n  @@@nonsense@@@\n", "@@@"},
        // S0 correctness sweep: constructs that previously misparsed or
        // vanished in silence now surface as diagnostics.
        {"flowchart trailing tail",
         "flowchart TD\n  A --> B & C\n", "&"},
        {"flowchart @{} icon metadata",
         "flowchart TD\n  A@{ icon: \"aws:s3\" }\n", "icon"},
        {"sequence participant lifecycle",
         "sequenceDiagram\n  A->>B: hi\n  destroy B\n", "destroy"},
        {"gitgraph unknown commit type",
         "gitGraph\n  commit type: WEIRD\n", "WEIRD"},
        {"gitgraph cherry-pick unknown id",
         "gitGraph\n  commit id: \"a\"\n  cherry-pick id: \"nope\"\n", "nope"},
        {"mindmap cloud shape",
         "mindmap\n  root\n    c)Cloud label(\n", "cloud"},
        {"mindmap inline :::",
         "mindmap\n  root\n    task:::urgent\n", ":::urgent"},
        {"quadrant point style tail",
         "quadrantChart\n  A: [0.2, 0.4] glow: 5\n", "glow"},
        {"architecture icon",
         "architecture-beta\n  service db(database)[DB]\n", "database"},
    };
    return all;
}

bool any_message_contains(const cworks::Diagnostics& d, const std::string& needle) {
    for (const cworks::Diagnostic& diag : d)
        if (diag.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("every parser reports its offending line rather than dropping it") {
    for (const Case& c : cases()) {
        const cworks::Diagnostics d = cdiagram::check(c.source);
        if (d.empty()) {
            microtest::report_failure(__FILE__, __LINE__,
                                      std::string(c.name) + ": expected a diagnostic",
                                      "got none");
            continue;
        }
        if (!any_message_contains(d, c.needle)) {
            std::string got;
            for (const cworks::Diagnostic& x : d) got += "\n  " + x.message;
            microtest::report_failure(__FILE__, __LINE__,
                                      std::string(c.name) + ": a diagnostic names '" +
                                          c.needle + "'",
                                      "got:" + got);
        } else {
            CHECK(true);
        }
    }
}
