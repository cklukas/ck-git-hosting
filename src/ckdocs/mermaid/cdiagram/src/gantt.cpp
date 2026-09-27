// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Gantt chart (`gantt`): tasks with a start date (or `after <id>`) and a
// duration, grouped in sections, drawn as bars on a day grid. Dates use
// the suite's own proleptic-Gregorian civil arithmetic (no libc, no
// locale) so the layout is byte-deterministic.

#include <algorithm>
#include <cmath>
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
#include "links.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram::detail {

namespace {

// Hinnant's civil<->days algorithms (days since 1970-01-01).
std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
void civil_from_days(std::int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    y = static_cast<int>(yy + (m <= 2));
}

bool parse_date(const std::string& s, double& day) {
    std::int64_t y = 0, m = 0, d = 0;
    const std::size_t a = s.find('-'), b = s.find('-', a + 1);
    if (a == std::string::npos || b == std::string::npos) return false;
    if (!cworks::parse_int(s.substr(0, a), y) ||
        !cworks::parse_int(s.substr(a + 1, b - a - 1), m) ||
        !cworks::parse_int(s.substr(b + 1), d))
        return false;
    if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31)
        throw Error(cworks::validation_failed("gantt: date is out of range"));
    day = static_cast<double>(days_from_civil(y, m, d));
    return true;
}
std::string format_day(double day) {
    int y = 0, m = 0, d = 0;
    civil_from_days(static_cast<std::int64_t>(day), y, m, d);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return buf;
}

double parse_duration(const std::string& s) {
    if (s.empty()) return 1.0;
    const char unit = s.back();
    std::string num = s;
    double mult = 1.0;
    if (unit == 'd' || unit == 'w' || unit == 'h') {
        num = s.substr(0, s.size() - 1);
        mult = unit == 'w' ? 7.0 : unit == 'h' ? 1.0 / 24.0 : 1.0;
    }
    double n = 0.0;
    if (!parse_number(num, n)) return 1.0;
    return n * mult;
}

enum class Status { Normal, Done, Active, Crit, Milestone };

struct GTask {
    std::string name;
    std::string id;
    Status status = Status::Normal;
    double start = 0.0;
    double end = 0.0;
    int section = -1;
};

std::vector<std::string> split_comma(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ',') {
            out.push_back(cworks::trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cworks::trim(cur));
    return out;
}

Color status_color(Status s, const DiagramStyle& sty) {
    switch (s) {
    case Status::Done: return sty.task_done;
    case Status::Active: return sty.task_active;
    case Status::Crit: return sty.task_crit;
    default: return sty.task_default;
    }
}

} // namespace

