// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "cdiagram/render.hpp"

#include <string>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/text.hpp>

#include "builders.hpp"
#include "frontmatter.hpp"
#include "source.hpp"
#include "style.hpp"

namespace cdiagram {

namespace {

std::string leading_token(std::string_view source) {
    const std::vector<detail::SourceLine> lines = detail::significant_lines(source);
    if (lines.empty()) return "";
    const std::string& line = lines.front().text;
    std::size_t i = 0;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    return line.substr(0, i);
}

[[noreturn]] void throw_unknown(std::string_view source) {
    const std::string token = leading_token(source);
    // The two halves of this helper are deliberately different codes: with no
    // token there is no name to look up and nothing to offer, but with one
    // there is a catalogue to choose from. A helper that could not tell them
    // apart would have to guess, so this one splits rather than flatten.
    if (token.empty())
        throw Error(
            cworks::validation_failed("empty diagram: expected a diagram type on the first line "
                                      "(e.g. 'flowchart TD', 'sequenceDiagram', 'pie')"));
    // Suggest from the live catalogue so every real type is a candidate
    // (the list can never drift out of sync with the detector).
    std::vector<std::string> known;
    known.reserve(catalogue().size());
    for (const CatalogEntry& e : catalogue()) known.push_back(e.keyword);
    std::string message = "unknown diagram type '" + token + "'";
    const std::string suggestion = cworks::closest_match(token, known);
    if (!suggestion.empty()) message += " — did you mean '" + suggestion + "'?";
    // ObjectNotFound: catalogue() is public and lists every type, so cwrite's
    // grouped Insert dialog is the answer here — not a retype prompt.
    cworks::AppError e = cworks::object_not_found("diagram type", token);
    e.summary = std::move(message);
    throw Error(std::move(e));
}

cplot::Scene dispatch(std::string_view source, const RenderOptions& options) {
    switch (detect_type(source)) {
    case DiagramType::Pie:
        return detail::build_pie(source, options);
    case DiagramType::Sequence:
        return detail::build_sequence(source, options);
    case DiagramType::Flowchart:
        return detail::build_flowchart(source, options);
    case DiagramType::State:
        return detail::build_state(source, options);
    case DiagramType::Er:
        return detail::build_er(source, options);
    case DiagramType::Class:
        return detail::build_class(source, options);
    case DiagramType::Quadrant:
        return detail::build_quadrant(source, options);
    case DiagramType::Mindmap:
        return detail::build_mindmap(source, options);
    case DiagramType::Timeline:
        return detail::build_timeline(source, options);
    case DiagramType::Journey:
        return detail::build_journey(source, options);
    case DiagramType::Gantt:
        return detail::build_gantt(source, options);
    case DiagramType::Git:
        return detail::build_git(source, options);
    case DiagramType::XyChart:
        return detail::build_xychart(source, options);
    case DiagramType::Kanban:
        return detail::build_kanban(source, options);
    case DiagramType::Packet:
        return detail::build_packet(source, options);
    case DiagramType::Requirement:
        return detail::build_requirement(source, options);
    case DiagramType::Radar:
        return detail::build_radar(source, options);
    case DiagramType::Treemap:
        return detail::build_treemap(source, options);
    case DiagramType::Block:
        return detail::build_block(source, options);
    case DiagramType::C4:
        return detail::build_c4(source, options);
    case DiagramType::Sankey:
        return detail::build_sankey(source, options);
    case DiagramType::Architecture:
        return detail::build_architecture(source, options);
    case DiagramType::ZenUml:
        return detail::build_zenuml(source, options);
    case DiagramType::Unknown:
        throw_unknown(source);
    default:
        // A recognised type the engine does not lay out yet. Name it
        // precisely so the author knows it is a coverage gap, not a
        // syntax error.
        // Unsupported, not ValidationFailed: the source is legal Mermaid that
        // this build does not lay out yet, so nothing the author edits fixes
        // it. The code carries the coverage gap the comment describes.
        throw Error(cworks::unsupported("diagram type '" + type_keyword(detect_type(source)) +
                                        "' is recognised but not yet supported by ckdiagram"));
    }
}

// Mermaid's front-matter `title:` applies to every diagram type. A
// builder that drew its own title (in-body or via a chart title) has
// stamped scene.meta_title; otherwise reserve a band above the content
// — the same geometry the in-body titles use — and draw it there.
void apply_frontmatter_title(std::string_view source, const RenderOptions& options,
                             cplot::Scene& scene) {
    const detail::FrontMatter matter(source, options, "");
    if (matter.title().empty() || !scene.meta_title.empty()) return;
    const cplot::Font title_font = options.theme.title_font();
    const double band = title_font.size * 1.9;
    // The content moves down by the band, the title is drawn in it. The move
    // is one transform on the content's own group — the diagram's coordinates
    // are left exactly as its builder produced them, and the title is a
    // sibling that the move therefore cannot reach. A diagram that drew
    // nothing gets no group at all: an empty one still writes a
    // `<g transform=…>` into the document, and there is nothing in it.
    cplot::Group content = std::move(scene.root);
    scene.root = cplot::Group{};
    if (!content.children.empty() || !content.link_regions.empty()) {
        content.transform = content.transform.then(cplot::Transform::translate(0.0, band));
        scene.root.add(std::move(content));
    }
    scene.height += band;
    cplot::TextItem item;
    item.pos = {scene.width / 2.0, band / 4.0};
    item.text = matter.title();
    item.font = title_font;
    item.color = detail::style_for(options).text;
    item.halign = cplot::HAlign::Center;
    item.valign = cplot::VAlign::Top;
    scene.root.add(std::move(item));
    scene.meta_title = matter.title();
}

// Front-matter `link:` makes the whole diagram a hyperlink (a cworks
// extension). It rides on the scene and every backend renders it beneath any
// per-element link, so the overall link is the click target everywhere except
// on an element that carries its own.
void apply_frontmatter_link(std::string_view source, const RenderOptions& options,
                            cplot::Scene& scene) {
    const detail::FrontMatter matter(source, options, "");
    if (matter.link().empty()) return;
    scene.link = matter.link();
    scene.link_target = matter.link_target();
}

} // namespace

cplot::Scene render(std::string_view source, const RenderOptions& options) {
    cplot::Scene scene = dispatch(source, options);
    apply_frontmatter_title(source, options, scene);
    apply_frontmatter_link(source, options, scene);
    return scene;
}

cworks::Diagnostics check(std::string_view source) {
    cworks::Diagnostics diagnostics;
    RenderOptions options;
    options.diagnostics = &diagnostics; // collect non-fatal (skipped-line) reports
    try {
        (void)render(source, options);
    } catch (const cworks::Error& e) {
        // A fatal parse/layout error becomes the terminal Error diagnostic,
        // after any non-fatal warnings already recorded during rendering.
        diagnostics.push_back({cworks::Diagnostic::Severity::Error, e.what()});
    }
    return diagnostics;
}

} // namespace cdiagram
