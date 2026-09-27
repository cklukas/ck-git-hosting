// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "canvas.hpp"

#include <cmath>
#include <utility>

#include <cplot/text.hpp>

namespace cdiagram::detail {

double text_width(std::string_view text, const Font& font) {
    return cplot::default_text_measurer().measure(text, font).width;
}

Color palette_color(const cplot::Theme& theme, std::size_t index) {
    if (!theme.palette.empty()) return theme.palette[index % theme.palette.size()];
    // A calm, print-friendly categorical fallback (used when a theme
    // ships no palette). Fixed, so colouring is deterministic.
    static const Color kFallback[] = {
        Color::rgb(0x4C78A8), Color::rgb(0xF58518), Color::rgb(0x54A24B),
        Color::rgb(0xE45756), Color::rgb(0x72B7B2), Color::rgb(0xEECA3B),
        Color::rgb(0xB279A2), Color::rgb(0xFF9DA6), Color::rgb(0x9D755D),
        Color::rgb(0xBAB0AC),
    };
    constexpr std::size_t n = sizeof(kFallback) / sizeof(kFallback[0]);
    return kFallback[index % n];
}

Point box_border(Point center, double w, double h, Point toward) {
    double dx = toward.x - center.x;
    double dy = toward.y - center.y;
    if (std::abs(dx) < 1e-9 && std::abs(dy) < 1e-9) return center;
    const double hw = w / 2.0, hh = h / 2.0;
    const double tx = std::abs(dx) < 1e-9 ? 1e18 : hw / std::abs(dx);
    const double ty = std::abs(dy) < 1e-9 ? 1e18 : hh / std::abs(dy);
    const double t = std::min(tx, ty);
    return {center.x + dx * t, center.y + dy * t};
}

Point diamond_border(Point center, double w, double h, Point toward) {
    const double dx = toward.x - center.x, dy = toward.y - center.y;
    const double hw = w / 2.0, hh = h / 2.0;
    const double denom = (hw > 1e-9 ? std::abs(dx) / hw : 0.0) +
                         (hh > 1e-9 ? std::abs(dy) / hh : 0.0);
    if (denom < 1e-9) return center;
    const double t = 1.0 / denom;
    return {center.x + dx * t, center.y + dy * t};
}

Point circle_border(Point center, double radius, Point toward) {
    const double dx = toward.x - center.x, dy = toward.y - center.y;
    const double len = std::hypot(dx, dy);
    if (len < 1e-9) return center;
    return {center.x + dx / len * radius, center.y + dy / len * radius};
}

Point stadium_border(Point center, double w, double h, Point toward) {
    if (w <= h) return box_border(center, w, h, toward);  // not a horizontal pill
    const double r = h / 2.0, a = w / 2.0 - r;
    double dx = toward.x - center.x, dy = toward.y - center.y;
    const double len = std::hypot(dx, dy);
    if (len < 1e-9) return center;
    dx /= len;
    dy /= len;
    // Exit through the flat top/bottom when the crossing x stays over the
    // straight section.
    if (std::abs(dy) > 1e-9) {
        const double t = r / std::abs(dy);
        const double x = center.x + dx * t;
        if (std::abs(x - center.x) <= a) return {x, center.y + dy * t};
    }
    // Otherwise exit through a semicircular cap, centred at (cx ± a, cy).
    const double cap_x = center.x + (dx >= 0.0 ? a : -a);
    const double ox = center.x - cap_x;  // oy == 0
    const double b = ox * dx;
    const double c = ox * ox - r * r;
    const double disc = b * b - c;
    if (disc < 0.0) return box_border(center, w, h, toward);
    const double t = -b + std::sqrt(disc);
    return {center.x + dx * t, center.y + dy * t};
}

std::vector<Point> retract_end(std::vector<Point> route, double dist) {
    if (route.size() < 2 || dist <= 0.0) return route;
    double remaining = dist;
    while (route.size() >= 2) {
        const Point end = route.back();
        const Point prev = route[route.size() - 2];
        const double dx = end.x - prev.x, dy = end.y - prev.y;
        const double len = std::hypot(dx, dy);
        if (len >= remaining) {
            const double t = remaining / len;
            route.back() = {end.x - dx * t, end.y - dy * t};
            return route;
        }
        remaining -= len;
        route.pop_back();
    }
    return route;
}

Point polyline_midpoint(const std::vector<Point>& points) {
    double total = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i)
        total += std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
    const double half = total / 2.0;
    double acc = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const double seg =
            std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
        if (acc + seg >= half) {
            const double t = seg > 1e-9 ? (half - acc) / seg : 0.0;
            return {points[i - 1].x + (points[i].x - points[i - 1].x) * t,
                    points[i - 1].y + (points[i].y - points[i - 1].y) * t};
        }
        acc += seg;
    }
    return points.empty() ? Point{} : points.back();
}

