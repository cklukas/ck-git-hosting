// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The diagram catalogue: the single, ordered table of every diagram type
// the engine renders, with its family, human title, one-line summary,
// "use it for" suggestion, and idiomatic starter bodies. cwrite's
// Insert-Diagram dialog and the documentation gallery both read this, so
// the list, the grouping, and the prose live in exactly one place. The
// examples are small, idiomatic, and render cleanly (a golden test pins
// that every one passes `check`).

#include "cdiagram/diagram.hpp"

namespace cdiagram {

namespace {

CatalogEntry make(DiagramType type, DiagramFamily family, std::string name,
                  std::string keyword, std::string title, std::string summary,
                  std::string use_case, std::string example, std::string minimal,
                  std::vector<std::string> chart_types = {}) {
    return CatalogEntry{type,
                        family,
                        std::move(name),
                        std::move(keyword),
                        std::move(title),
                        std::move(summary),
                        std::move(use_case),
                        std::move(example),
                        std::move(minimal),
                        std::move(chart_types)};
}

// The catalogue, in presentation order: families in display order
// (Graph, Sequence, Charts, Project, Data & flow, Layout), types in
// their within-family order. This is the order the dialog and the docs
// show. `keyword` is the token the `example` begins with (the detector
// also accepts aliases — see detect.cpp).
const std::vector<CatalogEntry>& entries() {
    static const std::vector<CatalogEntry> data = {
        // -- Graph --------------------------------------------------------
        make(DiagramType::Flowchart, DiagramFamily::Graph, "flowchart",
             "flowchart", "Flowchart",
             "Boxes and decisions connected by arrows, laid out "
             "top-to-bottom or left-to-right.",
             "Processes, decision logic, and workflows.",
             R"mmd(flowchart TD
    A([Start]) --> B[Collect input]
    B --> C{Valid?}
    C -->|yes| D[Process]
    C -->|no| E[Report error]
    D --> F([Done])
    E --> B)mmd",
             R"mmd(flowchart TD
    A[Start] --> B[Next]
    B --> C[End])mmd"),
        make(DiagramType::Class, DiagramFamily::Graph, "class",
             "classDiagram", "Class diagram",
             "UML classes with attributes and methods, connected by "
             "inheritance, composition, aggregation, and association.",
             "Object models and type hierarchies.",
             R"mmd(classDiagram
    Animal <|-- Dog
    Animal <|-- Cat
    Animal : +String name
    Animal : +int age
    Animal : +makeSound() void
    Dog : +fetch() void)mmd",
             R"mmd(classDiagram
    class A
    class B
    A <|-- B)mmd"),
        make(DiagramType::State, DiagramFamily::Graph, "state",
             "stateDiagram-v2", "State diagram",
             "A state machine: states and the transitions between them, "
             "with [*] start/end markers.",
             "Lifecycles and protocols.",
             R"mmd(stateDiagram-v2
    [*] --> Idle
    Idle --> Running : start
    Running --> Idle : stop
    Running --> [*] : shutdown)mmd",
             R"mmd(stateDiagram-v2
    [*] --> A
    A --> [*])mmd"),
        make(DiagramType::Er, DiagramFamily::Graph, "er",
             "erDiagram", "Entity-relationship",
             "Database entities with attributes, linked by relationships "
             "with crow's-foot cardinality.",
             "Data models and schemas.",
             R"mmd(erDiagram
    CUSTOMER ||--o{ ORDER : places
    ORDER ||--|{ LINE_ITEM : contains
    CUSTOMER {
        string name
        string email
    })mmd",
             R"mmd(erDiagram
    A ||--o{ B : has)mmd"),
        make(DiagramType::Requirement, DiagramFamily::Graph, "requirement",
             "requirementDiagram", "Requirement diagram",
             "SysML requirements and elements linked by typed "
             "relationships (satisfies, traces, verifies).",
             "Systems engineering and traceability.",
             R"mmd(requirementDiagram
    requirement test_req {
        id: 1
        risk: high
    }
    element test_entity {
        type: simulation
    }
    test_entity - satisfies -> test_req)mmd",
             R"mmd(requirementDiagram
    requirement req {
        id: 1
    }
    element e {
        type: test
    }
    e - satisfies -> req)mmd"),
        make(DiagramType::C4, DiagramFamily::Graph, "c4",
             "C4Context", "C4 diagram",
             "C4-model architecture: people, systems and containers with "
             "relationships.",
             "Software architecture context diagrams.",
             R"mmd(C4Context
    Person(user, "Customer", "A user of the site")
    System(web, "Web App", "Serves pages")
    System(pay, "Payments", "Stripe")
    Rel(user, web, "uses")
    Rel(web, pay, "charges via"))mmd",
             R"mmd(C4Context
    Person(u, "User")
    System(s, "System")
    Rel(u, s, "uses"))mmd"),
        make(DiagramType::Mindmap, DiagramFamily::Graph, "mindmap",
             "mindmap", "Mindmap",
             "A hierarchical idea tree branching out from a central root.",
             "Brainstorming and outlines.",
             R"mmd(mindmap
    root((Project))
    Goals
        Ship v1
        Write docs
    Team
        Design
        Engineering)mmd",
             R"mmd(mindmap
    root((Idea))
        Branch A
        Branch B)mmd"),
        // -- Sequence -----------------------------------------------------
        make(DiagramType::Sequence, DiagramFamily::Sequence, "sequence",
             "sequenceDiagram", "Sequence diagram",
             "Interactions between participants over time — messages flow "
             "top to bottom along vertical lifelines.",
             "API calls, protocols, and step-by-step workflows.",
             R"mmd(sequenceDiagram
    participant U as User
    participant S as Server
    participant DB as Database
    U->>S: POST /login
    S->>DB: lookup user
    DB-->>S: user record
    S-->>U: 200 OK + token
    Note over U,S: session established)mmd",
             R"mmd(sequenceDiagram
    participant A
    participant B
    A->>B: message)mmd"),
        make(DiagramType::ZenUml, DiagramFamily::Sequence, "zenuml",
             "zenuml", "ZenUML sequence",
             "A method-call sequence written in the ZenUML DSL, rendered "
             "as a sequence diagram.",
             "Code-like sequence descriptions and method-call flows.",
             R"mmd(zenuml
    title Order flow
    User->Web: place order
    Web->API.createOrder()
    API->DB: insert
    API->Web: 201
    Web->User: confirmation)mmd",
             R"mmd(zenuml
    A->B: hello
    B->A: hi)mmd"),
        // -- Charts -------------------------------------------------------
        make(DiagramType::Pie, DiagramFamily::Chart, "pie",
             "pie", "Pie chart",
             "A proportional pie chart — each labelled slice is sized by "
             "its value.",
             "Parts of a whole: market share, a budget split, survey "
             "results.",
             R"mmd(pie showData title The Turkish Empire, as Playfair drew it in 1801
    "European" : 25
    "Asiatic" : 60
    "African" : 15)mmd",
             R"mmd(pie title Untitled
    "A" : 1
    "B" : 1)mmd",
             {"pie"}),
        make(DiagramType::XyChart, DiagramFamily::Chart, "xychart",
             "xychart-beta", "XY chart",
             "Bars and/or a line over category and value axes.",
             "Revenue, counts, and trends over a small set of categories.",
             R"mmd(xychart-beta
    title "Monthly revenue"
    x-axis [Jan, Feb, Mar, Apr]
    y-axis "Revenue (k)" 0 --> 100
    bar [30, 55, 45, 80]
    line [20, 40, 38, 70])mmd",
             R"mmd(xychart-beta
    title "Chart"
    x-axis [A, B, C]
    bar [3, 5, 4])mmd",
             {"bar", "line"}),
        make(DiagramType::Radar, DiagramFamily::Chart, "radar",
             "radar-beta", "Radar chart",
             "One polygon per series over shared radial axes.",
             "Comparing multi-dimensional profiles: skills, scores, specs.",
             R"mmd(radar-beta
    title Skills
    axis m["Math"], p["Physics"], c["Chemistry"], b["Biology"]
    curve a["Alice"]{85, 90, 80, 70}
    curve o["Bob"]{70, 85, 95, 90})mmd",
             R"mmd(radar-beta
    title Radar
    axis a["A"], b["B"], c["C"]
    curve x["X"]{3, 5, 4})mmd",
             {"radar"}),
        make(DiagramType::Quadrant, DiagramFamily::Chart, "quadrant",
             "quadrantChart", "Quadrant chart",
             "Points plotted in a 2x2 matrix with labelled axes and "
             "quadrants.",
             "Prioritisation and positioning.",
             R"mmd(quadrantChart
    title Reach vs Engagement
    x-axis Low Reach --> High Reach
    y-axis Low Engagement --> High Engagement
    quadrant-1 Expand
    quadrant-2 Promote
    quadrant-3 Re-evaluate
    quadrant-4 Improve
    Campaign A: [0.3, 0.6]
    Campaign B: [0.7, 0.8])mmd",
             R"mmd(quadrantChart
    title Quadrant
    x-axis Low --> High
    y-axis Low --> High
    Item A: [0.4, 0.5])mmd",
             {"quadrant"}),
        // -- Project ------------------------------------------------------
        make(DiagramType::Gantt, DiagramFamily::Project, "gantt",
             "gantt", "Gantt chart",
             "A project schedule: tasks with start dates or dependencies "
             "and durations, grouped in sections, drawn as bars on a date "
             "grid.",
             "Project plans and schedules with dependencies.",
             R"mmd(gantt
    title Project plan
    dateFormat YYYY-MM-DD
    section Design
    Research :a1, 2024-01-01, 7d
    Mockups :a2, after a1, 5d
    section Build
    Backend :2024-01-10, 10d
    Frontend :after a2, 8d)mmd",
             R"mmd(gantt
    title Plan
    section Work
    First :a1, 2024-01-01, 3d
    Second :after a1, 2d)mmd"),
        make(DiagramType::Timeline, DiagramFamily::Project, "timeline",
             "timeline", "Timeline",
             "Events grouped by time period along a horizontal axis.",
             "Histories, roadmaps, and chronologies.",
             R"mmd(timeline
    title Company history
    2019 : Founded
    2021 : Series A : First hire
    2023 : IPO)mmd",
             R"mmd(timeline
    title Timeline
    2020 : Start
    2021 : Next)mmd"),
        make(DiagramType::Journey, DiagramFamily::Project, "journey",
             "journey", "User journey",
             "Steps a user takes, each scored 1-5 for satisfaction and "
             "tagged with actors, grouped into sections.",
             "UX flows and experience mapping.",
             R"mmd(journey
    title Shopping trip
    section Browse
    Search: 4: Me
    Compare: 3: Me
    section Buy
    Checkout: 2: Me
    Confirm: 5: Me)mmd",
             R"mmd(journey
    title Journey
    section Do
    Task: 3: Me)mmd"),
        make(DiagramType::Kanban, DiagramFamily::Project, "kanban",
             "kanban", "Kanban board",
             "Cards arranged in workflow columns.",
             "Task boards and simple project status.",
             R"mmd(kanban
    todo[To Do]
        t1[Design API]
        t2[Write specs]
    doing[In Progress]
        t3[Build engine]
    done[Done]
        t4[Kickoff])mmd",
             R"mmd(kanban
    todo[To Do]
        t1[A task]
    done[Done])mmd"),
        // -- Data & flow --------------------------------------------------
        make(DiagramType::Sankey, DiagramFamily::DataFlow, "sankey",
             "sankey-beta", "Sankey diagram",
             "Weighted flows between nodes, with band widths proportional "
             "to magnitude.",
             "Energy, budget, and conversion flows.",
             R"mmd(sankey-beta
Coal,Electricity,25
Gas,Electricity,15
Electricity,Homes,20
Electricity,Industry,20)mmd",
             R"mmd(sankey-beta
