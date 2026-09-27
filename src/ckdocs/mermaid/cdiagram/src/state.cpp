// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// State diagram (`stateDiagram` / `stateDiagram-v2`): states connected by
// labelled transitions, with `[*]` start/end pseudo-states, laid out with
// the layered engine. Composite (nested) states are flattened in this
// version — their inner transitions still lay out.

#include <algorithm>
#include <cctype>
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

struct SNode {
    std::string id;
    std::vector<std::string> lines;
    bool start = false;
    bool end = false;
    DagNodeSize size;
};
struct STrans {
    int from = 0;
    int to = 0;
    std::string label;
};
struct SModel {
    FlowDir dir = FlowDir::Down;
    std::vector<SNode> nodes;
    std::vector<STrans> trans;
    int start_index = -1;
    int end_index = -1;
    StyleSheet styles;  ///< classDef / class / ::: / style resolution
    LinkTable links;    ///< state id -> hyperlink (click …)
};

int state_index(SModel& model, const std::string& id) {
    for (std::size_t i = 0; i < model.nodes.size(); ++i)
        if (model.nodes[i].id == id) return static_cast<int>(i);
    model.nodes.push_back({id, {id}, false, false, {}});
    return static_cast<int>(model.nodes.size()) - 1;
}
int start_node(SModel& model) {
    if (model.start_index < 0) {
        model.start_index = static_cast<int>(model.nodes.size());
        model.nodes.push_back({"\x01start", {}, true, false, {}});
    }
    return model.start_index;
}
int end_node(SModel& model) {
    if (model.end_index < 0) {
        model.end_index = static_cast<int>(model.nodes.size());
        model.nodes.push_back({"\x01end", {}, false, true, {}});
    }
    return model.end_index;
}
int endpoint(SModel& model, const std::string& token, bool as_source) {
    if (token == "[*]") return as_source ? start_node(model) : end_node(model);
    return state_index(model, token);
}

bool starts_word(const std::string& line, const char* word) {
    const std::string w = word;
    if (line.compare(0, w.size(), w) != 0) return false;
    return line.size() == w.size() || line[w.size()] == ' ' || line[w.size()] == '\t';
}

SModel parse_state(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> kKnown = {"direction", "state", "-->", "[*]", "click"};
    SModel model;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        if (starts_word(line, "direction")) {
            const std::string d = cworks::trim(line.substr(9));
            model.dir = d == "LR" ? FlowDir::Right : d == "RL" ? FlowDir::Left
                        : d == "BT" ? FlowDir::Up : FlowDir::Down;
            continue;
        }
        const std::size_t arrow = line.find("-->");
        if (arrow != std::string::npos) {
            // A `:::class` annotation binds tighter than the label colon
            // (`A --> B:::warn : label`): record the assignment before the
            // label split so it never bleeds into the label or the id.
            const std::string left =
                take_class_annotations(cworks::trim(line.substr(0, arrow)), model.styles);
            std::string rest =
                take_class_annotations(cworks::trim(line.substr(arrow + 3)), model.styles);
            std::string label;
            const std::size_t colon = rest.find(':');
            if (colon != std::string::npos) {
                label = cworks::trim(rest.substr(colon + 1));
                rest = cworks::trim(rest.substr(0, colon));
            }
            const int from = endpoint(model, left, /*as_source=*/true);
            const int to = endpoint(model, rest, /*as_source=*/false);
            model.trans.push_back({from, to, label});
            continue;
        }
        if (starts_word(line, "state")) {
            // `state "desc" as X`  or  `state X` / `state X {`
            std::string rest = cworks::trim(line.substr(5));
            if (!rest.empty() && rest.back() == '{') rest = cworks::trim(rest.substr(0, rest.size() - 1));
            rest = take_class_annotations(std::move(rest), model.styles);
            if (rest.empty() || rest == "}") continue;
            // A stereotype (`state X <<fork>>` / `<<join>>` / `<<choice>>`)
            // declares a fork/join/choice pseudo-state this engine does not
            // render. Don't fabricate a node whose id is the literal
            // `X <<fork>>` text — report it and move on.
            const std::size_t stereo = rest.find("<<");
            if (stereo != std::string::npos) {
                diagnose_unsupported(options, "stateDiagram", number,
                                     cworks::trim(rest.substr(stereo)),
                                     "fork/join/choice pseudo-states are not rendered");
                continue;
            }
            const std::size_t as = rest.find(" as ");
            if (as != std::string::npos) {
                std::string desc = cworks::trim(rest.substr(0, as));
                const std::string id = cworks::trim(rest.substr(as + 4));
                const int idx = state_index(model, id);
                model.nodes[static_cast<std::size_t>(idx)].lines = label_lines(unquote(desc));
            } else {
                state_index(model, rest);
            }
            continue;
        }
        if (line == "}") continue;
        const std::string keyword = line.substr(0, line.find_first_of(" \t"));
        // C2: classDef / class / style (the shared styling statements).
        if (parse_style_statement(line, model.styles, options, "stateDiagram", number))
            continue;
        // Interaction: `click <id> "url" ["tip"] [_target]` attaches a
        // hyperlink to a state box. JS callback forms carry no URL and cannot
        // be exported, so they are reported, not linked.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!model.links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("stateDiagram", number,
                                     "state '" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "stateDiagram", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        // Recognised stateDiagram directives the engine does not render.
        if (keyword == "note") {
            diagnose_unsupported(options, "stateDiagram", number, keyword,
                                 "notes are not rendered");
            continue;
        }
        // Nothing matched: report the line rather than dropping it
        // (the never-silently-drop-input charter).
        diagnose_unrecognized(options, "stateDiagram", number, keyword, kKnown);
    }
    if (model.nodes.empty()) throw Error(cworks::validation_failed("state diagram: no states"));
    return model;
}

} // namespace

