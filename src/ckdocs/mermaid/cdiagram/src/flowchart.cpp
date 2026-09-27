// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Flowchart (`flowchart`/`graph`): nodes with shapes and labelled edges,
// laid out with the layered (dagre-style) engine in dag_layout. Supports
// the full classic shape set ([] rect, () round, ([]) stadium, {} diamond,
// (()) circle, ((())) double circle, {{}} hexagon, [()] cylinder,
// [[]] subroutine, >..] flag, [/../] and [\..\] parallelograms, [/..\]
// and [\../] trapezoids), the four directions (TD/TB, BT, LR, RL),
// chained edges (A --> B --> C), both edge-label forms (`A -->|x| B` and
// `A -- x --> B`), the edge variants (dotted, thick, invisible `~~~`,
// bidirectional `<-->`, circle/cross terminals `--o`/`--x`), subgraphs
// (styleable by id), and the styling directives (`style`,
// `classDef`/`class`/`:::` with a `default` class, `linkStyle`).

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "frontmatter.hpp"
#include "canvas.hpp"
#include "cdiagram/error.hpp"
#include "dag_layout.hpp"
#include "diagnostics.hpp"
#include "links.hpp"
#include "placement_layout.hpp"
#include "styleprops.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram::detail {

namespace {

enum class Shape {
    Rect, Round, Stadium, Subroutine, Cylinder, Circle, DoubleCircle, Diamond,
    Hexagon, Flag, LeanRight, LeanLeft, Trapezoid, TrapezoidAlt
};
enum class EdgeStyle { Solid, Dotted, Thick };
/// Edge terminals: Mermaid's arrow (`>`), circle (`o`), and cross (`x`)
/// ends; None is an open line end.
enum class EdgeMarker { None, Arrow, Circle, Cross };


struct Node {
    std::string id;
    std::string label;
    Shape shape = Shape::Rect;
    bool shaped = false;               ///< the source gave it a shape/label of its own
    std::size_t shaped_line = 0;       ///< …on this line
    int cluster = -1;                  ///< C3: innermost subgraph, -1 = none
    std::vector<std::string> classes;  ///< C2: class names applied via `class`/`:::`
    StyleProps direct_style;           ///< C2: `style <id> ...` overrides
    StyleProps resolved;               ///< C2: classes merged, then direct_style
};
struct Edge {
    /// C3: either end may name a subgraph instead of a node; the link then
    /// lands on that subgraph's frame.
    DagEnd from;
    DagEnd to;
    std::string label;
    EdgeStyle style = EdgeStyle::Solid;
    EdgeMarker start = EdgeMarker::None;  ///< marker at the source end
    EdgeMarker end = EdgeMarker::Arrow;   ///< marker at the target end
    bool hidden = false;                  ///< `~~~`: shapes layout, draws nothing
    StyleProps link;      ///< C2: `linkStyle` override for this edge
    std::size_t line = 0;  ///< source line, for diagnostics
};
/// C3: one `subgraph … end` cluster; `parent` gives nesting (-1 = top).
struct Cluster {
    std::string id;      ///< edge-addressable subgraph id (empty if none)
    std::string title;
    int parent = -1;
    std::vector<std::string> classes;  ///< C2: classes applied via `class`
    StyleProps direct_style;           ///< C2: `style <subgraphId> ...`
    StyleProps resolved;               ///< C2: classes merged, then direct
};
struct FlowModel {
    FlowDir dir = FlowDir::Down;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<Cluster> clusters;                 ///< C3
    std::map<std::string, StyleProps> class_defs;  ///< C2: classDef table
    LinkTable links;                               ///< node id -> hyperlink (click …)
};

/// How much tint ONE cluster frame at `depth` paints, so that the tint
/// accumulated through its ancestors lands on a bounded ladder rather than
/// compounding toward black.
///
/// A frame paints over the frames enclosing it, so a fixed per-frame alpha
/// accumulates: at the 32% sty.grid carries, two nested frames reached 54%
/// and three reached 69%, and the muted title fell from an already-marginal
/// 3.1:1 contrast to 1.5:1 and then 1.2:1 — a heading nobody can read.
///
/// Each level therefore adds kPerLevel of the REMAINING headroom until the
/// total reaches kCap, after which a deeper frame adds nothing and is read
/// from its stroke and title alone. kPerLevel is the weight kanban gives a
/// lane, which is the same visual job. kCap is where the muted title still
/// clears the 4.5:1 that text of its size needs: 0.18 leaves it at 4.6:1 on
/// white, and every shallower level is lighter still.
constexpr double kClusterTintPerLevel = 0.06;
constexpr double kClusterTintCap = 0.18;

double cluster_tint_total(int depth) {
    // 1-(1-a)^(depth+1) is the accumulation of depth+1 coats of alpha a.
    return std::min(kClusterTintCap,
                    1.0 - std::pow(1.0 - kClusterTintPerLevel, depth + 1));
}

double cluster_tint_alpha(int depth) {
    const double below = depth > 0 ? cluster_tint_total(depth - 1) : 0.0;
    if (below >= 1.0) return 0.0;
    // What this coat must carry to lift `below` to the total for `depth`.
    return (cluster_tint_total(depth) - below) / (1.0 - below);
}

bool ident_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}
bool link_char(char c) {
    return c == '-' || c == '=' || c == '.' || c == '<' || c == '>' || c == 'x' ||
           c == 'o' || c == '~';
}

int node_index(FlowModel& model, const std::string& id) {
    for (std::size_t i = 0; i < model.nodes.size(); ++i)
        if (model.nodes[i].id == id) return static_cast<int>(i);
    Node node;
    node.id = id;
    node.label = id;
    model.nodes.push_back(std::move(node));
    return static_cast<int>(model.nodes.size()) - 1;
}

void skip_spaces(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
}

/// Opening → closing shape delimiters, longest first.
struct Delim {
    const char* open;
    const char* close;
    Shape shape;
};
const std::vector<Delim>& delimiters() {
    // `[/` and `[\` each open two shapes (the closer decides which), so
    // parse_node_ref keeps trying later entries when a matched opener's
    // closer is absent from the line.
    static const std::vector<Delim> d = {
        {"(((", ")))", Shape::DoubleCircle},
        {"([", "])", Shape::Stadium},   {"[[", "]]", Shape::Subroutine},
        {"[(", ")]", Shape::Cylinder},  {"((", "))", Shape::Circle},
        {"{{", "}}", Shape::Hexagon},   {"[/", "/]", Shape::LeanRight},
        {"[/", "\\]", Shape::Trapezoid}, {"[\\", "\\]", Shape::LeanLeft},
        {"[\\", "/]", Shape::TrapezoidAlt},
        {"[", "]", Shape::Rect},        {"(", ")", Shape::Round},
        {"{", "}", Shape::Diamond},     {">", "]", Shape::Flag},
    };
    return d;
}