A,B,10
B,C,5)mmd",
             {"sankey"}),
        make(DiagramType::Treemap, DiagramFamily::DataFlow, "treemap",
             "treemap-beta", "Treemap",
             "Nested rectangles sized by value.",
             "Proportions within a hierarchy: storage, budget, categories.",
             R"mmd(treemap-beta
"Products"
    "Hardware"
        "Laptops": 40
        "Phones": 30
    "Software"
        "Apps": 20
        "Cloud": 50)mmd",
             R"mmd(treemap-beta
"Root"
    "A": 3
    "B": 5)mmd",
             {"treemap"}),
        make(DiagramType::Packet, DiagramFamily::DataFlow, "packet",
             "packet-beta", "Packet diagram",
             "Named bit fields on a 32-bit grid.",
             "Network protocols and binary formats.",
             R"mmd(packet-beta
    0-15: "Source Port"
    16-31: "Destination Port"
    32-63: "Sequence Number"
    64-95: "Acknowledgment Number")mmd",
             R"mmd(packet-beta
    0-7: "Type"
    8-15: "Length")mmd"),
        // -- Layout -------------------------------------------------------
        make(DiagramType::Block, DiagramFamily::Layout, "block",
             "block-beta", "Block diagram",
             "Blocks flowed into a fixed-column grid, with optional column "
             "spans.",
             "Simple layouts and system sketches.",
             R"mmd(block-beta
    columns 3
    a["Frontend"] b["API"] c["Cache"]
    d["Auth"]:2 e["DB"])mmd",
             R"mmd(block-beta
    columns 2
    a["A"] b["B"])mmd"),
        make(DiagramType::Architecture, DiagramFamily::Layout, "architecture",
             "architecture-beta", "Architecture",
             "Cloud/service architecture: services grouped into boxes and "
             "connected by edges.",
             "Deployment and system topology.",
             // The examples stay icon-free: icon names in `service db(database)`
             // are valid Mermaid but not rendered (they are diagnosed), and the
             // showcase examples demonstrate the supported dialect.
             R"mmd(architecture-beta
    group api[API]
    service db[Database] in api
    service server[Server] in api
    service gateway[Gateway]
    db:R -- L:server
    gateway:B --> T:server)mmd",
             R"mmd(architecture-beta
    service a[Service A]
    service b[Service B]
    a:R --> L:b)mmd"),
        make(DiagramType::Git, DiagramFamily::Layout, "git",
             "gitGraph", "Git graph",
             "Commits on branch lanes with branch and merge links.",
             "Illustrating branching workflows.",
             R"mmd(gitGraph
    commit
    commit tag: "v0.1"
    branch develop
    commit
    checkout main
    commit
    merge develop tag: "v1.0")mmd",
             R"mmd(gitGraph
    commit
    branch dev
    commit
    checkout main
    merge dev)mmd"),
    };
    return data;
}

} // namespace

