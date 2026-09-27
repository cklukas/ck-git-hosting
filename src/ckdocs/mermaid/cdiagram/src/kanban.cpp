// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Kanban board (`kanban`): workflow columns (least-indented lines) each
// holding a stack of cards (more-indented lines). Pure geometry.

#include <algorithm>
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

/// `id[Label]` / `id(Label)` / plain text → the display label.
std::string strip_label(const std::string& raw) {
    const std::string s = cworks::trim(raw);
    const std::size_t lb = s.find_first_of("[(");
    if (lb != std::string::npos) {
        const char close = s[lb] == '[' ? ']' : ')';
        const std::size_t rb = s.rfind(close);
        if (rb != std::string::npos && rb > lb) return cworks::trim(s.substr(lb + 1, rb - lb - 1));
    }
    return s;
}

struct Column {
    std::string key;
    std::string title;
    std::vector<std::string> cards;
};

} // namespace

cplot::Scene build_kanban(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> kKnown = {
        "kanban", "column id[Title]", "card id[Label]", "click"};
    std::vector<Column> columns;
    LinkTable links;  ///< card label -> hyperlink (click …)
    std::size_t base_indent = 0;
    bool have_base = false;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const SourceLine& sl = lines[li];
        const std::size_t number = sl.number;
        std::string entry = cworks::trim(sl.text);
        // Interaction: `click <label> "url" ["tip"] [_target]` attaches a
        // hyperlink to the card whose text matches <label>. A JS callback form
        // carries no URL and cannot be exported, so it is reported, not linked.
        if (is_click_statement(entry)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(entry, ref, link) && !link.callback) {
                if (!links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("kanban", number,
                                     "'" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "kanban", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        // A card may carry `id[Label]@{ ticket:…, assigned:…, priority:… }`
        // metadata. The board is pure geometry, so the metadata is dropped —
        // but say so instead of silently swallowing it, and never let a bare
        // `@{…}` line with no node fabricate a bogus empty column or card.
        const std::size_t meta = entry.find("@{");
        if (meta != std::string::npos) {
            const std::string node = cworks::trim(entry.substr(0, meta));
            if (node.empty()) {
                diagnose_unrecognized(options, "kanban", number, entry, kKnown);
                continue;
            }
            diagnose_unsupported(options, "kanban", number, "@{…}",
                                 "card metadata (assignee, priority, ticket) is not rendered");
            entry = node;
        }
        if (!have_base) {
            base_indent = sl.indent;
            have_base = true;
        }
        if (sl.indent <= base_indent || columns.empty())
            // The object model keys a column by its whole trimmed source
            // line. Keep that identity alongside its display title so the
            // scene map can select the lane without re-parsing its syntax.
            columns.push_back({sl.text, strip_label(entry), {}});
        else
            columns.back().cards.push_back(strip_label(entry));
    }
    if (columns.empty()) throw Error(cworks::validation_failed("kanban: no columns"));

    const Font font = theme.base_font();
    const Font header_font = theme.base_font().with_weight(cplot::FontWeight::Bold);
    const double line_h = font.size * 1.4;
    const double margin = 20.0;
    const double col_gap = 16.0, card_gap = 8.0, pad = 10.0;
    const double header_h = line_h + 12.0;
    const double card_h = line_h + 2.0 * pad;

    std::vector<double> col_w(columns.size());
    std::size_t max_cards = 0;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        double w = text_width(columns[i].title, header_font);
        for (const std::string& c : columns[i].cards)
            w = std::max(w, text_width(c, font));
        col_w[i] = std::max(w + 2.0 * pad, 120.0);
        max_cards = std::max(max_cards, columns[i].cards.size());
    }

    double total = margin;
    std::vector<double> col_x(columns.size());
    for (std::size_t i = 0; i < columns.size(); ++i) {
        col_x[i] = total;
        total += col_w[i] + col_gap;
    }

    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = total - col_gap + margin;
    canvas.height = margin + header_h + 10.0 +
                    static_cast<double>(max_cards) * (card_h + card_gap) + margin;

    // Cards are counted across every column, because document order runs
    // column by column and that is the order the model stores them in.
    std::size_t card_ordinal = 0;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const Color col = sty.series(i);
        const double x = col_x[i];
        // Column background.
        ShapeStyle lane;
        lane.fill = sty.grid.with_alpha(0.06);
        const RectF lane_box{x, margin, col_w[i],
                             canvas.height - margin - margin + 0.0};
        canvas.rounded_rect(lane_box, 8.0, lane);
        // A column is a container, so it is reported before the cards it
        // contains. Scene-map hit testing can then favour a card over its
        // lane while clicks on the header or unused lane area select the
        // Column itself.
        if (options.regions != nullptr) {
            options.regions->push_back(
                {columns[i].key, 2 /* HitRole::Group */, lane_box});
        }
        // Header.
        ShapeStyle header;
        header.fill = col.with_alpha(0.85);
        canvas.rounded_rect(RectF{x, margin, col_w[i], header_h}, 8.0, header);
        canvas.text({x + col_w[i] / 2.0, margin + header_h / 2.0}, columns[i].title,
                    header_font, cplot::colors::white, HAlign::Center, VAlign::Middle);
        // Cards.
        double y = margin + header_h + 10.0;
        for (const std::string& card : columns[i].cards) {
            ShapeStyle cs;
            cs.fill = sty.entity_fill;
            cs.stroke = col;
            cs.stroke_width = 1.0;
            const RectF card_box{x + pad, y, col_w[i] - 2.0 * pad, card_h};
            canvas.rounded_rect(card_box, 5.0, cs);
            // Report where this card was drawn, so it can be selected. The
            // ordinal is over cards in document order, which is what
            // `document.of_kind` returns.
            if (options.regions != nullptr) {
                options.regions->push_back(
                    {"#" + std::to_string(card_ordinal++), 0 /* HitRole::Node */, card_box});
            }
            canvas.text({x + col_w[i] / 2.0, y + card_h / 2.0}, card, font,
                        sty.text, HAlign::Center, VAlign::Middle);
            if (const auto it = links.find(card); it != links.end())
                canvas.add_link(card_box, it->second.href, it->second.title, it->second.target);
            y += card_h + card_gap;
        }
    }

    return canvas.bake();
}

} // namespace cdiagram::detail