/// Map a Mermaid v11 `@{ shape: <name> }` name onto the classic shape set.
/// Names whose geometry the engine does not draw distinctly return nullopt
/// (the caller diagnoses and falls back to a rectangle).
std::optional<Shape> shape_by_name(const std::string& name) {
    static const std::map<std::string, Shape> names = {
        {"rect", Shape::Rect},           {"rectangle", Shape::Rect},
        {"proc", Shape::Rect},           {"process", Shape::Rect},
        {"rounded", Shape::Round},       {"event", Shape::Round},
        {"stadium", Shape::Stadium},     {"pill", Shape::Stadium},
        {"terminal", Shape::Stadium},
        {"subroutine", Shape::Subroutine}, {"subproc", Shape::Subroutine},
        {"subprocess", Shape::Subroutine}, {"fr-rect", Shape::Subroutine},
        {"framed-rectangle", Shape::Subroutine},
        {"cyl", Shape::Cylinder},        {"cylinder", Shape::Cylinder},
        {"database", Shape::Cylinder},   {"db", Shape::Cylinder},
        {"circle", Shape::Circle},       {"circ", Shape::Circle},
        {"dbl-circ", Shape::DoubleCircle}, {"double-circle", Shape::DoubleCircle},
        {"diam", Shape::Diamond},        {"diamond", Shape::Diamond},
        {"decision", Shape::Diamond},    {"question", Shape::Diamond},
        {"hex", Shape::Hexagon},         {"hexagon", Shape::Hexagon},
        {"prepare", Shape::Hexagon},
        {"lean-r", Shape::LeanRight},    {"lean-right", Shape::LeanRight},
        {"in-out", Shape::LeanRight},
        {"lean-l", Shape::LeanLeft},     {"lean-left", Shape::LeanLeft},
        {"out-in", Shape::LeanLeft},
        {"trap-b", Shape::Trapezoid},    {"trapezoid", Shape::Trapezoid},
        {"trapezoid-bottom", Shape::Trapezoid}, {"priority", Shape::Trapezoid},
        {"trap-t", Shape::TrapezoidAlt}, {"trapezoid-top", Shape::TrapezoidAlt},
        {"inv-trapezoid", Shape::TrapezoidAlt}, {"manual", Shape::TrapezoidAlt},
        {"odd", Shape::Flag},            {"flag", Shape::Flag},
        {"paper-tape", Shape::Flag},
    };
    const auto it = names.find(name);
    if (it == names.end()) return std::nullopt;
    return it->second;
}

/// Parse a node reference at `i`: an identifier and an optional shape
/// with a label. Returns the node index, or -1 when there is no id.
int parse_node_ref(FlowModel& model, const std::string& s, std::size_t& i,
                   std::size_t line_no, const RenderOptions& options) {
    skip_spaces(s, i);
    const std::size_t start = i;
    while (i < s.size() && ident_char(s[i])) ++i;
    if (i == start) return -1;
    const std::string id = s.substr(start, i - start);
    const int index = node_index(model, id);

    const char* unclosed = nullptr;
    for (const Delim& d : delimiters()) {
        const std::string open = d.open;
        if (s.compare(i, open.size(), open) != 0) continue;
        std::size_t j = i + open.size();
        std::string label;
        if (j < s.size() && s[j] == '"') {
            const std::size_t q = s.find('"', j + 1);
            if (q == std::string::npos)
                throw Error(cworks::validation_failed("flowchart (line " + std::to_string(line_no) +
                            "): unterminated quoted label"));
            label = s.substr(j + 1, q - j - 1);
            j = q + 1;
        }
        const std::string close = d.close;
        const std::size_t c = s.find(close, j);
        if (c == std::string::npos) {
            // Another delimiter with the same opener may still close this
            // node (`[/..\]` vs `[/../]`); remember the miss and keep going.
            if (!unclosed) unclosed = d.close;
            continue;
        }
        if (label.empty()) label = cworks::trim(s.substr(j, c - j));
        model.nodes[static_cast<std::size_t>(index)].label = label;
        model.nodes[static_cast<std::size_t>(index)].shape = d.shape;
        model.nodes[static_cast<std::size_t>(index)].shaped = true;
        model.nodes[static_cast<std::size_t>(index)].shaped_line = line_no;
        i = c + close.size();
        unclosed = nullptr;
        break;
    }
    if (unclosed)
        throw Error(cworks::validation_failed("flowchart (line " + std::to_string(line_no) +
                                              "): node '" + id + "' is missing its closing '" +
                                              std::string(unclosed) + "'"));
    // C2: inline class assignments, `A:::a` (repeatable: `A:::a:::b`).
    while (s.compare(i, 3, ":::") == 0) {
        std::size_t j = i + 3;
        const std::size_t cstart = j;
        while (j < s.size() && ident_char(s[j])) ++j;
        if (j == cstart) break;
        model.nodes[static_cast<std::size_t>(index)].classes.push_back(
            s.substr(cstart, j - cstart));
        i = j;
    }
    // v11 node metadata: `A@{ shape: cyl, label: "Store" }`. Shape names
    // map onto the classic set; icons, images, and animation are
    // recognised Mermaid the engine does not render.
    if (s.compare(i, 2, "@{") == 0) {
        const std::size_t close = s.find('}', i + 2);
        if (close == std::string::npos)
            throw Error(cworks::validation_failed("flowchart (line " + std::to_string(line_no) +
                                                  "): node '" + id + "' has an unterminated '@{'"));
        const std::string body = s.substr(i + 2, close - i - 2);
        i = close + 1;
        for (const std::string& entry : split_style_entries(body)) {
            const std::size_t colon = entry.find(':');
            if (colon == std::string::npos) {
                diagnose_unsupported(options, "flowchart", line_no, entry,
                                     "expected 'key: value' node metadata");
                continue;
            }
            const std::string key = cworks::trim(entry.substr(0, colon));
            const std::string value = cworks::trim(entry.substr(colon + 1));
            if (key == "shape") {
                if (const std::optional<Shape> shape = shape_by_name(value)) {
                    model.nodes[static_cast<std::size_t>(index)].shape = *shape;
                } else {
                    diagnose_unsupported(options, "flowchart", line_no, value,
                                         "shape is not drawn distinctly (rendered as a "
                                         "rectangle)");
                    model.nodes[static_cast<std::size_t>(index)].shape = Shape::Rect;
                }
            } else if (key == "label") {
                model.nodes[static_cast<std::size_t>(index)].label = unquote(value);
            } else {
                diagnose_unsupported(options, "flowchart", line_no, key,
                                     "icons, images, and animation are not rendered");
            }
        }
    }
    return index;
}

