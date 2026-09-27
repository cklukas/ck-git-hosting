// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Scene helpers shared by backends. tessellate_sector converts circular
// wedges/rings into polygons deterministically so raster and SIXEL
// output is identical across platforms, and flatten_path does the same
// for the Bézier segments of a PathItem — which only the raster backends
// need, because SVG and PDF emit the curve itself.
#include "cplot/scene.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <cworks/trig.hpp>

#include "cplot/text.hpp"
#include "format_c.hpp"

namespace cplot {

namespace {

Point on_circle(Point center, double radius, double angle_deg) {
    // cworks::sincos_deg, not libm: libm's sine is accurate but not
    // bit-pinned across platforms, and every pie, doughnut, polar, radar
    // and wind-rose chart in the suite has its vertices computed right
    // here. A byte-identical SVG on Linux, macOS and Windows requires the
    // arithmetic to be ours. Degrees also make the quadrant angles exact,
    // so a sector edge at 90° lands on the axis instead of 6e-17 beside it.
    double sine = 0.0;
    double cosine = 0.0;
    cworks::sincos_deg(angle_deg, sine, cosine);
    // 0° = 12 o'clock, positive clockwise (screen coordinates, y down).
    return {center.x + radius * sine, center.y - radius * cosine};
}

} // namespace

DashPattern DashPattern::parse(std::string_view svg_dasharray) {
    DashPattern pattern;
    // Copied so the C-locale strtod below has a NUL-terminated buffer.
    const std::string text(svg_dasharray);
    const char* p = text.c_str();
    const char* const end = p + text.size();
    while (p != end) {
        // Separators: SVG's whitespace-and/or-comma list grammar.
        if (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            ++p;
            continue;
        }
        char* stop = nullptr;
        const double value = detail::strtod_c(p, &stop);
        // Any non-numeric token (`none` included) or negative length makes
        // the whole value invalid; SVG then renders the stroke solid rather
        // than guessing at what was meant.
        if (stop == p || !std::isfinite(value) || value < 0.0) return DashPattern{};
        pattern.lengths.push_back(value);
        p = stop;
    }
    // An all-zero dasharray is "in error" per SVG, i.e. rendered solid.
    double total = 0.0;
    for (double v : pattern.lengths) total += v;
    if (total <= 0.0) return DashPattern{};
    return pattern;
}

// -- Transform -----------------------------------------------------------------

Transform Transform::rotate_deg(double degrees) noexcept {
    // cworks::sincos_deg, never libm: a group's rotation multiplies every
    // coordinate its subtree emits, and those coordinates are written into
    // the SVG, the PDF and the PNG. libm's sine is accurate to within an ulp
    // but is not bit-pinned across platforms, versions or optimisation
    // levels; the suite's own degree trigonometry is bit-identical by
    // construction and exact on the quarter turns.
    double sine = 0.0;
    double cosine = 0.0;
    cworks::sincos_deg(degrees, sine, cosine);
    // SVG's rotate(θ) verbatim: matrix(cos θ, sin θ, −sin θ, cos θ, 0, 0),
    // which turns clockwise on a y-down canvas.
    return Transform{cosine, sine, -sine, cosine, 0.0, 0.0};
}

RectF Transform::apply_bounds(const RectF& r) const noexcept {
    // An axis-aligned transform maps a rectangle to a rectangle, and the
    // extents then scale rather than being rebuilt from mapped corners.
    // That is not an optimisation: `(x + w) - x` is not `w` in binary
    // floating point, so the general path would perturb the width of every
    // rectangle it touched — including the identity's.
    if (is_axis_aligned()) {
        // b == c == 0: the axes scale in place. a == d == 0: they swap.
        const double x0 = a * r.x + c * r.y + e;
        const double y0 = b * r.x + d * r.y + f;
        const double w = a * r.w + c * r.h;
        const double h = b * r.w + d * r.h;
        RectF out{x0, y0, w, h};
        if (out.w < 0.0) {
            out.x += out.w;
            out.w = -out.w;
        }
        if (out.h < 0.0) {
            out.y += out.h;
            out.h = -out.h;
        }
        return out;
    }
    const Point p0 = apply({r.x, r.y});
    const Point p1 = apply({r.x + r.w, r.y});
    const Point p2 = apply({r.x + r.w, r.y + r.h});
    const Point p3 = apply({r.x, r.y + r.h});
    const double min_x = std::min(std::min(p0.x, p1.x), std::min(p2.x, p3.x));
    const double max_x = std::max(std::max(p0.x, p1.x), std::max(p2.x, p3.x));
    const double min_y = std::min(std::min(p0.y, p1.y), std::min(p2.y, p3.y));
    const double max_y = std::max(std::max(p0.y, p1.y), std::max(p2.y, p3.y));
    return RectF{min_x, min_y, max_x - min_x, max_y - min_y};
}

std::optional<Transform> Transform::inverse() const noexcept {
    const double det = a * d - b * c;
    if (!std::isfinite(det) || det == 0.0) return std::nullopt;
    const double ia = d / det;
    const double ib = -b / det;
    const double ic = -c / det;
    const double id = a / det;
    return Transform{ia, ib, ic, id, -(ia * e + ic * f), -(ib * e + id * f)};
}

// -- Node ----------------------------------------------------------------------
//
// Every special member is defined here rather than in the header: the boxed
// alternative is a unique_ptr<Group>, and destroying or copying one needs
// Group to be complete — which it is not until scene.hpp has finished
// defining Node itself.

Node::Node(SceneItem item) : value_(std::move(item)) {}
Node::Node(Group group) : value_(std::make_unique<Group>(std::move(group))) {}

Node::~Node() = default;
Node::Node(Node&& other) noexcept = default;
Node& Node::operator=(Node&& other) noexcept = default;

Node::Node(const Node& other) : value_(SceneItem{}) {
    if (other.is_group()) value_ = std::make_unique<Group>(other.group());
    else value_ = other.item();
}

Node& Node::operator=(const Node& other) {
    if (this == &other) return *this;
    if (other.is_group()) value_ = std::make_unique<Group>(other.group());
    else value_ = other.item();
    return *this;
}

std::vector<Point> tessellate_sector(const SectorItem& sector) {
    std::vector<Point> pts;
    const double span = sector.end_angle - sector.start_angle;
    if (span == 0.0 || sector.radius_outer <= 0.0) return pts;

    // Deterministic segment count: one segment per ~3 degrees, at least 2.
    const int segments = std::clamp(static_cast<int>(std::ceil(std::abs(span) / 3.0)), 2, 256);
    pts.reserve(static_cast<std::size_t>(segments) * 2 + 4);

    for (int i = 0; i <= segments; ++i) {
        const double a = sector.start_angle + span * i / segments;
        pts.push_back(on_circle(sector.center, sector.radius_outer, a));
    }
    if (sector.radius_inner > 0.0) {
        for (int i = segments; i >= 0; --i) {
            const double a = sector.start_angle + span * i / segments;
            pts.push_back(on_circle(sector.center, sector.radius_inner, a));
        }
    } else {
        pts.push_back(sector.center);
    }
    return pts;
}

PathItem ellipse_path(const RectF& box, ShapeStyle style) {
    const double rx = box.w / 2.0;
    const double ry = box.h / 2.0;
    const double cx = box.x + rx;
    const double cy = box.y + ry;
    const double kx = kBezierKappa * rx;
    const double ky = kBezierKappa * ry;
    PathItem path;
    path.style = std::move(style);
    // Clockwise on screen from the right-hand extreme, so the winding
    // matches every other closed primitive this engine emits.
    path.move_to({cx + rx, cy})
        .curve_to({cx + rx, cy + ky}, {cx + kx, cy + ry}, {cx, cy + ry})
        .curve_to({cx - kx, cy + ry}, {cx - rx, cy + ky}, {cx - rx, cy})
        .curve_to({cx - rx, cy - ky}, {cx - kx, cy - ry}, {cx, cy - ry})
        .curve_to({cx + kx, cy - ry}, {cx + rx, cy - ky}, {cx + rx, cy})
        .close();
    return path;
}

PathItem rounded_rect_path(const RectF& box, double radius, ShapeStyle style) {
    const double r = std::min(radius, std::min(box.w, box.h) / 2.0);
    const double x0 = box.x, y0 = box.y;
    const double x1 = box.x + box.w, y1 = box.y + box.h;
    PathItem path;
    path.style = std::move(style);
    if (!(r > 0.0)) {
        path.move_to({x0, y0}).line_to({x1, y0}).line_to({x1, y1}).line_to({x0, y1}).close();
        return path;
    }
    const double k = kBezierKappa * r;
    // Clockwise on screen from the top edge, each corner one cubic.
    path.move_to({x0 + r, y0})
        .line_to({x1 - r, y0})
        .curve_to({x1 - r + k, y0}, {x1, y0 + r - k}, {x1, y0 + r})
        .line_to({x1, y1 - r})
        .curve_to({x1, y1 - r + k}, {x1 - r + k, y1}, {x1 - r, y1})
        .line_to({x0 + r, y1})
        .curve_to({x0 + r - k, y1}, {x0, y1 - r + k}, {x0, y1 - r})
        .line_to({x0, y0 + r})
        .curve_to({x0, y0 + r - k}, {x0 + r - k, y0}, {x0 + r, y0})
        .close();
    return path;
}

namespace {

/// Segments a cubic needs so its polyline stays within `kFlatness` of the
/// true curve, in the units the points are already expressed in.
///
/// Uniform subdivision of a curve into n pieces leaves each piece within
/// max|B''|·(1/n)²/8 of its chord, and for a cubic
/// B''(t) = 6·(1−t)·(P0 − 2C1 + C2) + 6·t·(C1 − 2C2 + P1), so
/// max|B''| ≤ 6·max(|P0 − 2C1 + C2|, |C1 − 2C2 + P1|) — the two second
/// differences of the control polygon. Solving 6·m/(8·n²) ≤ tol gives
/// n = ⌈√(3·m / (4·tol))⌉.
///
/// The point of the closed form is determinism, exactly as in
/// circle_segments: it is built from multiplication, division and `sqrt`,
/// every one of them exactly specified by IEEE 754, so the count is the
/// same integer on every platform. A count derived from a transcendental
/// could differ by one somewhere and move every vertex of the curve.
/// `std::hypot` is avoided for the same reason — it is a libm routine
/// with no bit-exact specification — in favour of the plain sqrt of a sum
/// of squares.
int curve_segments(Point p0, Point c1, Point c2, Point p1) {
    constexpr double kFlatness = 0.05; // the sagitta bound circle_segments uses
    const double ax = p0.x - 2.0 * c1.x + c2.x;
    const double ay = p0.y - 2.0 * c1.y + c2.y;
    const double bx = c1.x - 2.0 * c2.x + p1.x;
    const double by = c1.y - 2.0 * c2.y + p1.y;
    const double m2 = std::max(ax * ax + ay * ay, bx * bx + by * by);
    if (!(m2 > 0.0)) return 1; // a straight (or degenerate) cubic
    const double n = std::ceil(std::sqrt(0.75 * std::sqrt(m2) / kFlatness));
    return static_cast<int>(std::clamp(n, 1.0, 1024.0));
}

} // namespace

std::vector<std::vector<Point>> flatten_path(const PathItem& path, double scale) {
    return flatten_path(path, Transform::scale(scale, scale));
}

std::vector<std::vector<Point>> flatten_path(const PathItem& path, const Transform& transform) {
    std::vector<std::vector<Point>> out;
    out.reserve(path.subpaths.size());
    for (const SubPath& sub : path.subpaths) {
        std::vector<Point> pts;
        Point current = transform.apply(sub.start);
        pts.push_back(current);
        for (const PathSegment& seg : sub.segments) {
            const Point to = transform.apply(seg.to);
            if (!seg.curve) {
                pts.push_back(to);
                current = to;
                continue;
            }
            const Point c1 = transform.apply(seg.c1);
            const Point c2 = transform.apply(seg.c2);
            const int n = curve_segments(current, c1, c2, to);
            for (int i = 1; i <= n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(n);
                const double u = 1.0 - t;
                const double b0 = u * u * u;
                const double b1 = 3.0 * u * u * t;
                const double b2 = 3.0 * u * t * t;
                const double b3 = t * t * t;
                pts.push_back({b0 * current.x + b1 * c1.x + b2 * c2.x + b3 * to.x,
                               b0 * current.y + b1 * c1.y + b2 * c2.y + b3 * to.y});
            }
            current = to;
        }
        // A closed subpath repeats its start so the stroker sees a seam to
        // join rather than two ends to cap.
        if (sub.closed && pts.size() >= 2) pts.push_back(pts.front());
        out.push_back(std::move(pts));
    }
    return out;
}

SceneItem text_halo(const TextItem& text, const TextMeasurer& measurer, double padding,
                    ShapeStyle style) {
    const TextMetrics m = measurer.measure(text.text, text.font);
    double left = text.pos.x;
    if (text.halign == HAlign::Center)
        left -= m.width / 2.0;
    else if (text.halign == HAlign::Right)
        left -= m.width;
    double baseline = text.pos.y;
    switch (text.valign) {
        case VAlign::Top: baseline += m.ascent; break;
        case VAlign::Middle: baseline += m.ascent - m.height / 2.0; break;
        case VAlign::Bottom: baseline -= m.descent; break;
        case VAlign::Baseline: break;
    }
    const RectF box{left - padding, baseline - m.ascent - padding,
                    m.width + 2.0 * padding, m.height + 2.0 * padding};
    if (text.rotation == 0.0) return RectItem{box, std::move(style)};

    // Rotated text: rotate the box corners clockwise around the anchor,
    // matching the rotate(angle, x, y) semantics every backend applies
    // to the TextItem itself.
    double s = 0.0;
    double c = 0.0;
    cworks::sincos_deg(text.rotation, s, c);
    const auto corner = [&](double x, double y) {
        const double dx = x - text.pos.x;
        const double dy = y - text.pos.y;
        return Point{text.pos.x + dx * c - dy * s, text.pos.y + dx * s + dy * c};
    };
    return PolygonItem{{corner(box.x, box.y), corner(box.x + box.w, box.y),
                        corner(box.x + box.w, box.y + box.h), corner(box.x, box.y + box.h)},
                       std::move(style)};
}

namespace {

/// Document order, carrying the running CTM — the same order and the same
/// composition every backend performs, so what this reports is what gets
/// drawn rather than a second opinion about it.
void collect_group_items(const Group& group, const Transform& parent_ctm,
                         std::vector<PlacedItem>& out) {
    const Transform ctm = group.transform.then(parent_ctm);
    for (const Node& child : group.children) {
        if (child.is_group()) collect_group_items(child.group(), ctm, out);
        else out.push_back({&child.item(), ctm});
    }
}

/// Pre-order, carrying the running CTM: a group's own regions before its
/// children's, so a nested anchor lands after — and therefore wins over —
/// the one that encloses it.
void collect_group_hits(const Group& group, const Transform& parent_ctm,
                        std::vector<HitRegion>& out) {
    const Transform ctm = group.transform.then(parent_ctm);
    for (const HitRegion& region : group.hit_regions)
        out.push_back({ctm.apply_bounds(region.bbox), region.id});
    for (const Node& child : group.children)
        if (child.is_group()) collect_group_hits(child.group(), ctm, out);
}

void collect_group_links(const Group& group, const Transform& parent_ctm,
                         std::vector<PlacedLink>& out) {
    const Transform ctm = group.transform.then(parent_ctm);
    for (const LinkRegion& region : group.link_regions)
        out.push_back({ctm.apply_bounds(region.bbox), region.href, region.title, region.target});
    for (const Node& child : group.children)
        if (child.is_group()) collect_group_links(child.group(), ctm, out);
}

} // namespace

std::vector<PlacedItem> collect_items(const Group& group) {
    std::vector<PlacedItem> items;
    collect_group_items(group, Transform{}, items);
    return items;
}

std::vector<HitRegion> collect_hit_regions(const Scene& scene) {
    std::vector<HitRegion> hits;
    collect_group_hits(scene.root, Transform{}, hits);
    return hits;
}

std::vector<PlacedLink> collect_links(const Scene& scene) {
    std::vector<PlacedLink> links;
    // The whole canvas is the least specific anchor there is, so it is the
    // first entry rather than a case each backend remembers to special-case
    // at its own end of the list — which is how the two of them came to
    // disagree about everything else.
    if (!scene.link.empty())
        links.push_back(
            {RectF{0.0, 0.0, scene.width, scene.height}, scene.link, "", scene.link_target});
    collect_group_links(scene.root, Transform{}, links);
    return links;
}

} // namespace cplot
