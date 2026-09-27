// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Block diagram (`block-beta`): blocks flowed into a fixed-column grid,
// with optional column spans and blank `space` cells. Arrows and nested
// blocks are skipped in this version.

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/format.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "canvas.hpp"
#include "cdiagram/error.hpp"
#include "diagnostics.hpp"
#include "links.hpp"
#include "styleprops.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram::detail {

namespace {

struct Block {
    std::string id;  ///< the token's identifier — the style/class target
    std::string label;
    int span = 1;
    bool spacer = false;
    std::size_t source_row = 0;
};

std::vector<std::string> tokenize(const std::string& line) {
    // Split on whitespace but keep `["..."]` contents together.
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    bool quote = false;
    for (const char c : line) {
        if (c == '"') quote = !quote;
        if (!quote && (c == '[' || c == '(')) ++depth;
        if (!quote && (c == ']' || c == ')')) --depth;
        if ((c == ' ' || c == '\t') && depth == 0 && !quote) {
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

Block parse_block(const std::string& token) {
    Block b;
    std::string t = token;
    // trailing :span
    const std::size_t colon = t.rfind(':');
    if (colon != std::string::npos) {
        std::int64_t n = 0;
        if (cworks::parse_int(t.substr(colon + 1), n) && n > 0) {
            if (n > 1024) throw Error(cworks::validation_failed("block: span exceeds 1024 columns"));
            b.span = static_cast<int>(n);
            t = t.substr(0, colon);
        }
    }
    if (t == "space") {
        b.spacer = true;
        return b;
    }
    const std::size_t lb = t.find_first_of("[(");
    b.id = cworks::trim(lb == std::string::npos ? t : t.substr(0, lb));
    if (lb != std::string::npos) {
        const char close = t[lb] == '[' ? ']' : ')';
        const std::size_t rb = t.rfind(close);
        b.label = rb != std::string::npos && rb > lb ? cworks::trim(t.substr(lb + 1, rb - lb - 1))
                                                     : t;
        // strip surrounding quotes
        if (b.label.size() >= 2 && b.label.front() == '"' && b.label.back() == '"')
            b.label = b.label.substr(1, b.label.size() - 2);
    } else {
        b.label = t;
    }
    return b;
}

} // namespace

cplot::Scene build_block(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> kKnown = {
        "columns", "space", "block", "end", "style", "class", "classDef", "click"};
    int columns = 0;
    std::vector<Block> blocks;
    std::size_t source_row_count = 0;
    StyleSheet styles;  ///< C2: classDef / class / style resolution (by block id)
    LinkTable links;    ///< block id -> hyperlink (click …)
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        if (line.rfind("columns ", 0) == 0) {
            std::int64_t n = 0;
            if (cworks::parse_int(cworks::trim(line.substr(8)), n) && n > 0) {
                if (n > 1024) throw Error(cworks::validation_failed("block: exceeds 1024 columns"));
                columns = static_cast<int>(n);
            }
            continue;
        }
        const std::string keyword = line.substr(0, line.find_first_of(" \t"));
        // Interaction: `click <id> "url" ["tip"] [_target]` attaches a
        // hyperlink to the block. Handled before the edge/`--` check because a
        // URL may itself contain `--` or `<`. JS callback forms carry no URL
        // and cannot be exported, so they are reported, not linked.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("block-beta", number,
                                     "block '" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "block-beta", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        // Edges between blocks are valid block-beta but not laid out in this
        // version. Report rather than drop them (the never-silently-drop
        // charter): without this they were skipped without a trace.
        if (line.find("--") != std::string::npos || line.find("<") != std::string::npos) {
            diagnose_unsupported(options, "block-beta", number, keyword,
                                 "edges between blocks are not rendered");
            continue;
        }
        // Nested block groups (`block:id ... end`) are flattened away, not
        // laid out — say so instead of skipping in silence.
        if (line == "end" || line.rfind("block", 0) == 0) {
            diagnose_unsupported(options, "block-beta", number, keyword,
                                 "nested block groups are not rendered");
            continue;
        }
        // C2: classDef / class / style, on block ids.
        if (parse_style_statement(line, styles, options, "block-beta", number))
            continue;
        // Everything else is block content: one block per whitespace token.
        const std::vector<std::string> toks = tokenize(line);
        if (toks.empty()) {
            // A significant line that yields no token is neither a directive
            // nor block content — report it rather than ignore it.
            diagnose_unrecognized(options, "block-beta", number, keyword, kKnown);
            continue;
        }
        for (const std::string& tok : toks) {
            Block block = parse_block(tok);
            block.source_row = source_row_count;
            blocks.push_back(std::move(block));
        }
        ++source_row_count;
    }
    if (blocks.empty()) throw Error(cworks::validation_failed("block: no blocks"));
    if (columns <= 0) columns = static_cast<int>(blocks.size());

    const Font font = theme.base_font();
    const double margin = 22.0;
    const double cell_h = 52.0, gap = 12.0, pad = 12.0;
    // Grow the uniform cell width so the widest label fits its box. A
    // block spanning `span` cells gets span*cell_w + (span-1)*gap of room,
    // so the per-cell width it demands is (label + 2*pad - (span-1)*gap)/span.
    double cell_w = 120.0;
    for (const Block& b : blocks) {
        if (b.spacer) continue;
        const double span = static_cast<double>(std::max(1, b.span));
        const double need = text_width(b.label, font) + 2.0 * pad;
        cell_w = std::max(cell_w, (need - (span - 1.0) * gap) / span);
    }

    Canvas canvas;
    canvas.background = sty.page;
    // Flow blocks into rows to learn the row count.
    int col = 0, row = 0;
    std::vector<std::tuple<Block, int, int>> placed;  // block, col, row
    for (const Block& b : blocks) {
        if (col + b.span > columns && col > 0) {
            col = 0;
            ++row;
        }
        placed.push_back({b, col, row});
        col += b.span;
        if (col >= columns) {
            col = 0;
            ++row;
        }
    }
    const int rows = (col > 0 ? row + 1 : row);
    canvas.width = 2.0 * margin + columns * cell_w + (columns - 1) * gap;
    canvas.height = 2.0 * margin + rows * cell_h + (rows - 1) * gap;

    std::size_t idx = 0;
    std::vector<RectF> row_bounds(source_row_count);
    std::vector<bool> has_row_bounds(source_row_count, false);
    for (const auto& [b, c, r] : placed) {
        if (!b.spacer) {
            const double x = margin + c * (cell_w + gap);
            const double y = margin + r * (cell_h + gap);
            const double w = b.span * cell_w + (b.span - 1) * gap;
            // C2: layer the resolved classDef/style properties over the
            // per-index palette colouring.
            const StyleProps sp = styles.resolve(b.id);
            ShapeStyle s;
            s.fill = sp.fill.value_or(sty.series(idx).with_alpha(0.2));
            s.stroke = sp.stroke.value_or(sty.series(idx));
            s.stroke_width = sp.stroke_width.value_or(1.2);
            if (sp.dash) s.dash = *sp.dash;
            const RectF box{x, y, w, cell_h};
            if (!has_row_bounds[b.source_row]) {
                row_bounds[b.source_row] = box;
                has_row_bounds[b.source_row] = true;
            } else {
                RectF& bounds = row_bounds[b.source_row];
                const double min_x = std::min(bounds.x, box.x);
                const double min_y = std::min(bounds.y, box.y);
                const double max_x = std::max(bounds.x + bounds.w, box.x + box.w);
                const double max_y = std::max(bounds.y + bounds.h, box.y + box.h);
                bounds = {min_x, min_y, max_x - min_x, max_y - min_y};
            }
            canvas.rounded_rect(box, 6.0, s);
            canvas.text({x + w / 2.0, y + cell_h / 2.0}, b.label, font,
                        sp.text.value_or(sty.text), HAlign::Center, VAlign::Middle);
            if (const auto it = links.find(b.id); it != links.end())
                canvas.add_link(box, it->second.href, it->second.title, it->second.target);
        }
        ++idx;
    }
    if (options.regions != nullptr) {
        for (std::size_t row_index = 0; row_index < row_bounds.size(); ++row_index) {
            if (!has_row_bounds[row_index]) continue;
            options.regions->push_back({"#" + std::to_string(row_index),
                                        0 /* HitRole::Node */, row_bounds[row_index]});
        }
    }

    return canvas.bake();
}

} // namespace cdiagram::detail
