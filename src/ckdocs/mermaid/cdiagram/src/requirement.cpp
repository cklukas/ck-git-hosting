// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Requirement diagram (`requirementDiagram`): SysML requirements and
// elements with typed relationships, laid out with the layered engine.

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

struct RNode {
    std::string name;
    std::string stereotype;  // «requirement» / «element»
    std::string detail;      // a short field (risk / type)
    DagNodeSize size;
    double header_h = 0.0;
};
struct RRel {
    int from = 0;
    int to = 0;
    std::string type;
};
struct RModel {
    std::vector<RNode> nodes;
    std::vector<RRel> rels;
    StyleSheet styles;  ///< classDef / class / ::: / style resolution (v11)
    LinkTable links;    ///< requirement/element name -> hyperlink (click …)
};

int node_index(RModel& model, const std::string& name) {
    for (std::size_t i = 0; i < model.nodes.size(); ++i)
        if (model.nodes[i].name == name) return static_cast<int>(i);
    model.nodes.push_back({name, "requirement", "", {}, 0.0});
    return static_cast<int>(model.nodes.size()) - 1;
}

/// True when `token` is one of the seven SysML requirement relationship
/// keywords Mermaid defines. A real relationship line has one of these as
/// its middle token; requiring it stops a stray line that merely happens to
/// contain ` - ` from fabricating phantom requirement boxes via node_index.
bool is_rel_type(const std::string& token) {
    return token == "contains" || token == "copies" || token == "derives" ||
           token == "satisfies" || token == "verifies" || token == "refines" ||
           token == "traces";
}

RModel parse_requirement(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> kKnown = {
        "requirement", "element", "functionalRequirement", "interfaceRequirement",
        "performanceRequirement", "physicalRequirement", "designConstraint", "click"};
    RModel model;
    int block = -1;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        if (block >= 0) {
            if (line == "}") {
                block = -1;
                continue;
            }
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::string key = cworks::trim(line.substr(0, colon));
                const std::string value = cworks::trim(line.substr(colon + 1));
                if (key == "risk" || key == "type" || key == "verifymethod")
                    model.nodes[static_cast<std::size_t>(block)].detail = value;
            }
            continue;
        }
        // `<kind> <name> {`
        if (!line.empty() && line.back() == '{') {
            const std::string head = cworks::trim(line.substr(0, line.size() - 1));
            const std::size_t sp = head.find_first_of(" \t");
            if (sp != std::string::npos) {
                const std::string kind = head.substr(0, sp);
                const std::string name = cworks::trim(head.substr(sp + 1));
                const int idx = node_index(model, name);
                model.nodes[static_cast<std::size_t>(idx)].stereotype =
                    kind == "element" ? "element" : "requirement";
                block = idx;
                continue;
            }
        }
        const std::string keyword = line.substr(0, line.find_first_of(" \t"));
        // Interaction: `click <name> "url" ["tip"] [_target]` attaches a
        // hyperlink to the requirement/element box. JS callback forms carry no
        // URL and cannot be exported, so they are reported, not linked.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!model.links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("requirementDiagram", number,
                                     "'" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "requirementDiagram", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        // Recognised requirementDiagram directives the engine does not
        // render. Without this, `direction TB` was dropped silently.
        if (keyword == "direction") {
            diagnose_unsupported(options, "requirementDiagram", number, keyword,
                                 "layout direction is not rendered");
            continue;
        }
        // C2 (v11 requirement styling): classDef / class / style.
        if (parse_style_statement(line, model.styles, options, "requirementDiagram", number))
            continue;
        // Relationship: `src - type -> dst`  or  `src - type - dst`. The
        // middle token must be a SysML relationship keyword; otherwise the
        // loose ` - … - ` shape would let node_index() fabricate phantom
        // requirement boxes for any stray line that merely contains ` - `.
        std::string src, type, dst;
        bool matched = false;
        const std::size_t arrow = line.find(" -> ");
        if (arrow != std::string::npos) {
            const std::string before = line.substr(0, arrow);
            const std::size_t dash = before.find(" - ");
            if (dash != std::string::npos) {
                src = cworks::trim(before.substr(0, dash));
                type = cworks::trim(before.substr(dash + 3));
                dst = cworks::trim(line.substr(arrow + 4));
                matched = true;
            }
        } else {
            const std::size_t d1 = line.find(" - ");
            const std::size_t d2 =
                d1 == std::string::npos ? std::string::npos : line.find(" - ", d1 + 3);
            if (d1 != std::string::npos && d2 != std::string::npos) {
                src = cworks::trim(line.substr(0, d1));
                type = cworks::trim(line.substr(d1 + 3, d2 - d1 - 3));
                dst = cworks::trim(line.substr(d2 + 3));
                matched = true;
            }
        }
        if (matched && !src.empty() && !dst.empty() && is_rel_type(type)) {
            model.rels.push_back({node_index(model, src), node_index(model, dst), type});
            continue;
        }
        // Nothing matched: report the line rather than dropping or
        // misparsing it (the never-silently-drop-input charter).
        diagnose_unrecognized(options, "requirementDiagram", number, keyword, kKnown);
    }
    if (model.nodes.empty())
        throw Error(cworks::validation_failed("requirementDiagram: no requirements"));
    return model;
}

} // namespace