const std::vector<CatalogEntry>& catalogue() { return entries(); }

const CatalogEntry* catalog_entry(DiagramType type) {
    for (const CatalogEntry& e : entries())
        if (e.type == type) return &e;
    return nullptr;
}

const std::vector<DiagramFamily>& families() {
    static const std::vector<DiagramFamily> order = {
        DiagramFamily::Graph,    DiagramFamily::Sequence,
        DiagramFamily::Chart,    DiagramFamily::Project,
        DiagramFamily::DataFlow, DiagramFamily::Layout,
    };
    return order;
}

DiagramFamily type_family(DiagramType type) {
    const CatalogEntry* e = catalog_entry(type);
    return e ? e->family : DiagramFamily::Graph;
}

std::string_view family_title(DiagramFamily family) {
    switch (family) {
    case DiagramFamily::Graph: return "Graph";
    case DiagramFamily::Sequence: return "Sequence";
    case DiagramFamily::Chart: return "Charts";
    case DiagramFamily::Project: return "Project";
    case DiagramFamily::DataFlow: return "Data & flow";
    case DiagramFamily::Layout: return "Layout";
    }
    return "";
}

std::string_view family_id(DiagramFamily family) {
    switch (family) {
    case DiagramFamily::Graph: return "graph";
    case DiagramFamily::Sequence: return "sequence";
    case DiagramFamily::Chart: return "chart";
    case DiagramFamily::Project: return "project";
    case DiagramFamily::DataFlow: return "data-flow";
    case DiagramFamily::Layout: return "layout";
    }
    return "";
}

