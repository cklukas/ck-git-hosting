// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Class diagram (`classDiagram`): UML classes with attribute/method
// compartments and relationships (inheritance, composition, aggregation,
// association, dependency, realization), laid out with the layered
// engine. Multiplicity labels are not rendered in this version.

#include <algorithm>
#include <cmath>
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

enum class Marker { None, TriangleHollow, DiamondFilled, DiamondHollow, Arrow };

struct CNode {
    std::string name;
    std::vector<std::string> attrs;
    std::vector<std::string> methods;
    DagNodeSize size;
    double name_h = 0.0;
    double attr_h = 0.0;
};
struct CRel {
    int from = 0;
    int to = 0;
    Marker from_marker = Marker::None;
    Marker to_marker = Marker::None;
    bool dashed = false;
    std::string label;
};
struct CModel {
    std::vector<CNode> nodes;
    std::vector<CRel> rels;
    StyleSheet styles;  ///< classDef / cssClass / ::: / style resolution
    LinkTable links;    ///< class name -> hyperlink (link / click … href)
};

int class_index(CModel& model, const std::string& name) {
    for (std::size_t i = 0; i < model.nodes.size(); ++i)
        if (model.nodes[i].name == name) return static_cast<int>(i);
    model.nodes.push_back({name, {}, {}, {}, 0.0, 0.0});
    return static_cast<int>(model.nodes.size()) - 1;
}

