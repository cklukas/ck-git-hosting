// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Backend-independent scene graph. Chart logic emits these primitives;
// renderers (SVG, later PNG/SIXEL) consume them. Keeping this layer flat
// and explicit makes output deterministic and easy to test.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "color.hpp"
#include "jpeg.hpp"
#include "limits.hpp"
#include "raster_image.hpp"
#include "theme.hpp"

namespace cplot {

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct RectF {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
};

// -- placement -----------------------------------------------------------------

/// An affine transform, in SVG `matrix(a b c d e f)` and PDF `cm` operand
/// order **verbatim**:
///
///     x' = a·x + c·y + e
///     y' = b·x + d·y + f
///
/// The order is not a preference. It is what both vector backends already
/// speak, so a group's placement reaches SVG and PDF with no conversion step
/// and no chance of a transposition bug between them. (The PDF backend does
/// conjugate it with its own y-flip, because PDF user space is y-up while a
/// Scene is y-down — but the six numbers keep their meaning and their order.)
///
/// Both determinism obligations are met here rather than asked of callers:
///
///  1. `rotate_deg` routes through cworks::sincos_deg, never libm. Every
///     coordinate a rotated group produces is written into the output, and
///     libm's sine is accurate to an ulp but is not bit-pinned across
///     platforms; the suite's own degree trigonometry is bit-identical by
///     construction.
///  2. `then` and `apply` fix their operation order in the header and never
///     reassociate. `-ffp-contract=off` is set suite-wide
///     (cmake/CWorksSettings.cmake), so a fixed order of IEEE multiply and
///     add gives the same bits everywhere.
struct Transform {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;

    static constexpr Transform translate(double dx, double dy) noexcept {
        return Transform{1.0, 0.0, 0.0, 1.0, dx, dy};
    }
    static constexpr Transform scale(double sx, double sy) noexcept {
        return Transform{sx, 0.0, 0.0, sy, 0.0, 0.0};
    }
    /// Rotation by `degrees`, clockwise on screen (y down) — SVG's
    /// `rotate()` and the same sense as TextItem::rotation. Exact on the
    /// quarter turns, so a right-angled group lands on (±1, 0) and (0, ±1)
    /// rather than a rounding beside them.
    static Transform rotate_deg(double degrees) noexcept;

    /// `inner.then(outer)` — the transform that applies `inner` first, i.e.
    /// the matrix product `outer · inner`. That is the order SVG nests
    /// `<g transform>`: the outer group's placement applies to whatever the
    /// inner one produced.
    constexpr Transform then(const Transform& outer) const noexcept {
        return Transform{outer.a * a + outer.c * b,
                         outer.b * a + outer.d * b,
                         outer.a * c + outer.c * d,
                         outer.b * c + outer.d * d,
                         outer.a * e + outer.c * f + outer.e,
                         outer.b * e + outer.d * f + outer.f};
    }

    constexpr Point apply(Point p) const noexcept {
        return Point{a * p.x + c * p.y + e, b * p.x + d * p.y + f};
    }

    /// The axis-aligned bounds of the mapped rectangle.
    ///
    /// A rotated rectangle is not a rectangle, so this is a *reduction*: it
    /// returns the bounding box of the mapped shape, which is larger than
    /// the shape wherever the transform rotates or shears. Callers that need
    /// the exact quadrilateral must map the four corners themselves. It is
    /// named as a bounds function for that reason, and the renderers use it
    /// only where the target format itself demands an axis-aligned rectangle
    /// — a hyperlink hit box in SVG, PDF and every HTML overlay.
    ///
    /// When `is_axis_aligned()` holds the result is not merely tight but
    /// *exact to the bit*: the extents are scaled rather than rebuilt as a
    /// difference of mapped corners, so an identity or a pure translation
    /// returns the input rectangle unchanged instead of a width that has
    /// lost its last bit to `(x + w) - x`.
    RectF apply_bounds(const RectF& r) const noexcept;

