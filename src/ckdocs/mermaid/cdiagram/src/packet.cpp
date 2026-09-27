// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Packet diagram (`packet-beta`): named bit fields laid out on a 32-bit
// grid, wrapping across rows. Pure geometry — deterministic.

#include <algorithm>
#include <string>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/format.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "frontmatter.hpp"
#include "canvas.hpp"
#include "cdiagram/error.hpp"
#include "diagnostics.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram::detail {

namespace {

struct Field {
    int start = 0;
    int end = 0;
    std::string label;
};

} // namespace

cplot::Scene build_packet(std::string_view source, const RenderOptions& options) {
    const FrontMatter matter(source, options, "packet");
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    const std::vector<SourceLine> lines = significant_lines(source);
    // `title` is valid packet-beta but carries no geometry; every other
    // token is a near-miss of it. The did-you-mean list stays minimal.
    static const std::vector<std::string> kKnown = {"title"};
    std::vector<Field> fields;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            // A field line is `range: label`; a colon-less line is either the
            // `title` directive (recognised, not rendered) or noise. Report it
            // rather than dropping it in silence (never-silently-drop charter).
            const std::string keyword = line.substr(0, line.find_first_of(" \t"));
            if (keyword == "title")
                diagnose_unsupported(options, "packet-beta", number, "title",
                                     "titles are not rendered");
            else
                diagnose_unrecognized(options, "packet-beta", number, keyword, kKnown);
            continue;
        }
        const std::string range = cworks::trim(line.substr(0, colon));
        const std::string label = unquote(cworks::trim(line.substr(colon + 1)));
        std::int64_t s = 0, e = 0;
        const std::size_t dash = range.find('-');
        if (dash == std::string::npos) {
            if (!cworks::parse_int(range, s)) {
                diagnose_unrecognized(options, "packet-beta", number, range, kKnown);
                continue;
            }
            e = s;
        } else {
            if (!cworks::parse_int(range.substr(0, dash), s) ||
                !cworks::parse_int(range.substr(dash + 1), e)) {
                diagnose_unrecognized(options, "packet-beta", number, range, kKnown);
                continue;
            }
        }
        // Guard the geometry: a reversed or negative range (`7-0`, `-1`) used
        // to store {start=7,end=0} and fabricate an overlapping/degenerate
        // cell. Report the bad span rather than emit wrong output.
        if (s < 0 || e < s) {
            diagnose_unrecognized(options, "packet-beta", number, range, kKnown);
            continue;
        }
        if (e > 65535) throw Error(cworks::validation_failed("packet: bit positions must not exceed 65535"));
        fields.push_back({static_cast<int>(s), static_cast<int>(e), label});
    }
    if (fields.empty()) throw Error(cworks::validation_failed("packet: no fields"));

    // Mermaid packet configuration; each key feeds the constant it names.
    const double configured_bits = matter.number("bitsPerRow").value_or(32.0);
    if (!(configured_bits >= 1 && configured_bits <= 1024))
        throw Error(cworks::validation_failed("packet: bitsPerRow must be between 1 and 1024"));
    const int bits_per_row = static_cast<int>(configured_bits);
    const bool show_bits = matter.boolean("showBits").value_or(true);

    const Font font = theme.base_font();
    const Font small = font.with_size(font.size - 2.0);
    const double margin = 22.0;
    const double row_h = std::max(16.0, matter.number("rowHeight").value_or(34.0));
    const double row_gap = show_bits ? 20.0 : 8.0;  // room for bit-number labels

    // One bit is 17px wide by default, but widen every cell uniformly so
    // that each field's centered label fits within its span. A field
    // covering `span_bits` bits gets `span_bits * cell_w` px, so it fits
    // when cell_w >= (text_width(label) + pad) / span_bits.
    const double label_pad = 12.0;
    double cell_w = std::max(4.0, matter.number("bitWidth").value_or(17.0));
    for (const Field& f : fields) {
        const int span_bits = std::max(1, f.end - f.start + 1);
        cell_w = std::max(cell_w, (text_width(f.label, font) + label_pad) / span_bits);
    }

    int max_bit = 0;
    for (const Field& f : fields) max_bit = std::max(max_bit, f.end);
    const int rows = max_bit / bits_per_row + 1;
    if (rows > 1024) throw Error(cworks::validation_failed("packet: exceeds 1024 rows"));

    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = margin * 2.0 + bits_per_row * cell_w;
    canvas.height = margin * 2.0 + rows * (row_h + row_gap);

    for (std::size_t fi = 0; fi < fields.size(); ++fi) {
        const Field& f = fields[fi];
        const Color col = sty.series(fi);
        RectF region_bounds;
        bool has_region = false;
        for (int r = f.start / bits_per_row; r <= f.end / bits_per_row; ++r) {
            const int col_start = std::max(f.start, r * bits_per_row) - r * bits_per_row;
            const int col_end = std::min(f.end, r * bits_per_row + bits_per_row - 1) -
                                r * bits_per_row;
            const double x = margin + col_start * cell_w;
            const double w = (col_end - col_start + 1) * cell_w;
            const double y = margin + row_gap + r * (row_h + row_gap);
            const RectF segment{x, y, w, row_h};
            if (!has_region) {
                region_bounds = segment;
                has_region = true;
            } else {
                const double min_x = std::min(region_bounds.x, segment.x);
                const double min_y = std::min(region_bounds.y, segment.y);
                const double max_x = std::max(region_bounds.x + region_bounds.w,
                                              segment.x + segment.w);
                const double max_y = std::max(region_bounds.y + region_bounds.h,
                                              segment.y + segment.h);
                region_bounds = {min_x, min_y, max_x - min_x, max_y - min_y};
            }
            ShapeStyle cell;
            cell.fill = col.with_alpha(0.18);
            cell.stroke = col;
            cell.stroke_width = 1.0;
            canvas.rect(segment, cell);
            canvas.text({x + w / 2.0, y + row_h / 2.0}, f.label, font, sty.text,
                        HAlign::Center, VAlign::Middle);
            // Bit indices above the segment ends.
            if (show_bits) {
                const int abs_start = r * bits_per_row + col_start;
                const int abs_end = r * bits_per_row + col_end;
                canvas.text({x + 2.0, y - 3.0}, cworks::format_int(abs_start), small,
                            sty.muted, HAlign::Left, VAlign::Bottom);
                if (abs_end != abs_start)
                    canvas.text({x + w - 2.0, y - 3.0}, cworks::format_int(abs_end), small,
                                sty.muted, HAlign::Right, VAlign::Bottom);
            }
        }
        if (options.regions != nullptr && has_region) {
            options.regions->push_back(
                {"#" + std::to_string(fi), 0 /* HitRole::Node */, region_bounds});
        }
    }

    return canvas.bake();
}

} // namespace cdiagram::detail