std::string_view family_summary(DiagramFamily family) {
    switch (family) {
    case DiagramFamily::Graph:
        return "Node-and-edge structural diagrams, laid out by the "
               "engine's layered (Sugiyama/dagre-style) graph layout — for "
               "logic, models, and architecture.";
    case DiagramFamily::Sequence:
        return "Interactions between participants over time, drawn along "
               "vertical lifelines.";
    case DiagramFamily::Chart:
        return "Quantitative charts — proportions, trends, and "
               "multi-dimensional comparisons — from Mermaid's chart "
               "syntaxes.";
    case DiagramFamily::Project:
        return "Planning and process views over time: schedules, "
               "histories, journeys, and boards.";
    case DiagramFamily::DataFlow:
        return "Quantities moving through or nested within a system — "
               "flows and hierarchies sized by magnitude.";
    case DiagramFamily::Layout:
        return "Free-form structural sketches: blocks, service "
               "topologies, and commit histories on a grid or lanes.";
    }
    return "";
}

std::string type_keyword(DiagramType type) {
    const CatalogEntry* e = catalog_entry(type);
    return e ? e->keyword : "";
}

std::string type_title(DiagramType type) {
    const CatalogEntry* e = catalog_entry(type);
    return e ? e->title : "Unknown";
}

} // namespace cdiagram