    /// The inverse transform, or nullopt when the linear part is singular
    /// (a degenerate group collapses its content to a line or a point and
    /// nothing can be mapped back out of it).
    std::optional<Transform> inverse() const noexcept;

    constexpr bool is_identity() const noexcept {
        return a == 1.0 && b == 0.0 && c == 0.0 && d == 1.0 && e == 0.0 && f == 0.0;
    }

    /// True when the linear part maps axis-aligned rectangles to
    /// axis-aligned rectangles: either it scales the axes in place
    /// (`b == c == 0`) or it swaps them (`a == d == 0`, the 90°/270°
    /// family, with or without a mirror).
    ///
    /// This is the property both a rectangular clip and a hyperlink hit box
    /// turn on. Where it holds, a rectangle stays a rectangle and the exact
    /// closed forms apply; where it does not, a rectangle is a
    /// parallelogram and the honest answer costs a coverage mask (the
    /// rasterizer) or a bounding box (a link region).
    constexpr bool is_axis_aligned() const noexcept {
        return (b == 0.0 && c == 0.0) || (a == 0.0 && d == 0.0);
    }
};

constexpr bool operator==(const Transform& x, const Transform& y) noexcept {
    return x.a == y.a && x.b == y.b && x.c == y.c && x.d == y.d && x.e == y.e && x.f == y.f;
}
constexpr bool operator!=(const Transform& x, const Transform& y) noexcept { return !(x == y); }

/// A rectangular clickable region carrying one hyperlink. Backends map it
/// onto their native anchor concept: SVG `<a xlink:href>` over a transparent
/// hit rect, PDF a `/Link` annotation with a `/URI` action. `bbox` is in the
/// same top-left/y-down CSS-pixel space as every SceneItem, and in the local
/// space of the group that owns it — so it moves, scales and rotates with the
/// geometry it labels instead of having to be rewritten at every placement.
/// `title` is an optional tooltip and `target` an optional link target (e.g.
/// `_blank`); both may be empty.
struct LinkRegion {
    RectF bbox;
    std::string href;
    std::string title;
    std::string target;
};

/// A producer-owned identity and its laid-out hit rectangle. Unlike a link,
/// this has no navigation action and is never embedded in exported graphics.
struct HitRegion {
    RectF bbox;
    std::uint64_t id = 0;
};

/// Stroke geometry vocabulary. Names and value sets are SVG 2's, verbatim
/// (`stroke-linecap`, `stroke-linejoin`, `stroke-dasharray` /
/// `stroke-dashoffset`, `stroke-miterlimit`, `fill-rule`), so every backend
/// maps them onto its native operator without a translation table and
/// without inventing a fourth vocabulary.

/// SVG `stroke-linecap`: the shape drawn at the two ends of an open
/// subpath (and at every dash, when the stroke is dashed).
enum class LineCap {
    Butt,  ///< the path stops at the endpoint
    Round, ///< a half-disc of radius width/2 caps the endpoint
    Square ///< the stroke extends width/2 past the endpoint
};

/// SVG `stroke-linejoin`: the shape drawn on the outside of a corner.
enum class LineJoin {
    Miter, ///< extend both edges to their intersection, up to `miter_limit`
    Round, ///< a disc of radius width/2 at the corner
    Bevel  ///< a straight cut across the outer corner
};

/// SVG `fill-rule`: how a self-intersecting or multi-loop path decides
/// which regions are inside.
enum class FillRule {
    NonZero, ///< winding number != 0
    EvenOdd  ///< odd number of crossings
};

/// SVG `stroke-dasharray` plus `stroke-dashoffset`, parsed once into
/// numbers instead of carried as backend-specific text.
///
/// `lengths` alternates on/off run lengths in scene pixels. An odd-sized
/// list repeats to make the on/off cycle even, exactly as SVG specifies.
/// `phase` is the distance already consumed at the start of every subpath.
struct DashPattern {
    std::vector<double> lengths;
    double phase = 0.0;

