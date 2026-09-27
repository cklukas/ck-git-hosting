// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Pie chart syntax adapter: `pie [showData] [title …]` followed by
// `"Label" : value` lines. cplot owns validation and all chart geometry.

#include <algorithm>
#include <string>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/format.hpp>

#include "builders.hpp"
#include "chart_adapter.hpp"
#include "cdiagram/error.hpp"
#include "diagnostics.hpp"
#include "source.hpp"

namespace cdiagram::detail {

namespace {

struct Slice {
    std::string label;
    double value;
};

struct PieModel {
    std::string title;
    bool show_data = false;
    std::vector<Slice> slices;
};

bool starts_word(const std::string& line, const char* word) {
    const std::string w = word;
    if (line.compare(0, w.size(), w) != 0) return false;
    return line.size() == w.size() || line[w.size()] == ' ' || line[w.size()] == '\t';
}

PieModel parse_pie(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    // Directives that are valid Mermaid for a pie but declared on the
    // header line (title, showData) or carry no visual (accessibility
    // metadata) — the did-you-mean candidates and the not-rendered set.
    static const std::vector<std::string> kKnown = {"title", "showData", "accTitle",
                                                    "accDescr"};
    PieModel model;

    // Header: "pie" then optional "showData" then optional "title …".
    std::string rest = cworks::trim(lines.front().text.substr(3));
    if (starts_word(rest, "showData")) {
        model.show_data = true;
        rest = cworks::trim(rest.substr(8));
    }
    if (starts_word(rest, "title")) model.title = cworks::trim(rest.substr(5));

    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i].text;
        const std::size_t number = lines[i].number;

        // The only statement a pie body carries is a data slice:
        //   "Label" : value
        // A malformed *data* line (bad quote/colon/number) is a hard error,
        // exactly as before; only non-data lines are reported and skipped.
        if (!line.empty() && line[0] == '"') {
            const auto fail = [&](const std::string& what) {
                throw Error(cworks::validation_failed("pie (line " + std::to_string(number) +
                                                      "): " + what));
            };
            const std::size_t close = line.find('"', 1);
            if (close == std::string::npos)
                fail("unterminated label — missing closing quote");
            const std::string label = line.substr(1, close - 1);
            std::string tail = cworks::trim(line.substr(close + 1));
            if (tail.empty() || tail[0] != ':') fail("expected ':' after the label");
            tail = cworks::trim(tail.substr(1));
            double value = 0.0;
            if (!parse_number(tail, value)) fail("'" + tail + "' is not a number");
            if (value < 0.0) fail("value must not be negative");
            model.slices.push_back({label, value});
            continue;
        }

        // Not a data line. The leading token decides how to report it.
        const std::string keyword = line.substr(0, line.find_first_of(" \t:"));
        if (keyword == "title" || keyword == "showData" || keyword == "accTitle" ||
            keyword == "accDescr") {
            // Recognised pie directive, but not a body statement. Do NOT let
            // a `title …` line silently reassign the title: an unquoted
            // slice such as `title : 5` would hijack it and vanish. Report
            // it and leave the model untouched.
            diagnose_unsupported(options, "pie", number, keyword,
                                 "title and showData belong on the 'pie' header line; "
                                 "accessibility metadata is not rendered");
            continue;
        }
        // A line that is not a slice and not a known directive at all.
        diagnose_unrecognized(options, "pie", number, keyword, kKnown);
    }
    if (model.slices.empty()) throw Error(cworks::validation_failed("pie: no data slices"));
    return model;
}

} // namespace

cplot::Scene build_pie(std::string_view source, const RenderOptions& options) {
    PieModel model = parse_pie(source, options);

    // Descending by value, stable in source order (Mermaid's ordering).
    std::stable_sort(model.slices.begin(), model.slices.end(),
                     [](const Slice& a, const Slice& b) { return a.value > b.value; });
    const std::size_t slice_count = model.slices.size();
    cplot::Scene scene = render_chart(
        options, "pie", 680.0, 440.0,
        [model = std::move(model)](cplot::Figure& figure) mutable {
            std::vector<std::string> labels;
            std::vector<double> values;
            labels.reserve(model.slices.size());
            values.reserve(model.slices.size());
            for (const Slice& slice : model.slices) {
                std::string label = slice.label;
                if (model.show_data)
                    label += " [" + cworks::format_double_fixed(slice.value, 2) + "]";
                labels.push_back(std::move(label));
                values.push_back(slice.value);
            }
            cplot::Axes& panel = figure.axes();
            panel.title(std::move(model.title));
            panel.pie(std::move(labels), values)
                .slice_labels(cplot::PieLabelMode::Percent);
        });
    // A wedge is not a rectangle, so each slice reports the outline it was
    // drawn with; see cdiagram::HitRegion::shape.
    report_chart_regions(options, scene, ChartMark::Sector, slice_count);
    return scene;
}

} // namespace cdiagram::detail
