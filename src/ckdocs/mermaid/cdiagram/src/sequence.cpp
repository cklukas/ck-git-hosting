// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Sequence diagram: `sequenceDiagram` then participant/actor
// declarations (groupable in coloured `box … end` bands), messages
// (`A->>B: text`, optionally auto-numbered), notes, activations,
// alt/opt/loop/par/critical/break frames with else/and/option dividers,
// and `rect <color>` background bands. Participants get fixed lifelines;
// steps stack top to bottom in source order — a deterministic layout
// with no graph solver. Lifeline gaps widen to fit the widest message
// label, so text never overlaps.

#include <algorithm>
#include <array>
#include <cstddef>
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
#include "styleprops.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram::detail {

namespace {

enum class Arrow { SolidNoHead, DashedNoHead, SolidHead, DashedHead, Cross, Open };

struct ArrowSpec {
    const char* token;
    Arrow kind;
    bool dashed;
};

// Longest tokens first so `-->>` wins over `->>`, `-->` over `->`.
constexpr std::array<ArrowSpec, 8> kArrows{{
    {"-->>", Arrow::DashedHead, true},
    {"-->", Arrow::DashedNoHead, true},
    {"--x", Arrow::Cross, true},
    {"--)", Arrow::Open, true},
    {"->>", Arrow::SolidHead, false},
    {"->", Arrow::SolidNoHead, false},
    {"-x", Arrow::Cross, false},
    {"-)", Arrow::Open, false},
}};

struct Participant {
    std::string id;
    std::string label;
};

enum class NotePos { LeftOf, RightOf, Over };

/// What a step is. Messages and notes lay out as before; the C8 kinds
/// (control-block frames + activation) drive the frame/bar geometry.
enum class StepKind { Message, Note, BlockStart, BlockDivider, BlockEnd, Activate, Deactivate };
enum class BlockType { Alt, Opt, Loop, Par, Critical, Break, Band };  // Band = `rect <color>`

struct Step {
    StepKind kind = StepKind::Message;
    // message
    std::size_t from = 0;
    std::size_t to = 0;
    Arrow arrow = Arrow::SolidHead;
    bool dashed = false;
    bool activate_target = false;  ///< `+` shorthand: activate the recipient
    bool deactivate_source = false; ///< `-` shorthand: deactivate the sender
    // note
    NotePos note_pos = NotePos::Over;
    std::size_t note_a = 0;
    std::size_t note_b = 0;
    // block / activation
    BlockType block = BlockType::Alt;
    std::optional<Color> color;  ///< Band fill (`rect rgb(...)`)
    std::size_t actor = 0;  ///< target of activate/deactivate
    std::string text;       ///< message/note text, or block/divider label
};

/// A `box <color?> <label?> … end` participant group.
struct PBox {
    std::optional<Color> fill;
    std::string label;
    std::size_t lo = 0, hi = 0;  ///< spanned participant range
    bool has_members = false;
};

struct SeqModel {
    std::string title;
    std::vector<Participant> participants;
    std::vector<Step> steps;
    std::vector<PBox> boxes;
    LinkTable links;  ///< participant id -> hyperlink (link/links actor menus)
};

bool starts_word(const std::string& line, const char* word) {
    const std::string w = word;
    if (line.compare(0, w.size(), w) != 0) return false;
    return line.size() == w.size() || line[w.size()] == ' ' || line[w.size()] == '\t';
}

/// Case-insensitive check that `line` begins with `word` as a token.
bool starts_word_ci(const std::string& line, const char* word) {
    std::string lower = line;
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    std::string w = word;
    for (char& c : w)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return starts_word(lower, w.c_str());
}

/// Find the arrow operator in `line`: the earliest position matching any
/// token, longest token at that position. Returns false when none.
bool find_arrow(const std::string& line, std::size_t& pos, const ArrowSpec*& spec) {
    for (std::size_t i = 0; i < line.size(); ++i) {
        for (const ArrowSpec& s : kArrows) {
            const std::size_t len = std::string(s.token).size();
            if (line.compare(i, len, s.token) == 0) {
                pos = i;
                spec = &s;
                return true;
            }
        }
    }
    return false;
}

std::string strip_activation(std::string id) {
    id = cworks::trim(id);
    while (!id.empty() && (id.front() == '+' || id.front() == '-')) id.erase(id.begin());
    while (!id.empty() && (id.back() == '+' || id.back() == '-')) id.pop_back();
    return cworks::trim(id);
}

/// Index of participant `id`, creating it (label = id) when first seen.
std::size_t participant_index(SeqModel& model, const std::string& id) {
    for (std::size_t i = 0; i < model.participants.size(); ++i)
        if (model.participants[i].id == id) return i;
    model.participants.push_back({id, id});
    return model.participants.size() - 1;
}

std::size_t parse_participant(SeqModel& model, const std::string& rest, std::size_t number) {
    // "<id>" or "<id> as <label>"
    const std::string body = cworks::trim(rest);
    if (body.empty())
        throw Error(cworks::validation_failed("sequence (line " + std::to_string(number) +
                                              "): participant needs a name"));
    const std::size_t as = body.find(" as ");
    std::string id = as == std::string::npos ? body : cworks::trim(body.substr(0, as));
    std::string label = as == std::string::npos ? id : cworks::trim(body.substr(as + 4));
    const std::size_t idx = participant_index(model, id);
    model.participants[idx].label = label;
    return idx;
}

void parse_note(SeqModel& model, const std::string& line, std::size_t number) {
    // Note left of X: t | Note right of X: t | Note over X[,Y]: t
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos)
        throw Error(cworks::validation_failed("sequence (line " + std::to_string(number) +
                                              "): note needs ': text'"));
    std::string head = cworks::trim(line.substr(0, colon));  // "Note over A,B"
    const std::string text = cworks::trim(line.substr(colon + 1));
    // drop the leading "Note"
    head = cworks::trim(head.substr(4));
    Step step;
    step.kind = StepKind::Note;
    step.text = text;
    std::string targets;
    if (starts_word_ci(head, "left of")) {
        step.note_pos = NotePos::LeftOf;
        targets = cworks::trim(head.substr(7));
    } else if (starts_word_ci(head, "right of")) {
        step.note_pos = NotePos::RightOf;
        targets = cworks::trim(head.substr(8));
    } else if (starts_word_ci(head, "over")) {
        step.note_pos = NotePos::Over;
        targets = cworks::trim(head.substr(4));
    } else {
        throw Error(cworks::validation_failed("sequence (line " + std::to_string(number) +
                    "): note must be 'left of', 'right of', or 'over'"));
    }
    const std::size_t comma = targets.find(',');
    const std::string a = cworks::trim(comma == std::string::npos ? targets
                                                                  : targets.substr(0, comma));
    step.note_a = participant_index(model, a);
    step.note_b = comma == std::string::npos
                      ? step.note_a
                      : participant_index(model, cworks::trim(targets.substr(comma + 1)));
    model.steps.push_back(std::move(step));
}