/// Parse an edge link at `i`. Returns true and fills the edge style,
/// the terminal markers (`>` arrow, `o` circle, `x` cross at either end,
/// `<` an arrow at the source), the hidden flag (`~~~` links shape the
/// layout but draw nothing), and the label; false when there is no link.
bool parse_link(const std::string& s, std::size_t& i, EdgeStyle& style,
                EdgeMarker& start_marker, EdgeMarker& end_marker, bool& hidden,
                std::string& label) {
    skip_spaces(s, i);
    if (i >= s.size()) return false;
    const char first = s[i];
    // `o`/`x` open a link only when a dash/equals follows — otherwise they
    // are ordinary identifier characters of the next node.
    const bool marker_lead = (first == 'o' || first == 'x') && i + 1 < s.size() &&
                             (s[i + 1] == '-' || s[i + 1] == '=' || s[i + 1] == '.');
    if (first != '-' && first != '=' && first != '<' && first != '~' && !marker_lead)
        return false;
    const std::size_t lead_start = i;
    while (i < s.size() && link_char(s[i])) ++i;
    std::string lead = s.substr(lead_start, i - lead_start);
    label.clear();
    hidden = lead.find('~') != std::string::npos;
    start_marker = EdgeMarker::None;
    end_marker = EdgeMarker::None;
    if (!hidden) {
        if (lead.front() == '<') start_marker = EdgeMarker::Arrow;
        else if (lead.front() == 'o') start_marker = EdgeMarker::Circle;
        else if (lead.front() == 'x') start_marker = EdgeMarker::Cross;
        if (lead.back() == '>') end_marker = EdgeMarker::Arrow;
        else if (lead.back() == 'o') end_marker = EdgeMarker::Circle;
        else if (lead.back() == 'x') end_marker = EdgeMarker::Cross;
    }
    style = lead.find('.') != std::string::npos
                ? EdgeStyle::Dotted
                : (lead.find('=') != std::string::npos ? EdgeStyle::Thick
                                                       : EdgeStyle::Solid);

    if (i < s.size() && s[i] == '|') {  // pipe label: -->|text|
        const std::size_t close = s.find('|', i + 1);
        if (close == std::string::npos)
            throw Error(cworks::validation_failed("flowchart: unterminated edge label '|'"));
        label = cworks::trim(s.substr(i + 1, close - i - 1));
        i = close + 1;
    } else if (lead.size() == 2 && !hidden && start_marker == EdgeMarker::None &&
               end_marker == EdgeMarker::None) {  // middle label: -- text -->
        skip_spaces(s, i);
        const std::size_t text_start = i;
        while (i < s.size()) {
            // The tail run starts with '-' or '=' (`-->`, `--`, `-.->`,
            // `==>`, `--o`, `--x`) — letter runs like the 'ox' in
            // `A -- box --> B` are ordinary label text.
            if ((s[i] == '-' || s[i] == '=') && link_char(s[i])) {
                std::size_t j = i;
                while (j < s.size() && link_char(s[j])) ++j;
                if (j - i >= 2) break;  // the tail run
            }
            ++i;
        }
        label = cworks::trim(s.substr(text_start, i - text_start));
        const std::size_t tail_start = i;
        while (i < s.size() && link_char(s[i])) ++i;
        const std::string tail = s.substr(tail_start, i - tail_start);
        if (!tail.empty()) {
            if (tail.back() == '>') end_marker = EdgeMarker::Arrow;
            else if (tail.back() == 'o') end_marker = EdgeMarker::Circle;
            else if (tail.back() == 'x') end_marker = EdgeMarker::Cross;
        }
        if (tail.find('.') != std::string::npos) style = EdgeStyle::Dotted;
        else if (tail.find('=') != std::string::npos) style = EdgeStyle::Thick;
    }
    return true;
}

FlowDir parse_dir(std::string token) {
    // Legacy `graph LR;` writes a statement terminator after the token.
    while (!token.empty() && token.back() == ';') token.pop_back();
    if (token == "TD" || token == "TB") return FlowDir::Down;
    if (token == "BT") return FlowDir::Up;
    if (token == "LR") return FlowDir::Right;
    if (token == "RL") return FlowDir::Left;
    return FlowDir::Down;
}



