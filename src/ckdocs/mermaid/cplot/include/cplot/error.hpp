// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The library's exception type. It lives in its own header because the
// scene and render core throws it too, and that tier must not include
// the chart engine's <cplot/figure.hpp> — which reaches the columnar
// data engine — merely to name an exception.
#pragma once

#include <cworks/error.hpp>

namespace cplot {

/// Exception type used across the library. Derives from cworks::Error
/// so `catch (const cworks::Error&)` covers the whole suite.
///
/// Inheriting the base constructors is what lets a throw site carry a
/// structured cworks::AppError: a message-only constructor of its own would
/// silently make every cplot refusal reach a frontend as CWORKS_E_UNKNOWN, no
/// matter what the site knew.
class Error : public cworks::Error {
public:
    using cworks::Error::Error;
};

} // namespace cplot
