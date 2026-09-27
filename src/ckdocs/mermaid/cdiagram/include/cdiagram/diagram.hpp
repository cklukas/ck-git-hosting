// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The catalogue of Mermaid diagram types: the single source of truth
// the whole suite reads from. It carries, per type, the family it
// belongs to, a human title, a one-line summary, a "use it for"
// suggestion, and idiomatic starter bodies (example + minimal). cwrite's
// Insert-Diagram dialog and the documentation gallery both consume this
// catalogue, so the type list, its grouping, and its descriptions can
// never drift between engine, editor, and docs.
//
// A cheap detector (`detect_type`) classifies a source by its leading
// keyword without parsing the body — so the editor and the dialog can
// label a diagram without running the full engine. Types the engine does
// not yet lay out still classify (so an error names the type precisely
// rather than "unknown diagram").
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cdiagram {

/// A Mermaid diagram type. This is an internal tag, not a wire identity:
/// nothing outside the library sees these values, so a new type is simply
/// appended before the `Unknown` sentinel and the recognised types stay one
/// contiguous run. Across the C ABI a diagram type is named by its catalogue
/// slug instead (see cdiagram_app.h). The presentation order (menus, docs) is
/// `catalogue()` order, grouped by family.
enum class DiagramType {
    Flowchart,   ///< `flowchart` / `graph` — layered directed graph
    Sequence,    ///< `sequenceDiagram` — actor lifelines over time
    Class,       ///< `classDiagram` — UML classes + relationships
    State,       ///< `stateDiagram` / `stateDiagram-v2`
    Er,          ///< `erDiagram` — entity/relationship model
    Pie,         ///< `pie` — proportional slices
    Gantt,       ///< `gantt` — task schedule on a date grid
    Journey,     ///< `journey` — user-journey scores
    Mindmap,     ///< `mindmap` — hierarchical idea tree
    Timeline,    ///< `timeline` — events in time bands
    Quadrant,    ///< `quadrantChart` — 2x2 scatter
    Git,         ///< `gitGraph` — commit DAG on branch lanes
    XyChart,     ///< `xychart-beta` — bar/line chart on axes
    Kanban,      ///< `kanban` — cards in workflow columns
    Packet,      ///< `packet-beta` — bit-field layout
    Requirement, ///< `requirementDiagram` — requirements + relationships
    Radar,       ///< `radar-beta` — radar/spider chart
    Treemap,     ///< `treemap-beta` — nested rectangles by value
    Block,       ///< `block-beta` — blocks in a grid
    C4,          ///< `C4Context`/… — C4 architecture
    Sankey,      ///< `sankey-beta` — weighted flow
    Architecture, ///< `architecture-beta` — cloud/service architecture
    ZenUml,      ///< `zenuml` — method-call sequence DSL
    Unknown,     ///< the leading keyword matched nothing — always last
};

/// The family a diagram type belongs to. `families()` returns these in
/// display order; the family is how the dialog and the docs group types.
enum class DiagramFamily {
    Graph,     ///< node/edge structural diagrams (layered layout)
    Sequence,  ///< interactions over lifelines
    Chart,     ///< quantitative charts
    Project,   ///< schedules, histories, journeys, boards
    DataFlow,  ///< flows and hierarchies sized by magnitude
    Layout,    ///< free-form structural sketches
};

/// One catalogue entry: everything the suite needs to present a diagram
/// type. `name` is the stable slug used as a template key (and as a user
/// diagram-template name); `keyword` is the canonical Mermaid keyword the
/// `example` begins with. `summary` says what the diagram is; `use_case`
/// says when to reach for it. `example` is idiomatic starter content;
/// `minimal` is the bare skeleton. `chart_types` names the equivalent
/// cplot YAML series or whole-canvas chart types when this Mermaid form is
/// a text syntax adapter over the shared chart engine; it is empty for
/// structural diagrams and for superficially similar but different models.
struct CatalogEntry {
    DiagramType type;
    DiagramFamily family;
    std::string name;
    std::string keyword;
    std::string title;
    std::string summary;
    std::string use_case;
    std::string example;
    std::string minimal;
    std::vector<std::string> chart_types;
};

/// The whole catalogue, in presentation order: families in display order,
/// and types in their intended within-family order. This one ordering
/// drives the Insert-Diagram dialog and the documentation gallery.
const std::vector<CatalogEntry>& catalogue();

/// The catalogue entry for `type`, or nullptr for DiagramType::Unknown.
const CatalogEntry* catalog_entry(DiagramType type);

/// The families, in display order.
const std::vector<DiagramFamily>& families();

/// The family of `type` (DiagramFamily::Graph as a harmless fallback for
/// DiagramType::Unknown, which has no family).
DiagramFamily type_family(DiagramType type);

/// Family metadata. Each view is a string literal: it is NUL-terminated and
/// valid for the process lifetime, so a C boundary can hand the pointer out
/// as a borrowed string instead of restating the table.

/// A human title for a family ("Graph", "Charts", "Data & flow", …).
std::string_view family_title(DiagramFamily family);

/// A stable slug for a family ("graph", "chart", "data-flow", …) — the key
/// a consumer groups by, and the name used for documentation filenames and
/// anchors.
std::string_view family_id(DiagramFamily family);

/// A one-paragraph introduction to a family, for the docs and dialog.
std::string_view family_summary(DiagramFamily family);

/// Classify `source` by its first significant line (comments, blank
/// lines and a leading YAML front-matter block are skipped). Returns
/// DiagramType::Unknown when no keyword matches.
DiagramType detect_type(std::string_view source);

/// The canonical Mermaid keyword for a type ("flowchart", "pie", …).
/// Unknown maps to "".
std::string type_keyword(DiagramType type);

/// A human title for menus and messages ("Flowchart", "Pie chart", …).
/// Unknown maps to "Unknown".
std::string type_title(DiagramType type);

} // namespace cdiagram
