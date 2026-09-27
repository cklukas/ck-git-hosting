// ckdiagram — treemap syntax adapter
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <cplot/structured_charts.hpp>
#include <cworks/app_error.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "chart_adapter.hpp"
#include "cdiagram/error.hpp"
#include "diagnostics.hpp"
#include "source.hpp"

namespace cdiagram::detail {
namespace {

struct ParsedNode {
    std::string label;
    std::optional<double> value;
    int parent = -1;
    std::vector<int> children;
};

std::string strip_quotes(const std::string& value) {
    const std::string text = cworks::trim(value);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        return text.substr(1, text.size() - 2);
    return text;
}

} // namespace

cplot::Scene build_treemap(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> known = {"classDef", "class"};
    std::vector<ParsedNode> parsed;
    std::vector<std::pair<std::size_t, int>> stack;
    for (std::size_t index = 1; index < lines.size(); ++index) {
        const SourceLine& line = lines[index];
        const std::string keyword = line.text.substr(0, line.text.find_first_of(" \t"));
        if (keyword == "classDef" || keyword == "class") {
            diagnose_unsupported(options, "treemap", line.number, keyword,
                                 "class styling is not rendered");
            continue;
        }
        ParsedNode node;
        const std::size_t colon = line.text.rfind(':');
        double value = 0.0;
        if (colon != std::string::npos &&
            parse_number(cworks::trim(line.text.substr(colon + 1)), value)) {
            node.label = strip_quotes(line.text.substr(0, colon));
            node.value = value;
        } else {
            const std::string text = cworks::trim(line.text);
            if (text.empty() || text.front() != '"') {
                diagnose_unrecognized(options, "treemap", line.number, keyword, known);
                continue;
            }
            node.label = strip_quotes(text);
        }
        while (!stack.empty() && stack.back().first >= line.indent) stack.pop_back();
        node.parent = stack.empty() ? -1 : stack.back().second;
        const int node_index = static_cast<int>(parsed.size());
        if (node.parent >= 0)
            parsed[static_cast<std::size_t>(node.parent)].children.push_back(node_index);
        parsed.push_back(std::move(node));
        stack.push_back({line.indent, node_index});
    }
    if (parsed.empty()) throw Error(cworks::validation_failed("treemap: no nodes"));

    const std::function<cplot::TreemapNode(int)> convert = [&](int index) {
        const ParsedNode& source_node = parsed[static_cast<std::size_t>(index)];
        cplot::TreemapNode node;
        node.id = "node:" + std::to_string(index);
        node.label = source_node.label;
        node.value = source_node.value;
        for (const int child : source_node.children) node.children.push_back(convert(child));
        return node;
    };
    cplot::TreemapChart chart;
    for (std::size_t index = 0; index < parsed.size(); ++index)
        if (parsed[index].parent < 0) chart.roots.push_back(convert(static_cast<int>(index)));

    const std::size_t box_count = parsed.size();
    cplot::Scene scene = render_chart(options, "treemap", 660.0, 440.0,
                        [chart = std::move(chart)](cplot::Figure& figure) mutable {
                            figure.treemap(std::move(chart));
                        });
    // A treemap box really is a rectangle, so its bounds are its shape.
    report_chart_regions(options, scene, ChartMark::Rect, box_count);
    return scene;
}

} // namespace cdiagram::detail
