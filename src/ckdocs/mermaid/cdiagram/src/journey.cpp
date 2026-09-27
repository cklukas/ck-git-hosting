// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// User journey (`journey`): tasks in sequence, each scored (1..5) and
// tagged with actors, grouped into sections. Pure geometry.

#include <algorithm>
#include <string>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/format.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "canvas.hpp"
#include "cdiagram/error.hpp"
#include "diagnostics.hpp"
#include "links.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram::detail {

namespace {

struct JTask {
    std::string name;
    int score = 3;
    std::string actors;
    int section = -1;
};

std::vector<std::string> split_colon(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ':') {
            out.push_back(cworks::trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cworks::trim(cur));
    return out;
}

Color score_color(int score, const DiagramStyle& sty) {
    const int clamped = std::min(5, std::max(1, score));
    return sty.score_scale[static_cast<std::size_t>(clamped - 1)];
}

} // namespace

cplot::Scene build_journey(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    const std::vector<SourceLine> lines = significant_lines(source);
    std::string title;
    std::vector<std::string> sections;
    std::vector<JTask> tasks;
    LinkTable links;  ///< task name -> hyperlink (click …)
    int section = -1;
    static const std::vector<std::string> kKnown = {"title", "section", "click"};
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        if (line.rfind("title ", 0) == 0) {
            title = cworks::trim(line.substr(6));
            continue;
        }
        if (line.rfind("section ", 0) == 0) {
            sections.push_back(cworks::trim(line.substr(8)));
            section = static_cast<int>(sections.size()) - 1;
            continue;
        }
        const std::string keyword = line.substr(0, line.find_first_of(" \t:"));
        // Accessibility directives are valid journey syntax but carry no task;
        // without this they split on ':' and fabricate a bogus task card.
        if (keyword == "accTitle" || keyword == "accDescr") {
            diagnose_unsupported(options, "journey", number, keyword,
                                 "accessibility metadata is not rendered");
            continue;
        }
        // Interaction: `click <task> "url" ["tip"] [_target]` attaches a
        // hyperlink to the matching task card. JS callback forms carry no URL
        // and cannot be exported, so they are reported, not linked.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!links.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("journey", number,
                                     "task '" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "journey", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        const std::vector<std::string> parts = split_colon(line);
        // A task is `Name: score: actors` with a numeric 1..5 score. Guard the
        // fabrication: a line without a parseable score is metadata or a typo,
        // not a task, and must not become a card with a silently-defaulted score
        // (the never-silently-drop-or-coerce charter).
        std::int64_t score = 0;
        if (parts.size() < 2 || !cworks::parse_int(parts[1], score)) {
            diagnose_unrecognized(options, "journey", number, keyword, kKnown);
            continue;
        }
        JTask t;
        t.name = parts[0];
        t.score = static_cast<int>(std::clamp<std::int64_t>(score, 1, 5));
        if (parts.size() >= 3) t.actors = parts[2];
        t.section = section;
        tasks.push_back(std::move(t));
    }
    if (tasks.empty()) throw Error(cworks::validation_failed("journey: no tasks"));

    const Font font = theme.base_font();
    const Font title_font = theme.title_font();
    const Font section_font = theme.base_font().with_weight(cplot::FontWeight::Bold);
    const Font small = font.with_size(font.size - 1.0);
    const double line_h = font.size * 1.35;
    const double margin = 24.0;
    const double title_h = title.empty() ? 0.0 : title_font.size * 1.9;
    const double section_h = sections.empty() ? 0.0 : line_h + 8.0;
    const double gap = 12.0;
    const double card_h = line_h * 2.0 + 26.0;

    std::vector<double> w(tasks.size());
    double total = margin;
    std::vector<double> x(tasks.size());
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        double cw = text_width(tasks[i].name, font);
        cw = std::max(cw, text_width(tasks[i].actors, small));
        w[i] = std::max(cw + 24.0, 90.0);
        x[i] = total;
        total += w[i] + gap;
    }
    total += margin - gap;

    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = total;
    canvas.height = margin + title_h + section_h + card_h + margin;
    canvas.title = title;

    if (!title.empty())
        canvas.text({canvas.width / 2.0, margin}, title, title_font, sty.text,
                    HAlign::Center, VAlign::Top);

    const double card_y = margin + title_h + section_h;

    // Section bands above the tasks they contain.
    if (!sections.empty()) {
        std::size_t i = 0;
        while (i < tasks.size()) {
            const int sec = tasks[i].section;
            std::size_t j = i;
            while (j < tasks.size() && tasks[j].section == sec) ++j;
            if (sec >= 0) {
                const double bx0 = x[i];
                const double bx1 = x[j - 1] + w[j - 1];
                const double by = margin + title_h;
                ShapeStyle band;
                band.fill = sty.series(static_cast<std::size_t>(sec)).with_alpha(0.15);
                canvas.rect(RectF{bx0, by, bx1 - bx0, section_h - 4.0}, band);
                canvas.text({(bx0 + bx1) / 2.0, by + (section_h - 4.0) / 2.0},
                            sections[static_cast<std::size_t>(sec)], section_font,
                            sty.text, HAlign::Center, VAlign::Middle);
            }
            i = j;
        }
    }

    // Connecting line through the score dots.
    ShapeStyle track;
    track.stroke = sty.grid;
    track.stroke_width = 2.0;
    if (tasks.size() > 1)
        canvas.line({x.front() + w.front() / 2.0, card_y + 14.0},
                    {x.back() + w.back() / 2.0, card_y + 14.0}, track);

    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const JTask& t = tasks[i];
        const double cx = x[i] + w[i] / 2.0;
        // Score dot on the track.
        ShapeStyle dot;
        dot.fill = score_color(t.score, sty);
        canvas.circle({cx, card_y + 14.0}, 11.0, dot);
        canvas.text({cx, card_y + 14.0}, cworks::format_int(t.score),
                    small.with_weight(cplot::FontWeight::Bold), cplot::colors::white,
                    HAlign::Center, VAlign::Middle);
        // Name and actors below.
        canvas.text({cx, card_y + 34.0}, t.name, font, sty.text, HAlign::Center,
                    VAlign::Top);
        if (!t.actors.empty())
            canvas.text({cx, card_y + 34.0 + line_h}, t.actors, small,
                        sty.muted, HAlign::Center, VAlign::Top);
        // The whole column is the task's hit area — the dot, its name and its
        // actors are one thing to a reader, and aiming at any of them means
        // the task.
        if (options.regions != nullptr) {
            options.regions->push_back(
                {"#" + std::to_string(i), 0 /* HitRole::Node */,
                 RectF{x[i], card_y, w[i], card_h}});
        }
        // Per-task hyperlink over the task's column (dot, name, and actors).
        if (const auto it = links.find(t.name); it != links.end())
            canvas.add_link(RectF{x[i], card_y, w[i], card_h}, it->second.href,
                            it->second.title, it->second.target);
    }

    return canvas.bake();
}

} // namespace cdiagram::detail