void parse_message(SeqModel& model, const std::string& line, std::size_t pos,
                   const ArrowSpec& spec) {
    const std::string from_id = strip_activation(line.substr(0, pos));
    std::string rest = line.substr(pos + std::string(spec.token).size());
    std::string to_id;
    std::string text;
    const std::size_t colon = rest.find(':');
    if (colon == std::string::npos) {
        to_id = strip_activation(rest);
    } else {
        to_id = strip_activation(rest.substr(0, colon));
        text = cworks::trim(rest.substr(colon + 1));
    }
    // C8: activation shorthand — `+` before the recipient activates it,
    // `-` deactivates the sender (Mermaid's `->>+`/`->>-` convention).
    const std::string rtrim = cworks::trim(rest);
    Step step;
    step.kind = StepKind::Message;
    if (!rtrim.empty() && rtrim[0] == '+') step.activate_target = true;
    else if (!rtrim.empty() && rtrim[0] == '-') step.deactivate_source = true;
    step.from = participant_index(model, from_id);
    step.to = participant_index(model, to_id);
    step.arrow = spec.kind;
    step.dashed = spec.dashed;
    step.text = text;
    model.steps.push_back(std::move(step));
}

SeqModel parse_sequence(std::string_view source, const RenderOptions& options) {
    const std::vector<SourceLine> lines = significant_lines(source);
    SeqModel model;
    int open_blocks = 0;   // for matching-'end' diagnostics
    int open_box = -1;      // the `box … end` group currently collecting members
    bool autonumber = false;
    std::int64_t auto_next = 1;
    std::int64_t auto_step = 1;
    for (std::size_t i = 1; i < lines.size(); ++i) {  // [0] is the header
        const std::string& line = lines[i].text;
        const std::size_t number = lines[i].number;
        if (starts_word(line, "participant") || starts_word(line, "actor")) {
            const std::size_t sp = line.find(' ');
            const std::size_t idx = parse_participant(model, line.substr(sp + 1), number);
            if (open_box >= 0) {
                PBox& box = model.boxes[static_cast<std::size_t>(open_box)];
                if (!box.has_members) {
                    box.lo = box.hi = idx;
                    box.has_members = true;
                } else {
                    box.lo = std::min(box.lo, idx);
                    box.hi = std::max(box.hi, idx);
                }
            }
            continue;
        }
        if (starts_word(line, "title")) {
            model.title = cworks::trim(line.substr(5));
            continue;
        }
        // C8: control blocks — alt/opt/loop/par with else/and dividers.
        {
            BlockType bt;
            std::size_t kw = 0;
            bool is_block = true;
            if (starts_word(line, "alt")) { bt = BlockType::Alt; kw = 3; }
            else if (starts_word(line, "opt")) { bt = BlockType::Opt; kw = 3; }
            else if (starts_word(line, "loop")) { bt = BlockType::Loop; kw = 4; }
            else if (starts_word(line, "par")) { bt = BlockType::Par; kw = 3; }
            else if (starts_word(line, "critical")) { bt = BlockType::Critical; kw = 8; }
            else if (starts_word(line, "break")) { bt = BlockType::Break; kw = 5; }
            else is_block = false;
            if (is_block) {
                Step s;
                s.kind = StepKind::BlockStart;
                s.block = bt;
                s.text = cworks::trim(line.substr(kw));
                model.steps.push_back(std::move(s));
                ++open_blocks;
                continue;
            }
        }
        if (starts_word(line, "else") || starts_word(line, "and") ||
            starts_word(line, "option")) {
            if (open_blocks == 0) {
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("sequence", number, "'" + line.substr(0, line.find(' ')) +
                                                         "' outside a control block (ignored)"));
                continue;
            }
            Step s;
            s.kind = StepKind::BlockDivider;
            const std::size_t sp = line.find(' ');
            s.text = sp == std::string::npos ? "" : cworks::trim(line.substr(sp + 1));
            model.steps.push_back(std::move(s));
            continue;
        }
        if (line == "end") {
            // An `end` first closes an open participant box (boxes contain
            // only declarations, never nested control blocks).
            if (open_box >= 0) {
                open_box = -1;
                continue;
            }
            if (open_blocks == 0) {
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("sequence", number, "'end' without a matching block (ignored)"));
                continue;
            }
            Step s;
            s.kind = StepKind::BlockEnd;
            model.steps.push_back(std::move(s));
            --open_blocks;
            continue;
        }
        // C8: explicit activation.
        if (starts_word(line, "activate") || starts_word(line, "deactivate")) {
            const bool act = starts_word(line, "activate");
            const std::string who = cworks::trim(line.substr(act ? 8 : 10));
            if (who.empty()) {
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("sequence", number, "activate/deactivate needs a participant (ignored)"));
                continue;
            }
            Step s;
            s.kind = act ? StepKind::Activate : StepKind::Deactivate;
            s.actor = participant_index(model, who);
            model.steps.push_back(std::move(s));
            continue;
        }
        // Message numbering: `autonumber [start [step]]` / `autonumber off`.
        if (starts_word(line, "autonumber")) {
            const std::string rest = cworks::trim(line.substr(10));
            if (rest == "off") {
                autonumber = false;
            } else {
                autonumber = true;
                auto_next = 1;
                auto_step = 1;
                std::int64_t value = 0;
                const std::size_t sp = rest.find(' ');
                const std::string first = sp == std::string::npos ? rest : rest.substr(0, sp);
                const std::string second =
                    sp == std::string::npos ? "" : cworks::trim(rest.substr(sp + 1));
                if (!first.empty()) {
                    if (cworks::parse_int(first, value) && value > 0)
                        auto_next = value;
                    else
                        diagnose(options, cworks::Diagnostic::Severity::Warning,
                                 at_line("sequence", number,
                                         "autonumber start '" + first +
                                             "' is not a positive number (using 1)"));
                }
                if (!second.empty()) {
                    if (cworks::parse_int(second, value) && value > 0)
                        auto_step = value;
                    else
                        diagnose(options, cworks::Diagnostic::Severity::Warning,
                                 at_line("sequence", number,
                                         "autonumber step '" + second +
                                             "' is not a positive number (using 1)"));
                }
            }
            continue;
        }
        // `rect <color>` opens a tinted background band; its `end` closes
        // the band like any control block.
        if (starts_word(line, "rect")) {
            Step s;
            s.kind = StepKind::BlockStart;
            s.block = BlockType::Band;
            const std::string color_text = cworks::trim(line.substr(4));
            s.color = color_text == "transparent" ? cplot::colors::transparent
                                                  : cplot::parse_css_color(color_text);
            if (!s.color && !color_text.empty() && color_text != "transparent")
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line("sequence", number,
                                 "unrecognized rect colour '" + color_text + "' (band drawn untinted)"));
            model.steps.push_back(std::move(s));
            ++open_blocks;
            continue;
        }
        // `box <color?> <label?>` groups the participants declared inside.
        if (starts_word(line, "box")) {
            PBox box;
            std::string rest = cworks::trim(line.substr(3));
            if (rest.rfind("rgb", 0) == 0) {
                const std::size_t close = rest.find(')');
                if (close != std::string::npos) {
                    box.fill = cplot::parse_css_color(rest.substr(0, close + 1));
                    if (box.fill) rest = cworks::trim(rest.substr(close + 1));
                }
            } else {
                const std::size_t sp = rest.find(' ');
                const std::string first = sp == std::string::npos ? rest : rest.substr(0, sp);
                if (first == "transparent") {
                    rest = sp == std::string::npos ? "" : cworks::trim(rest.substr(sp + 1));
                } else if (const std::optional<Color> c = cplot::parse_css_color(first)) {
                    box.fill = c;
                    rest = sp == std::string::npos ? "" : cworks::trim(rest.substr(sp + 1));
                }
            }
            box.label = rest;
            open_box = static_cast<int>(model.boxes.size());
            model.boxes.push_back(std::move(box));
            continue;
        }
        // Actor menus attach a hyperlink to a participant:
        //   link <actor>: <label> @ <url>   /   links <actor>: {"label": "url"}
        // The suite renders one link per element (the first), not a popup menu.
        if (starts_word(line, "link") || starts_word(line, "links")) {
            std::string ref;
            LinkDirective link;
            if (parse_sequence_link_statement(line, ref, link) && !link.href.empty()) {
                const std::size_t idx = participant_index(model, ref);
                if (!model.links.emplace(model.participants[idx].id, std::move(link)).second)
                    diagnose(options, cworks::Diagnostic::Severity::Warning,
                             at_line("sequence", number,
                                     "participant '" + ref + "' already has a link (kept the first)"));
            } else {
                diagnose_unsupported(options, "sequence", number,
                                     line.substr(0, line.find_first_of(" \t")),
                                     "the actor menu could not be parsed as a link");
            }
            continue;
        }
        // Participant lifecycle: recognised Mermaid the engine does not render.
        // `create participant D` still declares D (later messages auto-create
        // participants anyway).
        if (starts_word(line, "create") || starts_word(line, "destroy") ||
            starts_word(line, "properties") || starts_word(line, "details")) {
            diagnose_unsupported(options, "sequence", number,
                                 line.substr(0, line.find_first_of(" \t")),
                                 "participant lifecycle is not rendered");
            continue;
        }
        if (starts_word_ci(line, "note")) {
            parse_note(model, line, number);
            continue;
        }
        std::size_t pos = 0;
        const ArrowSpec* spec = nullptr;
        if (find_arrow(line, pos, spec)) {
            parse_message(model, line, pos, *spec);
            if (autonumber && !model.steps.empty() &&
                model.steps.back().kind == StepKind::Message) {
                Step& message = model.steps.back();
                const std::string prefix = cworks::format_int(auto_next);
                message.text =
                    message.text.empty() ? prefix + "." : prefix + ". " + message.text;
                auto_next += auto_step;
            }
            continue;
        }
        throw Error(cworks::validation_failed("sequence (line " + std::to_string(number) +
                    "): expected a message ('A->>B: text'), participant, note, or control block"));
    }
    if (open_blocks != 0)
        diagnose(options, cworks::Diagnostic::Severity::Warning,
                 at_line("sequence", lines.back().number,
                         "unterminated control block (missing 'end')"));
    if (model.participants.empty())
        throw Error(cworks::validation_failed("sequence: no participants"));
    return model;
}

} // namespace