std::vector<Point> clip_route(std::vector<Point> route, Point a, double aw, double ah,
                              Point b, double bw, double bh) {
    if (route.size() < 2) return route;
    // Drop route points that lie INSIDE the end boxes before snapping the
    // endpoints. A bowed or smoothed route runs centre to centre, so the
    // samples next to each end sit inside the node. Snapping only the
    // endpoint and then aiming the arrowhead from that inner sample builds
    // the triangle on the wrong side of the border, where the node (drawn
    // over the edges) covers it — and a bounding box over the route reaches
    // the node centres instead of spanning just the visible curve.
    const auto inside = [](Point p, Point c, double w, double h) {
        return std::abs(p.x - c.x) < w / 2.0 && std::abs(p.y - c.y) < h / 2.0;
    };
    while (route.size() > 2 && inside(route[route.size() - 2], b, bw, bh)) route.pop_back();
    while (route.size() > 2 && inside(route[1], a, aw, ah)) route.erase(route.begin());
    route.front() = box_border(a, aw, ah, route[1]);
    route.back() = box_border(b, bw, bh, route[route.size() - 2]);
    return route;
}

void LabelLayout::add(const std::vector<Point>& route, std::string text, const Font& font,
                      std::optional<Color> color) {
    if (text.empty() || route.size() < 2) return;
    // Arc-length midpoint and the direction of the segment it lies on
    // (the local tangent — for bowed or smoothed routes the start→end
    // chord direction no longer describes the line near its middle).
    double total = 0.0;
    for (std::size_t i = 1; i < route.size(); ++i)
        total += std::hypot(route[i].x - route[i - 1].x, route[i].y - route[i - 1].y);
    const double half = total / 2.0;
    Point mid = route.back();
    double dx = route.back().x - route.front().x;
    double dy = route.back().y - route.front().y;
    double acc = 0.0;
    for (std::size_t i = 1; i < route.size(); ++i) {
        const double sx = route[i].x - route[i - 1].x;
        const double sy = route[i].y - route[i - 1].y;
        const double seg = std::hypot(sx, sy);
        if (acc + seg >= half) {
            const double t = seg > 1e-9 ? (half - acc) / seg : 0.0;
            mid = {route[i - 1].x + sx * t, route[i - 1].y + sy * t};
            if (seg > 1e-9) {
                dx = sx;
                dy = sy;
            }
            break;
        }
        acc += seg;
    }
    const double len = std::hypot(dx, dy);
    if (len < 1e-9) {
        dx = 1.0;
        dy = 0.0;
    } else {
        dx /= len;
        dy /= len;
    }
    const double px = -dy, py = dx;  // unit perpendicular (left of travel)
    // Offset the label clear of the line: beside vertical edges,
    // above/below horizontal ones (so it never sits on the line). Put it
    // on the side the route bows toward — parallel-pair bows and
    // smoothed multi-rank routes displace the midpoint off the chord,
    // and the outside of the bow is the side that clears both curves.
    const Point chord_mid{(route.front().x + route.back().x) / 2.0,
                          (route.front().y + route.back().y) / 2.0};
    const double bow = (mid.x - chord_mid.x) * px + (mid.y - chord_mid.y) * py;
    const double side = bow < -0.5 ? -1.0 : 1.0;
    const double lw = text_width(text, font);
    const double lh = font.size * 1.35;
    const bool vertical = std::abs(dy) >= std::abs(dx);
    const double dist = vertical ? lw / 2.0 + 6.0 : lh / 2.0 + 3.0;
    entries_.push_back({{mid.x + px * side * dist, mid.y + py * side * dist},
                        lw,
                        lh,
                        std::move(text),
                        font,
                        color});
}

void LabelLayout::draw(Canvas& canvas, Color text_color, Color background) {
    // Declutter: nudge overlapping labels apart along the axis of least
    // overlap. Fixed iteration count and ordered passes keep it
    // deterministic.
    constexpr double kPad = 3.0;
    for (int iter = 0; iter < 30; ++iter) {
        bool moved = false;
        for (std::size_t i = 0; i < entries_.size(); ++i)
            for (std::size_t j = i + 1; j < entries_.size(); ++j) {
                Entry& a = entries_[i];
                Entry& b = entries_[j];
                const double ox =
                    (a.w + b.w) / 2.0 + kPad - std::abs(a.pos.x - b.pos.x);
                const double oy =
                    (a.h + b.h) / 2.0 + kPad - std::abs(a.pos.y - b.pos.y);
                if (ox <= 0.0 || oy <= 0.0) continue;  // no overlap
                if (oy <= ox) {  // separate vertically (smaller shift)
                    const double s = oy / 2.0 + 0.25;
                    if (a.pos.y <= b.pos.y) {
                        a.pos.y -= s;
                        b.pos.y += s;
                    } else {
                        a.pos.y += s;
                        b.pos.y -= s;
                    }
                } else {  // separate horizontally
                    const double s = ox / 2.0 + 0.25;
                    if (a.pos.x <= b.pos.x) {
                        a.pos.x -= s;
                        b.pos.x += s;
                    } else {
                        a.pos.x += s;
                        b.pos.x -= s;
                    }
                }
                moved = true;
            }
        if (!moved) break;
    }
    for (Entry& e : entries_) {
        // Keep every label inside the canvas: declutter nudges and bowed
        // routes near the border can otherwise push a label off the edge.
        if (canvas.width > 0.0) {
            const double half = e.w / 2.0 + 2.0;
            e.pos.x = std::min(std::max(e.pos.x, half), canvas.width - half);
        }
        if (canvas.height > 0.0) {
            const double half = e.h / 2.0 + 2.0;
            e.pos.y = std::min(std::max(e.pos.y, half), canvas.height - half);
        }
        canvas.masked_text(e.pos, std::move(e.text), e.font, e.color.value_or(text_color),
                           HAlign::Center, VAlign::Middle, background);
    }
}

