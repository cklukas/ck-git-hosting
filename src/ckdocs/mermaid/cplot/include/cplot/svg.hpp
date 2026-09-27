// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <utility>

#include <cworks/task.hpp>

#include "scene.hpp"

namespace cplot {

/// Renders a Scene into a deterministic, standalone SVG document.
/// No timestamps, no random IDs — identical scenes yield identical bytes.
class SvgRenderer {
public:
    struct Options {
        /// Prefix for every generated element id (e.g. `clipPath` ids). Empty
        /// keeps the historical bare ids and byte-identical output. Set a unique
        /// prefix when several SVGs are inlined into one HTML document so their
        /// document-global ids cannot collide.
        std::string id_prefix{};
        /// Polled between scene items and while an embedded picture is
        /// encoded; a request throws the structured Cancelled error and no
        /// document is returned. Borrowed for each render call.
        const cworks::CancelToken* cancellation = nullptr;
    };

    SvgRenderer() = default;
    explicit SvgRenderer(Options options) : options_(std::move(options)) {}

    std::string render(const Scene& scene) const;

private:
    Options options_;
};

} // namespace cplot
