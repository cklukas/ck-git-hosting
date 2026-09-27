// ckdiagram — quadrant-chart syntax adapter
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include <string>
#include <vector>

#include <cplot/structured_charts.hpp>
#include <cworks/app_error.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "frontmatter.hpp"
#include "chart_adapter.hpp"
#include "cdiagram/error.hpp"
#include "diagnostics.hpp"
#include "styleprops.hpp"
#include "source.hpp"

namespace cdiagram::detail {
namespace {

bool starts_with(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

void parse_endpoints(const std::string& text, cplot::AxisEndpoints& endpoints) {
    const std::size_t arrow = text.find("-->");
    if (arrow == std::string::npos) {
        endpoints.lower = cworks::trim(text);
    } else {
        endpoints.lower = cworks::trim(text.substr(0, arrow));
        endpoints.upper = cworks::trim(text.substr(arrow + 3));
    }
}

} // namespace

/// Mermaid quadrant point styling — `radius:` / `color:` / `stroke-color:` /
/// `stroke-width:`. A different vocabulary from the shared box styles
/// (points have no fill/dash), so it stays local to this builder; used by
/// both `classDef` definitions and inline tails after the coordinates.
struct PointProps {
    std::optional<double> radius;
    std::optional<cplot::Color> color;
    std::optional<cplot::Color> stroke_color;
    std::optional<double> stroke_width;
    void merge(const PointProps& o) {
        if (o.radius) radius = o.radius;
        if (o.color) color = o.color;
        if (o.stroke_color) stroke_color = o.stroke_color;
        if (o.stroke_width) stroke_width = o.stroke_width;
    }
};

PointProps parse_point_props(std::string_view text, const RenderOptions& options,
                             std::size_t line) {
    PointProps p;
    for (const std::string& entry : split_style_entries(text)) {
        const std::size_t colon = entry.find(':');
        if (colon == std::string::npos) {
            diagnose(options, cworks::Diagnostic::Severity::Warning,
                     at_line("quadrantChart", line,
                             "point style entry '" + entry + "' is not 'key:value' (ignored)"));
            continue;
        }
        const std::string key = cworks::trim(entry.substr(0, colon));
        const std::string val = cworks::trim(entry.substr(colon + 1));
        if (key == "radius") {
            double r = 0.0;
            if (parse_number(val, r) && r > 0.0) p.radius = r;
            else
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("quadrantChart", line, "invalid radius '" + val + "' (ignored)"));
        } else if (key == "color" || key == "stroke-color") {
            const std::optional<cplot::Color> c = cplot::parse_css_color(val);
            if (!c) {
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("quadrantChart", line,
                                 "unrecognized colour '" + val + "' (ignored)"));
                continue;
            }
            if (key == "color") p.color = c;
            else p.stroke_color = c;
        } else if (key == "stroke-width") {
            std::string num = val;
            if (num.size() >= 2 && num.substr(num.size() - 2) == "px") num.resize(num.size() - 2);
            double w = 0.0;
            if (parse_number(cworks::trim(num), w)) p.stroke_width = w;
            else
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("quadrantChart", line,
                                 "invalid stroke-width '" + val + "' (ignored)"));
        } else {
            diagnose(options, cworks::Diagnostic::Severity::Warning,
                     at_line("quadrantChart", line,
                             "unsupported point style '" + key + "' (ignored)"));
        }
    }
    return p;
}

