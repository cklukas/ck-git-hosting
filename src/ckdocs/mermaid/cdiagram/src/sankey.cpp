// ckdiagram — Sankey syntax adapter
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// This file owns only the Mermaid-compatible text boundary. The typed model,
// validation, layout, and scene construction are cplot responsibilities.

#include <cmath>
#include <string>
#include <vector>

#include <cplot/structured_charts.hpp>
#include <cworks/app_error.hpp>
#include <cworks/format.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "chart_adapter.hpp"
#include "frontmatter.hpp"
#include "cdiagram/error.hpp"
#include "diagnostics.hpp"
#include "source.hpp"

namespace cdiagram::detail {
namespace {

std::vector<std::string> csv_row(const std::string& line) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (const char c : line) {
        if (c == '"') {
            quoted = !quoted;
        } else if (c == ',' && !quoted) {
            out.push_back(cworks::trim(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    out.push_back(cworks::trim(current));
    return out;
}

// config.sankey.nodeAlignment picks the d3-sankey alignment family;
// justify (the Mermaid default) applies when the key is absent or invalid.
cplot::SankeyAlignment sankey_alignment(const FrontMatter& matter,
                                        const RenderOptions& options) {
    const std::optional<std::string> value = matter.text("nodeAlignment");
    if (!value) return cplot::SankeyAlignment::Justify;
    if (*value == "justify") return cplot::SankeyAlignment::Justify;
    if (*value == "left") return cplot::SankeyAlignment::Left;
    if (*value == "right") return cplot::SankeyAlignment::Right;
    if (*value == "center") return cplot::SankeyAlignment::Center;
    static const std::vector<std::string> known = {"justify", "left", "right", "center"};
    diagnose_unrecognized(options, "sankey", 1, *value, known);
    return cplot::SankeyAlignment::Justify;
}

} // namespace

cplot::Scene build_sankey(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> known = {"sankey-beta"};
    const FrontMatter matter(source, options, "sankey");
    cplot::SankeyChart chart;
    chart.alignment = sankey_alignment(matter, options);
    const auto ensure_node = [&](const std::string& name) {
        for (const cplot::SankeyNode& node : chart.nodes)
            if (node.id == name) return;
        chart.node(name, name);
    };
    for (std::size_t index = 1; index < lines.size(); ++index) {
        const SourceLine& line = lines[index];
        const std::vector<std::string> row = csv_row(line.text);
        const std::string keyword = row.empty() ? cworks::trim(line.text) : row[0];
        if (keyword == "sankey-beta") {
            diagnose_unsupported(options, "sankey", line.number, keyword,
                                 "the sankey-beta header is only valid as the first line");
            continue;
        }
        if (row.size() != 3) {
            diagnose_unrecognized(options, "sankey", line.number, keyword, known);
            continue;
        }
        double value = 0.0;
        if (row[0].empty() || row[1].empty() || !parse_number(row[2], value)) {
            diagnose_unrecognized(options, "sankey", line.number, line.text, known);
            continue;
        }
        ensure_node(row[0]);
        ensure_node(row[1]);
        chart.link(row[0], row[1], value);
    }
    if (chart.links.empty()) throw Error(cworks::validation_failed("sankey: no links"));

    // Mermaid shows each node's total by default; prefix/suffix wrap the
    // number (rounded to two decimals, as Mermaid rounds it). The value
    // rides on the label so the shared label layout handles it.
    if (matter.boolean("showValues").value_or(true)) {
        const std::string prefix = matter.text("prefix").value_or("");
        const std::string suffix = matter.text("suffix").value_or("");
        for (cplot::SankeyNode& node : chart.nodes) {
            double in = 0.0;
            double out = 0.0;
            for (const cplot::SankeyLink& link : chart.links) {
                if (link.source == node.id) out += link.value;
                if (link.target == node.id) in += link.value;
            }
            const double total = std::round(std::max(in, out) * 100.0) / 100.0;
            node.label += " " + prefix + cworks::format_double_fixed(total, 2) + suffix;
        }
    }

    const double width = matter.dimension("width", 800.0);
    const double height = matter.dimension("height", 440.0);
    const std::size_t node_count = chart.nodes.size();
    cplot::Scene scene = render_chart(options, "sankey", width, height,
                        [chart = std::move(chart)](cplot::Figure& figure) mutable {
                            figure.sankey(std::move(chart));
                        });
    // A sankey NODE is a rectangle and is what a reader aims at; a flow is a
    // ribbon whose path is not a region this map can express, so the nodes
    // are reported and the ribbons are not.
    report_chart_regions(options, scene, ChartMark::Rect, node_count);
    return scene;
}

} // namespace cdiagram::detail