cplot::Scene build_sequence(std::string_view source, const RenderOptions& options) {
    const cplot::Theme& theme = options.theme;
    const DiagramStyle sty = style_for(options);
    const SeqModel model = parse_sequence(source, options);
    const std::size_t n = model.participants.size();

    const Font label_font = theme.base_font();
    const Font title_font = theme.title_font();
    const double line_h = label_font.size * 1.35;

    const double box_h = 34.0;
    const double box_pad_x = 14.0;
    const double min_box_w = 64.0;
    const double margin = 20.0;
    const double min_gap = 60.0;    // clear space between adjacent lifelines
    const double msg_gap = 46.0;    // vertical step for a message
    const double self_w = 44.0;     // self-message loop width
    const double note_pad_x = 10.0;
    const double note_pad_y = 7.0;
    const double title_h = model.title.empty() ? 0.0 : title_font.size * 1.7;

    // Box widths from labels.
    std::vector<double> box_w(n);
    for (std::size_t i = 0; i < n; ++i)
        box_w[i] = std::max(min_box_w, text_width(model.participants[i].label, label_font) +
                                           2.0 * box_pad_x);

    // Adjacent-centre gaps: enough to separate boxes, widened to fit the
    // widest message label spanning each pair.
    std::vector<double> gap(n > 0 ? n - 1 : 0, 0.0);
    for (std::size_t i = 0; i + 1 < n; ++i)
        gap[i] = box_w[i] / 2.0 + box_w[i + 1] / 2.0 + min_gap;
    for (const Step& s : model.steps) {
        if (s.kind != StepKind::Message || s.from == s.to) continue;
        const std::size_t lo = std::min(s.from, s.to);
        const std::size_t hi = std::max(s.from, s.to);
        const double need = text_width(s.text, label_font) + 24.0;
        double span = 0.0;
        for (std::size_t k = lo; k < hi; ++k) span += gap[k];
        if (need > span) {
            const double add = (need - span) / static_cast<double>(hi - lo);
            for (std::size_t k = lo; k < hi; ++k) gap[k] += add;
        }
    }

    // Pre-shift centres.
    std::vector<double> center(n, 0.0);
    center[0] = 0.0;
    for (std::size_t i = 1; i < n; ++i) center[i] = center[i - 1] + gap[i - 1];

    // Horizontal bounds, including notes and self-loops.
    double left = center.front() - box_w.front() / 2.0;
    double right = center.back() + box_w.back() / 2.0;
    const auto note_span = [&](const Step& s, double& x0, double& x1) {
        const double tw = text_width(s.text, label_font) + 2.0 * note_pad_x;
        const std::size_t a = std::min(s.note_a, s.note_b);
        const std::size_t b = std::max(s.note_a, s.note_b);
        if (s.note_pos == NotePos::Over) {
            const double c = (center[a] + center[b]) / 2.0;
            const double w = std::max(tw, center[b] - center[a] + 48.0);
            x0 = c - w / 2.0;
            x1 = c + w / 2.0;
        } else if (s.note_pos == NotePos::LeftOf) {
            x1 = center[a] - 8.0;
            x0 = x1 - tw;
        } else {
            x0 = center[a] + 8.0;
            x1 = x0 + tw;
        }
    };
    for (const Step& s : model.steps) {
        if (s.kind == StepKind::Note) {
            double x0 = 0.0, x1 = 0.0;
            note_span(s, x0, x1);
            left = std::min(left, x0);
            right = std::max(right, x1);
        } else if (s.kind == StepKind::Message && s.from == s.to) {
            right = std::max(right, center[s.from] + self_w +
                                        text_width(s.text, label_font) + 16.0);
        }
    }

    const double offset_x = margin - left;
    const auto X = [&](std::size_t i) { return center[i] + offset_x; };

    // C8: control-block frames and activation bars, computed alongside the
    // per-step y assignment. Frames stack in source order; a frame's span is
    // the min/max lifeline any of its (nested) steps touches.
    struct Frame {
        BlockType type;
        std::string label;
        std::optional<Color> fill;  ///< Band tint (`rect <color>`)
        double top = 0.0, bottom = 0.0;
        std::size_t lo = 0, hi = 0;
        int depth = 0;
        std::vector<std::pair<double, std::string>> dividers;
    };
    struct OpenBlock {
        BlockType type;
        std::string label;
        std::optional<Color> fill;
        double top = 0.0;
        std::size_t lo = 0, hi = 0;
        bool has_span = false;
        std::vector<std::pair<double, std::string>> dividers;
    };
    struct Bar {
        std::size_t actor;
        double top, bottom;
    };
    std::vector<Frame> frames;
    std::vector<OpenBlock> open;
    std::vector<Bar> bars;
    std::vector<std::vector<double>> act(n);  // per-participant open activation tops
    const double block_label_h = line_h + 10.0;
    const double block_pad = 10.0;
    const auto touch = [&](std::size_t idx) {
        for (OpenBlock& ob : open) {
            if (!ob.has_span) {
                ob.lo = ob.hi = idx;
                ob.has_span = true;
            } else {
                ob.lo = std::min(ob.lo, idx);
                ob.hi = std::max(ob.hi, idx);
            }
        }
    };

    // Participant groups (`box … end`) sit behind the header boxes and,
    // when labelled, add a label row above them.
    bool any_box_label = false;
    for (const PBox& b : model.boxes)
        if (b.has_members && !b.label.empty()) any_box_label = true;
    bool any_box = false;
    for (const PBox& b : model.boxes)
        if (b.has_members) any_box = true;
    const double box_extra = (any_box ? 6.0 : 0.0) + (any_box_label ? line_h + 4.0 : 0.0);

    // Vertical layout: assign a y to each step, then the diagram bottom.
    const double first_y = margin + title_h + box_extra + box_h + 28.0;
    std::vector<double> step_y(model.steps.size(), 0.0);
    double y = first_y;
    for (std::size_t i = 0; i < model.steps.size(); ++i) {
        const Step& s = model.steps[i];
        step_y[i] = y;
        switch (s.kind) {
        case StepKind::BlockStart:
            // Reserve the tab/label band plus one label line of headroom:
            // the first message inside the block draws its label above its
            // own line, and must clear the keyword/condition band. A Band
            // (`rect`) has no keyword row — a small pad suffices.
            open.push_back({s.block, s.text, s.color, y, 0, 0, false, {}});
            y += s.block == BlockType::Band ? 6.0 : block_label_h + line_h;
            break;
        case StepKind::BlockDivider:
            if (!open.empty()) {
                open.back().dividers.push_back({y, s.text});
                y += block_label_h + line_h;
            }
            break;
        case StepKind::BlockEnd:
            if (!open.empty()) {
                OpenBlock ob = open.back();
                open.pop_back();
                y += block_pad;
                Frame f;
                f.type = ob.type;
                f.label = ob.label;
                f.fill = ob.fill;
                f.top = ob.top;
                f.bottom = y;
                f.lo = ob.has_span ? ob.lo : 0;
                f.hi = ob.has_span ? ob.hi : (n ? n - 1 : 0);
                f.depth = static_cast<int>(open.size());
                f.dividers = std::move(ob.dividers);
                frames.push_back(std::move(f));
                // The next message draws its label above its own line — it
                // needs a label line of headroom to clear the frame edge.
                y += block_pad + line_h;
            }
            break;
        case StepKind::Activate:
            act[s.actor].push_back(y);
            break;
        case StepKind::Deactivate:
            if (!act[s.actor].empty()) {
                bars.push_back({s.actor, act[s.actor].back(), y});
                act[s.actor].pop_back();
            }
            break;
        case StepKind::Note: {
            touch(s.note_a);
            touch(s.note_b);
            const std::size_t lines_n = label_lines(s.text).size();
            y += static_cast<double>(lines_n) * line_h + 2.0 * note_pad_y + 18.0;
            break;
        }
        case StepKind::Message:
            touch(s.from);
            touch(s.to);
            if (s.activate_target) act[s.to].push_back(y);
            if (s.deactivate_source && !act[s.from].empty()) {
                bars.push_back({s.from, act[s.from].back(), y});
                act[s.from].pop_back();
            }
            y += s.from == s.to ? msg_gap + 22.0 : msg_gap;
            break;
        }
    }
    const double bottom = y + 6.0;
    // Activations left open run to the diagram bottom.
    for (std::size_t p = 0; p < n; ++p)
        for (const double top : act[p]) bars.push_back({p, top, bottom - 6.0});
    // Draw outer frames first so nested frames layer on top.
    std::stable_sort(frames.begin(), frames.end(),
                     [](const Frame& a, const Frame& b) { return a.depth < b.depth; });

    Canvas canvas;
    canvas.background = sty.page;
    canvas.title = model.title;
    canvas.width = right - left + 2.0 * margin;
    canvas.height = bottom + margin;

    if (!model.title.empty())
        canvas.text({canvas.width / 2.0, margin}, model.title, title_font, sty.text,
                    HAlign::Center, VAlign::Top);

    // Lifelines (behind everything).
    ShapeStyle lifeline;
    lifeline.stroke = sty.grid;
    lifeline.stroke_width = 1.0;
    lifeline.dash = cplot::DashPattern{{3.0, 3.0}};
    const double top_y = margin + title_h + box_extra;
    for (std::size_t i = 0; i < n; ++i)
        canvas.line({X(i), top_y + box_h}, {X(i), bottom}, lifeline);

    // Participant group bands, behind the header boxes.
    for (const PBox& b : model.boxes) {
        if (!b.has_members) continue;
        const double gx0 = X(b.lo) - box_w[b.lo] / 2.0 - 10.0;
        const double gx1 = X(b.hi) + box_w[b.hi] / 2.0 + 10.0;
        const double gy0 = margin + title_h;
        const double gy1 = top_y + box_h + 6.0;
        if (b.fill) {
            ShapeStyle group;
            group.fill = b.fill->with_alpha(b.fill->a * 0.45);
            group.stroke = *b.fill;
            group.stroke_width = 1.0;
            canvas.rounded_rect(RectF{gx0, gy0, gx1 - gx0, gy1 - gy0}, 6.0, group);
        } else {
            ShapeStyle group;
            group.stroke = sty.grid;
            group.stroke_width = 1.0;
            canvas.rounded_rect(RectF{gx0, gy0, gx1 - gx0, gy1 - gy0}, 6.0, group);
        }
        if (!b.label.empty())
            canvas.text({(gx0 + gx1) / 2.0, gy0 + 2.0}, b.label, label_font, sty.text,
                        HAlign::Center, VAlign::Top);
    }

    // Participant boxes.
    ShapeStyle box_style;
    box_style.fill = sty.node_fill;
    box_style.stroke = sty.node_stroke;
    box_style.stroke_width = 1.0;
    for (std::size_t i = 0; i < n; ++i) {
        const RectF head{X(i) - box_w[i] / 2.0, top_y, box_w[i], box_h};
        canvas.rounded_rect(head, 4.0, box_style);
        // Say where this participant was drawn, so it can be selected. A
        // participant is addressed by its own id rather than an ordinal,
        // because it has one and renaming it should follow the object rather
        // than its position.
        if (options.regions != nullptr) {
            options.regions->push_back(
                {model.participants[i].id, 0 /* HitRole::Node */, head});
        }
        if (const auto it = model.links.find(model.participants[i].id); it != model.links.end())
            canvas.add_link(head, it->second.href, it->second.title, it->second.target);
        canvas.text({X(i), top_y + box_h / 2.0}, model.participants[i].label, label_font,
                    sty.text, HAlign::Center, VAlign::Middle);
    }

    // Background bands (`rect <color>`) go behind everything else.
    const double band_inset = 28.0;
    for (const Frame& f : frames) {
        if (f.type != BlockType::Band) continue;
        ShapeStyle band;
        band.fill = f.fill.value_or(sty.grid.with_alpha(0.12));
        canvas.rect(RectF{X(f.lo) - band_inset, f.top, (X(f.hi) + band_inset) - (X(f.lo) - band_inset),
                          f.bottom - f.top},
                    band);
    }

    // C8: activation bars, then control-block frames (both behind messages).
    ShapeStyle bar_style;
    bar_style.fill = sty.node_fill;
    bar_style.stroke = sty.node_stroke;
    bar_style.stroke_width = 1.0;
    const double bar_w = 8.0;
    for (const Bar& b : bars) {
        const double x = X(b.actor);
        canvas.rect(RectF{x - bar_w / 2.0, b.top, bar_w, std::max(6.0, b.bottom - b.top)},
                    bar_style);
    }
    const double frame_inset = 24.0;
    for (const Frame& f : frames) {
        if (f.type == BlockType::Band) continue;  // drawn above, no tab/frame
        const double fx0 = X(f.lo) - frame_inset;
        const double fx1 = X(f.hi) + frame_inset;
        ShapeStyle fs;
        fs.stroke = sty.accent;
        fs.stroke_width = 1.0;
        canvas.rect(RectF{fx0, f.top, fx1 - fx0, f.bottom - f.top}, fs);
        const char* kw = f.type == BlockType::Alt        ? "alt"
                         : f.type == BlockType::Opt       ? "opt"
                         : f.type == BlockType::Loop      ? "loop"
                         : f.type == BlockType::Critical  ? "critical"
                         : f.type == BlockType::Break     ? "break"
                                                          : "par";
        const double tab_w = text_width(kw, label_font) + 14.0;
        ShapeStyle tab_style;
        tab_style.fill = sty.note_fill;
        tab_style.stroke = sty.accent;
        tab_style.stroke_width = 1.0;
        canvas.rect(RectF{fx0, f.top, tab_w, block_label_h}, tab_style);
        canvas.text({fx0 + 7.0, f.top + block_label_h / 2.0}, kw, label_font, sty.text,
                    HAlign::Left, VAlign::Middle);
        if (!f.label.empty())
            canvas.masked_text({fx0 + tab_w + (fx1 - fx0 - tab_w) / 2.0,
                                f.top + block_label_h / 2.0},
                               "[" + f.label + "]", label_font, sty.muted, HAlign::Center,
                               VAlign::Middle, sty.label_mask);
        for (const auto& [dy, dlabel] : f.dividers) {
            ShapeStyle dl;
            dl.stroke = sty.accent;
            dl.stroke_width = 1.0;
            dl.dash = cplot::DashPattern{{4.0, 4.0}};
            canvas.line({fx0, dy}, {fx1, dy}, dl);
            if (!dlabel.empty())
                canvas.masked_text({(fx0 + fx1) / 2.0, dy + block_label_h / 2.0},
                                   "[" + dlabel + "]", label_font, sty.muted, HAlign::Center,
                                   VAlign::Middle, sty.label_mask);
        }
    }

    const Color msg_color = sty.edge;
    ShapeStyle note_style;
    note_style.fill = sty.note_fill;
    note_style.stroke = sty.note_stroke;
    note_style.stroke_width = 1.0;

    const auto report_message_region = [&](std::size_t ordinal,
                                           const std::vector<Point>& path) {
        if (options.regions == nullptr || path.size() < 2) return;
        double min_x = path.front().x, max_x = path.front().x;
        double min_y = path.front().y, max_y = path.front().y;
        for (const Point& point : path) {
            min_x = std::min(min_x, point.x);
            max_x = std::max(max_x, point.x);
            min_y = std::min(min_y, point.y);
            max_y = std::max(max_y, point.y);
        }
        constexpr double kGrip = 5.0;
        DrawnRegion region{"#" + std::to_string(ordinal), 1 /* HitRole::Edge */,
                           RectF{min_x - kGrip, min_y - kGrip,
                                 (max_x - min_x) + 2 * kGrip,
                                 (max_y - min_y) + 2 * kGrip}};
        region.path = path;
        options.regions->push_back(std::move(region));
    };

    std::size_t message_ordinal = 0;
    for (std::size_t i = 0; i < model.steps.size(); ++i) {
        const Step& s = model.steps[i];
        if (s.kind != StepKind::Message && s.kind != StepKind::Note) continue;
        const double sy = step_y[i];
        if (s.kind == StepKind::Note) {
            double x0 = 0.0, x1 = 0.0;
            note_span(s, x0, x1);
            x0 += offset_x;
            x1 += offset_x;
            const std::vector<std::string> lines = label_lines(s.text);
            const double h = static_cast<double>(lines.size()) * line_h + 2.0 * note_pad_y;
            canvas.rect(RectF{x0, sy, x1 - x0, h}, note_style);
            canvas.text_block({(x0 + x1) / 2.0, sy + note_pad_y}, lines, label_font,
                              sty.text, HAlign::Center, line_h);
            continue;
        }
        const std::size_t ordinal = message_ordinal++;

        ShapeStyle line_style;
        line_style.stroke = msg_color;
        line_style.stroke_width = 1.4;
        if (s.dashed) line_style.dash = cplot::DashPattern{{5.0, 3.0}};

        if (s.from == s.to) {
            // Self-message: a loop to the right of the lifeline.
            const double x = X(s.from);
            const double loop_h = 20.0;
            const std::vector<Point> path{
                {x, sy}, {x + self_w, sy}, {x + self_w, sy + loop_h}, {x, sy + loop_h}};
            canvas.polyline(path, line_style);
            report_message_region(ordinal, path);
            canvas.arrow_head({x, sy + loop_h}, {x + self_w, sy + loop_h}, 9.0, 8.0, msg_color);
            if (!s.text.empty())
                canvas.text({x + self_w + 6.0, sy + loop_h / 2.0}, s.text, label_font,
                            sty.text, HAlign::Left, VAlign::Middle);
            continue;
        }

        const double fx = X(s.from);
        const double tx = X(s.to);
        const bool has_head = s.arrow == Arrow::SolidHead ||
                              s.arrow == Arrow::DashedHead || s.arrow == Arrow::Open;
        // Stop the stroke at the arrowhead base so the tip stays sharp.
        const Point line_end =
            has_head ? retract_end({{fx, sy}, {tx, sy}}, 9.0).back() : Point{tx, sy};
        canvas.line({fx, sy}, line_end, line_style);
        report_message_region(ordinal, {{fx, sy}, {tx, sy}});
        switch (s.arrow) {
        case Arrow::SolidHead:
        case Arrow::DashedHead:
        case Arrow::Open:
            canvas.arrow_head({tx, sy}, {fx, sy}, 10.0, 8.0, msg_color);
            break;
        case Arrow::Cross: {
            // A small cross at the target end.
            ShapeStyle cross = line_style;
            cross.dash = {};
            const double d = 5.0;
            canvas.line({tx - d, sy - d}, {tx + d, sy + d}, cross);
            canvas.line({tx - d, sy + d}, {tx + d, sy - d}, cross);
            break;
        }
        case Arrow::SolidNoHead:
        case Arrow::DashedNoHead:
            break;
        }
        if (!s.text.empty())
            canvas.masked_text({(fx + tx) / 2.0, sy - 6.0}, s.text, label_font, sty.text,
                               HAlign::Center, VAlign::Bottom, sty.label_mask);
    }

    return canvas.bake();
}

} // namespace cdiagram::detail
