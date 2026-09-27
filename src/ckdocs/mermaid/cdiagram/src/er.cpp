// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Entity-relationship diagram (`erDiagram`): entities with attribute
// tables, connected by relationships with crow's-foot cardinality. Laid
// out with the layered engine.

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

struct Attr {
    std::string type;
    std::string name;
    std::string key;  // PK / FK / UK (or comma combinations), else empty
};
struct Entity {
    std::string name;   ///< identity, referenced by relationships
    std::string label;  ///< displayed title — the alias label, default the name
    std::vector<Attr> attrs;
    DagNodeSize size;
    double header_h = 0.0;
    double type_w = 0.0;
    double name_w = 0.0;
    double key_w = 0.0;
};
struct ERel {
    int from = 0;
    int to = 0;
    std::string left;   // cardinality at the `from` end
    std::string right;  // cardinality at the `to` end
    bool dashed = false;
    std::string label;
};
struct ERModel {
    std::vector<Entity> entities;
    std::vector<ERel> rels;
    StyleSheet styles;  ///< classDef / class / ::: / style resolution (by entity id)
    LinkTable links;    ///< entity name -> hyperlink (click …)
};

int entity_index(ERModel& model, const std::string& name) {
    for (std::size_t i = 0; i < model.entities.size(); ++i)
        if (model.entities[i].name == name) return static_cast<int>(i);
    model.entities.push_back({name, name, {}, {}, 0.0, 0.0});
    return static_cast<int>(model.entities.size()) - 1;
}

/// Resolve an entity reference, honouring Mermaid's alias form
/// `id[Display label]` (the id stays the relationship identity, the
/// bracketed text becomes the drawn title), quoted names
/// (`"Customer Account"`), and inline `:::class` annotations — which are
/// recorded under the bare id, not the raw token.
int entity_ref(ERModel& model, const std::string& raw) {
    std::vector<std::string> classes;
    const std::string token = take_class_annotations(cworks::trim(raw), classes);
    int idx = -1;
    const std::size_t open = token.find('[');
    if (open != std::string::npos && open > 0 && token.back() == ']') {
        const std::string id = cworks::trim(token.substr(0, open));
        const std::string label =
            unquote(cworks::trim(token.substr(open + 1, token.size() - open - 2)));
        idx = entity_index(model, id);
        if (!label.empty()) model.entities[static_cast<std::size_t>(idx)].label = label;
    } else {
        idx = entity_index(model, unquote(token));
    }
    for (const std::string& cls : classes)
        model.styles.assign(model.entities[static_cast<std::size_t>(idx)].name, cls);
    return idx;
}

/// The relationship-label colon: the first ':' that is not part of a
/// `:::` class annotation (`c:::big ||--o{ o : places`).
std::size_t label_colon(const std::string& s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != ':') continue;
        const bool joined = (i > 0 && s[i - 1] == ':') || (i + 1 < s.size() && s[i + 1] == ':');
        if (!joined) return i;
    }
    return std::string::npos;
}