FlowModel parse_flowchart(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    FlowModel model;
    {  // header: "flowchart TD" / "graph LR"
        const std::string& head = lines.front().text;
        std::size_t sp = head.find_first_of(" \t");
        if (sp != std::string::npos)
            model.dir = parse_dir(cworks::trim(head.substr(sp + 1)));
    }
    std::vector<int> stack;  // C3: open subgraph cluster ids (innermost last)
    std::map<std::string, int> sub_by_id;  // C3: subgraph id -> cluster index
    std::vector<std::pair<std::string, StyleProps>> pending_links;  // C2: linkStyle

    // Record `node` as a member of the innermost currently-open cluster.
    const auto note_membership = [&](int node) {
        if (stack.empty()) return;
        if (model.nodes[static_cast<std::size_t>(node)].cluster < 0)
            model.nodes[static_cast<std::size_t>(node)].cluster = stack.back();
    };

    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;

        // C3: subgraph … end grouping.
        if (line == "subgraph" || line.rfind("subgraph ", 0) == 0) {
            std::string rest = line.size() > 8 ? cworks::trim(line.substr(8)) : "";
            std::string id = rest;    // `subgraph id[Title]` or `subgraph Title`
            std::string title = rest;
            if (const std::size_t lb = rest.find('['); lb != std::string::npos) {
                id = cworks::trim(rest.substr(0, lb));
                const std::size_t rb = rest.find(']', lb);
                title = rb != std::string::npos ? rest.substr(lb + 1, rb - lb - 1)
                                                : rest.substr(lb + 1);
            }
            id = cworks::trim(id);
            const int ci = static_cast<int>(model.clusters.size());
            Cluster cluster;
            cluster.id = id;
            cluster.title = unquote(cworks::trim(title));
            cluster.parent = stack.empty() ? -1 : stack.back();
            model.clusters.push_back(std::move(cluster));
            // Only a single-word id is edge-addressable (an edge target is one
            // identifier); a bracketless multi-word title is not.
            if (!id.empty() && id.find_first_of(" \t") == std::string::npos)
                sub_by_id[id] = ci;
            stack.push_back(ci);
            continue;
        }
        if (line == "end") {
            if (!stack.empty()) stack.pop_back();
            else
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("flowchart", number, "'end' without a matching 'subgraph' (ignored)"));
            continue;
        }
        if (line.rfind("direction ", 0) == 0) {  // per-diagram/subgraph flow direction
            model.dir = parse_dir(cworks::trim(line.substr(10)));
            continue;
        }
        // C2: classDef <name[,name]> k:v,...
        if (line.rfind("classDef ", 0) == 0) {
            const std::string rest = cworks::trim(line.substr(9));
            const std::size_t sp = rest.find_first_of(" \t");
            if (sp == std::string::npos) {
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("flowchart", number, "classDef needs a name and properties (ignored)"));
                continue;
            }
            const StyleProps props =
                parse_style_props(cworks::trim(rest.substr(sp + 1)), options, "flowchart", number);
            for (const std::string& name : split_commas(rest.substr(0, sp)))
                model.class_defs[name].merge(props);
            continue;
        }
        // C2: class <id[,id]> <name>
        if (line.rfind("class ", 0) == 0) {
            const std::string rest = cworks::trim(line.substr(6));
            const std::size_t sp = rest.find_last_of(" \t");
            if (sp == std::string::npos) {
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("flowchart", number, "class needs node ids and a class name (ignored)"));
                continue;
            }
            const std::string cls = cworks::trim(rest.substr(sp + 1));
            for (const std::string& id : split_commas(rest.substr(0, sp))) {
                if (const auto sub = sub_by_id.find(id); sub != sub_by_id.end())
                    model.clusters[static_cast<std::size_t>(sub->second)].classes.push_back(cls);
                else
                    model.nodes[static_cast<std::size_t>(node_index(model, id))]
                        .classes.push_back(cls);
            }
            continue;
        }
        // C2: style <id> k:v,...  — the id may name a node or a subgraph
        // (a subgraph id styles the cluster frame, never a phantom node).
        if (line.rfind("style ", 0) == 0) {
            const std::string rest = cworks::trim(line.substr(6));
            const std::size_t sp = rest.find_first_of(" \t");
            if (sp == std::string::npos) {
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("flowchart", number, "style needs a node id and properties (ignored)"));
                continue;
            }
            const std::string target = rest.substr(0, sp);
            const StyleProps props =
                parse_style_props(cworks::trim(rest.substr(sp + 1)), options, "flowchart", number);
            if (const auto sub = sub_by_id.find(target); sub != sub_by_id.end())
                model.clusters[static_cast<std::size_t>(sub->second)].direct_style.merge(props);
            else
                model.nodes[static_cast<std::size_t>(node_index(model, target))]
                    .direct_style.merge(props);
            continue;
        }
        // C2: linkStyle <default|index[,index]> k:v,...  (applied after edges).
        if (line.rfind("linkStyle", 0) == 0) {
            const std::string rest = cworks::trim(line.substr(9));
            const std::size_t sp = rest.find_first_of(" \t");
            const std::string sel = sp == std::string::npos ? rest : rest.substr(0, sp);
            const std::string body = sp == std::string::npos ? "" : cworks::trim(rest.substr(sp + 1));
            pending_links.push_back({sel, parse_style_props(body, options, "flowchart", number)});
            continue;
        }
        // Interaction: `click <id> "url" ["tip"] [_target]` (and the `href`
        // form) attaches a hyperlink to the node. JS callback forms carry no
        // URL and cannot be exported, so they are reported, not linked.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!model.links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("flowchart", number,
                                     "node '" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "flowchart", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }

        std::size_t i = 0;
        int prev = parse_node_ref(model, line, i, number, options);
        if (prev < 0) {  // C1: not a node/edge statement and not a known directive
            const std::string token = line.substr(0, line.find_first_of(" \t"));
            diagnose_unrecognized(options, "flowchart", number, token,
                                  {"subgraph", "end", "direction", "classDef", "class", "style",
                                   "linkStyle", "click"});
            continue;
        }
        note_membership(prev);
        while (true) {
            const std::size_t save = i;
            EdgeStyle style = EdgeStyle::Solid;
            EdgeMarker start_marker = EdgeMarker::None;
            EdgeMarker end_marker = EdgeMarker::Arrow;
            bool hidden = false;
            std::string label;
            if (!parse_link(line, i, style, start_marker, end_marker, hidden, label)) {
                i = save;
                break;
            }
            int next = parse_node_ref(model, line, i, number, options);
            if (next < 0)
                throw Error(cworks::validation_failed("flowchart (line " + std::to_string(number) +
                            "): expected a node after the edge"));
            note_membership(next);
            Edge edge;
            edge.from = DagEnd(prev);
            edge.to = DagEnd(next);
            edge.label = label;
            edge.style = style;
            edge.start = start_marker;
            edge.end = end_marker;
            edge.hidden = hidden;
            edge.line = number;
            model.edges.push_back(std::move(edge));
            prev = next;
        }
        // The whole line must have been consumed: an unparsed tail is a
        // construct this engine does not lay out (`& B` node lists,
        // `~~~` invisible links, `@{...}` metadata) and silently
        // dropping it would misrepresent the diagram. Legacy statement
        // terminators (`A-->B;`) are consumed, not reported.
        skip_spaces(line, i);
        while (i < line.size() && line[i] == ';') {
            ++i;
            skip_spaces(line, i);
        }
        if (i < line.size())
            diagnose_unsupported(options, "flowchart", number, line.substr(i),
                                 "this part of the statement is not laid out yet");
    }
    if (!stack.empty())
        diagnose(options, cworks::Diagnostic::Severity::Warning,
                 at_line("flowchart", lines.back().number, "unterminated 'subgraph' (missing 'end')"));

    // C3: a single-word subgraph id names the CLUSTER, so a statement that
    // uses it addresses the frame — `X --> sub` reaches the boundary, not
    // some member inside it. The statement parsers cannot know that (the
    // subgraph may be declared further down), so they fabricate a node and
    // this pass folds it away: the edge ends become cluster ends, and any
    // class or style that landed on the phantom moves to the cluster.
    {
        std::vector<int> as_cluster(model.nodes.size(), -1);
        std::vector<int> remap(model.nodes.size(), -1);
        std::vector<Node> kept;
        kept.reserve(model.nodes.size());
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const auto sub = sub_by_id.find(model.nodes[i].id);
            if (sub == sub_by_id.end()) {
                remap[i] = static_cast<int>(kept.size());
                kept.push_back(std::move(model.nodes[i]));
                continue;
            }
            as_cluster[i] = sub->second;
            Cluster& cluster = model.clusters[static_cast<std::size_t>(sub->second)];
            Node& phantom = model.nodes[i];
            cluster.classes.insert(cluster.classes.end(), phantom.classes.begin(),
                                   phantom.classes.end());
            cluster.direct_style.merge(phantom.direct_style);
            if (phantom.shaped)
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("flowchart", phantom.shaped_line,
                                 "'" + phantom.id +
                                     "' is a subgraph id, so its node shape is not drawn"));
        }
        model.nodes = std::move(kept);
        const auto rebind = [&](DagEnd end) {
            const std::size_t i = static_cast<std::size_t>(end.index);
            if (as_cluster[i] >= 0) return DagEnd::on_cluster(as_cluster[i]);
            return DagEnd(remap[i]);
        };
        for (Edge& e : model.edges) {
            e.from = rebind(e.from);
            e.to = rebind(e.to);
        }
    }

    // C2: apply linkStyle selectors now that every edge index is known.
    for (const auto& [sel, props] : pending_links) {
        if (sel == "default") {
            for (Edge& e : model.edges) e.link.merge(props);
            continue;
        }
        for (const std::string& tok : split_commas(sel)) {
            double idx = 0.0;
            // Guard before the cast: a negative double to size_t is UB.
            if (parse_number(tok, idx) && idx >= 0.0 &&
                idx < static_cast<double>(model.edges.size()))
                model.edges[static_cast<std::size_t>(idx)].link.merge(props);
        }
    }

    // C2: resolve each element's style — the `default` class first (Mermaid
    // applies it to every node), then named classes in application order,
    // then the direct `style` override on top (last wins).
    const auto default_class = model.class_defs.find("default");
    for (Node& n : model.nodes) {
        if (default_class != model.class_defs.end()) n.resolved.merge(default_class->second);
        for (const std::string& cls : n.classes) {
            const auto it = model.class_defs.find(cls);
            if (it != model.class_defs.end()) n.resolved.merge(it->second);
        }
        n.resolved.merge(n.direct_style);
    }
    for (Cluster& c : model.clusters) {
        if (default_class != model.class_defs.end()) c.resolved.merge(default_class->second);
        for (const std::string& cls : c.classes) {
            const auto it = model.class_defs.find(cls);
            if (it != model.class_defs.end()) c.resolved.merge(it->second);
        }
        c.resolved.merge(c.direct_style);
    }

    if (model.nodes.empty()) throw Error(cworks::validation_failed("flowchart: no nodes"));
    return model;
}