    bool empty() const { return lengths.empty(); }

    /// Parse an SVG `stroke-dasharray` value: comma- and/or
    /// whitespace-separated lengths, `none` for solid. Returns a solid
    /// (empty) pattern for anything SVG requires to be ignored — a
    /// negative length, a non-numeric entry, or an all-zero list.
    /// Locale-independent: the radix is always '.'.
    static DashPattern parse(std::string_view svg_dasharray);
};

inline bool operator==(const DashPattern& a, const DashPattern& b) {
    return a.lengths == b.lengths && a.phase == b.phase;
}
inline bool operator!=(const DashPattern& a, const DashPattern& b) { return !(a == b); }

struct ShapeStyle {
    std::optional<Color> fill;
    std::optional<Color> stroke;
    double stroke_width = 1.0;
    DashPattern dash;                       ///< empty = solid
    LineCap cap = LineCap::Butt;            ///< SVG initial value
    LineJoin join = LineJoin::Miter;        ///< SVG initial value
    double miter_limit = 4.0;               ///< SVG initial value (PDF's is 10)
    FillRule fill_rule = FillRule::NonZero; ///< SVG initial value
};

enum class HAlign { Left, Center, Right };
enum class VAlign { Baseline, Top, Middle, Bottom };

struct LineItem {
    Point a, b;
    ShapeStyle style;
};

struct PolylineItem {
    std::vector<Point> points;
    ShapeStyle style;
};

struct PolygonItem {
    std::vector<Point> points;
    ShapeStyle style;
};

struct RectItem {
    RectF rect;
    ShapeStyle style;
};

struct CircleItem {
    Point center;
    double radius = 1.0;
    ShapeStyle style;
};

/// One step of a path: a straight line to `to`, or a cubic Bézier to
/// `to` through the control points `c1` and `c2`.
///
/// The controls are only read when `curve` is true, so a straight step
/// costs no thought at the call site and no ambiguity at the backend.
struct PathSegment {
    Point c1;
    Point c2;
    Point to;
    bool curve = false;

    static PathSegment line_to(Point end) { return PathSegment{{}, {}, end, false}; }
    static PathSegment curve_to(Point control1, Point control2, Point end) {
        return PathSegment{control1, control2, end, true};
    }
};

/// One continuous run of a path: a start point and the steps that follow
/// it. `closed` joins the last point back to `start`, which is what makes
/// the run fillable and gives it a join at the seam rather than two caps.
struct SubPath {
    Point start;
    std::vector<PathSegment> segments;
    bool closed = false;
};

/// A path of straight and cubic-Bézier segments, in one or more subpaths.
///
/// This is the primitive that lets a curve stay a curve. Producers used
/// to flatten their own arcs and ribbons to polylines before handing them
/// over, which threw away resolution independence in exactly the two
/// backends that could have kept it: an SVG or PDF viewer renders `C`
/// analytically at any zoom, while a polygon baked at authoring time is
/// stuck with its segment count forever. The vector backends therefore
/// emit the curve; only the rasterizer flattens, and it does so at output
/// resolution, deterministically (see flatten_path).
struct PathItem {
    std::vector<SubPath> subpaths;
    ShapeStyle style;