cplot::Scene build_requirement(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    RModel model = parse_requirement(source, options);
    const Font name_font = theme.base_font().with_weight(cplot::FontWeight::Bold);
    const Font font = theme.base_font();
    const Font small = font.with_size(font.size - 1.0);
    const double line_h = font.size * 1.4;

    std::vector<DagNodeSize> sizes(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        RNode& node = model.nodes[i];
        double w = text_width(node.name, name_font);
        w = std::max(w, text_width("\xC2\xAB" + node.stereotype + "\xC2\xBB", small));
        if (!node.detail.empty()) w = std::max(w, text_width(node.detail, small));
        node.header_h = small.size * 1.4;
        const double h = node.header_h + line_h + (node.detail.empty() ? 6.0 : line_h);
        node.size = {w + 26.0, h};
        sizes[i] = node.size;
    }
    std::vector<DagEdge> edges;
    for (const RRel& r : model.rels) edges.push_back({r.from, r.to});

    DagParams params;
    params.dir = FlowDir::Down;
    const DagResult layout = layout_dag(DagGraph{sizes, edges, {}, {}}, params);

    const double margin = 24.0;
    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = layout.width + 2.0 * margin;
    canvas.height = layout.height + 2.0 * margin;
    const auto shift = [&](Point p) { return Point{p.x + margin, p.y + margin}; };
    const Color line_color = sty.entity_stroke;
    LabelLayout labels;

    for (std::size_t k = 0; k < model.rels.size(); ++k) {
        const RRel& r = model.rels[k];
        std::vector<Point> route = layout.routes[k];
        if (route.size() < 2) continue;
        for (Point& p : route) p = shift(p);
        const DagNodeSize sf = sizes[static_cast<std::size_t>(r.from)];
        const DagNodeSize st = sizes[static_cast<std::size_t>(r.to)];
        route = clip_route(route, shift(layout.centers[static_cast<std::size_t>(r.from)]),
                           sf.w, sf.h, shift(layout.centers[static_cast<std::size_t>(r.to)]),
                           st.w, st.h);
        ShapeStyle ls;
        ls.stroke = line_color;
        ls.stroke_width = 1.3;
        ls.dash = cplot::DashPattern{{5.0, 3.0}};  // requirement relations are dashed
        const Point tip = route.back();
        const Point from = route[route.size() - 2];
        canvas.polyline(retract_end(route, 8.0), ls);
        canvas.arrow_head(tip, from, 9.0, 8.0, line_color);
        if (!r.type.empty())
            labels.add(route, "\xC2\xAB" + r.type + "\xC2\xBB", small);
    }

    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const RNode& node = model.nodes[i];
        const Point c = shift(layout.centers[i]);
        const RectF box{c.x - node.size.w / 2.0, c.y - node.size.h / 2.0, node.size.w,
                        node.size.h};
        // C2: layer the resolved classDef/style properties over the theme.
        const StyleProps sp = model.styles.resolve(node.name);
        ShapeStyle body;
        body.fill = sp.fill.value_or(sty.entity_fill);
        body.stroke = sp.stroke.value_or(sty.node_stroke);
        body.stroke_width = sp.stroke_width.value_or(1.0);
        if (sp.dash) body.dash = *sp.dash;
        canvas.rect(box, body);
        if (options.regions != nullptr)
            options.regions->push_back({node.name, 0 /* HitRole::Node */, box});
        if (const auto it = model.links.find(node.name); it != model.links.end())
            canvas.add_link(box, it->second.href, it->second.title, it->second.target);
        double y = box.y + node.header_h / 2.0 + 2.0;
        canvas.text({c.x, y}, "\xC2\xAB" + node.stereotype + "\xC2\xBB", small,
                    sty.muted, HAlign::Center, VAlign::Middle);
        y = box.y + node.header_h + line_h / 2.0 + 2.0;
        canvas.text({c.x, y}, node.name, name_font, sp.text.value_or(sty.text),
                    HAlign::Center, VAlign::Middle);
        if (!node.detail.empty())
            canvas.text({c.x, y + line_h}, node.detail, small, sty.muted,
                        HAlign::Center, VAlign::Middle);
    }
    labels.draw(canvas, sty.muted, sty.label_mask);

    return canvas.bake();
}

} // namespace cdiagram::detail
