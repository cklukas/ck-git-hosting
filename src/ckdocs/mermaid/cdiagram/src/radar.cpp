// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Radar / spider chart (`radar-beta`) syntax adapter. cplot owns the
// radar model validation, shared radial layout, legends, and scene geometry.

#include <algorithm>
#include <string>
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

std::string unquote_str(const std::string& s) {
    const std::string t = cworks::trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
    return t;
}

/// The label of an axis entry `id["Label"]`, `id(Label)` or bare token.
std::string entry_label(const std::string& raw) {
    const std::string s = cworks::trim(raw);
    const std::size_t lb = s.find_first_of("[(\"");
    if (lb != std::string::npos) {
        const char open = s[lb];
        const char close = open == '[' ? ']' : open == '(' ? ')' : '"';
        const std::size_t rb = s.rfind(close);
        if (rb != std::string::npos && rb > lb)
            return unquote_str(s.substr(lb + 1, rb - lb - 1));
    }
    return unquote_str(s);
}

std::vector<std::string> split_top(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    bool quote = false;
    for (const char c : s) {
        if (c == '"') quote = !quote;
        if (!quote && (c == '[' || c == '(' || c == '{')) ++depth;
        if (!quote && (c == ']' || c == ')' || c == '}')) --depth;
        if (c == sep && depth == 0 && !quote) {
            out.push_back(cworks::trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cworks::trim(cur).empty()) out.push_back(cworks::trim(cur));
    return out;
}

struct Curve {
    std::string label;
    std::vector<double> values;
};

/// True when `line` begins with `kw` as a whole word — the next character
/// is a space, a tab, or the end of the line. A bare rfind(kw, 0) == 0
/// prefix test matched `titled Q1` as `title` and `axisRadius 5` as
/// `axis`, fabricating a bogus title/axis from the tail; the word boundary
/// forces those lines to the fallback where they are reported instead.
bool starts_with_word(const std::string& line, std::string_view kw) {
    if (line.size() < kw.size() || line.compare(0, kw.size(), kw) != 0) return false;
    if (line.size() == kw.size()) return true;
    const char c = line[kw.size()];
    return c == ' ' || c == '\t';
}

} // namespace

cplot::Scene build_radar(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    std::string title;
    std::vector<std::string> axes;
    std::vector<Curve> curves;
    double max_value = 0.0;
    static const std::vector<std::string> kKnown = {"title", "axis", "curve", "max"};
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& l = lines[li].text;
        const std::size_t number = lines[li].number;
        if (starts_with_word(l, "title")) {
            title = entry_label(cworks::trim(l.substr(5)));
        } else if (starts_with_word(l, "axis")) {
            for (const std::string& item : split_top(cworks::trim(l.substr(4)), ','))
                axes.push_back(entry_label(item));
        } else if (starts_with_word(l, "curve")) {
            const std::string rest = cworks::trim(l.substr(5));
            const std::size_t lb = rest.find('{');
            const std::size_t rb = rest.rfind('}');
            if (lb == std::string::npos || rb == std::string::npos) {
                diagnose_unrecognized(options, "radar-beta", number, l, kKnown);
                continue;
            }
            Curve c;
            c.label = entry_label(cworks::trim(rest.substr(0, lb)));
            for (const std::string& num : split_top(rest.substr(lb + 1, rb - lb - 1), ',')) {
                double v = 0.0;
                if (parse_number(num, v)) {
                    c.values.push_back(v);
                    max_value = std::max(max_value, v);
                }
            }
            curves.push_back(std::move(c));
        } else if (starts_with_word(l, "max")) {
            double m = 0.0;
            if (parse_number(cworks::trim(l.substr(3)), m)) max_value = std::max(max_value, m);
        } else {
            // Neither a rendered directive nor a whole-word keyword. Report
            // the line rather than fabricating a bogus title/axis from a
            // prefix collision (the never-silently-drop-input charter).
            const std::string keyword = l.substr(0, l.find_first_of(" \t"));
            if (keyword == "min" || keyword == "ticks" || keyword == "graticule" ||
                keyword == "showLegend") {
                diagnose_unsupported(options, "radar-beta", number, keyword,
                                     "radial-scale and legend options are not rendered");
            } else {
                diagnose_unrecognized(options, "radar-beta", number, keyword, kKnown);
            }
        }
    }
    if (axes.empty() || curves.empty())
        throw Error(cworks::validation_failed("radar: needs axes and curves"));
    if (max_value <= 0.0) max_value = 1.0;
    const std::size_t curve_count = curves.size();
    const FrontMatter matter(source, options, "radar");
    const double width = matter.dimension("width", 680.0);
    const double height = matter.dimension("height", 560.0);
    cplot::Scene scene = render_chart(
        options, "radar-beta", width, height,
        [title = std::move(title), axes = std::move(axes),
         curves = std::move(curves), max_value](cplot::Figure& figure) mutable {
            cplot::Axes& panel = figure.axes();
            panel.title(std::move(title));
            for (Curve& curve : curves) {
                panel.radar(axes, curve.values)
                    .label(std::move(curve.label))
                    .range(0.0, max_value);
            }
        });
    // A radar curve draws as a polygon, which cplot emits as a path rather than
    // a leaf this map can key on; its axis labels are what remain selectable.
    report_chart_regions(options, scene, ChartMark::Rect, curve_count);
    return scene;
}

} // namespace cdiagram::detail