    /// Begin a new subpath at `p`.
    PathItem& move_to(Point p) {
        subpaths.push_back(SubPath{p, {}, false});
        return *this;
    }
    /// Straight step. Ignored before the first move_to.
    PathItem& line_to(Point p) {
        if (!subpaths.empty()) subpaths.back().segments.push_back(PathSegment::line_to(p));
        return *this;
    }
    /// Cubic-Bézier step. Ignored before the first move_to.
    PathItem& curve_to(Point c1, Point c2, Point p) {
        if (!subpaths.empty())
            subpaths.back().segments.push_back(PathSegment::curve_to(c1, c2, p));
        return *this;
    }
    /// Close the current subpath. Ignored before the first move_to.
    PathItem& close() {
        if (!subpaths.empty()) subpaths.back().closed = true;
        return *this;
    }
};

/// The control-point offset that makes a cubic Bézier approximate a
/// quarter circle of radius 1 to within 2.7e-4 of the true arc — the
/// standard constant 4·(√2 − 1)/3. Four such segments are how every
/// backend, and every producer that needs a round corner, spells a
/// circle or an ellipse.
inline constexpr double kBezierKappa = 0.5522847498307936;

/// A closed PathItem tracing the axis-aligned ellipse inscribed in
/// `box`, as four cubic Béziers. The workhorse behind round corners,
/// discs and cylinder ends: a real curve in SVG and PDF, flattened at
/// output resolution in the raster backends.
PathItem ellipse_path(const RectF& box, ShapeStyle style);

/// A closed PathItem tracing `box` with corners rounded by `radius`,
/// as four cubic Bézier corners joined by straight edges. `radius` is
/// clamped to half the shorter side; a non-positive radius yields the
/// plain rectangle.
PathItem rounded_rect_path(const RectF& box, double radius, ShapeStyle style);

/// Flatten `path` into one polyline per subpath, with every point
/// multiplied by `scale` — the form the raster backends paint.
///
/// The segment count of each curve is derived from its own control
/// polygon at that scale, so a hairline arc costs a handful of segments
/// and a full-page one costs enough to stay smooth. It is computed in
/// closed form from division, multiplication and `sqrt` alone — all
/// exactly specified by IEEE 754 — rather than through a transcendental,
/// because a count that differed by one between platforms would move
/// every vertex of the curve and break byte-identical raster output.
/// A closed subpath's polyline repeats its start point at the end.
///
/// The Transform overload is the general form and the one a group tree
/// uses: flattening happens **after** the transform, never before, so a
/// scaled or rotated group gets a curve resolved for where it actually
/// lands rather than a polygon baked at authoring size and then magnified.
/// The scalar overload is the same call with a uniform scale.
std::vector<std::vector<Point>> flatten_path(const PathItem& path, double scale = 1.0);
std::vector<std::vector<Point>> flatten_path(const PathItem& path, const Transform& transform);

struct TextItem {
    Point pos;
    std::string text;
    Font font;
    Color color;
    HAlign halign = HAlign::Left;
    VAlign valign = VAlign::Baseline;
    double rotation = 0.0; ///< degrees, rotates around pos
};

/// Circular wedge or ring segment (pie/doughnut slices).
///
/// Angles are in degrees, measured clockwise from 12 o'clock (screen
/// convention). radius_inner > 0 produces a ring segment (doughnut).
struct SectorItem {
    Point center;
    double radius_inner = 0.0;
    double radius_outer = 1.0;
    double start_angle = 0.0;
    double end_angle = 0.0;
    ShapeStyle style;
};

/// What an ImageItem draws: decoded pixels, or a JPEG carried by its
/// encoded bytes.
///
/// Both are pictures every backend shows, which is the condition for being
/// here. Pixels reach each backend as they are. A JPEG reaches the two
/// vector backends exactly as it was authored — PDF embeds the stream as a
/// `/DCTDecode` image and SVG as a `data:image/jpeg` URI, both formats
/// that decode JPEG themselves — and only the rasterizer decodes it, with
/// the library's own decoder (<cplot/jpeg.hpp>). A photograph on a slide
/// therefore reaches a PDF at the size and the fidelity its author gave it,
/// not as re-encoded samples several times larger, and all three backends
/// still draw one picture: a JpegPicture holds only a stream the decoder
/// accepted.
///
/// **No vector source, deliberately.** A picture that stayed vector
/// through the SVG and PDF exports would be strictly better output, but a
/// vector source reaches only the backend that already speaks its format:
/// SVG bytes cannot enter a PDF, and neither can enter the rasterizer,
/// without an importer this library does not have. Three backends that
/// disagree about whether a picture appears at all is a worse contract
/// than one that always shows the same pixels, so the vector source waits
/// for the importer that would make it uniform.
using ImageContent = std::variant<RasterImage, JpegPicture>;

/// The width and height `image` fills its box with: the pixels' own, or a
/// JPEG's once its EXIF orientation is applied.

/// Picture content placed into a scene.
///
/// `image` is the picture itself, never a path: a Scene is a completed,
/// self-contained graph, and a producer that resolved a filename at build
/// time must not hand the renderers a second chance to resolve it
/// differently. (Reading the file is the caller's step: `decode_picture`
/// in <cplot/picture.hpp> decodes PNG and JPEG alike, and
/// `JpegPicture::from_bytes` keeps a JPEG encoded.)
///
/// A JPEG's EXIF orientation is part of the picture. Every backend draws
/// the stored image turned as the orientation says, so what fills `dest`
/// is the picture as its author saw it.
///
/// `dest` places the picture in local space and the image is scaled to
/// fill it, aspect ratio included — the scene has already decided where
/// the picture goes, so no backend letterboxes it. A negative extent
/// describes the same rectangle rather than a mirror: mirroring is a
/// group transform, which every backend expresses, while a negative
/// `width` is not something SVG or PDF has. `style`'s fill paints *under*
/// the image and its stroke *over* it, so the item can carry its own matte
/// and frame; both are normally unset.
///
/// Pixels whose `rgba` is shorter than `width · height · 4` draw nothing
/// at all, in every backend: a half-filled buffer is a producer bug, and
/// reading past it would be worse than showing nothing.
struct ImageItem {
    ImageContent image;
    RectF dest;
    double opacity = 1.0;
    ShapeStyle style;
};

using SceneItem = std::variant<LineItem, PolylineItem, PolygonItem, RectItem, CircleItem,
                               PathItem, TextItem, SectorItem, ImageItem>;

/// Deterministic polygon tessellation of a sector, shared by the raster
/// and SIXEL backends (and usable by tests). Points run along the outer
/// arc clockwise, then back along the inner arc (or via the center).
std::vector<Point> tessellate_sector(const SectorItem& sector);

class TextMeasurer;

/// A measured backdrop box for `text`, expanded by `padding` on every
/// side, honouring the item's alignment anchors (and rotated with the
/// text when it is rotated). Pushed immediately before its TextItem it
/// keeps a label legible over lines and filled marks; a semi-transparent
/// fill dims the geometry behind the glyphs instead of erasing it.
SceneItem text_halo(const TextItem& text, const TextMeasurer& measurer, double padding,
                    ShapeStyle style);

// -- the scene graph -----------------------------------------------------------

/// Stable identity for a group, empty by default.
///
/// It exists so that two renderings of the same figure can be *compared* by
/// name rather than by position: present in both is the same object,
/// present in only one is an object that appeared or left. Without stable
/// identity that relationship has to be worked out object by object, and a
/// node that merely moved is indistinguishable from a different node.
///
/// Ids are producer-assigned and need be unique only within their scene. An
/// empty id means "not tracked" and never takes part in a diff.
using ItemId = std::string;

struct Group;

/// One child of a group, in paint order: a drawable leaf or a nested group.
///
/// A leaf is stored inline. A nested group is boxed, because a variant
/// cannot hold an incomplete type and a group is defined in terms of this
/// very type — `std::variant<SceneItem, Group>` compiles on all three
/// standard libraries and is nonetheless ill-formed ([variant.variant]/2
/// requires complete alternatives), which is exactly what makes it a trap.
/// The allocation is per *group*, never per item: scenes hold items in the
/// tens of thousands and groups in the tens.
///
/// A Node has value semantics — copying one deep-copies the whole subtree —
/// so a Scene is still copied, assigned and returned by value exactly as it
/// was when it was a flat list. Nothing may key on the address of a boxed
/// group: it does not survive that copy, which is what ItemId is for.
class Node {
public:
    Node(SceneItem item);
    Node(Group group);