cplot::Scene build_gantt(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    const FrontMatter matter(source, options, "gantt");
    const std::string display_mode = !matter.display_mode().empty()
                                         ? matter.display_mode()
                                         : matter.text("displayMode").value_or("");
    if (display_mode == "compact")
        diagnose_unsupported(options, "gantt", 1, "displayMode: compact",
                             "compact row packing is not yet supported");
    const std::vector<SourceLine> lines = significant_lines(source);
    std::string title;
    std::vector<std::string> sections;
    std::vector<GTask> tasks;
    LinkTable links;  ///< task id (or name) -> hyperlink (click …)
    static const std::vector<std::string> kKnown = {
        "title", "dateFormat", "axisFormat", "excludes", "todayMarker", "section", "click"};
    int section = -1;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        if (line.rfind("title ", 0) == 0) {
            title = cworks::trim(line.substr(6));
            continue;
        }
        // Interaction: `click <task-id> "url" ["tip"] [_target]` attaches a
        // hyperlink to the task's bar (its id, or its name when it has none).
        // JS callback forms carry no URL and cannot be exported, so they are
        // reported, not linked.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("gantt", number,
                                     "task '" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "gantt", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        // `dateFormat`/`axisFormat`/`excludes`/`todayMarker` are recognised
        // gantt directives; this engine derives dates and the axis itself and
        // draws every day, so they change nothing here and are accepted
        // silently (they are part of ordinary, valid gantt source).
        if (line.rfind("dateFormat", 0) == 0 || line.rfind("axisFormat", 0) == 0 ||
            line.rfind("excludes", 0) == 0 || line.rfind("todayMarker", 0) == 0)
            continue;
        if (line.rfind("section ", 0) == 0) {
            sections.push_back(cworks::trim(line.substr(8)));
            section = static_cast<int>(sections.size()) - 1;
            continue;
        }
        const std::size_t colon = line.find(':');
        // A task is `Name : fields`. Guard the fabrication below: a line whose
        // leading token is a recognised directive keyword did not match the
        // forms above (e.g. `section: Design` with a colon, or a bare `title`)
        // — it is a malformed directive, not a task, and must NOT create a
        // spurious zero-day bar. Report it instead of misparsing in silence.
        const std::string keyword = line.substr(0, line.find_first_of(" \t:"));
        if (keyword == "title" || keyword == "section" || keyword == "dateFormat" ||
            keyword == "axisFormat" || keyword == "excludes" || keyword == "todayMarker") {
            diagnose_unsupported(options, "gantt", number, keyword,
                                 "recognised gantt keyword, but not in a form this "
                                 "engine renders");
            continue;
        }
        if (colon == std::string::npos) {
            // Neither a directive nor a `Name : fields` task — report the line
            // rather than dropping it (the never-silently-drop-input charter).
            diagnose_unrecognized(options, "gantt", number, keyword, kKnown);
            continue;
        }
        GTask t;
        t.name = cworks::trim(line.substr(0, colon));
        t.section = section;
        std::vector<std::string> f = split_comma(line.substr(colon + 1));
        if (f.empty()) continue;
        const std::string dur = f.back();
        f.pop_back();
        std::string start_field;
        if (!f.empty()) {
            start_field = f.back();
            f.pop_back();
        }
        for (const std::string& tag : f) {
            if (tag == "done") t.status = Status::Done;
            else if (tag == "active") t.status = Status::Active;
            else if (tag == "crit") t.status = Status::Crit;
            else if (tag == "milestone") t.status = Status::Milestone;
            else if (!tag.empty()) t.id = tag;
        }
        double start = 0.0;
        if (start_field.rfind("after ", 0) == 0) {
            const std::string dep = cworks::trim(start_field.substr(6));
            for (const GTask& prev : tasks)
                if (prev.id == dep) start = std::max(start, prev.end);
        } else if (!parse_date(start_field, start)) {
            start = tasks.empty() ? 0.0 : tasks.back().end;
        }
        t.start = start;
        t.end = start + parse_duration(dur);
        tasks.push_back(std::move(t));
    }
    if (tasks.empty()) throw Error(cworks::validation_failed("gantt: no tasks"));

    double min_day = tasks.front().start, max_day = tasks.front().end;
    for (const GTask& t : tasks) {
        min_day = std::min(min_day, t.start);
        max_day = std::max(max_day, t.end);
    }
    const double span = std::max(1.0, max_day - min_day);
    if (!std::isfinite(max_day) || !std::isfinite(min_day) || span > 3650)
        throw Error(cworks::validation_failed("gantt: timespan exceeds 3650 days"));

    const Font font = theme.base_font();
    const Font title_font = theme.title_font();
    const Font section_font = theme.base_font().with_weight(cplot::FontWeight::Bold);
    const double line_h = font.size * 1.4;
    const double margin = 24.0;
    const double title_h = title.empty() ? 0.0 : title_font.size * 1.9;
    const double row_h = line_h + 12.0;
    const double axis_h = line_h + 6.0;

    double name_w = 0.0;
    for (const GTask& t : tasks) name_w = std::max(name_w, text_width(t.name, font));
    for (const std::string& s : sections)
        name_w = std::max(name_w, text_width(s, section_font));
    name_w += 16.0;

    const double chart_w = std::max(360.0, span * 26.0);
    const double scale = chart_w / span;

    // Rows: a header row per section, then its tasks.
    std::vector<int> row_of(tasks.size());
    int rows = 0;
    int cur_section = -2;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i].section != cur_section && tasks[i].section >= 0) {
            cur_section = tasks[i].section;
            ++rows;  // section header row
        }
        row_of[i] = rows++;
    }

    const double chart_x = margin + name_w;
    const double rows_top = margin + title_h + axis_h;

    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = chart_x + chart_w + margin;
    canvas.height = rows_top + rows * row_h + margin;
    canvas.title = title;

    if (!title.empty())
        canvas.text({canvas.width / 2.0, margin}, title, title_font, sty.text,
                    HAlign::Center, VAlign::Top);

    // Weekly gridlines with date labels.
    ShapeStyle grid;
    grid.stroke = sty.grid;
    grid.stroke_width = 1.0;
    const double grid_bottom = rows_top + rows * row_h;
    for (double d = 0.0; d <= span + 0.001; d += 7.0) {
        const double gx = chart_x + d * scale;
        canvas.line({gx, rows_top}, {gx, grid_bottom}, grid);
        canvas.text({gx, margin + title_h}, format_day(min_day + d),
                    font.with_size(font.size - 1.5), sty.muted, HAlign::Center,
                    VAlign::Top);
    }

    // Section header rows.
    int cur = -2;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i].section != cur && tasks[i].section >= 0) {
            cur = tasks[i].section;
            const double y = rows_top + (row_of[i] - 1) * row_h;
            canvas.text({margin, y + row_h / 2.0},
                        sections[static_cast<std::size_t>(cur)], section_font,
                        sty.text, HAlign::Left, VAlign::Middle);
        }
    }

    // Task bars.
    std::size_t task_ordinal = 0;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const GTask& t = tasks[i];
        const double y = rows_top + row_of[i] * row_h;
        canvas.text({margin, y + row_h / 2.0}, t.name, font, sty.text, HAlign::Left,
                    VAlign::Middle);
        const double bx = chart_x + (t.start - min_day) * scale;
        const Color col = status_color(t.status, sty);
        RectF box;
        if (t.status == Status::Milestone) {
            ShapeStyle dot;
            dot.fill = col;
            const double cy = y + row_h / 2.0;
            canvas.polygon({{bx, cy - 7.0}, {bx + 7.0, cy}, {bx, cy + 7.0}, {bx - 7.0, cy}},
                           dot);
            box = RectF{bx - 7.0, cy - 7.0, 14.0, 14.0};
        } else {
            const double bw = std::max(3.0, (t.end - t.start) * scale);
            ShapeStyle bar;
            bar.fill = col.with_alpha(t.status == Status::Done ? 0.9 : 0.85);
            bar.stroke = col;
            bar.stroke_width = 1.0;
            box = RectF{bx, y + 4.0, bw, row_h - 8.0};
            canvas.rounded_rect(box, 3.0, bar);
        }
        // Where this task was drawn, so it can be selected. `box` is set by
        // both branches above, so a milestone is as selectable as a bar.
        if (options.regions != nullptr)
            options.regions->push_back({"#" + std::to_string(task_ordinal++),
                                        0 /* HitRole::Node */, box});
        // Per-task hyperlink: keyed by the task id, or its name when it has none.
        if (const auto it = links.find(t.id.empty() ? t.name : t.id); it != links.end())
            canvas.add_link(box, it->second.href, it->second.title, it->second.target);
    }

    return canvas.bake();
}

} // namespace cdiagram::detail
