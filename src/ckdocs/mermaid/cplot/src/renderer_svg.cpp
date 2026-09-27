// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// SVG backend. Output is deterministic: fixed number formatting, fixed
// attribute order, no timestamps, no random IDs.
#include "cplot/svg.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#include <cworks/base64.hpp>
#include <cworks/trig.hpp>

#include "cplot/text.hpp"
#include "format_c.hpp"
#include "scene_internal.hpp"

namespace cplot {

namespace detail {

/// Trim the trailing zeros (and then the dot) a fixed-precision format
/// leaves behind, and normalise "-0" — a negative zero is the same number
/// as zero but a different byte sequence, which a byte-exact golden must
/// not have to tolerate.
std::string trim_fixed(std::string s) {
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string svg_num(double v) {
    if (!std::isfinite(v)) return "0";
    // C-locale format: every SVG/PDF coordinate flows through here, and a
    // comma-radix locale would turn "12.50" into "12,50" — which also breaks
    // the comma-separated points="x,y" geometry (see format_c.hpp).
    return trim_fixed(format_c("%.2f", v));
}

std::string matrix_num(double v) {
    if (!std::isfinite(v)) return "0";
    return trim_fixed(format_c("%.6f", v));
}

} // namespace detail

namespace {

using detail::svg_num;

std::string escape_xml(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string alpha_num(double a) {
    std::string s = detail::format_c("%.3f", a);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

/// Stroke and fill presentation attributes. Every property is written
/// only when it differs from SVG's own initial value, so a document
/// that uses the defaults stays as small — and as byte-stable — as the
/// geometry it describes.
void append_style(std::ostringstream& o, const ShapeStyle& s) {
    if (s.fill) {
        if (s.fill->a <= 0.0) {
            o << " fill=\"none\"";
        } else {
            o << " fill=\"" << s.fill->hex() << "\"";
            if (s.fill->a < 1.0) o << " fill-opacity=\"" << alpha_num(s.fill->a) << "\"";
            if (s.fill_rule == FillRule::EvenOdd) o << " fill-rule=\"evenodd\"";
        }
    } else {
        o << " fill=\"none\"";
    }
    if (s.stroke && s.stroke->a > 0.0 && s.stroke_width > 0.0) {
        o << " stroke=\"" << s.stroke->hex() << "\"";
        if (s.stroke->a < 1.0) o << " stroke-opacity=\"" << alpha_num(s.stroke->a) << "\"";
        o << " stroke-width=\"" << svg_num(s.stroke_width) << "\"";
        if (!s.dash.empty()) {
            o << " stroke-dasharray=\"";
            for (std::size_t i = 0; i < s.dash.lengths.size(); ++i) {
                if (i) o << ",";
                o << svg_num(s.dash.lengths[i]);
            }
            o << "\"";
            if (s.dash.phase != 0.0)
                o << " stroke-dashoffset=\"" << svg_num(s.dash.phase) << "\"";
        }
        switch (s.cap) {
        case LineCap::Butt: break; // SVG initial value
        case LineCap::Round: o << " stroke-linecap=\"round\""; break;
        case LineCap::Square: o << " stroke-linecap=\"square\""; break;
        }
        switch (s.join) {
        case LineJoin::Miter: break; // SVG initial value
        case LineJoin::Round: o << " stroke-linejoin=\"round\""; break;
        case LineJoin::Bevel: o << " stroke-linejoin=\"bevel\""; break;
        }
        // SVG's initial stroke-miterlimit is 4, PDF's is 10. ShapeStyle
        // adopts SVG's, so SVG stays silent on the default while the PDF
        // backend always states it — both then clip miters identically.
        if (s.miter_limit != 4.0)
            o << " stroke-miterlimit=\"" << svg_num(s.miter_limit) << "\"";
    }
}

/// What painting a scene carries from group to group: the cancellation it
/// polls, the id namespace and the next clip id, and the fonts the text is
/// set in when the document carries them.
struct RenderContext {
    const cworks::CancelToken* cancellation = nullptr;
    const std::string& id_prefix;
    std::size_t next_clip = 0;
};

/// Whether `text` has a space an SVG viewer would collapse: at either end,
/// or next to another.
bool keeps_spaces(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t'; };
    if (text.empty()) return false;
    if (space(text.front()) || space(text.back())) return true;
    for (std::size_t i = 1; i < text.size(); ++i)
        if (space(text[i]) && space(text[i - 1])) return true;
    return false;
}

void append_font(std::ostringstream& o, const TextItem& text) {
    const Font& f = text.font;
    o << " font-family=\"" << escape_xml(f.family)
      << "\" font-size=\"" << svg_num(f.size) << "\"";
    if (f.weight == FontWeight::Bold) o << " font-weight=\"bold\"";
    if (f.style == FontStyle::Italic) o << " font-style=\"italic\"";
}

void render_item(std::ostringstream& o, const SceneItem& item, const RenderContext& context) {
    (void)context;
    if (const auto* line = std::get_if<LineItem>(&item)) {
        o << "  <line x1=\"" << svg_num(line->a.x) << "\" y1=\"" << svg_num(line->a.y)
          << "\" x2=\"" << svg_num(line->b.x) << "\" y2=\"" << svg_num(line->b.y) << "\"";
        ShapeStyle s = line->style;
        s.fill.reset(); // lines have no fill
        append_style(o, s);
        o << "/>\n";
    } else if (const auto* pl = std::get_if<PolylineItem>(&item)) {
        o << "  <polyline points=\"";
        for (std::size_t i = 0; i < pl->points.size(); ++i) {
            if (i) o << " ";
            o << svg_num(pl->points[i].x) << "," << svg_num(pl->points[i].y);
        }
        o << "\"";
        ShapeStyle s = pl->style;
        if (!s.fill) s.fill = colors::transparent;
        s.fill = colors::transparent;
        append_style(o, s);
        o << "/>\n";
    } else if (const auto* pg = std::get_if<PolygonItem>(&item)) {
        o << "  <polygon points=\"";
        for (std::size_t i = 0; i < pg->points.size(); ++i) {
            if (i) o << " ";
            o << svg_num(pg->points[i].x) << "," << svg_num(pg->points[i].y);
        }
        o << "\"";
        append_style(o, pg->style);
        o << "/>\n";
    } else if (const auto* r = std::get_if<RectItem>(&item)) {
        o << "  <rect x=\"" << svg_num(r->rect.x) << "\" y=\"" << svg_num(r->rect.y)
          << "\" width=\"" << svg_num(r->rect.w) << "\" height=\"" << svg_num(r->rect.h) << "\"";
        append_style(o, r->style);
        o << "/>\n";
    } else if (const auto* c = std::get_if<CircleItem>(&item)) {
        o << "  <circle cx=\"" << svg_num(c->center.x) << "\" cy=\"" << svg_num(c->center.y)
          << "\" r=\"" << svg_num(c->radius) << "\"";
        append_style(o, c->style);
        o << "/>\n";
    } else if (const auto* path = std::get_if<PathItem>(&item)) {
        if (path->subpaths.empty()) return;
        // Absolute commands throughout: `M`/`L`/`C`/`Z`. Relative forms
        // would shave bytes off the file at the cost of making every
        // coordinate depend on the rounding of the one before it, which
        // is the opposite of what a byte-pinned golden wants.
        o << "  <path d=\"";
        bool first = true;
        for (const SubPath& sub : path->subpaths) {
            if (!first) o << " ";
            first = false;
            o << "M " << svg_num(sub.start.x) << " " << svg_num(sub.start.y);
            for (const PathSegment& seg : sub.segments) {
                if (seg.curve) {
                    o << " C " << svg_num(seg.c1.x) << " " << svg_num(seg.c1.y) << " "
                      << svg_num(seg.c2.x) << " " << svg_num(seg.c2.y) << " "
                      << svg_num(seg.to.x) << " " << svg_num(seg.to.y);
                } else {
                    o << " L " << svg_num(seg.to.x) << " " << svg_num(seg.to.y);
                }
            }
            if (sub.closed) o << " Z";
        }
        o << "\"";
        append_style(o, path->style);
        o << "/>\n";
    } else if (const auto* sec = std::get_if<SectorItem>(&item)) {
        // Same convention and the same deterministic trigonometry as
        // scene.cpp's on_circle: these coordinates go straight into the
        // path data of every pie and doughnut, so they must not depend on
        // whose libm is linked.
        const auto point = [&](double radius, double angle_deg) {
            double sine = 0.0;
            double cosine = 0.0;
            cworks::sincos_deg(angle_deg, sine, cosine);
            return std::pair<double, double>{sec->center.x + radius * sine,
                                             sec->center.y - radius * cosine};
        };
        const double span = sec->end_angle - sec->start_angle;
        o << "  <path d=\"";
        if (std::abs(span) >= 360.0 - 1e-9) {
            // Full circle (or ring): two arcs per circle, even-odd hole.
            const auto emit_circle = [&](double r) {
                const auto top = point(r, 0.0);
                const auto bottom = point(r, 180.0);
                o << "M " << svg_num(top.first) << " " << svg_num(top.second) << " A "
                  << svg_num(r) << " " << svg_num(r) << " 0 1 1 "
                  << svg_num(bottom.first) << " " << svg_num(bottom.second) << " A "
                  << svg_num(r) << " " << svg_num(r) << " 0 1 1 "
                  << svg_num(top.first) << " " << svg_num(top.second) << " Z ";
            };
            emit_circle(sec->radius_outer);
            if (sec->radius_inner > 0.0) emit_circle(sec->radius_inner);
            o << "\" fill-rule=\"evenodd\"";
        } else {
            const int large = std::abs(span) > 180.0 ? 1 : 0;
            const int sweep = span >= 0.0 ? 1 : 0;
            const auto outer_start = point(sec->radius_outer, sec->start_angle);
            const auto outer_end = point(sec->radius_outer, sec->end_angle);
            if (sec->radius_inner > 0.0) {
                const auto inner_start = point(sec->radius_inner, sec->end_angle);
                const auto inner_end = point(sec->radius_inner, sec->start_angle);
                o << "M " << svg_num(outer_start.first) << " "
                  << svg_num(outer_start.second) << " A " << svg_num(sec->radius_outer)
                  << " " << svg_num(sec->radius_outer) << " 0 " << large << " " << sweep
                  << " " << svg_num(outer_end.first) << " " << svg_num(outer_end.second)
                  << " L " << svg_num(inner_start.first) << " "
                  << svg_num(inner_start.second) << " A " << svg_num(sec->radius_inner)
                  << " " << svg_num(sec->radius_inner) << " 0 " << large << " "
                  << (1 - sweep) << " " << svg_num(inner_end.first) << " "
                  << svg_num(inner_end.second) << " Z\"";
            } else {
                o << "M " << svg_num(sec->center.x) << " " << svg_num(sec->center.y)
                  << " L " << svg_num(outer_start.first) << " "
                  << svg_num(outer_start.second) << " A " << svg_num(sec->radius_outer)
                  << " " << svg_num(sec->radius_outer) << " 0 " << large << " " << sweep
                  << " " << svg_num(outer_end.first) << " " << svg_num(outer_end.second)
                  << " Z\"";
            }
        }
        append_style(o, sec->style);
        o << "/>\n";
    } else if (const auto* t = std::get_if<TextItem>(&item)) {
        // Vertical alignment is resolved via measured metrics so output
        // does not depend on renderer-specific baseline attributes.
        const TextMetrics m = default_text_measurer().measure(t->text, t->font);
        double y = t->pos.y;
        switch (t->valign) {
        case VAlign::Baseline: break;
        case VAlign::Top: y += m.ascent; break;
        case VAlign::Middle: y += m.ascent - m.height / 2.0; break;
        case VAlign::Bottom: y -= m.descent; break;
        }
        o << "  <text x=\"" << svg_num(t->pos.x) << "\" y=\"" << svg_num(y) << "\"";
        append_font(o, *t);
        o << " fill=\"" << t->color.hex() << "\"";
        if (t->color.a < 1.0) o << " fill-opacity=\"" << alpha_num(t->color.a) << "\"";
        if (t->halign == HAlign::Center) o << " text-anchor=\"middle\"";
        else if (t->halign == HAlign::Right) o << " text-anchor=\"end\"";
        if (t->rotation != 0.0) {
            o << " transform=\"rotate(" << svg_num(t->rotation) << " " << svg_num(t->pos.x)
              << " " << svg_num(t->pos.y) << ")\"";
        }
        // The text's spaces are part of where its glyphs stand — a run that
        // begins after a change of style begins with the space before it —
        // and a viewer collapses a leading, trailing or repeated one unless
        // told to keep it.
        if (keeps_spaces(t->text)) o << " xml:space=\"preserve\"";
        o << ">" << escape_xml(t->text) << "</text>\n";
    } else if (std::holds_alternative<ImageItem>(item)) {
        throw Error(cworks::validation_failed("bitmap images are not supported in ckdocs diagrams"));
    }
}

} // namespace

namespace {

bool group_has_image(const Group& group) {
    if (group.svg) return true;
    for (const Node& child : group.children) {
        if (child.is_group()) {
            if (group_has_image(child.group())) return true;
        } else if (std::holds_alternative<ImageItem>(child.item())) {
            return true;
        }
    }
    return false;
}

/// Both an anchor and an embedded picture reference `xlink:href`, and the
/// namespace declaration is emitted only when one of them is present — so a
/// scene that has neither stays byte-identical to the historical output.
bool scene_needs_image_xlink(const Scene& scene) { return group_has_image(scene.root); }

/// Every clip in the tree, in the order the walk will meet them — which is
/// how their generated ids are assigned, so this pre-pass and
/// render_group() must agree: a group's own clip is numbered before its
/// children's.
///
/// The pass exists because `<clipPath>` elements live in `<defs>`, which
/// SVG puts before the geometry that references them.
void collect_group_clips(const Group& group, std::vector<const RectF*>& out) {
    if (group.clip) out.push_back(&*group.clip);
    if (group.svg) return; // its children are not written
    for (const Node& child : group.children)
        if (child.is_group()) collect_group_clips(child.group(), out);
}

/// Emit one `<a>` wrapping a transparent, fully hit-testable rectangle. A
/// `fill="none"` rect with `pointer-events="all"` is clickable across its whole
/// interior without altering the drawing. A tooltip becomes a nested `<title>`.
void render_link(std::ostringstream& o, const RectF& box, const std::string& href,
                 const std::string& title, const std::string& target) {
    o << "  <a xlink:href=\"" << escape_xml(href) << "\"";
    if (!target.empty()) o << " target=\"" << escape_xml(target) << "\"";
    o << "><rect x=\"" << svg_num(box.x) << "\" y=\"" << svg_num(box.y) << "\" width=\""
      << svg_num(box.w) << "\" height=\"" << svg_num(box.h)
      << "\" fill=\"none\" pointer-events=\"all\"";
    if (title.empty()) {
        o << "/></a>\n";
    } else {
        o << "><title>" << escape_xml(title) << "</title></rect></a>\n";
    }
}

/// Paint one group and everything under it.
///
/// A group that neither transforms nor clips emits **no element of its
/// own** — not an empty `<g>`, not `transform="matrix(1 0 0 1 0 0)"`. That
/// is a correctness contract rather than a size optimisation: grouping is
/// how a producer organises its own code, and a document must not gain
/// markup because of it. Leaf items are written at a fixed indent
/// regardless of depth, so nesting never moves a geometry byte either.
void render_group(std::ostringstream& o, const Group& group, const Transform& parent_ctm,
                  RenderContext& context) {
    const Transform ctm = group.transform.then(parent_ctm);
    const bool moved = !group.transform.is_identity();
    const bool clipped = group.clip.has_value();
    if (moved || clipped) {
        o << "  <g";
        if (moved) {
            o << " transform=\"matrix(" << detail::matrix_num(group.transform.a) << " "
              << detail::matrix_num(group.transform.b) << " "
              << detail::matrix_num(group.transform.c) << " "
              << detail::matrix_num(group.transform.d) << " " << svg_num(group.transform.e)
              << " " << svg_num(group.transform.f) << ")\"";
        }
        // SVG resolves `clip-path` in the user space the element's own
        // `transform` establishes, which is exactly the local space
        // Group::clip is defined in — so the rectangle goes into `<defs>`
        // verbatim, with no mapping.
        if (clipped)
            o << " clip-path=\"url(#" << context.id_prefix << "clip-" << context.next_clip++
              << ")\"";
        o << ">\n";
    }
    if (group.svg) {
        // The SVG document itself, stretched to its rectangle as a
        // picture: its own children are the stand-in other formats draw.
        RectF box = group.svg->dest;
        if (box.w < 0.0) {
            box.x += box.w;
            box.w = -box.w;
        }
        if (box.h < 0.0) {
            box.y += box.h;
            box.h = -box.h;
        }
        if (box.w > 0.0 && box.h > 0.0)
            o << "  <image x=\"" << svg_num(box.x) << "\" y=\"" << svg_num(box.y)
              << "\" width=\"" << svg_num(box.w) << "\" height=\"" << svg_num(box.h)
              << "\" preserveAspectRatio=\"none\" xlink:href=\"data:image/svg+xml;base64,"
              << cworks::encode_base64(group.svg->document) << "\"/>\n";
    } else {
        for (const Node& child : group.children) {
            detail::check_cancelled(context.cancellation);
            if (child.is_group()) render_group(o, child.group(), ctm, context);
            else render_item(o, child.item(), context);
        }
    }
    if (moved || clipped) o << "  </g>\n";
}

} // namespace

std::string SvgRenderer::render(const Scene& scene) const {
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    const std::string w_attr =
        scene.svg_width_attr.empty() ? svg_num(scene.width) : scene.svg_width_attr;
    const std::string h_attr =
        scene.svg_height_attr.empty() ? svg_num(scene.height) : scene.svg_height_attr;
    const std::vector<PlacedLink> links = collect_links(scene);
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\"";
    // xlink is emitted only when something references it, so scenes with
    // neither an anchor nor a picture stay byte-identical to the historical
    // output (and their goldens unchanged).
    if (!links.empty() || scene_needs_image_xlink(scene))
        o << " xmlns:xlink=\"http://www.w3.org/1999/xlink\"";
    o << " width=\"" << w_attr << "\" height=\"" << h_attr << "\" viewBox=\"0 0 "
      << svg_num(scene.width) << " " << svg_num(scene.height) << "\">\n";

    if (!scene.meta_title.empty()) o << "  <title>" << escape_xml(scene.meta_title) << "</title>\n";
    if (!scene.meta_description.empty())
        o << "  <desc>" << escape_xml(scene.meta_description) << "</desc>\n";
    if (!scene.meta_generator.empty() || !scene.meta_extra.empty()) {
        o << "  <metadata>";
        if (!scene.meta_generator.empty()) o << escape_xml(scene.meta_generator);
        for (const auto& [key, value] : scene.meta_extra) {
            if (!scene.meta_generator.empty() || &key != &scene.meta_extra.front().first)
                o << "\n";
            o << escape_xml(key) << ": " << escape_xml(value);
        }
        o << "</metadata>\n";
    }

    std::vector<const RectF*> group_clips;
    collect_group_clips(scene.root, group_clips);

    const auto clip_path = [&](const std::string& id, const RectF& r) {
        o << "    <clipPath id=\"" << id << "\"><rect x=\"" << svg_num(r.x) << "\" y=\""
          << svg_num(r.y) << "\" width=\"" << svg_num(r.w) << "\" height=\"" << svg_num(r.h)
          << "\"/></clipPath>\n";
    };
    if (!group_clips.empty()) {
        o << "  <defs>\n";
        for (std::size_t i = 0; i < group_clips.size(); ++i)
            clip_path(options_.id_prefix + "clip-" + std::to_string(i), *group_clips[i]);
        o << "  </defs>\n";
    }

    if (scene.background.a > 0.0) {
        o << "  <rect width=\"" << svg_num(scene.width) << "\" height=\""
          << svg_num(scene.height) << "\" fill=\"" << scene.background.hex() << "\"";
        if (scene.background.a < 1.0)
            o << " fill-opacity=\"" << alpha_num(scene.background.a) << "\"";
        o << "/>\n";
    }

    RenderContext context{options_.cancellation, options_.id_prefix, 0};
    render_group(o, scene.root, Transform{}, context);

    // Anchors are painted last, above all geometry, and in collect_links()
    // order — least specific first. A viewer hands an overlapped click to
    // the topmost anchor, which is the one painted last, so painting the
    // list forward is what makes the most specific anchor win here.
    if (!links.empty()) {
        o << "  <g class=\"cplot-links\">\n";
        for (const PlacedLink& link : links)
            render_link(o, link.box, link.href, link.title, link.target);
        o << "  </g>\n";
    }

    o << "</svg>\n";
    return o.str();
}

} // namespace cplot
