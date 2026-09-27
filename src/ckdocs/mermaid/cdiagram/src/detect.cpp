// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Diagram-type detection: classify a source by the first token of its
// first significant line. The catalogue (catalogue.cpp) owns the
// canonical keyword, title, and family per type; this table only maps
// every accepted spelling — including aliases the catalogue does not
// list — to a type, so `graph`, `xychart`, `C4Container`, etc. still
// classify.

#include "cdiagram/diagram.hpp"

#include "source.hpp"

namespace cdiagram {

namespace {

struct Entry {
    const char* keyword;
    DiagramType type;
};

// The detector matches the first whitespace-delimited token of the
// first significant line against this table. `stateDiagram-v2` and
// `stateDiagram` both map to State; `flowchart` and `graph` to
// Flowchart. Order is irrelevant (exact-token match). The size is
// deduced from the initializer, so entries can be added without a
// hand-kept count.
constexpr Entry kTable[] = {
    {"flowchart", DiagramType::Flowchart},
    {"graph", DiagramType::Flowchart},
    {"sequenceDiagram", DiagramType::Sequence},
    {"classDiagram", DiagramType::Class},
    {"stateDiagram", DiagramType::State},
    {"stateDiagram-v2", DiagramType::State},
    {"erDiagram", DiagramType::Er},
    {"pie", DiagramType::Pie},
    {"gantt", DiagramType::Gantt},
    {"journey", DiagramType::Journey},
    {"mindmap", DiagramType::Mindmap},
    {"timeline", DiagramType::Timeline},
    {"quadrantChart", DiagramType::Quadrant},
    {"gitGraph", DiagramType::Git},
    {"xychart-beta", DiagramType::XyChart},
    {"xychart", DiagramType::XyChart},
    {"kanban", DiagramType::Kanban},
    {"packet-beta", DiagramType::Packet},
    {"packet", DiagramType::Packet},
    {"requirementDiagram", DiagramType::Requirement},
    {"radar-beta", DiagramType::Radar},
    {"radar", DiagramType::Radar},
    {"treemap-beta", DiagramType::Treemap},
    {"treemap", DiagramType::Treemap},
    {"block-beta", DiagramType::Block},
    {"block", DiagramType::Block},
    {"C4Context", DiagramType::C4},
    {"C4Container", DiagramType::C4},
    {"C4Component", DiagramType::C4},
    {"C4Dynamic", DiagramType::C4},
    {"C4Deployment", DiagramType::C4},
    {"sankey-beta", DiagramType::Sankey},
    {"sankey", DiagramType::Sankey},
    {"architecture-beta", DiagramType::Architecture},
    {"architecture", DiagramType::Architecture},
    {"zenuml", DiagramType::ZenUml},
};

std::string first_token(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    return line.substr(0, i);
}

} // namespace

DiagramType detect_type(std::string_view source) {
    const std::vector<detail::SourceLine> lines = detail::significant_lines(source);
    if (lines.empty()) return DiagramType::Unknown;
    const std::string token = first_token(lines.front().text);
    for (const Entry& e : kTable) {
        if (token == e.keyword) return e.type;
    }
    return DiagramType::Unknown;
}

} // namespace cdiagram
