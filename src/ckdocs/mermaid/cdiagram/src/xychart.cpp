// ckdiagram — Mermaid XY-chart syntax adapter
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Parsing and Mermaid diagnostics live here. cplot owns categorical/numeric
// axes, bar and line series, validation, layout, and scene construction.
//
// Every `bar` and `line` statement is a series of its own, in the order it
// was written: bars group side by side within a category, lines overlay
// them, and each takes the next colour of the theme. `xychart-beta
// horizontal` turns the whole chart on its side — the categories run down
// the y axis and the values along x — while `x-axis` and `y-axis` keep
// naming the category and the value axis, as they do in Mermaid.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
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

constexpr std::string_view kType = "xychart-beta";

/// Byte offset of each line's first character in the source exactly as the
/// caller wrote it, so a refusal can put its caret where the author looks.
class LineStarts {
public:
    explicit LineStarts(std::string_view source) {
        starts_.push_back(0);
        for (std::size_t i = 0; i < source.size(); ++i)
            if (source[i] == '\n') starts_.push_back(i + 1);
    }

    /// Where `column` of a significant line's trimmed text lies in the source.
    std::size_t offset(const SourceLine& line, std::size_t column) const {
        const std::size_t start = line.number - 1 < starts_.size() ? starts_[line.number - 1] : 0;
        return start + line.indent + column;
    }

private:
    std::vector<std::size_t> starts_;
};

/// A refusal anchored on the text that caused it: the message names the
/// line, and the structured error carries the span for a frontend's caret.
[[noreturn]] void refuse(const LineStarts& starts, const SourceLine& line, std::size_t column,
                         std::size_t length, const std::string& what) {
    cworks::AppError error = cworks::validation_failed(at_line(kType, line.number, what));
    error.source_span =
        cworks::SourceSpan{starts.offset(line, column), std::max<std::size_t>(length, 1)};
    throw Error(std::move(error));
}

/// One entry of a bracketed list, with where it sits in its line's text.
struct ListItem {
    std::string text;       ///< trimmed and unquoted
    std::size_t column = 0; ///< offset of the trimmed entry in the line
    std::size_t length = 0; ///< length of the trimmed entry
};

/// One bracketed list: its entries and the columns of its brackets.
struct BracketList {
    std::vector<ListItem> items;
    std::size_t open = 0;
    std::size_t close = 0;

    std::size_t length() const { return close - open + 1; }
};

/// Read the bracketed list that starts at `open` (a '['). Commas inside
/// double quotes belong to the entry. Returns nullopt when the list is never
/// closed.
std::optional<BracketList> read_list(const std::string& text, std::size_t open) {
    BracketList list;
    list.open = open;
    bool quoted = false;
    std::size_t entry = open + 1;
    const auto finish = [&](std::size_t end) {
        std::size_t first = entry;
        std::size_t last = end;
        while (first < last && (text[first] == ' ' || text[first] == '\t')) ++first;
        while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t')) --last;
        list.items.push_back(
            {unquote(std::string_view(text).substr(first, last - first)), first, last - first});
    };
    for (std::size_t i = open + 1; i < text.size(); ++i) {
        if (text[i] == '"') quoted = !quoted;
        if (quoted) continue;
        if (text[i] == ',') {
            finish(i);
            entry = i + 1;
        } else if (text[i] == ']') {
            finish(i);
            list.close = i;
            return list;
        }
    }
    return std::nullopt;
}

std::size_t skip_blanks(const std::string& text, std::size_t at) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
    return at;
}

/// The bracketed list that starts at or after `from`, refused with a caret
/// when it is missing or never closed.
BracketList required_list(const LineStarts& starts, const SourceLine& line, std::size_t from,
                          const std::string& what) {
    const std::string& text = line.text;
    const std::size_t open = text.find('[', from);
    if (open == std::string::npos)
        refuse(starts, line, from, text.size() - from, "expected " + what);
    const std::optional<BracketList> list = read_list(text, open);
    if (!list) refuse(starts, line, open, text.size() - open, "the list is never closed with ']'");
    return *list;
}