    ~Node();
    Node(const Node& other);
    Node& operator=(const Node& other);
    Node(Node&& other) noexcept;
    Node& operator=(Node&& other) noexcept;

    bool is_group() const noexcept { return value_.index() == 1; }

    /// The leaf. Throws std::bad_variant_access when `is_group()`.
    const SceneItem& item() const { return std::get<SceneItem>(value_); }
    SceneItem& item() { return std::get<SceneItem>(value_); }

    /// The nested group. Throws std::bad_variant_access when `!is_group()`.
    const Group& group() const { return *std::get<1>(value_); }
    Group& group() { return *std::get<1>(value_); }

private:
    std::variant<SceneItem, std::unique_ptr<Group>> value_;
};

/// A picture that is an SVG document of its own, which a backend that writes
/// SVG carries as it is: the document, and the rectangle it is stretched to
/// in its group's local space. No other backend can draw it; they paint the
/// group's children — the stand-in its producer drew — instead.
struct EmbeddedSvg {
    std::string document; ///< the SVG file, byte for byte
    RectF dest;           ///< where it is stretched to, in the group's local space
};

/// The size an SVG document asks to be shown at, in CSS pixels.
struct SvgDocumentSize {
    double width = 0.0;
    double height = 0.0;
};

/// The size `document` asks to be shown at, from its root element as a
/// browser reads it: `width` and `height` in absolute units (px, pt, pc,
/// in, cm, mm, Q, or none); for one that is missing or relative, the
/// `viewBox`'s extent, scaled to keep its proportions when the other is
/// given; else 300 by 150. Nullopt when the bytes hold no `<svg>` root
/// element.
std::optional<SvgDocumentSize> svg_document_size(std::string_view document);

/// A transformed, optionally clipped subtree of the scene.
///
/// `svg`, when set, is a picture the SVG backend writes in the group's
/// place (EmbeddedSvg); every other backend, and everything that walks the
/// scene's items, sees the children.
///
/// Paint order is document order — the painter's algorithm, across leaves
/// and nested groups alike. A producer that needs five layers writes five
/// and one that needs none writes none; nothing imposes a fixed set of
/// bands.
///
/// `clip` is in LOCAL space, i.e. *before* `transform`. That is the only
/// choice that composes: a clip must mean the same thing whether or not an
/// ancestor later moves the group, and one expressed in device space would
/// have to be rewritten at every placement. It is optional and defaults to
/// none, so a producer that does not need clipping does not pay for one.
///
/// `link_regions` are local-space too and transform with the group. Because
/// a hyperlink hit box is axis-aligned in every target format, a rotated
/// group's regions reduce to `Transform::apply_bounds` — see there.
struct Group {
    ItemId id;                            ///< empty = not tracked
    Transform transform;                  ///< identity by default
    std::optional<RectF> clip;            ///< local space, before `transform`
    std::vector<Node> children;           ///< paint order
    std::vector<LinkRegion> link_regions; ///< local space; transform with the group
    std::vector<HitRegion> hit_regions;   ///< local space; native interaction only
    std::optional<EmbeddedSvg> svg;       ///< what an SVG backend writes instead