// -- rendering ----------------------------------------------------------

struct NodeBox {
    DagNodeSize size;
    std::vector<std::string> lines;  // label lines
};

NodeBox size_node(const Node& node, const Font& font, double line_h) {
    NodeBox box;
    box.lines = label_lines(node.label);
    double text_w = 0.0;
    for (const std::string& line : box.lines)
        text_w = std::max(text_w, text_width(line, font));
    const double text_h = static_cast<double>(box.lines.size()) * line_h;
    const double pad_x = 16.0, pad_y = 9.0;
    switch (node.shape) {
    case Shape::Diamond: {
        // A w×h box inscribes in a rhombus only when text_w/W + text_h/H
        // <= 1; sizing each side to text_w + text_h guarantees that (the
        // box corners just reach the edges) with the padding as slack.
        const double side_w = text_w + text_h + 2.0 * pad_x;
        const double side_h = text_w + text_h + 2.0 * pad_y;
        box.size = {std::max(side_w, 66.0), std::max(side_h, 44.0)};
        break;
    }
    case Shape::Circle: {
        // The text box inscribes in the circle only if the diameter spans
        // its diagonal, so size from hypot(text_w, text_h), not max(w, h).
        const double d = std::hypot(text_w, text_h) + 2.0 * pad_x;
        box.size = {d, d};
        break;
    }
    case Shape::DoubleCircle: {
        // As Circle, plus room for the outer ring.
        const double d = std::hypot(text_w, text_h) + 2.0 * pad_x + 10.0;
        box.size = {d, d};
        break;
    }
    case Shape::Flag: {
        // The left-side notch eats into the label area; reserve for it.
        const double box_h = text_h + 2.0 * pad_y;
        box.size = {text_w + 2.0 * pad_x + std::min(12.0, box_h / 2.0), box_h};
        break;
    }
    case Shape::LeanRight:
    case Shape::LeanLeft:
    case Shape::Trapezoid:
    case Shape::TrapezoidAlt: {
        // Slanted sides eat k of width per side at the text extremes,
        // exactly like the hexagon's insets.
        const double box_h = text_h + 2.0 * pad_y;
        const double inset = 2.0 * 14.0 * (text_h / box_h);
        box.size = {text_w + 2.0 * pad_x + inset, box_h};
        break;
    }
    case Shape::Hexagon: {
        // draw_shape slants the top/bottom edges inward by k (<= 14). At
        // the text's extreme lines that eats k*(text_h/box_h) of width per
        // side, so reserve for k = 14 there on top of the usual padding.
        const double box_h = text_h + 2.0 * pad_y;
        const double inset = 2.0 * 14.0 * (text_h / box_h);
        box.size = {text_w + 2.0 * pad_x + inset, box_h};
        break;
    }
    case Shape::Cylinder:
        box.size = {text_w + 2.0 * pad_x, text_h + 2.0 * pad_y + 14.0};
        break;
    default:
        box.size = {text_w + 2.0 * pad_x, text_h + 2.0 * pad_y};
        break;
    }
    return box;
}

