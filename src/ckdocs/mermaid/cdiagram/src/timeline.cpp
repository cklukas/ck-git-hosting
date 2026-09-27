// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Timeline (`timeline`): time periods along a horizontal axis, each with
// its events stacked above. Pure geometry — deterministic.

#include <algorithm>
#include <string>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "frontmatter.hpp"
#include "canvas.hpp"
#include "cdiagram/error.hpp"
#include "diagnostics.hpp"
#include "links.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram::detail {

namespace {

struct Period {
    std::string label;
    std::vector<std::string> events;
    int section = -1;  ///< index into the section list (-1 = ungrouped)
};

std::vector<std::string> split_colon(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ':') {
            out.push_back(cworks::trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cworks::trim(cur));
    return out;
}

} // namespace

cplot::Scene build_timeline(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    const FrontMatter matter(source, options, "timeline");
    const bool multicolor = !matter.boolean("disableMulticolor").value_or(false);
    const std::vector<SourceLine> lines = significant_lines(source);
    // Keywords that are legal timeline statements (for did-you-mean hints).
    static const std::vector<std::string> kKnown = {"timeline", "title", "section", "click"};
    // Directives that are valid Mermaid in OTHER diagram types but never
    // begin a timeline period. Without this guard the period catch-all below
    // fabricated a bogus period from e.g. `classDef important fill:#f00`
    // (label "classDef important fill", event "#f00").
    static const std::vector<std::string> kForeign = {
        "classDef",   "class",    "style",      "cssClass",        "callback",
        "link",       "note",     "state",      "participant",
        "actor",      "subgraph", "direction",  "graph",           "flowchart",
        "gantt",      "pie",      "journey",    "kanban",          "mindmap",
        "sequenceDiagram", "erDiagram", "gitGraph", "quadrantChart", "requirement"};
    std::string title;
    std::vector<Period> periods;
    std::vector<std::string> sections;
    LinkTable links;  ///< event text -> hyperlink (click …)
    int section = -1;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        if (line.rfind("title ", 0) == 0) {
            title = cworks::trim(line.substr(6));
            continue;
        }
        // Interaction: `click <event> "url" ["tip"] [_target]` attaches a
        // hyperlink to the matching event card. JS callback forms carry no URL
        // and cannot be exported, so they are reported, not linked.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("timeline", number,
                                     "event '" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "timeline", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        const std::string keyword = line.substr(0, line.find_first_of(" \t:"));
        if (keyword == "section") {
            sections.push_back(cworks::trim(line.substr(7)));
            section = static_cast<int>(sections.size()) - 1;
            continue;
        }
        if (std::find(kForeign.begin(), kForeign.end(), keyword) != kForeign.end()) {
            // A directive borrowed from another diagram type: report it
            // rather than fabricating a spurious period from it.
            diagnose_unrecognized(options, "timeline", number, keyword, kKnown);
            continue;
        }
        const std::vector<std::string> parts = split_colon(line);
        if (parts.empty() || parts[0].empty()) {
            // No usable period label (e.g. a line starting with ':') — the
            // charter forbids silently dropping input, so flag it.
            diagnose_unrecognized(options, "timeline", number, keyword, kKnown);
            continue;
        }
        Period p;
        p.section = section;
        p.label = parts[0];
        for (std::size_t i = 1; i < parts.size(); ++i)
            if (!parts[i].empty()) p.events.push_back(parts[i]);
        periods.push_back(std::move(p));
    }
    if (periods.empty()) throw Error(cworks::validation_failed("timeline: no periods"));

    const Font font = theme.base_font();
    const Font title_font = theme.title_font();
    const Font period_font = theme.base_font().with_weight(cplot::FontWeight::Bold);
    const double line_h = font.size * 1.35;
    const double card_pad = 8.0, card_gap = 8.0, col_gap = 24.0;
    const double margin = 24.0;
    const double title_h = title.empty() ? 0.0 : title_font.size * 1.9;

    // Column widths and the tallest event stack.
    std::vector<double> col_w(periods.size());
    std::size_t max_events = 0;
    for (std::size_t i = 0; i < periods.size(); ++i) {
        double w = text_width(periods[i].label, period_font);
        for (const std::string& e : periods[i].events)
            w = std::max(w, text_width(e, font) + 2.0 * card_pad);
        col_w[i] = w;
        max_events = std::max(max_events, periods[i].events.size());
    }
    const double card_h = line_h + 2.0 * card_pad;
    const double events_h = static_cast<double>(max_events) * (card_h + card_gap);
    const double baseline_y = margin + title_h + events_h + 20.0;
    const double period_label_y = baseline_y + 14.0;

    double total_w = margin;
    std::vector<double> col_x(periods.size());
    for (std::size_t i = 0; i < periods.size(); ++i) {
        col_x[i] = total_w + col_w[i] / 2.0;
        total_w += col_w[i] + col_gap;
    }
    total_w += margin - col_gap;

    // Sections add a labelled band row under the period labels; periods in
    // a section share its colour.
    const double section_row_h = sections.empty() ? 0.0 : line_h + 12.0;
    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = total_w;
    canvas.height = period_label_y + line_h + section_row_h + margin;
    canvas.title = title;

    if (!title.empty())
        canvas.text({canvas.width / 2.0, margin}, title, title_font, sty.text,
                    HAlign::Center, VAlign::Top);

    // Axis.
    ShapeStyle axis;
    axis.stroke = sty.edge;
    axis.stroke_width = 2.0;
    canvas.line({margin, baseline_y}, {canvas.width - margin, baseline_y}, axis);

    // Section bands, spanning each section's period columns.
    for (std::size_t si = 0; si < sections.size(); ++si) {
        double x0 = 0.0, x1 = 0.0;
        bool any = false;
        for (std::size_t i = 0; i < periods.size(); ++i) {
            if (periods[i].section != static_cast<int>(si)) continue;
            const double left = col_x[i] - col_w[i] / 2.0;
            const double right = col_x[i] + col_w[i] / 2.0;
            if (!any) {
                x0 = left;
                x1 = right;
                any = true;
            } else {
                x0 = std::min(x0, left);
                x1 = std::max(x1, right);
            }
        }
        if (!any) continue;  // a section with no periods frames nothing
        const Color col = sty.series(multicolor ? si : 0);
        const double band_y = period_label_y + line_h + 6.0;
        ShapeStyle band;
        band.fill = col.with_alpha(0.16);
        band.stroke = col;
        band.stroke_width = 1.0;
        canvas.rounded_rect(RectF{x0, band_y, x1 - x0, line_h + 4.0}, 4.0, band);
        canvas.text({(x0 + x1) / 2.0, band_y + (line_h + 4.0) / 2.0}, sections[si], font,
                    sty.text, HAlign::Center, VAlign::Middle);
    }

    std::size_t event_ordinal = 0;
    for (std::size_t i = 0; i < periods.size(); ++i) {
        // Periods in a section wear the section's colour.
        const Color col = sty.series(
            multicolor ? (periods[i].section >= 0 ? static_cast<std::size_t>(periods[i].section)
                                                  : i)
                       : 0);
        const double cx = col_x[i];
        // Marker + period label.
        ShapeStyle dot;
        dot.fill = col;
        canvas.circle({cx, baseline_y}, 5.0, dot);
        canvas.text({cx, period_label_y}, periods[i].label, period_font, sty.text,
                    HAlign::Center, VAlign::Top);
        // Event cards stacked upward from just above the axis. They are
        // counted across every period, because document order runs period by
        // period and that is the order the model holds them in.
        double y = baseline_y - 18.0 - card_h;
        for (const std::string& e : periods[i].events) {
            ShapeStyle card;
            card.fill = col.with_alpha(0.16);
            card.stroke = col;
            card.stroke_width = 1.0;
            const RectF box{cx - col_w[i] / 2.0, y, col_w[i], card_h};
            canvas.rounded_rect(box, 6.0, card);
            if (options.regions != nullptr) {
                options.regions->push_back(
                    {"#" + std::to_string(event_ordinal++), 0 /* HitRole::Node */, box});
            }
            canvas.text({cx, y + card_h / 2.0}, e, font, sty.text, HAlign::Center,
                        VAlign::Middle);
            if (const auto it = links.find(e); it != links.end())
                canvas.add_link(box, it->second.href, it->second.title, it->second.target);
            y -= card_h + card_gap;
        }
    }

    return canvas.bake();
}

} // namespace cdiagram::detail
