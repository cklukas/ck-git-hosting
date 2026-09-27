// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Architecture diagram (`architecture-beta`): services (optionally in
// groups) connected by edges. Each group is a band of its services in a
// dashed box; edges connect service centres. The directional side hints
// (:L/:R/:T/:B) are parsed but not used for placement in this version.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "canvas.hpp"
#include "cdiagram/error.hpp"
#include "dag_layout.hpp"  // DagNodeSize
#include "diagnostics.hpp"
#include "links.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram::detail {

namespace {

struct Service {
    std::string id;
    std::string label;
    int group = -1;
    Point center;
    DagNodeSize size;
};
struct Group {
    std::string id;
    std::string label;
};
struct AEdge {
    int from = 0;
    int to = 0;
    bool arrow = false;
};
struct AModel {
    std::vector<Service> services;
    std::vector<Group> groups;
    std::vector<AEdge> edges;
    LinkTable links;  ///< service/junction id -> hyperlink (click …)
};

/// `id(icon)[Label]` → id + label. The icon name is reported through
/// `icon` so the caller can diagnose it (icon fonts are not rendered);
/// it must not vanish silently.
void parse_decl(const std::string& s, std::string& id, std::string& label,
                std::string& icon) {
    const std::size_t paren = s.find('(');
    const std::size_t lb = s.find('[');
    id = cworks::trim(s.substr(0, paren != std::string::npos ? paren
                                   : lb != std::string::npos  ? lb
                                                              : s.size()));
    if (lb != std::string::npos) {
        const std::size_t rb = s.rfind(']');
        if (rb != std::string::npos && rb > lb) {
            label = cworks::trim(s.substr(lb + 1, rb - lb - 1));
            if (label.size() >= 2 && label.front() == '"' && label.back() == '"')
                label = label.substr(1, label.size() - 2);
        }
    }
    if (label.empty()) label = id;
    if (paren != std::string::npos) {
        const std::size_t rp = s.find(')', paren + 1);
        if (rp != std::string::npos) icon = cworks::trim(s.substr(paren + 1, rp - paren - 1));
    }
}

int service_index(AModel& model, const std::string& id) {
    for (std::size_t i = 0; i < model.services.size(); ++i)
        if (model.services[i].id == id) return static_cast<int>(i);
    model.services.push_back({id, id, -1, {}, {}});
    return static_cast<int>(model.services.size()) - 1;
}
int group_index(AModel& model, const std::string& id) {
    for (std::size_t i = 0; i < model.groups.size(); ++i)
        if (model.groups[i].id == id) return static_cast<int>(i);
    return -1;
}
/// Look up an already-declared service without fabricating one; -1 if
/// unknown. Edges use this so a stray id never conjures a phantom box.
int service_find(const AModel& model, const std::string& id) {
    for (std::size_t i = 0; i < model.services.size(); ++i)
        if (model.services[i].id == id) return static_cast<int>(i);
    return -1;
}

/// The node id at one end of an edge token like `db:L` or `db{group}:L`.
std::string edge_endpoint(const std::string& token) {
    std::string t = cworks::trim(token);
    const std::size_t brace = t.find('{');
    if (brace != std::string::npos) t = t.substr(0, brace);
    const std::size_t colon = t.find(':');
    if (colon != std::string::npos) t = t.substr(0, colon);
    // an endpoint written `L:db` (side first) keeps the id after the colon
    return cworks::trim(t.empty() ? cworks::trim(token) : t);
}

} // namespace

