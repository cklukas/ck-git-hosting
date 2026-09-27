// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The library's one job: turn Mermaid text into a backend-independent
// cplot::Scene. The scene then renders — byte-deterministically — to
// vector SVG, vector PDF, raster PNG and SIXEL through cplot's existing
// renderers, so cdiagram never emits pixels or markup itself.
//
// Determinism is the contract: the same source always produces the same
// Scene, on every platform, with no dependence on time, locale, or map
// iteration order. Layout keeps nodes and edges in source order and
// flattens curves with a fixed segment count; number formatting is
// cplot's (shortest round-trip). Golden tests target the SVG output.
#pragma once

#include <string>
#include <string_view>

#include <cplot/scene.hpp>
#include <cplot/theme.hpp>
#include <cworks/limits.hpp>

#include "diagram.hpp"
#include "error.hpp"

namespace cdiagram {

/// The colour scheme a diagram renders in. The overall page background is
/// always transparent; `mode` selects the element colours so the same
/// diagram reads well on a light page, a dark page, or in print.
enum class ThemeMode { Light, Dark, Grayscale };

/// One region a builder drew, named the way the *source* names things rather
/// than by a model identity — a node id, a subgraph id, or `#<n>` for the nth
/// connection in source order. Keeping the raw key here means a builder needs
/// no knowledge of the object model to report its geometry; `scene_map()` does
/// the resolving in one place.
struct DrawnRegion {
    std::string key;
    /// A `cdiagram::HitRole`, carried as an int so this header stays free of
    /// the object model it would otherwise have to include.
    int role = 0;
    cplot::RectF bounds;
    /// The real outline, when a rectangle would misrepresent it — a pie
    /// wedge, a radar polygon. Empty means the bounds are the shape. Carried
    /// through to cdiagram::HitRegion, which explains why it exists.
    std::vector<cplot::Point> shape{};
    /// The exact centreline for a stroked connection. Empty for bounded and
    /// polygonal objects. Keeping this separate from `shape` prevents a route
    /// from being mistaken for a closed polygon by hit testing.
    std::vector<cplot::Point> path{};
};

/// Layout and styling controls. `theme` supplies fonts and layout metrics
/// (reuse cplot::Theme::print()); `mode` selects the colour scheme. The
/// engine adds diagram-specific spacing on top.
struct RenderOptions {
    cplot::Theme theme = cplot::Theme::print();
    /// The registry name that produced `theme`. Most themes provide layout
    /// metrics while the mode owns semantic diagram colours; names with a
    /// stronger rendering contract, such as `monochrome`, are resolved by
    /// the diagram style layer.
    std::string theme_name = "print";
    ThemeMode mode = ThemeMode::Light;

    /// Optional, non-owning sink for non-fatal diagnostics: a line the
    /// parser recognised but did not render (an unsupported directive, a
    /// flattened control block). Left null on the normal render path — the
    /// diagram still lays out; nothing is recorded. `check()` attaches a
    /// sink so it can report every ignored construct, not just the first
    /// fatal error. The charter forbids silently dropping input, so a
    /// parser that skips a line MUST diagnose it here.
    cworks::Diagnostics* diagnostics = nullptr;

    /// Optional, non-owning sink recording where each object was drawn.
    ///
    /// A rendered diagram is a picture, and a picture cannot be clicked:
    /// nothing in the output says which shape is which object. A builder with
    /// a sink attached reports the region it drew for each thing it drew, and
    /// `scene_map()` (scene_map.hpp) resolves those to model identities. Left
    /// null on the normal render path, which therefore pays nothing.
    std::vector<DrawnRegion>* regions = nullptr;
};

/// Parse `source` and lay it out into a ready-to-render Scene. Throws
/// cdiagram::Error on an unknown diagram type, a syntax error, or a
/// diagram type the engine does not yet support (the message names the
/// type). The returned Scene has its natural size in CSS pixels; the
/// caller sizes physical output through cplot.
cplot::Scene render(std::string_view source, const RenderOptions& options = {});

/// Validate `source` through the real layout path — for `ckwrite check`.
/// Returns one Error-severity diagnostic per problem (empty when the diagram
/// is well formed). Never throws. Interactive editors should prepare a scene
/// once and publish its retained diagnostics instead of calling this beside a
/// visible render and paying for the same layout twice.
cworks::Diagnostics check(std::string_view source);

} // namespace cdiagram