cplot::Scene build_state(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    SModel model = parse_state(source, options);
    const Font font = theme.base_font();
    const double line_h = font.size * 1.35;

    std::vector<DagNodeSize> sizes(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        SNode& node = model.nodes[i];
        if (node.start || node.end) {
            node.size = {20.0, 20.0};
        } else {
            double w = 0.0;
            for (const std::string& l : node.lines) w = std::max(w, text_width(l, font));
            node.size = {w + 28.0,
                         static_cast<double>(node.lines.size()) * line_h + 16.0};
        }
        sizes[i] = node.size;
    }
    std::vector<DagEdge> edges;
    for (const STrans& t : model.trans) edges.push_back({t.from, t.to});

    DagParams params;
    params.dir = model.dir;
    const DagResult layout = layout_dag(DagGraph{sizes, edges, {}, {}}, params);

    const double margin = 22.0;
    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = layout.width + 2.0 * margin;
    canvas.height = layout.height + 2.0 * margin;
    const auto shift = [&](Point p) { return Point{p.x + margin, p.y + margin}; };
    const Color edge_color = sty.edge;
    LabelLayout labels;

    // Transitions.
    for (std::size_t k = 0; k < model.trans.size(); ++k) {
        const STrans& t = model.trans[k];
        std::vector<Point> route = layout.routes[k];
        if (route.size() < 2) continue;
        for (Point& p : route) p = shift(p);
        const DagNodeSize sf = sizes[static_cast<std::size_t>(t.from)];
        const DagNodeSize st = sizes[static_cast<std::size_t>(t.to)];
        route = clip_route(route, shift(layout.centers[static_cast<std::size_t>(t.from)]),
                           sf.w, sf.h, shift(layout.centers[static_cast<std::size_t>(t.to)]),
                           st.w, st.h);
        if (options.regions != nullptr) {
            double min_x = route.front().x, max_x = route.front().x;
            double min_y = route.front().y, max_y = route.front().y;
            for (const Point& p : route) {
                min_x = std::min(min_x, p.x);
                max_x = std::max(max_x, p.x);
                min_y = std::min(min_y, p.y);
                max_y = std::max(max_y, p.y);
            }
            constexpr double kGrip = 5.0;
            DrawnRegion region{"#" + std::to_string(k), 1 /* HitRole::Edge */,
                               RectF{min_x - kGrip, min_y - kGrip,
                                     (max_x - min_x) + 2 * kGrip,
                                     (max_y - min_y) + 2 * kGrip}};
            region.path = route;
            options.regions->push_back(std::move(region));
        }
        ShapeStyle line_style;
        line_style.stroke = edge_color;
        line_style.stroke_width = 1.4;
        const Point tip = route.back();
        const Point from = route[route.size() - 2];
        canvas.polyline(retract_end(route, 8.0), line_style);
        canvas.arrow_head(tip, from, 9.0, 8.0, edge_color);
        labels.add(route, t.label, font);
    }

    // States.
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const SNode& node = model.nodes[i];
        const Point c = shift(layout.centers[i]);
        if (node.start) {
            ShapeStyle s;
            s.fill = edge_color;
            canvas.circle(c, 7.0, s);
        } else if (node.end) {
            ShapeStyle ring;
            ring.stroke = edge_color;
            ring.stroke_width = 1.4;
            canvas.circle(c, 9.0, ring);
            ShapeStyle dot;
            dot.fill = edge_color;
            canvas.circle(c, 4.5, dot);
        } else {
            if (options.regions != nullptr)
                options.regions->push_back(
                    {node.id, 0 /* HitRole::Node */,
                     RectF{c.x - node.size.w / 2.0, c.y - node.size.h / 2.0, node.size.w,
                           node.size.h}});
            // C2: layer the resolved classDef/style properties over the theme.
            const StyleProps sp = model.styles.resolve(node.id);
            ShapeStyle s;
            s.fill = sp.fill.value_or(sty.node_fill);
            s.stroke = sp.stroke.value_or(sty.node_stroke);
            s.stroke_width = sp.stroke_width.value_or(1.0);
            if (sp.dash) s.dash = *sp.dash;
            const RectF box{c.x - node.size.w / 2.0, c.y - node.size.h / 2.0,
                            node.size.w, node.size.h};
            canvas.rounded_rect(box, 8.0, s);
            const double th = static_cast<double>(node.lines.size()) * line_h;
            canvas.text_block({c.x, c.y - th / 2.0}, node.lines, font,
                              sp.text.value_or(sty.text), HAlign::Center, line_h);
            if (const auto it = model.links.find(node.id); it != model.links.end())
                canvas.add_link(box, it->second.href, it->second.title, it->second.target);
        }
    }
    labels.draw(canvas, sty.muted, sty.label_mask);

    return canvas.bake();
}

} // namespace cdiagram::detail