void Canvas::line(Point a, Point b, ShapeStyle style) {
    items_.push_back(cplot::LineItem{a, b, std::move(style)});
}

void Canvas::polyline(std::vector<Point> points, ShapeStyle style) {
    items_.push_back(cplot::PolylineItem{std::move(points), std::move(style)});
}

void Canvas::polygon(std::vector<Point> points, ShapeStyle style) {
    items_.push_back(cplot::PolygonItem{std::move(points), std::move(style)});
}

void Canvas::rect(RectF box, ShapeStyle style) {
    items_.push_back(cplot::RectItem{box, std::move(style)});
}

void Canvas::rounded_rect(RectF box, double radius, ShapeStyle style) {
    if (!(radius > 0.0)) {
        rect(box, std::move(style));
        return;
    }
    items_.push_back(cplot::rounded_rect_path(box, radius, std::move(style)));
}

void Canvas::path(cplot::PathItem item) { items_.push_back(std::move(item)); }

void Canvas::circle(Point center, double radius, ShapeStyle style) {
    items_.push_back(cplot::CircleItem{center, radius, std::move(style)});
}

void Canvas::ellipse(RectF box, ShapeStyle style) {
    items_.push_back(cplot::ellipse_path(box, std::move(style)));
}

void Canvas::sector(Point center, double radius_inner, double radius_outer,
                    double start_deg, double end_deg, ShapeStyle style) {
    items_.push_back(cplot::SectorItem{center, radius_inner, radius_outer, start_deg,
                                       end_deg, std::move(style)});
}

void Canvas::masked_text(Point pos, std::string content, Font font, Color color,
                         HAlign halign, VAlign valign, Color mask) {
    cplot::TextItem item;
    item.pos = pos;
    item.text = std::move(content);
    item.font = std::move(font);
    item.color = color;
    item.halign = halign;
    item.valign = valign;
    ShapeStyle backdrop;
    backdrop.fill = mask.with_alpha(0.75);
    items_.push_back(cplot::text_halo(item, cplot::default_text_measurer(), 3.0, backdrop));
    items_.push_back(std::move(item));
}

void Canvas::text(Point pos, std::string content, Font font, Color color, HAlign halign,
                  VAlign valign, double rotation) {
    cplot::TextItem item;
    item.pos = pos;
    item.text = std::move(content);
    item.font = std::move(font);
    item.color = color;
    item.halign = halign;
    item.valign = valign;
    item.rotation = rotation;
    items_.push_back(std::move(item));
}

double Canvas::text_block(Point top, const std::vector<std::string>& lines, Font font,
                          Color color, HAlign halign, double line_height) {
    // Each line is vertically centred within its own line-height slot, so
    // the whole block is centred about top + N*line_height/2 — a single
    // line then sits exactly on the box centre, not shifted up.
    for (std::size_t i = 0; i < lines.size(); ++i) {
        text({top.x, top.y + (static_cast<double>(i) + 0.5) * line_height}, lines[i], font,
             color, halign, VAlign::Middle);
    }
    return static_cast<double>(lines.size()) * line_height;
}

void Canvas::arrow_head(Point tip, Point from, double length, double spread, Color fill) {
    double dx = tip.x - from.x;
    double dy = tip.y - from.y;
    const double len = std::hypot(dx, dy);
    if (len < 1e-9) {
        dx = 1.0;
        dy = 0.0;
    } else {
        dx /= len;
        dy /= len;
    }
    const Point base{tip.x - dx * length, tip.y - dy * length};
    const double px = -dy;  // unit perpendicular
    const double py = dx;
    const double half = spread / 2.0;
    ShapeStyle style;
    style.fill = fill;
    style.stroke = fill;
    style.stroke_width = 0.0;
    polygon({tip, {base.x + px * half, base.y + py * half}, {base.x - px * half, base.y - py * half}},
            std::move(style));
}

void Canvas::add_link(RectF box, std::string href, std::string title, std::string target) {
    if (href.empty()) return;
    links_.push_back(
        cplot::LinkRegion{box, std::move(href), std::move(title), std::move(target)});
}

cplot::Scene Canvas::bake() const {
    cplot::Scene scene;
    scene.width = width;
    scene.height = height;
    scene.background = background;
    scene.meta_title = title;
    scene.meta_description = description;
    scene.meta_generator = "cdiagram";

    // A diagram is one drawing surface, not a plot: no clip, no bands, just
    // the items in the order they were drawn.
    scene.root.add(items_);
    scene.root.link_regions = links_;
    return scene;
}

} // namespace cdiagram::detail