/// Whitespace tokenizer that keeps quoted names and bracketed alias
/// labels together: `c["Customer Account"] ||--o{ "Big Order" : x` yields
/// three tokens plus the relation.
std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    int brackets = 0;
    for (const char c : s) {
        if (c == '"') quoted = !quoted;
        if (c == '[') ++brackets;
        if (c == ']' && brackets > 0) --brackets;
        if ((c == ' ' || c == '\t') && !quoted && brackets == 0) {
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

/// True when `token` is a Mermaid attribute key constraint: PK, FK or UK,
/// or a comma-joined combination of them (e.g. "PK,FK"). Case-sensitive,
/// as Mermaid emits them upper-case.
bool is_key_token(const std::string& token) {
    std::string part;
    const auto part_ok = [](const std::string& p) {
        return p == "PK" || p == "FK" || p == "UK";
    };
    for (const char c : token) {
        if (c == ',') {
            if (!part_ok(part)) return false;
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    return part_ok(part);
}

ERModel parse_er(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> kKnown = {"direction", "click"};
    ERModel model;
    int block = -1;  // entity index whose attribute block we are inside
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        if (block >= 0) {
            if (line == "}") {
                block = -1;
                continue;
            }
            const std::vector<std::string> t = split_ws(line);
            if (!t.empty()) {
                Attr a;
                a.type = t[0];
                a.name = t.size() > 1 ? t[1] : "";
                // The optional third token is the key constraint (PK/FK/UK,
                // possibly comma-joined); anything else (a quoted comment)
                // is not a key and is left out.
                if (t.size() > 2 && is_key_token(t[2])) a.key = t[2];
                model.entities[static_cast<std::size_t>(block)].attrs.push_back(a);
            }
            continue;
        }
        if (!line.empty() && line.back() == '{') {
            const std::string name = cworks::trim(line.substr(0, line.size() - 1));
            block = entity_ref(model, name);
            continue;
        }
        const std::string keyword = line.substr(0, line.find_first_of(" \t"));
        // Recognised erDiagram directives the engine does not render.
        // Without this, a line like `direction TB` was dropped silently and
        // a bare `direction` fell through to the bare-entity branch and
        // fabricated a phantom entity box.
        if (keyword == "direction") {
            diagnose_unsupported(options, "erDiagram", number, keyword,
                                 "layout direction is not rendered");
            continue;
        }
        // C2 (v11 erDiagram styling): classDef / class / style.
        if (parse_style_statement(line, model.styles, options, "erDiagram", number))
            continue;
        // Interaction: `click <entity> "url" ["tip"] [_target]` attaches a
        // hyperlink to an entity box. JS callback forms carry no URL and
        // cannot be exported, so they are reported, not linked.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!model.links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("erDiagram", number,
                                     "entity '" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "erDiagram", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        // Relationship: E1 <card><line><card> E2 : label. The label colon
        // must skip `:::` class annotations on the endpoints.
        std::string body = line;
        std::string label;
        const std::size_t colon = label_colon(body);
        if (colon != std::string::npos) {
            label = unquote(cworks::trim(body.substr(colon + 1)));
            body = cworks::trim(body.substr(0, colon));
        }
        const std::vector<std::string> t = split_ws(body);
        if (t.size() == 3) {
            const std::string& rel = t[1];
            std::size_t sep = rel.find("--");
            bool dashed = false;
            if (sep == std::string::npos) {
                sep = rel.find("..");
                dashed = true;
            }
            if (sep != std::string::npos) {
                ERel r;
                r.from = entity_ref(model, t[0]);
                r.to = entity_ref(model, t[2]);
                r.left = rel.substr(0, sep);
                r.right = rel.substr(sep + 2);
                r.dashed = dashed;
                r.label = label;
                model.rels.push_back(r);
                continue;
            }
        }
        if (t.size() == 1) {
            const std::string& tok = t[0];
            // Alias declaration `p[Person]` — identity p, shown as Person.
            const std::size_t open = tok.find('[');
            if (open != std::string::npos && open > 0 && tok.back() == ']') {
                entity_ref(model, tok);
                continue;
            }
            bool ident = !tok.empty();
            for (const char c : tok)
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '_')) {
                    ident = false;
                    break;
                }
            if (ident) {
                entity_index(model, tok);  // bare entity declaration
                continue;
            }
        }
        // Nothing matched (or a lone non-identifier token): report the line
        // rather than fabricating a bogus entity box (never-silently-drop).
        diagnose_unrecognized(options, "erDiagram", number, keyword, kKnown);
    }
    if (model.entities.empty()) throw Error(cworks::validation_failed("erDiagram: no entities"));
    return model;
}

/// A crow's-foot cardinality marker at border point `at`, with `out_dir`
/// the unit direction leaving the entity along the edge.
void draw_cardinality(Canvas& canvas, Point at, Point out_dir, const std::string& card,
                      Color color) {
    const bool many = card.find('{') != std::string::npos ||
                      card.find('}') != std::string::npos;
    const bool zero = card.find('o') != std::string::npos;
    const bool one = card.find('|') != std::string::npos;
    const double px = -out_dir.y, py = out_dir.x;  // perpendicular
    ShapeStyle s;
    s.stroke = color;
    s.stroke_width = 1.3;
    if (many) {
        const Point apex{at.x + out_dir.x * 14.0, at.y + out_dir.y * 14.0};
        canvas.line(apex, at, s);
        canvas.line(apex, {at.x + px * 7.0, at.y + py * 7.0}, s);
        canvas.line(apex, {at.x - px * 7.0, at.y - py * 7.0}, s);
    }
    if (one) {
        const double d = many ? 16.0 : 9.0;
        const Point c{at.x + out_dir.x * d, at.y + out_dir.y * d};
        canvas.line({c.x + px * 7.0, c.y + py * 7.0}, {c.x - px * 7.0, c.y - py * 7.0}, s);
    }
    if (zero) {
        const double d = many ? 20.0 : (one ? 16.0 : 9.0);
        canvas.circle({at.x + out_dir.x * d, at.y + out_dir.y * d}, 4.0, s);
    }
}

Point unit(Point a, Point b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len = std::hypot(dx, dy);
    return len < 1e-9 ? Point{1.0, 0.0} : Point{dx / len, dy / len};
}

} // namespace

