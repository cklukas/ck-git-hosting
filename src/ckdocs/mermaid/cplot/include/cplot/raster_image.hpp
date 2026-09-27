// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The one pixel buffer of the library: what the raster renderer produces,
// what the picture decoders return, and what a scene's ImageItem draws.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cplot {

/// 8-bit RGBA image, rows top to bottom. Colour components are *not*
/// premultiplied: a half-covered black edge is (0, 0, 0, 128), which is
/// what PNG stores and what a compositor expects.
///
/// It has a header of its own rather than living beside the raster
/// renderer because it is that renderer's result, the result of every
/// picture decoder (<cplot/raster.hpp>, <cplot/jpeg.hpp>) *and* the payload
/// of an ImageItem — a scene can hold a picture, and a rendered figure is a
/// picture. One type for all of them means a figure composites into another
/// scene without a conversion step.
struct RasterImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;

    std::uint8_t* pixel(int x, int y) {
        return &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(x)) *
                     4];
    }
    const std::uint8_t* pixel(int x, int y) const {
        return &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(x)) *
                     4];
    }
};

} // namespace cplot
