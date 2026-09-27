// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// C4 diagram (`C4Context`/`C4Container`/…): people, systems, containers
// and components with relationships, laid out with the layered engine.
// Boundaries are flattened in this version.

#include <algorithm>
#include <string>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "canvas.hpp"
#include "cdiagram/error.hpp"
#include "dag_layout.hpp"
#include "diagnostics.hpp"
#include "links.hpp"
#include "styleprops.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram::detail {

namespace {

struct C4Node {
    std::string id;
    std::string label;
    std::string kind;   // Person / System / Container / Component
    std::string tech;   // optional technology/description
    bool person = false;
    DagNodeSize size;
    std::vector<std::string> label_lines;
    std::optional<Color> bg;      // UpdateElementStyle $bgColor
    std::optional<Color> font;    // UpdateElementStyle $fontColor
    std::optional<Color> border;  // UpdateElementStyle $borderColor
};
struct C4Rel {
    int from = 0;
    int to = 0;
    std::string label;
    std::optional<Color> line;  // UpdateRelStyle $lineColor
    std::optional<Color> text;  // UpdateRelStyle $textColor
};
struct C4Model {
    std::vector<C4Node> nodes;
    std::vector<C4Rel> rels;
    LinkTable links;  ///< element alias -> hyperlink (click …)
};

std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool quote = false;
    for (const char c : s) {
        if (c == '"') {
            quote = !quote;
            continue;
        }
        if (c == ',' && !quote) {
            out.push_back(cworks::trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cworks::trim(cur).empty()) out.push_back(cworks::trim(cur));
    return out;
}

int node_index(C4Model& model, const std::string& id) {
    for (std::size_t i = 0; i < model.nodes.size(); ++i)
        if (model.nodes[i].id == id) return static_cast<int>(i);
    C4Node node;
    node.id = id;
    node.label = id;
    node.kind = "System";
    model.nodes.push_back(std::move(node));
    return static_cast<int>(model.nodes.size()) - 1;
}

C4Model parse_c4(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    // Every keyword C4 accepts: elements draw boxes, Rel* draw edges,
    // boundaries are flattened, and the Update* directives are recognised
    // but not drawn. Used for the did-you-mean on an unrecognised line.
    static const std::vector<std::string> kKnown = {
        "Person", "Person_Ext", "System", "System_Ext", "SystemDb",
        "SystemQueue", "Container", "ContainerDb", "ContainerQueue",
        "Component", "ComponentDb", "Node", "Rel", "BiRel", "Rel_U",
        "Rel_D", "Rel_L", "Rel_R", "Rel_Back", "Boundary",
        "System_Boundary", "Container_Boundary", "Enterprise_Boundary",
        "UpdateElementStyle", "UpdateRelStyle", "UpdateLayoutConfig", "click"};
    // The subset that draws a node; only these may vivify an element.
    static const std::vector<std::string> kElements = {
        "Person", "Person_Ext", "System", "System_Ext", "SystemDb",
        "SystemQueue", "Container", "ContainerDb", "ContainerQueue",
        "Component", "ComponentDb", "Node"};
    C4Model model;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        // Interaction: `click <alias> "url" ["tip"] [_target]` attaches a
        // hyperlink to the element box. Handled before the `Keyword(...)`
        // detection because a click line carries no such call (and a URL may
        // itself contain parentheses). JS callback forms carry no URL and
        // cannot be exported, so they are reported, not linked.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!model.links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("C4", number,
                                     "element '" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "C4", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        const std::size_t lp = line.find('(');
        const std::size_t rp = line.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos || rp < lp) {
            // No `Keyword(...)` call on this line. `{`/`}` are boundary
            // block delimiters we flatten; anything else is an
            // unrecognised statement, reported rather than dropped.
            if (line != "{" && line != "}") {
                const std::string tok = line.substr(0, line.find_first_of(" \t"));
                diagnose_unrecognized(options, "C4", number, tok, kKnown);
            }
            continue;
        }
        const std::string kind = cworks::trim(line.substr(0, lp));
        const std::vector<std::string> args = split_args(line.substr(lp + 1, rp - lp - 1));
        if (args.empty()) continue;
        if (kind.rfind("Rel", 0) == 0 || kind.rfind("BiRel", 0) == 0) {
            if (args.size() >= 2) {
                C4Rel rel;
                rel.from = node_index(model, args[0]);
                rel.to = node_index(model, args[1]);
                if (args.size() >= 3) rel.label = args[2];
                model.rels.push_back(std::move(rel));
            }
            continue;
        }
        if (kind.find("Boundary") != std::string::npos ||
            kind.find("Enterprise") != std::string::npos)
            continue;  // boundaries flattened
        // C2: per-element / per-relation styling directives. `$key="value"`
        // arguments; unknown keys and unparseable colours are diagnosed.
        if (kind == "UpdateElementStyle" || kind == "UpdateRelStyle") {
            const bool element = kind == "UpdateElementStyle";
            const std::size_t fixed = element ? 1 : 2;  // id / from,to
            if (args.size() < fixed) continue;
            std::optional<Color> bg, font, border, line_color, text_color;
            for (std::size_t a = fixed; a < args.size(); ++a) {
                const std::string& arg = args[a];
                const std::size_t eq = arg.find('=');
                if (arg.empty() || arg[0] != '$' || eq == std::string::npos) {
                    diagnose_unsupported(options, "C4", number, arg,
                                         "expected $key=\"value\" (ignored)");
                    continue;
                }
                const std::string key = cworks::trim(arg.substr(1, eq - 1));
                const bool colour_key = key == "bgColor" || key == "fontColor" ||
                                        key == "borderColor" || key == "lineColor" ||
                                        key == "textColor";
                if (!colour_key) {
                    diagnose_unsupported(options, "C4", number, "$" + key,
                                         "only colour keys are rendered");
                    continue;
                }
                const std::string value = unquote(cworks::trim(arg.substr(eq + 1)));
                const std::optional<Color> color = cplot::parse_css_color(value);
                if (!color) {
                    diagnose_unsupported(options, "C4", number, value,
                                         "unrecognized colour (ignored)");
                    continue;
                }
                if (key == "bgColor") bg = color;
                else if (key == "fontColor") font = color;
                else if (key == "borderColor") border = color;
                else if (key == "lineColor") line_color = color;
                else text_color = color;
            }
            if (element) {
                C4Node& node =
                    model.nodes[static_cast<std::size_t>(node_index(model, args[0]))];
                if (bg) node.bg = bg;
                if (font) node.font = font;
                if (border) node.border = border;
            } else {
                const int from = node_index(model, args[0]);
                const int to = node_index(model, args[1]);
                for (C4Rel& rel : model.rels)
                    if (rel.from == from && rel.to == to) {
                        if (line_color) rel.line = line_color;
                        if (text_color) rel.text = text_color;
                    }
            }
            continue;
        }
        if (kind == "UpdateLayoutConfig") {
            diagnose_unsupported(options, "C4", number, kind,
                                 "layout configuration is not rendered");
            continue;
        }
        // Only a known element keyword may create a node; any other token
        // is a typo or unsupported construct and must not fabricate output.
        if (std::find(kElements.begin(), kElements.end(), kind) == kElements.end()) {
            diagnose_unrecognized(options, "C4", number, kind, kKnown);
            continue;
        }
        const int idx = node_index(model, args[0]);
        C4Node& node = model.nodes[static_cast<std::size_t>(idx)];
        node.kind = kind;
        node.person = kind.rfind("Person", 0) == 0;
        if (args.size() >= 2) node.label = args[1];
        if (args.size() >= 3) node.tech = args[2];
    }
    if (model.nodes.empty()) throw Error(cworks::validation_failed("C4: no elements"));
    return model;
}

} // namespace