void draw_shape(Canvas& canvas, const Node& node, Point c, DagNodeSize s,
                const ShapeStyle& style) {
    const double hw = s.w / 2.0, hh = s.h / 2.0;
    const RectF box{c.x - hw, c.y - hh, s.w, s.h};
    switch (node.shape) {
    case Shape::Rect:
    case Shape::Subroutine:
        canvas.rect(box, style);
        if (node.shape == Shape::Subroutine) {
            ShapeStyle bar = style;
            bar.fill.reset();
            canvas.line({box.x + 6.0, box.y}, {box.x + 6.0, box.y + box.h}, bar);
            canvas.line({box.x + box.w - 6.0, box.y},
                        {box.x + box.w - 6.0, box.y + box.h}, bar);
        }
        break;
    case Shape::Round:
        canvas.rounded_rect(box, 10.0, style);
        break;
    case Shape::Stadium:
        canvas.rounded_rect(box, hh, style);
        break;
    case Shape::Circle:
        canvas.circle(c, hw, style);
        break;
    case Shape::DoubleCircle: {
        canvas.circle(c, hw, style);
        ShapeStyle ring = style;
        ring.fill.reset();
        canvas.circle(c, hw - 4.0, ring);
        break;
    }
    case Shape::Flag: {
        // Mermaid's asymmetric shape: flat top, right, and bottom; the
        // left side notches inward to a mid-height point.
        const double k = std::min(12.0, box.w / 4.0);
        canvas.polygon({{box.x, box.y}, {box.x + box.w, box.y},
                        {box.x + box.w, box.y + box.h}, {box.x, box.y + box.h},
                        {box.x + k, c.y}},
                       style);
        break;
    }
    case Shape::LeanRight: {
        const double k = std::min(14.0, box.w / 4.0);
        canvas.polygon({{box.x + k, box.y}, {box.x + box.w, box.y},
                        {box.x + box.w - k, box.y + box.h}, {box.x, box.y + box.h}},
                       style);
        break;
    }
    case Shape::LeanLeft: {
        const double k = std::min(14.0, box.w / 4.0);
        canvas.polygon({{box.x, box.y}, {box.x + box.w - k, box.y},
                        {box.x + box.w, box.y + box.h}, {box.x + k, box.y + box.h}},
                       style);
        break;
    }
    case Shape::Trapezoid: {
        const double k = std::min(14.0, box.w / 4.0);
        canvas.polygon({{box.x + k, box.y}, {box.x + box.w - k, box.y},
                        {box.x + box.w, box.y + box.h}, {box.x, box.y + box.h}},
                       style);
        break;
    }
    case Shape::TrapezoidAlt: {
        const double k = std::min(14.0, box.w / 4.0);
        canvas.polygon({{box.x, box.y}, {box.x + box.w, box.y},
                        {box.x + box.w - k, box.y + box.h}, {box.x + k, box.y + box.h}},
                       style);
        break;
    }
    case Shape::Diamond:
        canvas.polygon({{c.x, box.y}, {box.x + box.w, c.y}, {c.x, box.y + box.h},
                        {box.x, c.y}},
                       style);
        break;
    case Shape::Hexagon: {
        const double k = std::min(14.0, box.w / 4.0);
        canvas.polygon({{box.x + k, box.y}, {box.x + box.w - k, box.y},
                        {box.x + box.w, c.y}, {box.x + box.w - k, box.y + box.h},
                        {box.x + k, box.y + box.h}, {box.x, c.y}},
                       style);
        break;
    }
    case Shape::Cylinder: {
        const double ry = std::min(7.0, box.h / 4.0);
        // body
        canvas.rect(RectF{box.x, box.y + ry, box.w, box.h - 2.0 * ry}, style);
        // the end cap: a real ellipse, not a 24-gon squashed vertically
        canvas.ellipse(RectF{box.x, box.y, box.w, 2.0 * ry}, style);
        break;
    }
    }
}

} // namespace