    /// Append one leaf, one nested group, or a whole run of leaves, in
    /// paint order.
    ///
    /// These exist because `children` holds Nodes: writing
    /// `children.push_back(RectItem{...})` asks the language for two
    /// user-defined conversions in a row (RectItem → SceneItem → Node) and
    /// it performs only one, so every producer would otherwise spell the
    /// intermediate out by hand. The vector overload moves its elements
    /// rather than copying them.
    Group& add(SceneItem item) {
        children.emplace_back(std::move(item));
        return *this;
    }
    Group& add(Group group) {
        children.emplace_back(std::move(group));
        return *this;
    }
    Group& add(std::vector<SceneItem> items) {
        children.reserve(children.size() + items.size());
        for (SceneItem& item : items) children.emplace_back(std::move(item));
        return *this;
    }
};

/// A fully laid-out figure, ready for any renderer.
struct Scene {
    double width = 0.0;
    double height = 0.0;
    Color background = colors::white;

    /// The scene's content: one tree, walked in document order.
    Group root;

    std::string meta_title;       ///< SVG `<title>`
    std::string meta_description; ///< SVG `<desc>`
    std::string meta_generator;   ///< SVG `<metadata>` stamp (version/profile)
    std::vector<std::pair<std::string, std::string>> meta_extra; ///< extra metadata
    std::string svg_width_attr;   ///< physical width ("85mm"); empty = px
    std::string svg_height_attr;  ///< physical height; empty = px

