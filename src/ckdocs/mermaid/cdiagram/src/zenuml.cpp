// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// ZenUML (`zenuml`): an alternate method-call sequence DSL. This version
// renders the direct `A->B: message` / `A->B.method()` interactions as a
// classic sequence diagram; nesting, returns and fragments are not drawn.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "canvas.hpp"
#include "cdiagram/error.hpp"
#include "diagnostics.hpp"
#include "links.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram::detail {

namespace {

struct ZMsg {
    int from = 0;
    int to = 0;
    std::string text;
};

// Leading run of alphabetic characters — the statement keyword. Handles
// ZenUML fragment headers like `if(cond) {` and `loop {`, where the token
// runs into a paren or brace rather than whitespace.
std::string leading_word(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    std::size_t e = b;
    while (e < s.size() && std::isalpha(static_cast<unsigned char>(s[e]))) ++e;
    return s.substr(b, e - b);
}

int participant(std::vector<std::string>& names, const std::string& raw) {
    std::string id = cworks::trim(raw);
    if (!id.empty() && id.front() == '@') id = cworks::trim(id.substr(1));
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == id) return static_cast<int>(i);
    names.push_back(id);
    return static_cast<int>(names.size()) - 1;
}

} // namespace

cplot::Scene build_zenuml(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> kKnown = {
        "title", "if", "else", "while", "for", "loop", "opt", "alt", "par",
        "try", "catch", "finally", "return", "new", "group", "click"};
    std::vector<std::string> names;
    std::vector<ZMsg> msgs;
    LinkTable links;  ///< participant name -> hyperlink (click …)
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        // ZenUML uses `//` line comments; significant_lines only strips
        // Mermaid `%%` comments, so a `//` line survives here. It is never
        // an interaction — a comment that happens to contain '->' must not
        // fabricate a bogus message.
        if (line.rfind("//", 0) == 0) continue;
        // Interaction: `click <participant> "url" ["tip"] [_target]` attaches a
        // hyperlink to the participant box. Handled before the `->` message
        // scan because a URL may itself contain `->`. JS callback forms carry
        // no URL and cannot be exported, so they are reported, not linked.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("zenuml", number,
                                     "'" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "zenuml", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        const std::string keyword = leading_word(line);
        // For messages, fall back to the first whitespace-delimited token when
        // the line has no leading alphabetic word (e.g. `@@@`) so the report
        // names the offending text instead of an empty string.
        const std::string token =
            keyword.empty() ? line.substr(0, line.find_first_of(" \t")) : keyword;
        const std::size_t arrow = line.find("->");
        if (arrow == std::string::npos) {
            // Not an interaction. `title` is an accepted directive we simply
            // do not draw, and lone fragment braces are structural; the
            // control-flow keywords are recognised ZenUML fragments this
            // engine flattens; anything else is a line we cannot classify —
            // report it rather than drop it in silence.
            if (keyword == "title" || line == "{" || line == "}") continue;
            if (std::find(kKnown.begin(), kKnown.end(), keyword) != kKnown.end())
                diagnose_unsupported(options, "zenuml", number, keyword,
                                     "nesting, returns and fragments are not rendered");
            else
                diagnose_unrecognized(options, "zenuml", number, token, kKnown);
            continue;
        }
        if (line.rfind("title", 0) == 0) continue;
        const std::string from = line.substr(0, arrow);
        std::string rest = cworks::trim(line.substr(arrow + 2));
        // target ends at the first of ':', '.', '{', ' '
        std::size_t end = rest.size();
        for (const char d : {':', '.', '{'}) {
            const std::size_t p = rest.find(d);
            if (p != std::string::npos) end = std::min(end, p);
        }
        const std::string to = cworks::trim(rest.substr(0, end));
        std::string text;
        if (end < rest.size() && rest[end] == ':') {
            text = cworks::trim(rest.substr(end + 1));
        } else if (end < rest.size() && rest[end] == '.') {
            const std::size_t brace = rest.find('{', end);
            text = cworks::trim(rest.substr(end + 1,
                                            (brace == std::string::npos ? rest.size() : brace) - end - 1));
        }
        if (cworks::trim(from).empty() || to.empty()) {
            // An arrow with no source or target is not a usable interaction;
            // report it instead of silently discarding the line.
            diagnose_unrecognized(options, "zenuml", number, token, kKnown);
            continue;
        }
        msgs.push_back({participant(names, from), participant(names, to), text});
    }
    if (names.empty()) throw Error(cworks::validation_failed("zenuml: no interactions"));

    const Font font = theme.base_font();
    const double box_h = 34.0, box_pad_x = 14.0, min_box_w = 64.0;
    const double margin = 20.0, msg_gap = 46.0, self_w = 44.0;

    std::vector<double> box_w(names.size());
    double max_label = 0.0;
    for (const ZMsg& m : msgs) max_label = std::max(max_label, text_width(m.text, font));
    for (std::size_t i = 0; i < names.size(); ++i)
        box_w[i] = std::max(min_box_w, text_width(names[i], font) + 2.0 * box_pad_x);
    const double gap = std::max(60.0, max_label + 28.0);

    std::vector<double> center(names.size());
    center[0] = margin + box_w[0] / 2.0;
    for (std::size_t i = 1; i < names.size(); ++i)
        center[i] = center[i - 1] + box_w[i - 1] / 2.0 + gap + box_w[i] / 2.0;

    double bottom = margin + box_h + 28.0;
    for (const ZMsg& m : msgs) bottom += m.from == m.to ? msg_gap + 20.0 : msg_gap;

    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = center.back() + box_w.back() / 2.0 + margin;
    canvas.height = bottom + margin;

    // Lifelines.
    ShapeStyle lifeline;
    lifeline.stroke = sty.grid;
    lifeline.stroke_width = 1.0;
    lifeline.dash = cplot::DashPattern{{3.0, 3.0}};
    for (std::size_t i = 0; i < names.size(); ++i)
        canvas.line({center[i], margin + box_h}, {center[i], bottom}, lifeline);

    // Participant boxes.
    ShapeStyle box_style;
    box_style.fill = sty.node_fill;
    box_style.stroke = sty.node_stroke;
    box_style.stroke_width = 1.0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const RectF box{center[i] - box_w[i] / 2.0, margin, box_w[i], box_h};
        canvas.rounded_rect(box, 4.0, box_style);
        canvas.text({center[i], margin + box_h / 2.0}, names[i], font, sty.text,
                    HAlign::Center, VAlign::Middle);
        if (const auto it = links.find(names[i]); it != links.end())
            canvas.add_link(box, it->second.href, it->second.title, it->second.target);
    }

    const Color msg_color = sty.edge;
    double y = margin + box_h + 28.0;
    std::size_t message_ordinal = 0;
    for (const ZMsg& m : msgs) {
        ShapeStyle ls;
        ls.stroke = msg_color;
        ls.stroke_width = 1.4;
        if (m.from == m.to) {
            const double x = center[static_cast<std::size_t>(m.from)];
            const std::vector<Point> route{
                {x, y}, {x + self_w, y}, {x + self_w, y + 18.0}, {x, y + 18.0}};
            canvas.polyline(route, ls);
            canvas.arrow_head({x, y + 18.0}, {x + self_w, y + 18.0}, 9.0, 8.0, msg_color);
            if (options.regions != nullptr) {
                DrawnRegion region{
                    "#" + std::to_string(message_ordinal), 0 /* HitRole::Node */,
                    RectF{x - 5.0, y - 5.0, self_w + 10.0, 28.0}};
                region.path = route;
                options.regions->push_back(std::move(region));
            }
            if (!m.text.empty())
                canvas.text({x + self_w + 6.0, y + 9.0}, m.text, font, sty.text,
                            HAlign::Left, VAlign::Middle);
            y += msg_gap + 20.0;
        } else {
            const double fx = center[static_cast<std::size_t>(m.from)];
            const double tx = center[static_cast<std::size_t>(m.to)];
            const std::vector<Point> route{{fx, y}, {tx, y}};
            canvas.line({fx, y}, retract_end(route, 9.0).back(), ls);
            canvas.arrow_head({tx, y}, {fx, y}, 10.0, 8.0, msg_color);
            if (options.regions != nullptr) {
                const double min_x = std::min(fx, tx);
                DrawnRegion region{
                    "#" + std::to_string(message_ordinal), 0 /* HitRole::Node */,
                    RectF{min_x - 5.0, y - 5.0, std::abs(tx - fx) + 10.0, 10.0}};
                region.path = route;
                options.regions->push_back(std::move(region));
            }
            if (!m.text.empty())
                canvas.masked_text({(fx + tx) / 2.0, y - 6.0}, m.text, font, sty.text,
                                   HAlign::Center, VAlign::Bottom, sty.label_mask);
            y += msg_gap;
        }
        ++message_ordinal;
    }

    return canvas.bake();
}

} // namespace cdiagram::detail