void add_member(CNode& node, const std::string& raw) {
    const std::string member = cworks::trim(raw);
    if (member.empty()) return;
    if (member.find('(') != std::string::npos)
        node.methods.push_back(member);
    else
        node.attrs.push_back(member);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

bool has_relation_op(const std::string& s) {
    return s.find("--") != std::string::npos || s.find("..") != std::string::npos;
}

Marker left_marker(const std::string& m) {
    if (m.find("<|") != std::string::npos) return Marker::TriangleHollow;
    if (m.find('*') != std::string::npos) return Marker::DiamondFilled;
    if (m.find('o') != std::string::npos) return Marker::DiamondHollow;
    if (m.find('<') != std::string::npos) return Marker::Arrow;
    return Marker::None;
}
Marker right_marker(const std::string& m) {
    if (m.find("|>") != std::string::npos) return Marker::TriangleHollow;
    if (m.find('*') != std::string::npos) return Marker::DiamondFilled;
    if (m.find('o') != std::string::npos) return Marker::DiamondHollow;
    if (m.find('>') != std::string::npos) return Marker::Arrow;
    return Marker::None;
}

CModel parse_class(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> kKnown = {
        "class", "classDef", "style", "cssClass", "callback",
        "link", "note", "namespace", "direction", "click"};
    CModel model;
    int block = -1;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        if (block >= 0) {
            if (line == "}") {
                block = -1;
                continue;
            }
            add_member(model.nodes[static_cast<std::size_t>(block)], line);
            continue;
        }
        if (line.rfind("class ", 0) == 0) {
            std::string rest = cworks::trim(line.substr(6));
            const bool open = !rest.empty() && rest.back() == '{';
            if (open) rest = cworks::trim(rest.substr(0, rest.size() - 1));
            // strip a generic/annotation tail after the name
            const std::size_t sp = rest.find_first_of(" \t");
            std::string name = sp == std::string::npos ? rest : rest.substr(0, sp);
            // `class Foo:::highlight` assigns a classDef class inline.
            name = take_class_annotations(std::move(name), model.styles);
            if (name.empty()) continue;
            const int idx = class_index(model, name);
            if (open) block = idx;
            continue;
        }
        const std::string keyword = line.substr(0, line.find_first_of(" \t"));
        // C2: classDef / cssClass / style (the shared styling statements;
        // classDiagram assigns with `cssClass` because `class` declares).
        if (parse_style_statement(line, model.styles, options, "classDiagram", number,
                                  "cssClass"))
            continue;
        // Interactions attach a hyperlink to a class box: `link C "url" ["tt"]`
        // and `click C href "url" ["tt"]`. Callback forms carry no URL and are
        // reported, not linked.
        if (keyword == "click" || keyword == "link" || keyword == "callback") {
            std::string ref;
            LinkDirective link;
            const bool parsed = keyword == "click"
                                    ? parse_click_statement(line, ref, link)
                                    : parse_class_link_statement(line, ref, link);
            if (parsed && !link.callback) {
                if (!model.links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("classDiagram", number,
                                     "class '" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "classDiagram", number, keyword,
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        // Recognised classDiagram directives the engine does not render.
        // Without this, a line like `note "x"` fell through to the
        // member-shorthand branch and fabricated a bogus class box.
        if (keyword == "note" || keyword == "namespace" || keyword == "direction") {
            diagnose_unsupported(options, "classDiagram", number, keyword,
                                 "notes and namespaces are not rendered");
            continue;
        }
        // Relation or member-shorthand.
        std::string body = line;
        std::string label;
        const std::size_t colon = body.find(':');
        std::string before = colon == std::string::npos ? body : body.substr(0, colon);
        if (colon != std::string::npos) label = cworks::trim(body.substr(colon + 1));
        if (has_relation_op(before)) {
            const std::vector<std::string> t = split_ws(cworks::trim(before));
            if (t.size() == 3) {
                std::size_t sep = t[1].find("--");
                bool dashed = false;
                if (sep == std::string::npos) {
                    sep = t[1].find("..");
                    dashed = true;
                }
                CRel r;
                r.from = class_index(model, t[0]);
                r.to = class_index(model, t[2]);
                r.from_marker = left_marker(t[1].substr(0, sep));
                r.to_marker = right_marker(t[1].substr(sep + 2));
                r.dashed = dashed;
                r.label = label;
                model.rels.push_back(r);
            }
            continue;
        }
        if (colon != std::string::npos) {  // `Name : +int age`
            const std::string name = cworks::trim(before);
            // Only a single class identifier is a valid member target;
            // a multi-word left side is a directive or a typo, not shorthand.
            if (!name.empty() && name.find_first_of(" \t") == std::string::npos) {
                add_member(model.nodes[static_cast<std::size_t>(class_index(model, name))], label);
                continue;
            }
        }
        const std::vector<std::string> t = split_ws(body);
        if (t.size() == 1) {
            class_index(model, t[0]);
            continue;
        }
        // Nothing matched: report the line rather than dropping or
        // misparsing it (the never-silently-drop-input charter).
        diagnose_unrecognized(options, "classDiagram", number, keyword, kKnown);
    }
    if (model.nodes.empty()) throw Error(cworks::validation_failed("classDiagram: no classes"));
    return model;
}

void draw_marker(Canvas& canvas, Marker marker, Point at, Point out_dir, Color color,
                 Color background) {
    if (marker == Marker::None) return;
    const double px = -out_dir.y, py = out_dir.x;
    ShapeStyle s;
    s.stroke = color;
    s.stroke_width = 1.2;
    switch (marker) {
    case Marker::TriangleHollow: {
        const Point base{at.x + out_dir.x * 14.0, at.y + out_dir.y * 14.0};
        s.fill = background;
        canvas.polygon({at, {base.x + px * 8.0, base.y + py * 8.0},
                        {base.x - px * 8.0, base.y - py * 8.0}},
                       s);
        break;
    }
    case Marker::DiamondFilled:
    case Marker::DiamondHollow: {
        const Point mid{at.x + out_dir.x * 9.0, at.y + out_dir.y * 9.0};
        const Point far{at.x + out_dir.x * 18.0, at.y + out_dir.y * 18.0};
        s.fill = marker == Marker::DiamondFilled ? color : background;
        canvas.polygon({at, {mid.x + px * 6.0, mid.y + py * 6.0}, far,
                        {mid.x - px * 6.0, mid.y - py * 6.0}},
                       s);
        break;
    }
    case Marker::Arrow:
        canvas.arrow_head(at, {at.x + out_dir.x * 10.0, at.y + out_dir.y * 10.0}, 9.0, 8.0,
                          color);
        break;
    case Marker::None:
        break;
    }
}

Point unit(Point a, Point b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len = std::hypot(dx, dy);
    return len < 1e-9 ? Point{1.0, 0.0} : Point{dx / len, dy / len};
}

} // namespace

cplot::Scene build_class(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    CModel model = parse_class(source, options);
    const Font name_font = theme.base_font().with_weight(cplot::FontWeight::Bold);
    const Font font = theme.base_font();
    const double line_h = font.size * 1.4;

    std::vector<DagNodeSize> sizes(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        CNode& node = model.nodes[i];
        double w = text_width(node.name, name_font);
        for (const std::string& a : node.attrs) w = std::max(w, text_width(a, font));
        for (const std::string& mth : node.methods) w = std::max(w, text_width(mth, font));
        node.name_h = line_h + 6.0;
        node.attr_h = static_cast<double>(node.attrs.size()) * line_h;
        const double method_h = static_cast<double>(node.methods.size()) * line_h;
        const bool has_members = !node.attrs.empty() || !node.methods.empty();
        node.size = {w + 24.0, node.name_h + (has_members ? node.attr_h + method_h + 8.0
                                                          : 0.0)};
        sizes[i] = node.size;
    }
    std::vector<DagEdge> edges;
    for (const CRel& r : model.rels) edges.push_back({r.from, r.to});

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

    // Relationships (behind classes).
    for (std::size_t k = 0; k < model.rels.size(); ++k) {
        const CRel& r = model.rels[k];
        std::vector<Point> route = layout.routes[k];
        if (route.size() < 2) continue;
        for (Point& p : route) p = shift(p);
        const DagNodeSize sf = sizes[static_cast<std::size_t>(r.from)];
        const DagNodeSize st = sizes[static_cast<std::size_t>(r.to)];
        route = clip_route(route, shift(layout.centers[static_cast<std::size_t>(r.from)]),
                           sf.w, sf.h, shift(layout.centers[static_cast<std::size_t>(r.to)]),
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
        ShapeStyle ls;
        ls.stroke = line_color;
        ls.stroke_width = 1.3;
        if (r.dashed) ls.dash = cplot::DashPattern{{5.0, 3.0}};
        canvas.polyline(route, ls);
        draw_marker(canvas, r.from_marker, route.front(), unit(route.front(), route[1]),
                    line_color, sty.label_mask);
        draw_marker(canvas, r.to_marker, route.back(),
                    unit(route.back(), route[route.size() - 2]), line_color,
                    sty.label_mask);
        labels.add(route, r.label, font);
    }

    // Classes.
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const CNode& node = model.nodes[i];
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
        // Report where this class landed, so a point on the page can become a
        // selection (scene_map.hpp).
        if (options.regions != nullptr)
            options.regions->push_back({node.name, 0 /* HitRole::Node */, box});
        if (const auto it = model.links.find(node.name); it != model.links.end())
            canvas.add_link(box, it->second.href, it->second.title, it->second.target);
        canvas.text({c.x, box.y + node.name_h / 2.0}, node.name, name_font,
                    sp.text.value_or(sty.text), HAlign::Center, VAlign::Middle);
        const bool has_members = !node.attrs.empty() || !node.methods.empty();
        if (!has_members) continue;
        double y = box.y + node.name_h;
        canvas.line({box.x, y}, {box.x + box.w, y}, body);  // under name
        for (const std::string& a : node.attrs) {
            canvas.text({box.x + 8.0, y + line_h / 2.0}, a, font, sty.text,
                        HAlign::Left, VAlign::Middle);
            y += line_h;
        }
        y += 4.0;
        canvas.line({box.x, y}, {box.x + box.w, y}, body);  // between attrs and methods
        y += 4.0;
        for (const std::string& mth : node.methods) {
            canvas.text({box.x + 8.0, y + line_h / 2.0}, mth, font, sty.text,
                        HAlign::Left, VAlign::Middle);
            y += line_h;
        }
    }
    labels.draw(canvas, sty.muted, sty.label_mask);

    return canvas.bake();
}

} // namespace cdiagram::detail