    /// Whole-figure hyperlink: the entire canvas becomes clickable (a
    /// background anchor beneath any per-element `LinkRegion`). Empty = none.
    std::string link;
    std::string link_target; ///< optional target for `link` (e.g. `_blank`)

    /// Render-resource bounds, copied from the figure so renderers can
    /// enforce them (RasterRenderer checks raster_pixels).
    RenderLimits limits;
    /// Soft-limit warnings collected while building the scene
    /// (point-count checks); also echoed once to stderr.
    cworks::Diagnostics diagnostics;
};

/// One drawable leaf of a scene, and the transform that places it.
struct PlacedItem {
    const SceneItem* item; ///< never null; refers into the walked group
    Transform ctm;         ///< local space → the space the walk started in
};

/// Every drawable leaf under `group`, in paint order, each paired with the
/// accumulated transform that places it.
///
/// The renderers do not use this: they walk the tree themselves, because
/// they also need the clips, and a flat list cannot carry a clip without
/// inventing a shape type for the intersection. It answers the *other*
/// question a completed scene gets asked — not "how do I draw this" but
/// "what is in it". How many marks did that series emit; did anything land
/// outside its die-cut label; did this figure draw anything at all. Every
/// one of those wants the leaves and where they ended up, and none of them
/// should have to re-derive a recursive descent over a boxed variant to get
/// there.
///
/// Pointers refer into `group` and stay valid as long as it is unmodified.
std::vector<PlacedItem> collect_items(const Group& group);

/// One hyperlink of a scene, its hit box mapped into device space and its
/// text copied out of the node that carried it — so the value outlives the
/// scene it came from and a consumer can hold on to it.
struct PlacedLink {
    RectF box;          ///< device space, top-left origin, y down
    std::string href;   ///< never empty
    std::string title;  ///< tooltip; may be empty
    std::string target; ///< e.g. `_blank`; may be empty
};

/// Every hyperlink a scene carries, in specificity order: least specific
/// first, most specific last.
///
/// One walk fixes that order for everybody. The whole-canvas `Scene::link`
/// comes first, then `root` in pre-order — a group's own regions before its
/// children's, siblings in paint order. What the order encodes is a single
/// rule:
///
/// > **Where two hit boxes overlap, the click belongs to the later one.**
///
/// A nested group's anchor beats its parent's, a mark's beats the whole
/// figure's, and a producer that wants one anchor to win over another
/// expresses it by where it puts them — nesting or paint order, the same
/// two things that already decide what is drawn on top.
///
/// Backends resolve an overlap from opposite ends of their own list, so
/// each emits this one order in the direction its format needs: SVG paints
/// anchors in list order and a viewer takes the topmost, so it emits
/// forward; a PDF `/Annots` array is scanned first-match, so it emits
/// reversed. Any consumer building its own overlay — an HTML or LaTeX one —
/// answers the same question and owes the same answer.
///
/// Boxes are axis-aligned in every target format, so a region under a
/// rotated group reduces to the bounding box of its mapped rectangle.
std::vector<PlacedLink> collect_links(const Scene& scene);

/// Native interaction regions in device-space CSS pixels. The order is paint
/// order; a consumer resolves overlap from the last matching region.
std::vector<HitRegion> collect_hit_regions(const Scene& scene);

} // namespace cplot
