// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Git graph (`gitGraph`): commits on branch lanes, with branch and merge
// links. Commits are placed left-to-right in command order — a simple
// deterministic lane layout.

#include <algorithm>
#include <string>
#include <vector>

#include <cworks/app_error.hpp>
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

enum class CommitType { Normal, Highlight, Reverse };

struct Commit {
    int order = 0;
    int lane = 0;
    std::string tag;
    std::string id;  ///< `id:` label, drawn under the dot when present
    CommitType type = CommitType::Normal;
};
struct Link {
    int from = 0;
    int to = 0;
    int lane = 0;          // colour lane
    bool dashed = false;   // cherry-pick provenance
};

/// Extract the (possibly unquoted) word value of `key:` — `type: HIGHLIGHT`.
std::string keyed_word(const std::string& line, const std::string& key) {
    const std::size_t k = line.find(key);
    if (k == std::string::npos) return "";
    std::size_t start = k + key.size();
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
    std::size_t end = start;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
    return line.substr(start, end - start);
}

/// Extract the quoted value of `key:` from a command line, if present.
std::string keyed(const std::string& line, const std::string& key) {
    const std::size_t k = line.find(key);
    if (k == std::string::npos) return "";
    const std::size_t q1 = line.find('"', k);
    if (q1 == std::string::npos) return "";
    const std::size_t q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return line.substr(q1 + 1, q2 - q1 - 1);
}

} // namespace