/// The value list of a `bar`/`line` statement, after its keyword and an
/// optional series title (Mermaid's `bar "Revenue" [...]`).
std::vector<double> series_values(const LineStarts& starts, const SourceLine& line,
                                  std::size_t keyword_length, BracketList& list) {
    const std::string& text = line.text;
    list = required_list(starts, line, keyword_length, "a non-empty [value, ...] list");
    const std::size_t after = skip_blanks(text, list.close + 1);
    if (after < text.size())
        refuse(starts, line, after, text.size() - after, "unexpected text after the value list");

    std::vector<double> values;
    values.reserve(list.items.size());
    for (const ListItem& item : list.items) {
        double value = 0.0;
        if (!parse_number(item.text, value) || !std::isfinite(value))
            refuse(starts, line, item.column, item.length,
                   "'" + item.text + "' is not a finite number");
        values.push_back(value);
    }
    return values;
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

enum class SeriesKind { Bar, Line };

/// One `bar` or `line` statement, kept in source order.
struct Series {
    SeriesKind kind = SeriesKind::Bar;
    std::vector<double> values;
    SourceLine line;
    BracketList list; ///< where the values were written, for diagnostics
};

const char* kind_name(SeriesKind kind) { return kind == SeriesKind::Bar ? "bar" : "line"; }

/// `xychart-beta [vertical|horizontal]`, then config.xyChart.chartOrientation.
/// The keyword line is what the author reads, so it wins over front matter.
bool horizontal_orientation(const SourceLine& header, const FrontMatter& matter,
                            const RenderOptions& options) {
    static const std::vector<std::string> known = {"vertical", "horizontal"};
    const std::size_t space = header.text.find_first_of(" \t");
    const std::string written =
        space == std::string::npos ? std::string{} : cworks::trim(header.text.substr(space));
    if (written == "horizontal") return true;
    if (written == "vertical") return false;
    if (!written.empty()) {
        diagnose_unrecognized(options, kType, header.number, written, known);
        return false;
    }
    const std::optional<std::string> configured = matter.text("chartOrientation");
    if (!configured || *configured == "vertical") return false;
    if (*configured == "horizontal") return true;
    diagnose_unrecognized(options, kType, 1, *configured, known);
    return false;
}

} // namespace