cplot::Scene build_flowchart(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const FlowModel model = parse_flowchart(source, options);
    const DiagramStyle sty = style_for(options);
    const Font font = theme.base_font();
    const double line_h = font.size * 1.35;

    std::vector<NodeBox> boxes;
    std::vector<DagNodeSize> sizes;
    boxes.reserve(model.nodes.size());
    for (const Node& node : model.nodes) {
        boxes.push_back(size_node(node, font, line_h));
        sizes.push_back(boxes.back().size);
    }
    DagGraph graph;
    graph.nodes = sizes;
    graph.edges.reserve(model.edges.size());
    for (const Edge& e : model.edges) graph.edges.push_back({e.from, e.to});

    // C3: hand the subgraphs to the layout as a CONSTRAINT. The frame's
    // padding and label band are the room it has to reserve around the
    // members, and a title that would not fit inside is the smallest frame
    // it may produce — so the boundary is a region the layout planned for,
    // not a box drawn around wherever the nodes happened to land.
    const Font cluster_font = theme.base_font();
    const double cluster_pad = 12.0;
    const double cluster_label_h = cluster_font.size * 1.5;
    graph.clusters.reserve(model.clusters.size());
    for (const Cluster& c : model.clusters) {
        DagCluster dc;
        dc.parent = c.parent;
        dc.margin = {cluster_pad, cluster_pad, cluster_pad + cluster_label_h, cluster_pad};
        if (!c.title.empty())
            dc.min_width = text_width(c.title, cluster_font) + 2.0 * cluster_pad;
        graph.clusters.push_back(dc);
    }
    graph.node_cluster.reserve(model.nodes.size());
    for (const Node& node : model.nodes) graph.node_cluster.push_back(node.cluster);

    DagParams params;
    params.dir = model.dir;
    const FrontMatter matter(source, options, "flowchart");
    if (const std::optional<double> spacing = matter.number("nodeSpacing"); spacing && *spacing > 0.0)
        params.node_sep = *spacing;
    if (const std::optional<double> spacing = matter.number("rankSpacing"); spacing && *spacing > 0.0)
        params.rank_sep = *spacing;
    DagResult layout = layout_dag(graph, params);

    // Authored placement (the documented front-matter extension) corrects the
    // computed layout; it never replaces it. A diagram with no block, or one
    // read by a renderer that ignores the key, draws exactly as it always did.
    if (const Placement placement = Placement::parse(source, options.diagnostics);
        !placement.empty()) {
        std::vector<std::string> keys;
        keys.reserve(model.nodes.size());
        for (const Node& node : model.nodes) keys.push_back(node.id);
        apply_placement(placement, keys, graph, layout, options.diagnostics);
    }

    const double margin = 22.0;
    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = layout.width + 2.0 * margin;
    canvas.height = layout.height + 2.0 * margin;
    const auto shift = [&](Point p) { return Point{p.x + margin, p.y + margin}; };

    const Color node_fill = sty.node_fill;
    const Color node_stroke = sty.node_stroke;
    const Color edge_color = sty.edge;
    LabelLayout labels;

    // C3: subgraph boundary frames, drawn first so the graph sits on top.
    // The rectangles come from the layout, which reserved the room for
    // them; outermost frames draw first so nested ones layer over.
    if (!model.clusters.empty()) {
        std::vector<int> depth(model.clusters.size(), 0);
        for (std::size_t ci = 0; ci < model.clusters.size(); ++ci)
            for (int p = model.clusters[ci].parent; p >= 0; p = model.clusters[static_cast<std::size_t>(p)].parent)
                ++depth[ci];
        std::vector<int> order(model.clusters.size());
        for (std::size_t ci = 0; ci < order.size(); ++ci) order[ci] = static_cast<int>(ci);
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return depth[a] < depth[b]; });
        for (const int c : order) {
            const RectF& box = layout.cluster_rects[static_cast<std::size_t>(c)];
            if (box.w <= 0.0 || box.h <= 0.0) continue;  // an empty subgraph frames nothing
            const RectF frame{box.x + margin, box.y + margin, box.w, box.h};
            const Cluster& cluster = model.clusters[static_cast<std::size_t>(c)];
            if (options.regions != nullptr && !cluster.id.empty())
                options.regions->push_back({cluster.id, 2 /* HitRole::Group */, frame});
            ShapeStyle fs;
            fs.stroke = cluster.resolved.stroke.value_or(sty.entity_stroke);
            fs.stroke_width = cluster.resolved.stroke_width.value_or(1.2);
            // A faint tint so the group reads as one region — and a tint a
            // nested frame does not compound into a dark one.
            //
            // A frame paints over its ancestors, so a fixed per-frame alpha
            // accumulates with depth. At sty.grid's own 32% that reached 54%
            // one level in and 69% two levels in, and the title fell from an
            // already-marginal 3.1:1 contrast to 1.5:1 and then 1.2:1 — a
            // heading nobody can read. The weight below is the one kanban
            // gives a lane, which is the same visual job, and the cumulative
            // tint is CAPPED so that the title stays above the 4.5:1 its size
            // needs however deep the nesting goes. An author who names a fill
            // gets that fill: only the default is derived.
            fs.fill = cluster.resolved.fill.value_or(
                sty.grid.with_alpha(cluster_tint_alpha(depth[static_cast<std::size_t>(c)])));
            if (cluster.resolved.dash) fs.dash = *cluster.resolved.dash;
            canvas.rounded_rect(frame, 6.0, fs);
            if (!cluster.title.empty())
                canvas.text({frame.x + cluster_pad, frame.y + cluster_label_h / 2.0},
                            cluster.title, cluster_font,
                            cluster.resolved.text.value_or(sty.muted), HAlign::Left,
                            VAlign::Middle);
        }
    }

    // Land an edge on the actual node outline (rhombus / circle / pill),
    // not the bounding box — otherwise a diamond or circle leaves a gap
    // between its slanted/curved face and the edge end.
    const auto node_border = [&](const Node& node, Point center, DagNodeSize s,
                                 Point toward) -> Point {
        switch (node.shape) {
        case Shape::Diamond: return diamond_border(center, s.w, s.h, toward);
        case Shape::Circle:
        case Shape::DoubleCircle: return circle_border(center, s.w / 2.0, toward);
        case Shape::Stadium: return stadium_border(center, s.w, s.h, toward);
        default: return box_border(center, s.w, s.h, toward);
        }
    };

    // A terminal marker at `tip`, approached from `from`; the stroke is
    // retracted by marker_inset() so only the marker forms the line end.
    const auto draw_marker = [&](EdgeMarker marker, Point tip, Point from, const Color& col) {
        switch (marker) {
        case EdgeMarker::Arrow:
            canvas.arrow_head(tip, from, 10.0, 8.0, col);
            break;
        case EdgeMarker::Circle: {
            const double len = std::hypot(tip.x - from.x, tip.y - from.y);
            const double ux = len > 1e-9 ? (tip.x - from.x) / len : 1.0;
            const double uy = len > 1e-9 ? (tip.y - from.y) / len : 0.0;
            ShapeStyle dot;
            dot.fill = col;
            canvas.circle({tip.x - ux * 4.0, tip.y - uy * 4.0}, 4.0, dot);
            break;
        }
        case EdgeMarker::Cross: {
            const double len = std::hypot(tip.x - from.x, tip.y - from.y);
            const double ux = len > 1e-9 ? (tip.x - from.x) / len : 1.0;
            const double uy = len > 1e-9 ? (tip.y - from.y) / len : 0.0;
            const Point c{tip.x - ux * 5.0, tip.y - uy * 5.0};
            ShapeStyle bar;
            bar.stroke = col;
            bar.stroke_width = 1.8;
            canvas.line({c.x - 4.0, c.y - 4.0}, {c.x + 4.0, c.y + 4.0}, bar);
            canvas.line({c.x - 4.0, c.y + 4.0}, {c.x + 4.0, c.y - 4.0}, bar);
            break;
        }
        case EdgeMarker::None:
            break;
        }
    };
    const auto marker_inset = [](EdgeMarker marker) {
        switch (marker) {
        case EdgeMarker::Arrow: return 9.0;
        case EdgeMarker::Circle: return 8.0;
        case EdgeMarker::Cross: return 9.0;
        case EdgeMarker::None: return 0.0;
        }
        return 0.0;
    };

    // Edges first (behind nodes).
    std::size_t edge_ordinal = 0;
    for (std::size_t k = 0; k < model.edges.size(); ++k) {
        const Edge& e = model.edges[k];
        if (e.hidden) continue;  // `~~~`: shapes the layout, draws nothing
        // C2: linkStyle overrides the theme edge colour/width/dash.
        const Color arrow_col = e.link.stroke.value_or(edge_color);
        ShapeStyle line_style;
        line_style.stroke = arrow_col;
        line_style.stroke_width =
            e.link.stroke_width.value_or(e.style == EdgeStyle::Thick ? 2.6 : 1.5);
        if (e.link.dash) line_style.dash = *e.link.dash;
        else if (e.style == EdgeStyle::Dotted) line_style.dash = cplot::DashPattern{{3.0, 3.0}};

        std::vector<Point> route = layout.routes[k];
        if (route.size() < 2) {
            const bool self_loop =
                !e.from.is_cluster && !e.to.is_cluster && e.from.index == e.to.index;
            if (!self_loop) {
                // C3: an end named a subgraph the layout could not reach —
                // one with no members, or one whose only member is the
                // other end. Say so; do not drop the link in silence.
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("flowchart", e.line,
                                 "this link names a subgraph with nothing of its own to "
                                 "attach to and was not drawn"));
                continue;
            }
            // Self-loop: a small loop on the node's right side.
            const Point c = shift(layout.centers[static_cast<std::size_t>(e.from.index)]);
            const DagNodeSize s = sizes[static_cast<std::size_t>(e.from.index)];
            const double x = c.x + s.w / 2.0;
            std::vector<Point> loop{{x, c.y - 8.0}, {x + 26.0, c.y - 8.0},
                                    {x + 26.0, c.y + 8.0}, {x, c.y + 8.0}};
            const double inset = marker_inset(e.end);
            canvas.polyline(inset > 0.0 ? retract_end(loop, inset) : loop, line_style);
            draw_marker(e.end, {x, c.y + 8.0}, {x + 26.0, c.y + 8.0}, arrow_col);
            continue;
        }
        for (Point& p : route) p = shift(p);

        // A point is inside a node when it has not reached that node's own
        // outline in its own direction. Asked through node_border so every
        // shape answers with its real silhouette rather than a bounding box:
        // a diamond's corners are outside it and its middle is not.
        const auto inside = [&](std::size_t index, Point p) {
            const Point c = shift(layout.centers[index]);
            const Point edge = node_border(model.nodes[index], c, sizes[index], p);
            const double dp = (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y);
            const double de = (edge.x - c.x) * (edge.x - c.x) + (edge.y - c.y) * (edge.y - c.y);
            return dp < de;
        };

        // Drop route points that lie INSIDE the end nodes before clipping.
        //
        // A curved route - a back edge, a link between two nodes a layer
        // apart - can pass through the node it is heading for, so the point
        // next to the end is often inside it. Snapping the end to the border
        // and then aiming the marker from that inner point builds the
        // arrowhead on the wrong side of the border, where the node is drawn
        // over it: the line arrived and the arrow was invisible. Trimming
        // first means the marker is always aimed from outside.
        if (!e.to.is_cluster) {
            const std::size_t i = static_cast<std::size_t>(e.to.index);
            while (route.size() > 2 && inside(i, route[route.size() - 2])) route.pop_back();
        }
        if (!e.from.is_cluster) {
            const std::size_t i = static_cast<std::size_t>(e.from.index);
            while (route.size() > 2 && inside(i, route[1])) route.erase(route.begin());
        }

        // Clip the endpoints to the node borders. C3: an end that named a
        // subgraph already sits on that subgraph's frame — the layout put
        // it there — so it is left exactly where it is.
        if (!e.from.is_cluster) {
            const std::size_t i = static_cast<std::size_t>(e.from.index);
            route.front() =
                node_border(model.nodes[i], shift(layout.centers[i]), sizes[i], route[1]);
        }
        if (!e.to.is_cluster) {
            const std::size_t i = static_cast<std::size_t>(e.to.index);
            route.back() = node_border(model.nodes[i], shift(layout.centers[i]), sizes[i],
                                       route[route.size() - 2]);
        }
        const Point tip = route.back();
        const Point from = route[route.size() - 2];
        // Stop the stroke at each marker's base so only the marker forms
        // the line end; the line no longer pokes through it.
        std::vector<Point> stroke_route = route;
        if (const double inset = marker_inset(e.end); inset > 0.0)
            stroke_route = retract_end(std::move(stroke_route), inset);
        if (const double inset = marker_inset(e.start); inset > 0.0) {
            std::reverse(stroke_route.begin(), stroke_route.end());
            stroke_route = retract_end(std::move(stroke_route), inset);
            std::reverse(stroke_route.begin(), stroke_route.end());
        }
        canvas.polyline(stroke_route, line_style);
        draw_marker(e.end, tip, from, arrow_col);
        draw_marker(e.start, route.front(), route[1], arrow_col);

        // A connection's clickable region is the box its route occupies, grown
        // so a one-pixel line is still something a person can hit. Named by
        // ordinal because connections have no key of their own — the model
        // lists them in the same source order.
        if (options.regions != nullptr) {
            double min_x = route.front().x, max_x = route.front().x;
            double min_y = route.front().y, max_y = route.front().y;
            for (const Point& p : route) {
                min_x = std::min(min_x, p.x);
                max_x = std::max(max_x, p.x);
                min_y = std::min(min_y, p.y);
                max_y = std::max(max_y, p.y);
            }
            constexpr double kEdgeGrip = 5.0;
            DrawnRegion region{
                "#" + std::to_string(edge_ordinal++), 1 /* HitRole::Edge */,
                RectF{min_x - kEdgeGrip, min_y - kEdgeGrip,
                      (max_x - min_x) + 2 * kEdgeGrip,
                      (max_y - min_y) + 2 * kEdgeGrip}};
            region.path = route;
            options.regions->push_back(std::move(region));
        }

        // C2: linkStyle `color:` styles the edge label text.
        labels.add(route, e.label, font, e.link.text);
    }

    // Nodes.
    ShapeStyle node_style;
    node_style.fill = node_fill;
    node_style.stroke = node_stroke;
    node_style.stroke_width = 1.0;
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const Point c = shift(layout.centers[i]);
        // C2: layer per-node style (from class/style directives) over the theme.
        ShapeStyle ns = node_style;
        const StyleProps& sp = model.nodes[i].resolved;
        if (sp.fill) ns.fill = sp.fill;
        if (sp.stroke) ns.stroke = sp.stroke;
        if (sp.stroke_width) ns.stroke_width = *sp.stroke_width;
        draw_shape(canvas, model.nodes[i], c, sizes[i], ns);
        // Report where this node landed, so a consumer can turn a point back
        // into the object rather than guessing from the picture.
        if (options.regions != nullptr)
            options.regions->push_back(
                {model.nodes[i].id, 0 /* HitRole::Node */,
                 RectF{c.x - sizes[i].w / 2.0, c.y - sizes[i].h / 2.0, sizes[i].w, sizes[i].h}});
        const double text_h = static_cast<double>(boxes[i].lines.size()) * line_h;
        canvas.text_block({c.x, c.y - text_h / 2.0}, boxes[i].lines, font,
                          sp.text.value_or(sty.text), HAlign::Center, line_h);
        if (const auto it = model.links.find(model.nodes[i].id); it != model.links.end())
            canvas.add_link(RectF{c.x - sizes[i].w / 2.0, c.y - sizes[i].h / 2.0, sizes[i].w,
                                  sizes[i].h},
                            it->second.href, it->second.title, it->second.target);
    }
    labels.draw(canvas, sty.muted, sty.label_mask);

    return canvas.bake();
}

} // namespace cdiagram::detail