cplot::Scene build_quadrant(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    static const std::vector<std::string> known = {
        "title",      "x-axis",     "y-axis",     "quadrant-1",
        "quadrant-2", "quadrant-3", "quadrant-4"};
    cplot::QuadrantChart chart;
    std::map<std::string, PointProps> point_classes;  // classDef definitions
    struct PendingStyle {
        std::size_t point;
        std::vector<std::string> classes;
        PointProps inline_props;
    };
    std::vector<PendingStyle> pending;  // resolved after all classDefs are read
    for (std::size_t index = 1; index < lines.size(); ++index) {
        const SourceLine& source_line = lines[index];
        const std::string& line = source_line.text;
        if (starts_with(line, "title ")) {
            chart.title = cworks::trim(line.substr(6));
        } else if (starts_with(line, "x-axis")) {
            parse_endpoints(line.substr(6), chart.x_axis);
        } else if (starts_with(line, "y-axis")) {
            parse_endpoints(line.substr(6), chart.y_axis);
        } else if (starts_with(line, "quadrant-")) {
            const int quadrant = line.size() > 9 ? line[9] - '1' : -1;
            const std::string label = line.size() > 10 ? cworks::trim(line.substr(10)) : "";
            if (quadrant == 0)
                chart.quadrants.top_right = label;
            else if (quadrant == 1)
                chart.quadrants.top_left = label;
            else if (quadrant == 2)
                chart.quadrants.bottom_left = label;
            else if (quadrant == 3)
                chart.quadrants.bottom_right = label;
            else
                diagnose_unrecognized(options, "quadrantChart", source_line.number, line,
                                       known);
        } else if (starts_with(line, "classDef ")) {
            // C2: classDef <name[,name]> radius: .., color: .., ...
            const std::string rest = cworks::trim(line.substr(9));
            const std::size_t sp = rest.find_first_of(" \t");
            if (sp == std::string::npos) {
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("quadrantChart", source_line.number,
                                 "classDef needs a name and properties (ignored)"));
                continue;
            }
            const PointProps props = parse_point_props(cworks::trim(rest.substr(sp + 1)),
                                                       options, source_line.number);
            for (const std::string& name : split_commas(rest.substr(0, sp)))
                point_classes[name].merge(props);
        } else {
            // A data point: `Name: [x, y]`, optionally `Name:::class: [...]`
            // and/or a style tail after the coordinates. The name/coordinate
            // split is the last colon before '[' so `:::` never bleeds in.
            bool parsed = false;
            const std::size_t bracket = line.find('[');
            const std::size_t colon =
                bracket == std::string::npos ? std::string::npos : line.rfind(':', bracket);
            if (colon != std::string::npos) {
                std::string name = cworks::trim(line.substr(0, colon));
                std::string coordinates = cworks::trim(line.substr(colon + 1));
                PendingStyle style;
                // `Name:::class` assigns point classDefs inline (repeatable).
                if (std::size_t mark = name.find(":::"); mark != std::string::npos) {
                    std::string tail = name.substr(mark + 3);
                    name = cworks::trim(name.substr(0, mark));
                    while ((mark = tail.find(":::")) != std::string::npos) {
                        style.classes.push_back(cworks::trim(tail.substr(0, mark)));
                        tail = tail.substr(mark + 3);
                    }
                    if (!cworks::trim(tail).empty())
                        style.classes.push_back(cworks::trim(tail));
                }
                // Mermaid v11 per-point style tail after the coordinates
                // (`radius:`, `color:`, ...). Parsed — and it must never
                // cost the DATA POINT.
                if (const std::size_t close = coordinates.find(']');
                    close != std::string::npos && close + 1 < coordinates.size()) {
                    const std::string tail = cworks::trim(coordinates.substr(close + 1));
                    if (!tail.empty())
                        style.inline_props =
                            parse_point_props(tail, options, source_line.number);
                    coordinates = cworks::trim(coordinates.substr(0, close + 1));
                }
                if (!name.empty() && coordinates.size() >= 2 && coordinates.front() == '[' &&
                    coordinates.back() == ']') {
                    const std::size_t comma = coordinates.find(',');
                    double x = 0.0;
                    double y = 0.0;
                    if (comma != std::string::npos &&
                        parse_number(cworks::trim(coordinates.substr(1, comma - 1)), x) &&
                        parse_number(cworks::trim(coordinates.substr(
                                         comma + 1, coordinates.size() - comma - 2)),
                                     y)) {
                        style.point = chart.points.size();
                        chart.point("point:" + std::to_string(chart.points.size()), name, x,
                                    y);
                        if (!style.classes.empty() || style.inline_props.radius ||
                            style.inline_props.color || style.inline_props.stroke_color ||
                            style.inline_props.stroke_width)
                            pending.push_back(std::move(style));
                        parsed = true;
                    }
                }
            }
            if (!parsed) {
                const std::string keyword = line.substr(0, line.find_first_of(" \t"));
                diagnose_unrecognized(options, "quadrantChart", source_line.number, keyword,
                                       known);
            }
        }
    }
    if (chart.points.empty()) throw Error(cworks::validation_failed("quadrantChart: no points"));

    // C2: resolve point styles — classes in application order, then the
    // inline tail on top (last wins per property).
    for (const PendingStyle& style : pending) {
        PointProps resolved;
        for (const std::string& cls : style.classes)
            if (const auto it = point_classes.find(cls); it != point_classes.end())
                resolved.merge(it->second);
        resolved.merge(style.inline_props);
        cplot::QuadrantPoint& point = chart.points[style.point];
        point.color = resolved.color;
        point.radius = resolved.radius;
        point.stroke_color = resolved.stroke_color;
        point.stroke_width = resolved.stroke_width;
    }

    const FrontMatter matter(source, options, "quadrantChart");
    const double width = matter.dimension("chartWidth", 560.0);
    const double height = matter.dimension("chartHeight", 500.0);
    const std::size_t point_count = chart.points.size();
    cplot::Scene scene = render_chart(options, "quadrantChart", width, height,
                        [chart = std::move(chart)](cplot::Figure& figure) mutable {
                            figure.quadrant(std::move(chart));
                        });
    // A plotted point is a marker; the square around it is its hit area.
    report_chart_regions(options, scene, ChartMark::Circle, point_count);
    return scene;
}

} // namespace cdiagram::detail