cplot::Scene build_git(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    const std::vector<SourceLine> lines = significant_lines(source);

    const FrontMatter matter(source, options, "gitGraph");
    std::vector<std::string> branches = {matter.text("mainBranchName").value_or("main")};
    std::vector<int> head = {-1};  // head commit index per branch (-1 none)
    int current = 0;
    std::vector<Commit> commits;
    std::vector<Link> links;
    int order = 0;
    LinkTable hyperlinks;  ///< commit id / tag -> hyperlink (click …)

    const auto branch_index = [&](const std::string& name) {
        for (std::size_t i = 0; i < branches.size(); ++i)
            if (branches[i] == name) return static_cast<int>(i);
        return -1;
    };

    // Recognised gitGraph statements. Anything else is reported rather than
    // silently dropped (the never-silently-drop-input charter).
    static const std::vector<std::string> kKnown = {"commit", "branch", "checkout",
                                                     "merge", "cherry-pick"};
    for (std::size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li].text;
        const std::size_t number = lines[li].number;
        // Interaction: `click <id> "url" ["tip"] [_target]` attaches a
        // hyperlink to the commit named by its `id:` (or its tag). JS callback
        // forms carry no URL and cannot be exported, so they are reported.
        if (is_click_statement(line)) {
            std::string ref;
            LinkDirective link;
            if (parse_click_statement(line, ref, link) && !link.callback) {
                if (!hyperlinks.emplace(ref, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("gitGraph", number,
                                     "'" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "gitGraph", number, "click",
                                     "callbacks are not rendered in a static export");
            }
            continue;
        }
        if (line.rfind("commit", 0) == 0) {
            Commit c;
            c.order = order++;
            c.lane = current;
            c.tag = keyed(line, "tag:");
            c.id = keyed(line, "id:");
            const std::string type = keyed_word(line, "type:");
            if (type == "HIGHLIGHT") c.type = CommitType::Highlight;
            else if (type == "REVERSE") c.type = CommitType::Reverse;
            else if (!type.empty() && type != "NORMAL")
                diagnose_unsupported(options, "gitGraph", number, type,
                                     "unknown commit type (drawn as NORMAL)");
            const int idx = static_cast<int>(commits.size());
            if (head[static_cast<std::size_t>(current)] >= 0)
                links.push_back({head[static_cast<std::size_t>(current)], idx, current});
            head[static_cast<std::size_t>(current)] = idx;
            commits.push_back(c);
        } else if (line.rfind("branch ", 0) == 0) {
            const std::string name = cworks::trim(line.substr(7));
            if (branch_index(name) < 0) {
                branches.push_back(name);
                head.push_back(head[static_cast<std::size_t>(current)]);
                current = static_cast<int>(branches.size()) - 1;
            }
        } else if (line.rfind("checkout ", 0) == 0) {
            const std::string name = cworks::trim(line.substr(9));
            const int b = branch_index(name);
            if (b >= 0)
                current = b;
            else
                // Switching to a branch that was never created: a silent
                // no-op would hide the typo, so say so and carry on.
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("gitGraph", number,
                                 "checkout of unknown branch '" + name + "' (ignored)"));
        } else if (line.rfind("merge ", 0) == 0) {
            // The branch name is the first token; a `tag:`/`type:` suffix must
            // not be folded into it (or a valid merge would look unknown).
            const std::string rest = cworks::trim(line.substr(6));
            const std::string name = rest.substr(0, rest.find_first_of(" \t"));
            const int b = branch_index(name);
            if (b < 0) {
                // Unknown merge source: parsing it as a commit would fabricate
                // a bogus dot on the current lane. Report and skip rather than
                // draw an edge the source never described.
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("gitGraph", number,
                                 "merge of unknown branch '" + name + "' (ignored)"));
                continue;
            }
            Commit c;
            c.order = order++;
            c.lane = current;
            c.tag = keyed(line, "tag:");
            const int idx = static_cast<int>(commits.size());
            if (head[static_cast<std::size_t>(current)] >= 0)
                links.push_back({head[static_cast<std::size_t>(current)], idx, current});
            if (head[static_cast<std::size_t>(b)] >= 0)
                links.push_back({head[static_cast<std::size_t>(b)], idx, b});
            head[static_cast<std::size_t>(current)] = idx;
            commits.push_back(c);
        } else {
            const std::string keyword = line.substr(0, line.find_first_of(" \t"));
            if (keyword == "cherry-pick") {
                // A cherry-pick copies the commit named by `id:` onto the
                // current branch, linked back to its source with a dashed
                // edge. An unknown id must not fabricate a dot.
                const std::string source_id = keyed(line, "id:");
                int source = -1;
                for (std::size_t ci = 0; ci < commits.size(); ++ci)
                    if (!commits[ci].id.empty() && commits[ci].id == source_id)
                        source = static_cast<int>(ci);
                if (source < 0) {
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("gitGraph", number,
                                     "cherry-pick of unknown commit id '" + source_id +
                                         "' (ignored)"));
                    continue;
                }
                Commit c;
                c.order = order++;
                c.lane = current;
                c.id = source_id;
                c.tag = keyed(line, "tag:");
                const int idx = static_cast<int>(commits.size());
                if (head[static_cast<std::size_t>(current)] >= 0)
                    links.push_back({head[static_cast<std::size_t>(current)], idx, current});
                links.push_back(
                    {source, idx, commits[static_cast<std::size_t>(source)].lane, true});
                head[static_cast<std::size_t>(current)] = idx;
                commits.push_back(c);
            } else {
                diagnose_unrecognized(options, "gitGraph", number, keyword, kKnown);
            }
        }
    }
    if (commits.empty()) throw Error(cworks::validation_failed("gitGraph: no commits"));

    const Font font = theme.base_font();
    const Font label_font = theme.base_font().with_weight(cplot::FontWeight::Bold);
    const Font tag_font = font.with_size(font.size - 1.0);
    const double margin = 22.0;
    const double lane_gap = 46.0;
    const double x_step = 46.0;
    // Tag pills float `tag_lift` px above their commit dot; the pill height is
    // driven by the tag font's line height so the text always fits.
    const double tag_lift = 10.0;
    const double tag_pad_x = 4.0;
    const double tag_h = tag_font.size * 1.4;

    bool any_tag = false;
    for (const Commit& c : commits)
        if (!c.tag.empty()) { any_tag = true; break; }

    const bool show_branches = matter.boolean("showBranches").value_or(true);
    double label_w = 0.0;
    if (show_branches)
        for (const std::string& b : branches)
            label_w = std::max(label_w, text_width(b, label_font));
    const double x0 = margin + label_w + 14.0;
    // Reserve enough space above the first lane that the top-most tag pill
    // never clips the canvas top, even at large font sizes.
    const double y0 = margin + std::max(16.0, any_tag ? tag_h + tag_lift - margin : 16.0);

    Canvas canvas;
    canvas.background = sty.page;
    canvas.width = x0 + static_cast<double>(order) * x_step + margin;
    canvas.height = y0 + static_cast<double>(branches.size()) * lane_gap + margin;

    const auto cx = [&](const Commit& c) { return x0 + static_cast<double>(c.order) * x_step; };
    const auto ly = [&](int lane) { return y0 + static_cast<double>(lane) * lane_gap; };

    // Branch name labels + faint lane lines.
    if (show_branches) {
        for (std::size_t i = 0; i < branches.size(); ++i) {
            canvas.text({margin, ly(static_cast<int>(i))}, branches[i], label_font,
                        sty.series(i), HAlign::Left, VAlign::Middle);
        }
    }

    // Links.
    for (const Link& l : links) {
        const Commit& a = commits[static_cast<std::size_t>(l.from)];
        const Commit& b = commits[static_cast<std::size_t>(l.to)];
        ShapeStyle ls;
        ls.stroke = sty.series(static_cast<std::size_t>(l.lane));
        ls.stroke_width = 2.4;
        ls.cap = cplot::LineCap::Round;
        ls.join = cplot::LineJoin::Round;
        if (l.dashed) ls.dash = cplot::DashPattern{{4.0, 4.0}};  // cherry-pick provenance
        const Point pa{cx(a), ly(a.lane)};
        const Point pb{cx(b), ly(b.lane)};
        if (a.lane == b.lane) {
            canvas.line(pa, pb, ls);
        } else {
            // A gentle step: horizontal then diagonal.
            canvas.polyline({pa, {pb.x - x_step / 2.0, pa.y}, pb}, ls);
        }
    }

    // Commit dots + tags. HIGHLIGHT commits render as filled squares,
    // REVERSE commits carry a cross, matching Mermaid's distinction.
    std::size_t commit_ordinal = 0;
    for (const Commit& c : commits) {
        const Point p{cx(c), ly(c.lane)};
        ShapeStyle dot;
        dot.fill = sty.series(static_cast<std::size_t>(c.lane));
        dot.stroke = sty.label_mask;
        dot.stroke_width = 1.5;
        if (c.type == CommitType::Highlight)
            canvas.rect(RectF{p.x - 7.0, p.y - 7.0, 14.0, 14.0}, dot);
        else
            canvas.circle(p, 7.0, dot);
        // A commit dot is small, so its hit area is the square around it
        // rather than the 7-point circle: aiming at a dot should not demand
        // pixel accuracy.
        if (options.regions != nullptr) {
            options.regions->push_back({"#" + std::to_string(commit_ordinal++),
                                        0 /* HitRole::Node */,
                                        RectF{p.x - 9.0, p.y - 9.0, 18.0, 18.0}});
        }
        if (c.type == CommitType::Reverse) {
            ShapeStyle bar;
            bar.stroke = sty.label_mask;
            bar.stroke_width = 1.8;
            canvas.line({p.x - 3.5, p.y - 3.5}, {p.x + 3.5, p.y + 3.5}, bar);
            canvas.line({p.x - 3.5, p.y + 3.5}, {p.x + 3.5, p.y - 3.5}, bar);
        }
        if (!c.id.empty())
            canvas.text({p.x, p.y + 11.0}, c.id, tag_font, sty.muted, HAlign::Center,
                        VAlign::Top);
        if (!c.tag.empty()) {
            const double tw = text_width(c.tag, tag_font);
            const double box_w = tw + tag_pad_x * 2.0;
            const double box_top = p.y - tag_lift - tag_h;
            ShapeStyle tag;
            tag.fill = sty.note_fill;
            tag.stroke = sty.note_stroke;
            tag.stroke_width = 1.0;
            canvas.rounded_rect(RectF{p.x - box_w / 2.0, box_top, box_w, tag_h}, 3.0, tag);
            canvas.text({p.x, box_top + tag_h / 2.0}, c.tag, tag_font,
                        theme.text_color, HAlign::Center, VAlign::Middle);
        }
        // Per-element hyperlink: a `click <id>` / `click <tag>` region over the
        // commit marker. A commit is addressable by its stable `id:` or, failing
        // that, its tag text; the dot's bounding box is the clickable hit area.
        for (const std::string& ref : {c.id, c.tag}) {
            if (ref.empty()) continue;
            const auto it = hyperlinks.find(ref);
            if (it == hyperlinks.end()) continue;
            canvas.add_link(RectF{p.x - 7.0, p.y - 7.0, 14.0, 14.0}, it->second.href,
                            it->second.title, it->second.target);
            break;
        }
    }

    return canvas.bake();
}

} // namespace cdiagram::detail