cplot::Scene build_xychart(std::string_view source,
                           const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    const LineStarts starts(source);
    const FrontMatter matter(source, options, "xyChart");
    const bool horizontal =
        !lines.empty() && horizontal_orientation(lines.front(), matter, options);
    std::string title;
    AxisSpec x_axis;
    AxisSpec y_axis;
    std::vector<std::string> categories;
    std::vector<Series> series;
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
                const BracketList list =
                    required_list(starts, source_line, 6, "an [a, b, ...] category list");
                categories.clear();
                for (const ListItem& item : list.items) categories.push_back(item.text);
                if (categories.size() == 1 && categories.front().empty())
                    refuse(starts, source_line, list.open, list.length(),
                           "x-axis category list is empty");
            } else if (!parse_range(rest, x_axis)) {
                diagnose_unrecognized(options, kType, source_line.number, line, known);
            }
        } else if (keyword == "y-axis") {
            const std::string rest = cworks::trim(line.substr(6));
            (void)parse_range(rest, y_axis);
        } else if (keyword == "bar" || keyword == "line") {
            Series entry;
            entry.kind = keyword == "bar" ? SeriesKind::Bar : SeriesKind::Line;
            entry.line = source_line;
            entry.values = series_values(starts, source_line, keyword.size(), entry.list);
            series.push_back(std::move(entry));
        } else {
            diagnose_unrecognized(options, kType, source_line.number, keyword, known);
        }
    }
    if (series.empty())
        throw Error(cworks::validation_failed("xychart-beta: no bar or line data"));

    // Every series reads against the same x order, so each needs exactly one
    // value per category. Without categories, the first series fixes how many
    // evenly spaced positions the axis has.
    const std::size_t expected = categories.empty() ? series.front().values.size()
                                                    : categories.size();
    for (const Series& entry : series) {
        if (entry.values.size() == expected) continue;
        const std::string axis = categories.empty()
            ? "the first series has " + std::to_string(expected)
            : "the x axis has " + std::to_string(expected) + " categories";
        refuse(starts, entry.line, entry.list.open, entry.list.length(),
               std::string(kind_name(entry.kind)) + " series has " +
                   std::to_string(entry.values.size()) + " values but " + axis);
    }

    std::vector<const Series*> bars;
    for (const Series& entry : series)
        if (entry.kind == SeriesKind::Bar) bars.push_back(&entry);
    if (categories.empty() && !bars.empty()) {
        // Numeric-axis bars stand at the data positions themselves; there is
        // no category band to share or to lay sideways.
        if (horizontal)
            refuse(starts, lines.front(), 0, lines.front().text.size(),
                   "horizontal bars need categories: write the x axis as x-axis [a, b, ...]");
        if (bars.size() > 1)
            refuse(starts, bars[1]->line, 0, bars[1]->line.text.size(),
                   "several bar series need categories to group in: write the x axis as "
                   "x-axis [a, b, ...]");
    }
    if (!bars.empty() && y_axis.explicit_range &&
        (y_axis.minimum > 0.0 || y_axis.maximum < 0.0)) {
        throw Error(cworks::validation_failed(
            "xychart-beta: a bar chart's y-axis range must include zero"));
    }

    const double width = matter.dimension("width", 760.0);
    const double height = matter.dimension("height", 460.0);
    cplot::Scene scene = render_chart(
        options, "xychart-beta", width, height,
        [horizontal, title = std::move(title), x_axis = std::move(x_axis),
         y_axis = std::move(y_axis), categories = std::move(categories),
         series = std::move(series)](cplot::Figure& figure) mutable {
            cplot::Axes& panel = figure.axes();
            panel.title(std::move(title));
            // Mermaid's x-axis is the category axis and its y-axis the value
            // axis, whichever way the chart faces.
            cplot::Axis& category_axis = horizontal ? panel.y_axis() : panel.x_axis();
            cplot::Axis& value_axis = horizontal ? panel.x_axis() : panel.y_axis();
            category_axis.label(std::move(x_axis.label));
            value_axis.label(std::move(y_axis.label));
            // Read top down, as Mermaid lays a horizontal chart out: the
            // first category stands at the top.
            if (horizontal) category_axis.reverse();
            if (x_axis.explicit_range) category_axis.range(x_axis.minimum, x_axis.maximum);
            if (y_axis.explicit_range) value_axis.range(y_axis.minimum, y_axis.maximum);

            // Each series carries its source ordinal + 1 as a hit identity, so
            // a bar or a line's point resolves to the statement that drew it.
            for (std::size_t ordinal = 0; ordinal < series.size(); ++ordinal) {
                const Series& entry = series[ordinal];
                const std::uint64_t id = ordinal + 1;
                if (!categories.empty()) {
                    if (entry.kind == SeriesKind::Bar) {
                        (horizontal ? panel.barh(categories, entry.values)
                                    : panel.bar(categories, entry.values))
                            .hit_id(id);
                    } else {
                        panel.line(categories, entry.values)
                            .horizontal(horizontal)
                            .marker(cplot::Marker::Circle, 3.5)
                            .hit_id(id);
                    }
                    continue;
                }
                const std::vector<double> at = positions(entry.values.size(), x_axis);
                if (entry.kind == SeriesKind::Bar)
                    panel.xbar(at, entry.values).hit_id(id);
                else
                    (horizontal ? panel.line(entry.values, at) : panel.line(at, entry.values))
                        .marker(cplot::Marker::Circle, 3.5)
                        .hit_id(id);
            }
        });
    if (options.regions != nullptr) {
        for (const cplot::HitRegion& hit : cplot::collect_hit_regions(scene)) {
            DrawnRegion region;
            region.key = "#" + std::to_string(hit.id - 1);
            region.bounds = hit.bbox;
            options.regions->push_back(std::move(region));
        }
    }
    return scene;
}

} // namespace cdiagram::detail