cplot::Scene build_c4(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    C4Model model = parse_c4(source, options);
    const Font name_font = theme.base_font().with_weight(cplot::FontWeight::Bold);
    const Font font = theme.base_font();
    const Font small = font.with_size(font.size - 1.0);
    const double line_h = font.size * 1.35;

    std::vector<DagNodeSize> sizes(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        C4Node& node = model.nodes[i];
        node.label_lines = label_lines(node.label);
        double w = 0.0;
        for (const std::string& l : node.label_lines) w = std::max(w, text_width(l, name_font));
        w = std::max(w, text_width("[" + node.kind + "]", small));
        if (!node.tech.empty()) w = std::max(w, text_width(node.tech, small));
        const double h = small.size * 1.4 +
                         static_cast<double>(node.label_lines.size()) * line_h +
                         (node.tech.empty() ? 8.0 : line_h);
        node.size = {w + 28.0, h + 8.0};
        sizes[i] = node.size;
    }
    std::vector<DagEdge> edges;
    for (const C4Rel& r : model.rels) edges.push_back({r.from, r.to});

    DagParams params;
    params.dir = FlowDir::Down;
    params.rank_sep = 66.0;
    const DagResult layout = layout_dag(DagGraph{sizes, edges, {}, {}}, params);

    const double margin = 24.0;
    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = layout.width + 2.0 * margin;
    canvas.height = layout.height + 2.0 * margin;
    const auto shift = [&](Point p) { return Point{p.x + margin, p.y + margin}; };
    const Color rel_color = sty.entity_stroke;
    LabelLayout labels;

    for (std::size_t k = 0; k < model.rels.size(); ++k) {
        const C4Rel& r = model.rels[k];
        std::vector<Point> route = layout.routes[k];
        if (route.size() < 2) continue;
        for (Point& p : route) p = shift(p);
        const DagNodeSize sf = sizes[static_cast<std::size_t>(r.from)];
        const DagNodeSize st = sizes[static_cast<std::size_t>(r.to)];
        route = clip_route(route, shift(layout.centers[static_cast<std::size_t>(r.from)]),
                           sf.w, sf.h, shift(layout.centers[static_cast<std::size_t>(r.to)]),
                           st.w, st.h);
        ShapeStyle ls;
        const Color line_color = r.line.value_or(rel_color);
        ls.stroke = line_color;
        ls.stroke_width = 1.3;
        ls.dash = cplot::DashPattern{{5.0, 3.0}};
        const Point tip = route.back();
        const Point from = route[route.size() - 2];
        canvas.polyline(retract_end(route, 8.0), ls);
        canvas.arrow_head(tip, from, 9.0, 8.0, line_color);
        labels.add(route, r.label, small, r.text);
    }

    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const C4Node& node = model.nodes[i];
        const Point c = shift(layout.centers[i]);
        const RectF box{c.x - node.size.w / 2.0, c.y - node.size.h / 2.0, node.size.w,
                        node.size.h};
        ShapeStyle body;
        body.fill = node.bg.value_or(node.person ? sty.c4_person : sty.c4_element);
        body.stroke = node.border.value_or(*body.fill);
        if (node.border) body.stroke_width = 1.4;
        canvas.rounded_rect(box, node.person ? node.size.h / 2.5 : 4.0, body);
        if (options.regions != nullptr) {
            options.regions->push_back(
                {"#" + std::to_string(i), 0 /* HitRole::Node */, box});
        }
        if (const auto it = model.links.find(node.id); it != model.links.end())
            canvas.add_link(box, it->second.href, it->second.title, it->second.target);
        const Color ink = node.font.value_or(cplot::colors::white);
        double y = box.y + small.size * 0.9 + 4.0;
        canvas.text({c.x, y}, "[" + node.kind + "]", small, ink.with_alpha(0.85),
                    HAlign::Center, VAlign::Middle);
        y += line_h;
        for (const std::string& l : node.label_lines) {
            canvas.text({c.x, y}, l, name_font, ink, HAlign::Center, VAlign::Middle);
            y += line_h;
        }
        if (!node.tech.empty())
            canvas.text({c.x, y}, node.tech, small, ink.with_alpha(0.85),
                        HAlign::Center, VAlign::Middle);
    }
    labels.draw(canvas, sty.muted, sty.label_mask);

    return canvas.bake();
}

} // namespace cdiagram::detail