cplot::Scene build_er(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    ERModel model = parse_er(source, options);
    const Font header_font = theme.base_font().with_weight(cplot::FontWeight::Bold);
    const Font font = theme.base_font();
    const double line_h = font.size * 1.4;

    std::vector<DagNodeSize> sizes(model.entities.size());
    for (std::size_t i = 0; i < model.entities.size(); ++i) {
        Entity& e = model.entities[i];
        double type_w = 0.0, name_w = 0.0, key_w = 0.0;
        for (const Attr& a : e.attrs) {
            type_w = std::max(type_w, text_width(a.type, font));
            name_w = std::max(name_w, text_width(a.name, font));
            key_w = std::max(key_w, text_width(a.key, font));
        }
        e.type_w = type_w;
        e.name_w = name_w;
        e.key_w = key_w;
        const double row_w = type_w + (name_w > 0.0 ? 14.0 + name_w : 0.0) +
                             (key_w > 0.0 ? 14.0 + key_w : 0.0);
        const double w = std::max(text_width(e.label, header_font), row_w) + 24.0;
        e.header_h = line_h + 6.0;
        const double h = e.header_h + static_cast<double>(e.attrs.size()) * line_h;
        e.size = {w, h};
        sizes[i] = e.size;
    }
    std::vector<DagEdge> edges;
    for (const ERel& r : model.rels) edges.push_back({r.from, r.to});

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

    // Relationships (behind entities).
    for (std::size_t k = 0; k < model.rels.size(); ++k) {
        const ERel& r = model.rels[k];
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
        if (r.dashed) ls.dash = cplot::DashPattern{{4.0, 3.0}};
        canvas.polyline(route, ls);
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
        draw_cardinality(canvas, route.front(), unit(route.front(), route[1]), r.left,
                         line_color);
        draw_cardinality(canvas, route.back(),
                         unit(route.back(), route[route.size() - 2]), r.right, line_color);
        labels.add(route, r.label, font);
    }

    // Entities.
    for (std::size_t i = 0; i < model.entities.size(); ++i) {
        const Entity& e = model.entities[i];
        const Point c = shift(layout.centers[i]);
        const RectF box{c.x - e.size.w / 2.0, c.y - e.size.h / 2.0, e.size.w, e.size.h};
        // C2: layer the resolved classDef/style properties over the theme;
        // `fill:` tints the header band (the entity's coloured part).
        const StyleProps sp = model.styles.resolve(e.name);
        ShapeStyle body;
        body.fill = sty.entity_fill;
        body.stroke = sp.stroke.value_or(sty.entity_stroke);
        body.stroke_width = sp.stroke_width.value_or(1.0);
        if (sp.dash) body.dash = *sp.dash;
        canvas.rect(box, body);
        // Report where this entity landed, so a point on the page can become a
        // selection (scene_map.hpp).
        if (options.regions != nullptr)
            options.regions->push_back({e.name, 0 /* HitRole::Node */, box});
        if (const auto it = model.links.find(e.name); it != model.links.end())
            canvas.add_link(box, it->second.href, it->second.title, it->second.target);
        ShapeStyle header;
        header.fill = sp.fill.value_or(sty.header_fill);
        canvas.rect(RectF{box.x, box.y, box.w, e.header_h}, header);
        canvas.line({box.x, box.y + e.header_h}, {box.x + box.w, box.y + e.header_h}, body);
        canvas.text({c.x, box.y + e.header_h / 2.0}, e.label, header_font,
                    sp.text.value_or(sty.text), HAlign::Center, VAlign::Middle);
        for (std::size_t a = 0; a < e.attrs.size(); ++a) {
            const double y = box.y + e.header_h + (static_cast<double>(a) + 0.5) * line_h;
            canvas.text({box.x + 10.0, y}, e.attrs[a].type, font, sty.text,
                        HAlign::Left, VAlign::Middle);
            if (!e.attrs[a].name.empty())
                canvas.text({box.x + 10.0 + e.type_w + 14.0, y}, e.attrs[a].name, font,
                            sty.muted, HAlign::Left, VAlign::Middle);
            // The key constraint (PK/FK/UK) sits in its own right-aligned
            // column, accented so it reads at a glance.
            if (!e.attrs[a].key.empty())
                canvas.text({box.x + box.w - 10.0, y}, e.attrs[a].key, font,
                            sty.accent, HAlign::Right, VAlign::Middle);
        }
    }
    labels.draw(canvas, sty.muted, sty.label_mask);

    return canvas.bake();
}

} // namespace cdiagram::detail
