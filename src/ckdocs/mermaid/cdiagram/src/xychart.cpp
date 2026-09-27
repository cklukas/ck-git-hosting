// ckdiagram — Mermaid XY-chart syntax adapter
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Parsing and Mermaid diagnostics live here. cplot owns categorical/numeric
// axes, bar and line series, validation, layout, and scene construction.

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "frontmatter.hpp"
#include "chart_adapter.hpp"
#include "cdiagram/error.hpp"
#include "diagnostics.hpp"
#include "source.hpp"

namespace cdiagram::detail {
namespace {

std::vector<std::string> bracket_items(const std::string& text) {
    const std::size_t left = text.find('[');
    const std::size_t right = text.rfind(']');
    std::vector<std::string> out;
    if (left == std::string::npos || right == std::string::npos || right <= left)
        return out;
    std::string current;
    bool quoted = false;
    for (std::size_t i = left + 1; i < right; ++i) {
        if (text[i] == '"') quoted = !quoted;
        if (text[i] == ',' && !quoted) {
            out.push_back(unquote(cworks::trim(current)));
            current.clear();
        } else {
            current.push_back(text[i]);
        }
    }
    out.push_back(unquote(cworks::trim(current)));
    return out;
}

std::vector<double> number_list(const std::string& text, std::size_t line) {
    const std::vector<std::string> items = bracket_items(text);
    if (items.empty())
        throw Error(cworks::validation_failed("xychart-beta (line " + std::to_string(line) +
                    "): expected a non-empty [value, ...] list"));
    std::vector<double> out;
    out.reserve(items.size());
    for (const std::string& item : items) {
        double value = 0.0;
        if (!parse_number(item, value) || !std::isfinite(value))
            throw Error(cworks::validation_failed("xychart-beta (line " + std::to_string(line) +
                                                  "): '" + item + "' is not a finite number"));
        out.push_back(value);
    }
    return out;
}

struct AxisSpec {
    std::string label;
    bool explicit_range = false;
    double minimum = 0.0;
    double maximum = 0.0;
};

bool parse_range(std::string text, AxisSpec& axis) {
    text = cworks::trim(text);
    const std::size_t arrow = text.find("-->");
    if (arrow == std::string::npos) {
        axis.label = unquote(text);
        return false;
    }
    const std::string right = cworks::trim(text.substr(arrow + 3));
    std::string left = cworks::trim(text.substr(0, arrow));
    const std::size_t separator = left.find_last_of(" \t");
    const std::string lower =
        separator == std::string::npos ? left : left.substr(separator + 1);
    double minimum = 0.0;
    double maximum = 0.0;
    if (!parse_number(lower, minimum) || !parse_number(right, maximum) ||
        !std::isfinite(minimum) || !std::isfinite(maximum)) {
        axis.label = unquote(left);
        return false;
    }
    if (!(minimum < maximum))
        throw Error(cworks::validation_failed("xychart-beta: axis range needs minimum < maximum"));
    axis.label = separator == std::string::npos
                     ? std::string{}
                     : unquote(cworks::trim(left.substr(0, separator)));
    axis.explicit_range = true;
    axis.minimum = minimum;
    axis.maximum = maximum;
    return true;
}

std::vector<double> positions(std::size_t count, const AxisSpec& axis) {
    std::vector<double> out(count);
    if (count == 0) return out;
    if (axis.explicit_range) {
        if (count == 1) {
            out[0] = (axis.minimum + axis.maximum) / 2.0;
        } else {
            const double step = (axis.maximum - axis.minimum) /
                                static_cast<double>(count - 1);
            for (std::size_t i = 0; i < count; ++i)
                out[i] = axis.minimum + static_cast<double>(i) * step;
        }
    } else {
        for (std::size_t i = 0; i < count; ++i)
            out[i] = static_cast<double>(i);
    }
    return out;
}

void require_category_count(const std::vector<std::string>& categories,
                            const std::vector<double>& values,
                            const char* series) {
    if (!values.empty() && values.size() != categories.size())
        throw Error(cworks::validation_failed(std::string("xychart-beta: ") + series + " has " +
                    std::to_string(values.size()) + " values but the x axis has " +
                    std::to_string(categories.size()) + " categories"));
}

} // namespace

cplot::Scene build_xychart(std::string_view source,
                           const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    std::string title;
    AxisSpec x_axis;
    AxisSpec y_axis;
    std::vector<std::string> categories;
    std::vector<double> bars;
    std::vector<double> line_values;
    static const std::vector<std::string> known = {
        "title", "x-axis", "y-axis", "bar", "line"};

    for (std::size_t index = 1; index < lines.size(); ++index) {
        const SourceLine& source_line = lines[index];
        const std::string& line = source_line.text;
        const std::string keyword = line.substr(0, line.find_first_of(" \t["));
        if (keyword == "title") {
            title = unquote(cworks::trim(line.substr(5)));
        } else if (keyword == "x-axis") {
            const std::string rest = cworks::trim(line.substr(6));
            const std::size_t bracket = rest.find('[');
            if (bracket != std::string::npos) {
                x_axis.label = unquote(cworks::trim(rest.substr(0, bracket)));
                categories = bracket_items(rest);
                if (categories.empty())
                    throw Error(cworks::validation_failed("xychart-beta (line " +
                                std::to_string(source_line.number) +
                                "): x-axis category list is empty"));
            } else if (!parse_range(rest, x_axis)) {
                diagnose_unrecognized(options, "xychart-beta", source_line.number,
                                       line, known);
            }
        } else if (keyword == "y-axis") {
            const std::string rest = cworks::trim(line.substr(6));
            (void)parse_range(rest, y_axis);
        } else if (keyword == "bar") {
            bars = number_list(line, source_line.number);
        } else if (keyword == "line") {
            line_values = number_list(line, source_line.number);
        } else {
            diagnose_unrecognized(options, "xychart-beta", source_line.number,
                                   keyword, known);
        }
    }
    if (bars.empty() && line_values.empty())
        throw Error(cworks::validation_failed("xychart-beta: no bar or line data"));
    if (!categories.empty()) {
        require_category_count(categories, bars, "bar series");
        require_category_count(categories, line_values, "line series");
    }
    if (!bars.empty() && y_axis.explicit_range &&
        (y_axis.minimum > 0.0 || y_axis.maximum < 0.0)) {
        throw Error(cworks::validation_failed(
            "xychart-beta: a bar chart's y-axis range must include zero"));
    }

    const std::size_t bar_count = bars.size();
    const FrontMatter matter(source, options, "xyChart");
    const double width = matter.dimension("width", 760.0);
    const double height = matter.dimension("height", 460.0);
    cplot::Scene scene = render_chart(
        options, "xychart-beta", width, height,
        [title = std::move(title), x_axis = std::move(x_axis),
         y_axis = std::move(y_axis), categories = std::move(categories),
         bars = std::move(bars),
         line_values = std::move(line_values)](cplot::Figure& figure) mutable {
            cplot::Axes& panel = figure.axes();
            panel.title(std::move(title));
            panel.x_label(std::move(x_axis.label));
            panel.y_label(std::move(y_axis.label));
            if (x_axis.explicit_range)
                panel.x_axis().range(x_axis.minimum, x_axis.maximum);
            if (y_axis.explicit_range)
                panel.y_axis().range(y_axis.minimum, y_axis.maximum);

            if (!categories.empty()) {
                if (!bars.empty()) panel.bar(categories, bars);
                if (!line_values.empty())
                    panel.line(categories, line_values)
                        .marker(cplot::Marker::Circle, 3.5);
                return;
            }
            if (!bars.empty()) panel.xbar(positions(bars.size(), x_axis), bars);
            if (!line_values.empty())
                panel.line(positions(line_values.size(), x_axis), line_values)
                    .marker(cplot::Marker::Circle, 3.5);
        });
    // Bars are rectangles and are the marks a reader aims at.
    report_chart_regions(options, scene, ChartMark::Rect, bar_count);
    return scene;
}

} // namespace cdiagram::detail