cplot::Scene build_architecture(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> kKnown = {"group", "service", "junction", "click"};
    AModel model;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        // Interaction: `click <id> "url" ["tip"] [_target]` attaches a
        // hyperlink to the service/junction box. Handled before the edge
        // branch because a URL may itself contain `--`. JS callback forms
        // carry no URL and cannot be exported, so they are reported, not linked.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!model.links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("architecture", number,
                                     "'" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "architecture", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        if (line.rfind("group ", 0) == 0) {
            std::string id, label, icon;
            parse_decl(cworks::trim(line.substr(6)), id, label, icon);
            if (!icon.empty())
                diagnose_unsupported(options, "architecture", number, "(" + icon + ")",
                                     "icons are not rendered");
            if (group_index(model, id) < 0) model.groups.push_back({id, label});
        } else if (line.rfind("service ", 0) == 0 || line.rfind("junction ", 0) == 0) {
            const std::size_t sp = line.find(' ');
            std::string rest = cworks::trim(line.substr(sp + 1));
            int in_group = -1;
            const std::size_t in = rest.find(" in ");
            if (in != std::string::npos) {
                in_group = group_index(model, cworks::trim(rest.substr(in + 4)));
                rest = cworks::trim(rest.substr(0, in));
            }
            std::string id, label, icon;
            parse_decl(rest, id, label, icon);
            if (!icon.empty())
                diagnose_unsupported(options, "architecture", number, "(" + icon + ")",
                                     "icons are not rendered");
            const int idx = service_index(model, id);
            model.services[static_cast<std::size_t>(idx)].label = label;
            model.services[static_cast<std::size_t>(idx)].group = in_group;
        } else {
            // edge: `a:R -- L:b` / `a:R --> L:b`
            std::size_t op = line.find("--");
            if (op == std::string::npos) {
                // Not a group/service/junction declaration and not an edge:
                // report the line rather than dropping it in silence.
                const std::string keyword = line.substr(0, line.find_first_of(" \t"));
                diagnose_unrecognized(options, "architecture-beta", number, keyword, kKnown);
                continue;
            }
            const bool arrow = line.find("-->") != std::string::npos ||
                               line.find("<--") != std::string::npos;
            const std::size_t oplen = line.compare(op, 3, "-->") == 0 ? 3 : 2;
            const std::string left = edge_endpoint(line.substr(0, op));
            std::string right_tok = cworks::trim(line.substr(op + oplen));
            // strip a leading side like `L:b` → keep `b`
            const std::size_t colon = right_tok.find(':');
            if (colon != std::string::npos && colon <= 1) right_tok = right_tok.substr(colon + 1);
            const std::string right = edge_endpoint(right_tok);
            // An edge may only connect services that were declared. Feeding
            // an unknown id to service_index() would fabricate a phantom box
            // and a spurious edge, so look up without creating and report the
            // dangling reference instead of silently inventing structure.
            const int fi = service_find(model, left);
            const int ti = service_find(model, right);
            if (fi < 0 || ti < 0) {
                diagnose_unrecognized(options, "architecture-beta", number, line, kKnown);
                continue;
            }
            model.edges.push_back({fi, ti, arrow});
        }
    }
    if (model.services.empty()) throw Error(cworks::validation_failed("architecture: no services"));

    const Font font = theme.base_font();
    const Font group_font = theme.base_font().with_weight(cplot::FontWeight::Bold);
    const double line_h = font.size * 1.35;
    const double margin = 24.0;
    const double box_h = line_h + 18.0;
    const double svc_gap = 26.0;
    const double band_gap = 26.0;
    const double group_pad = 16.0;
    const double group_header = line_h + 6.0;

    for (Service& s : model.services)
        s.size = {std::max(text_width(s.label, font) + 26.0, 80.0), box_h};

    // Bands: one per group (its members), then the ungrouped services.
    std::vector<std::vector<int>> bands;
    for (std::size_t g = 0; g < model.groups.size(); ++g) {
        std::vector<int> members;
        for (std::size_t i = 0; i < model.services.size(); ++i)
            if (model.services[i].group == static_cast<int>(g)) members.push_back(static_cast<int>(i));
        if (!members.empty()) bands.push_back(members);
    }
    std::vector<int> ungrouped;
    for (std::size_t i = 0; i < model.services.size(); ++i)
        if (model.services[i].group < 0) ungrouped.push_back(static_cast<int>(i));
    if (!ungrouped.empty()) bands.push_back(ungrouped);

    Canvas canvas;
    canvas.background = sty.page;

    double y = margin;
    double max_x = 0.0;
    std::vector<RectF> group_boxes;
    std::vector<std::string> group_labels;
    for (std::size_t bi = 0; bi < bands.size(); ++bi) {
        const std::vector<int>& members = bands[bi];
        const bool is_group = members.empty() ? false : model.services[static_cast<std::size_t>(members[0])].group >= 0;
        const double band_top = y + (is_group ? group_header : 0.0);
        double x = margin + (is_group ? group_pad : 0.0);
        for (const int i : members) {
            Service& s = model.services[static_cast<std::size_t>(i)];
            s.center = {x + s.size.w / 2.0, band_top + s.size.h / 2.0};
            x += s.size.w + svc_gap;
        }
        const double band_right = x - svc_gap + (is_group ? group_pad : 0.0);
        max_x = std::max(max_x, band_right);
        if (is_group) {
            group_boxes.push_back(RectF{margin, y, band_right - margin,
                                        group_header + box_h + group_pad});
            group_labels.push_back(model.groups[static_cast<std::size_t>(
                model.services[static_cast<std::size_t>(members[0])].group)].label);
            y += group_header + box_h + group_pad + band_gap;
        } else {
            y += box_h + band_gap;
        }
    }
    canvas.width = max_x + margin;
    canvas.height = y - band_gap + margin;

    // Group boxes.
    for (std::size_t g = 0; g < group_boxes.size(); ++g) {
        ShapeStyle box;
        box.stroke = sty.node_stroke;
        box.stroke_width = 1.2;
        box.dash = cplot::DashPattern{{4.0, 3.0}};
        canvas.rounded_rect(group_boxes[g], 8.0, box);
        canvas.text({group_boxes[g].x + 10.0, group_boxes[g].y + group_header / 2.0 + 2.0},
                    group_labels[g], group_font, sty.text, HAlign::Left,
                    VAlign::Middle);
    }

    // Edges.
    const Color edge_color = sty.edge;
    for (const AEdge& e : model.edges) {
        const Service& a = model.services[static_cast<std::size_t>(e.from)];
        const Service& b = model.services[static_cast<std::size_t>(e.to)];
        const Point pa = box_border(a.center, a.size.w, a.size.h, b.center);
        const Point pb = box_border(b.center, b.size.w, b.size.h, a.center);
        ShapeStyle ls;
        ls.stroke = edge_color;
        ls.stroke_width = 1.4;
        // Stop the stroke at the arrowhead base so the tip stays sharp.
        const Point line_end =
            e.arrow ? retract_end({pa, pb}, 8.0).back() : pb;
        canvas.line(pa, line_end, ls);
        if (e.arrow) canvas.arrow_head(pb, pa, 9.0, 8.0, edge_color);
    }

    // Services.
    for (std::size_t service_index = 0; service_index < model.services.size();
         ++service_index) {
        const Service& s = model.services[service_index];
        ShapeStyle box;
        box.fill = sty.node_fill;
        box.stroke = sty.node_stroke;
        box.stroke_width = 1.0;
        const RectF rect{s.center.x - s.size.w / 2.0, s.center.y - s.size.h / 2.0,
                         s.size.w, s.size.h};
        canvas.rounded_rect(rect, 6.0, box);
        if (options.regions != nullptr) {
            options.regions->push_back(
                {"#" + std::to_string(service_index), 0 /* HitRole::Node */, rect});
        }
        canvas.text(s.center, s.label, font, sty.text, HAlign::Center,
                    VAlign::Middle);
        if (const auto it = model.links.find(s.id); it != model.links.end())
            canvas.add_link(rect, it->second.href, it->second.title, it->second.target);
    }

    return canvas.bake();
}

} // namespace cdiagram::detail
