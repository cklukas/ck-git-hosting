// ckdiagram — golden SVG tests: fixed sources, deterministic SVG output
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Each case renders a pinned Mermaid source to a cplot::Scene and emits SVG
// through cplot::SvgRenderer, then checks it two ways — byte-stable on a
// re-render everywhere, and byte-identical to the committed golden on unix.
// Both guarantees, the reason the second is unix-only, and how to regenerate
// are documented once in golden_check.hpp, which every suite here shares.
#include <string>
#include <vector>

#include <cplot/svg.hpp>
#include <cworks/microtest.hpp>

#include "cdiagram/render.hpp"
#include "golden_check.hpp"

namespace {

struct GoldenCase {
    const char* name;
    const char* source;
};

// A representative diagram from each major family (graph / sequence /
// class / proportion / data flow): nodes+edges+labels, lifelines+messages,
// UML compartments, pie slices (arcs), and a Sankey. The Sankey case is
// Mermaid's official "Energy flow" demo (the canonical d3-sankey UK energy
// dataset) — 46 nodes and 68 links exercise every hard layout path at
// once: deep chains, column-skipping links, wide fan-in/fan-out, hairline
// values next to huge ones, and crowded label columns.
const std::vector<GoldenCase>& cases() {
    static const std::vector<GoldenCase> all = {
        {"flowchart",
         "flowchart TD\n"
         "  A[Start] --> B{Choice}\n"
         "  B -->|yes| C[Go]\n"
         "  B -->|no| D[Stop]\n"},
        {"sequence",
         "sequenceDiagram\n"
         "  Alice->>Bob: Hello\n"
         "  Bob-->>Alice: Hi\n"},
        {"class",
         "classDiagram\n"
         "  Animal <|-- Dog\n"
         "  Animal : +String name\n"
         "  Dog : +fetch() void\n"},
        {"pie",
         "pie title Pets\n"
         "  \"Dogs\" : 5\n"
         "  \"Cats\" : 3\n"},
        {"flowchart-styling",
         "flowchart LR\n"
         "  subgraph pipe[Pipeline]\n"
         "    A[/Input/]:::hot -->|load| B{{Check}}\n"
         "    B --o C[(Store)]\n"
         "  end\n"
         "  C x--x D>Review]\n"
         "  D <--> E(((Hub)))\n"
         "  A ~~~ E\n"
         "  classDef hot fill:#f96,stroke:#333\n"
         "  classDef default stroke-width:1.2\n"
         "  style D fill:#bbf,stroke-width:3px\n"
         "  style pipe fill:#eef7ee,stroke:#5a5\n"
         "  linkStyle 0 stroke:#e45756,color:#e45756\n"},
        // C3: the three things a cluster has to get right. `flowchart-cluster`
        // pins the grouping CONSTRAINT — X fans out to A, M and B, and the
        // frame may enclose A and B without swallowing M, which only holds
        // if the ordering pass kept the members contiguous.
        // `flowchart-cluster-edge` pins the endpoints: an edge into a
        // subgraph id and one out of it, both landing on the frame, with the
        // id used before it is declared. `flowchart-cluster-nested` pins
        // nesting: two label bands stacked, an edge from outside straight
        // into the inner frame, and one leaving the outer one.
        {"flowchart-cluster",
         "flowchart TD\n"
         "  X[Ingest] --> A[Parse]\n"
         "  X --> M[Metrics]\n"
         "  X --> B[Validate]\n"
         "  subgraph core[Core Pipeline]\n"
         "    A\n"
         "    B\n"
         "  end\n"
         "  A --> Z[Report]\n"
         "  B --> Z\n"
         "  M --> Z\n"},
        {"flowchart-cluster-edge",
         "flowchart LR\n"
         "  C[Client] --> api\n"
         "  subgraph api[API Tier]\n"
         "    R[Router] --> H{Handler}\n"
         "  end\n"
         "  api --> D[(Store)]\n"
         "  style api fill:#eef7ee,stroke:#5a5\n"},
        {"flowchart-cluster-nested",
         "flowchart TD\n"
         "  E[Edge Router] --> mesh\n"
         "  subgraph plat[Platform]\n"
         "    subgraph mesh[Service Mesh]\n"
         "      P[Proxy] --> R[Route]\n"
         "    end\n"
         "    R --> K[(Cache)]\n"
         "  end\n"
         "  plat --> O[Observability]\n"},
        {"sankey-energy",
         "sankey-beta\n"
         "Agricultural 'waste',Bio-conversion,124.729\n"
         "Bio-conversion,Liquid,0.597\n"
         "Bio-conversion,Losses,26.862\n"
         "Bio-conversion,Solid,280.322\n"
         "Bio-conversion,Gas,81.144\n"
         "Biofuel imports,Liquid,35\n"
         "Biomass imports,Solid,35\n"
         "Coal imports,Coal,11.606\n"
         "Coal reserves,Coal,63.965\n"
         "Coal,Solid,75.571\n"
         "District heating,Industry,10.639\n"
         "District heating,Heating and cooling - commercial,22.505\n"
         "District heating,Heating and cooling - homes,46.184\n"
         "Electricity grid,Over generation / exports,104.453\n"
         "Electricity grid,Heating and cooling - homes,113.726\n"
         "Electricity grid,H2 conversion,27.14\n"
         "Electricity grid,Industry,342.165\n"
         "Electricity grid,Road transport,37.797\n"
         "Electricity grid,Agriculture,4.412\n"
         "Electricity grid,Heating and cooling - commercial,40.858\n"
         "Electricity grid,Losses,56.691\n"
         "Electricity grid,Rail transport,7.863\n"
         "Electricity grid,Lighting & appliances - commercial,90.008\n"
         "Electricity grid,Lighting & appliances - homes,93.494\n"
         "Gas imports,Ngas,40.719\n"
         "Gas reserves,Ngas,82.233\n"
         "Gas,Heating and cooling - commercial,0.129\n"
         "Gas,Losses,1.401\n"
         "Gas,Thermal generation,151.891\n"
         "Gas,Agriculture,2.096\n"
         "Gas,Industry,48.58\n"
         "Geothermal,Electricity grid,7.013\n"
         "H2 conversion,H2,20.897\n"
         "H2 conversion,Losses,6.242\n"
         "H2,Road transport,20.897\n"
         "Hydro,Electricity grid,6.995\n"
         "Liquid,Industry,121.066\n"
         "Liquid,International shipping,128.69\n"
         "Liquid,Road transport,135.835\n"
         "Liquid,Domestic aviation,14.458\n"
         "Liquid,International aviation,206.267\n"
         "Liquid,Agriculture,3.64\n"
         "Liquid,National navigation,33.218\n"
         "Liquid,Rail transport,4.413\n"
         "Marine algae,Bio-conversion,4.375\n"
         "Ngas,Gas,122.952\n"
         "Nuclear,Thermal generation,839.978\n"
         "Oil imports,Oil,504.287\n"
         "Oil reserves,Oil,107.703\n"
         "Oil,Liquid,611.99\n"
         "Other waste,Solid,56.587\n"
         "Other waste,Bio-conversion,77.81\n"
         "Pumped heat,Heating and cooling - homes,193.026\n"
         "Pumped heat,Heating and cooling - commercial,70.672\n"
         "Solar PV,Electricity grid,59.901\n"
         "Solar Thermal,Heating and cooling - homes,19.263\n"
         "Solar,Solar Thermal,19.263\n"
         "Solar,Solar PV,59.901\n"
         "Solid,Agriculture,0.882\n"
         "Solid,Thermal generation,400.12\n"
         "Solid,Industry,46.477\n"
         "Thermal generation,Electricity grid,525.531\n"
         "Thermal generation,Losses,787.129\n"
         "Thermal generation,District heating,79.329\n"
         "Tidal,Electricity grid,9.452\n"
         "UK land based bioenergy,Bio-conversion,182.01\n"
         "Wave,Electricity grid,19.013\n"
         "Wind,Electricity grid,289.366\n"},
    };
    return all;
}

} // namespace

TEST_CASE("SVG output is valid and deterministic (byte-exact vs golden on unix)") {
    for (const GoldenCase& c : cases()) {
        const std::string actual = cplot::SvgRenderer().render(cdiagram::render(c.source));

        // Everywhere: a valid SVG document, byte-stable on a re-render.
        CHECK(actual.rfind("<?xml", 0) == 0);
        CHECK(actual.find("<svg") != std::string::npos);
        CHECK_EQ(actual, cplot::SvgRenderer().render(cdiagram::render(c.source)));

        CHECK_GOLDEN(c.name, actual);
    }
}
