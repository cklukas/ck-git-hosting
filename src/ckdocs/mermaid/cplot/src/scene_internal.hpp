// ckplot — internal helpers of the scene and render core (not installed)
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The helpers the four backends share. Kept apart from internal.hpp
// because that header names the chart engine (Figure, ColorScale,
// MissingPolicy) and the render core must not depend on it.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <cworks/app_error.hpp>
#include <cworks/task.hpp>

#include "cplot/error.hpp"
#include "cplot/scene.hpp"

// The cplot *output-format* version, embedded in the PDF Producer
// string and the SVG "cplot-version:" tag together with a figure's
// compatibility profile. This is the render format's own version, not
// the product version: `ckplot --version` reports the one suite version
// (cworks::kVersion). The two are deliberately independent so the
// format tag stays byte-stable across suite releases (goldens depend on
// it); bump it only when the emitted SVG/PDF format itself changes.
#define CPLOT_VERSION "0.3.0"

namespace cplot::detail {

/// Throw the structured Cancelled error when `cancellation` was requested.
/// The backends and the PNG codec poll it between scene items, pages,
/// picture rows and compression chunks, so a request is honoured within one
/// of them rather than after a whole picture or document.
inline void check_cancelled(const cworks::CancelToken* cancellation) {
    if (cancellation != nullptr && cancellation->cancelled()) throw Error(cworks::cancelled());
}

/// Whether a picture has the pixels it claims to have.
///
/// Every backend asks before it reads, and draws nothing when the answer
/// is no. A buffer shorter than `width · height · 4` is a producer bug,
/// and reading past the end of it in three separate renderers is a worse
/// outcome than an absent picture in all three.
inline bool image_is_drawable(const RasterImage& image) {
    if (image.width <= 0 || image.height <= 0) return false;
    return image.rgba.size() >= static_cast<std::size_t>(image.width) *
                                    static_cast<std::size_t>(image.height) * 4u;
}

/// The same question for any picture content. A JpegPicture always has its
/// picture: it exists only once inspect_jpeg() accepted its stream.
inline bool image_is_drawable(const ImageContent& image) {
    if (const auto* pixels = std::get_if<RasterImage>(&image)) return image_is_drawable(*pixels);
    return true;
}

/// Deterministic float formatting for SVG ("12.5", "0", no trailing zeros).
std::string svg_num(double v);

/// The same formatting for a transform-matrix coefficient, which is a
/// ratio rather than a length and so needs more digits than the two a
/// pixel coordinate gets: a rotation's cosine written to two decimals
/// would be 0.71 instead of 0.707107, a 0.1% error applied to every
/// coordinate under it. Six decimals hold a unit coefficient to a
/// thousandth of a pixel across a thousand-pixel figure — the same order
/// as the coordinate precision it multiplies — and stay in plain decimal
/// notation, which PDF requires and SVG accepts.
std::string matrix_num(double v);

/// Decodes UTF-8 (invalid lead bytes are skipped). `byte_offsets`,
/// when given, receives the byte offset of every code point plus
/// the total size, so callers can slice the original string by
/// code-point index.
inline std::u32string decode_utf8(const std::string& s,
                                  std::vector<std::size_t>* byte_offsets = nullptr) {
    std::u32string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        int extra = 0;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1F;
            extra = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0F;
            extra = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07;
            extra = 3;
        } else {
            ++i;
            continue;
        }
        if (byte_offsets) byte_offsets->push_back(i);
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        }
        out.push_back(cp);
    }
    if (byte_offsets) byte_offsets->push_back(s.size());
    return out;
}

} // namespace cplot::detail
