// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Internal: the drawing surface every diagram layout builds on. It
// accumulates cplot scene primitives and bakes them into a single-panel
// cplot::Scene. Coordinates are CSS pixels with the origin at the
// top-left and y growing downward — the screen/Mermaid convention, and
// the one cplot's renderers use.
//
// Curved shapes (rounded rectangles, cylinder ends) are cplot::PathItem
// Béziers, not polygons approximated here: SVG and PDF then carry the
// real curve and stay smooth at any zoom, and only the rasterizer
// flattens — at output resolution, deterministically. Shapes that are
// genuinely straight-edged (arrowheads, diamonds) stay polygons.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cplot/scene.hpp>
#include <cplot/theme.hpp>

namespace cdiagram::detail {

using cplot::Color;
using cplot::Font;
using cplot::HAlign;
using cplot::Point;
using cplot::RectF;
using cplot::ShapeStyle;
using cplot::VAlign;

/// Width of one line of `text` in `font`, in pixels. Deterministic and
/// machine-independent (cplot's compiled-in Helvetica metrics), and the
/// exact width the SVG/PDF backends will render, so a box sized from
/// this fits its label precisely.
double text_width(std::string_view text, const Font& font);

/// A categorical colour for series/slice index `index`: the theme's
/// palette when it has one, otherwise cdiagram's calm built-in set. The
/// index wraps, so any count of items is coloured deterministically.
Color palette_color(const cplot::Theme& theme, std::size_t index);

/// The point where the ray from a box's `center` toward `toward` leaves
/// the box (width `w`, height `h`). Used to land edges on node borders.
Point box_border(Point center, double w, double h, Point toward);

/// Border point for a rhombus (decision diamond) inscribed in the w×h
/// box — where the ray from `center` toward `toward` crosses the rhombus
/// edge |x|/hw + |y|/hh = 1. Edges land on the slanted face, not the
/// bounding box (which would leave a visible gap).
Point diamond_border(Point center, double w, double h, Point toward);

/// Border point for a circle of `radius` about `center` toward `toward`.
Point circle_border(Point center, double radius, Point toward);

/// Border point for a horizontal stadium/pill (width `w` ≥ height `h`,
/// semicircular caps of radius h/2). Falls back to box_border when the
/// shape is not a horizontal pill.
Point stadium_border(Point center, double w, double h, Point toward);

/// Shorten `route` from its end by arc-length `dist`, trimming whole
/// segments as needed. Used to stop a stroked edge at an arrowhead's base
/// so only the triangle forms a sharp tip (the line no longer pokes
/// through). The route's other points are unchanged.
std::vector<Point> retract_end(std::vector<Point> route, double dist);

/// The point half-way along a polyline by arc length (where an edge
/// label sits).
Point polyline_midpoint(const std::vector<Point>& points);

/// Clip a (already-positioned) edge route so its first point lands on
/// box A's border and its last on box B's border. Route points inside the
/// end boxes are dropped first, so the trailing segment always approaches
/// from outside — an arrowhead aimed along it points at the border rather
/// than being buried under the node, and the route's bounding box spans
/// only the visible curve. Routes with fewer than two points are returned
/// unchanged.
std::vector<Point> clip_route(std::vector<Point> route, Point a, double aw, double ah,
                              Point b, double bw, double bh);

/// Collects edge labels and, once all are added, resolves overlaps —
/// shifting colliding labels apart in opposite directions — before
/// drawing them, so crowded diagrams stay readable. Each label is placed
/// beside its edge (perpendicular offset), which also puts a
/// bidirectional pair's two labels on opposite sides.
class LabelLayout {
public:
    /// Queue a label beside `route`. No-op for empty text. `color`
    /// overrides the draw() text colour for this one label (a linkStyle
    /// `color:` directive).
    void add(const std::vector<Point>& route, std::string text, const Font& font,
             std::optional<Color> color = std::nullopt);
    /// Resolve overlaps, then draw every queued label.
    void draw(class Canvas& canvas, Color text_color, Color background);

private:
    struct Entry {
        Point pos;
        double w = 0.0;
        double h = 0.0;
        std::string text;
        Font font;
        std::optional<Color> color;
    };
    std::vector<Entry> entries_;
};

class Canvas {
public:
    double width = 0.0;
    double height = 0.0;
    Color background = cplot::colors::white;
    std::string title;       ///< SVG <title> / accessibility name
    std::string description;  ///< SVG <desc>

    void line(Point a, Point b, ShapeStyle style);
    void polyline(std::vector<Point> points, ShapeStyle style);
    void polygon(std::vector<Point> points, ShapeStyle style);
    void rect(RectF box, ShapeStyle style);
    /// A rounded rectangle: straight edges joined by four cubic-Bézier
    /// corners, emitted as a real path rather than a polygon.
    void rounded_rect(RectF box, double radius, ShapeStyle style);
    /// Add an arbitrary vector path. Used by notation-defined symbols whose
    /// curved outline is neither an ellipse nor a rounded rectangle.
    void path(cplot::PathItem item);
    void circle(Point center, double radius, ShapeStyle style);
    /// The axis-aligned ellipse inscribed in `box`, as four cubic
    /// Béziers (a cylinder's end cap, and anything else oval).
    void ellipse(RectF box, ShapeStyle style);
    /// A wedge/ring segment; angles are degrees clockwise from 12
    /// o'clock (cplot's SectorItem convention).
    void sector(Point center, double radius_inner, double radius_outer,
                double start_deg, double end_deg, ShapeStyle style);

    /// Text over other geometry: paints a measured, semi-transparent
    /// backdrop (the mask colour at 75% opacity) before the glyphs, so
    /// lines and fills behind the label dim instead of colliding with
    /// it — nothing underneath is erased outright.
    void masked_text(Point pos, std::string content, Font font, Color color,
                     HAlign halign, VAlign valign, Color mask);
    void text(Point pos, std::string content, Font font, Color color,
              HAlign halign = HAlign::Left, VAlign valign = VAlign::Baseline,
              double rotation = 0.0);
    /// A stack of lines starting with the first line's TOP at `top`,
    /// each aligned horizontally about `top.x` by `halign`. Returns the
    /// total block height (lines * line_height).
    double text_block(Point top, const std::vector<std::string>& lines, Font font,
                      Color color, HAlign halign, double line_height);

    /// A filled arrowhead: an isosceles triangle whose tip sits at `tip`
    /// and which points from `from` toward `tip`. `length` is tip-to-
    /// base, `spread` the full base width.
    void arrow_head(Point tip, Point from, double length, double spread, Color fill);

    /// Register `box` as a clickable hyperlink region. Diagram builders call
    /// this at an element's draw site, where its rectangle is already known;
    /// `title` is an optional tooltip and `target` an optional link target
    /// (e.g. `_blank`). The region is baked onto the scene panel and rendered
    /// as an SVG `<a>` / PDF `/Link`. Empty `href` is ignored.
    void add_link(RectF box, std::string href, std::string title = {}, std::string target = {});

    cplot::Scene bake() const;

private:
    std::vector<cplot::SceneItem> items_;
    std::vector<cplot::LinkRegion> links_;
};

} // namespace cdiagram::detail
