// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The library's error type. Every failure — a diagram whose type is
// unknown, a line that does not parse, a semantic problem — is reported
// as a cdiagram::Error, which derives from cworks::Error so
// `catch (const cworks::Error&)` covers the whole suite. Messages name
// the source line and, where useful, carry a caret under the offending
// token (the suite's error style).
#pragma once

#include <cworks/error.hpp>

namespace cdiagram {

/// Inheriting the base constructors is what lets a throw site carry a
/// structured cworks::AppError: a message-only constructor of its own would
/// silently make every cdiagram refusal reach a frontend as CWORKS_E_UNKNOWN,
/// no matter what the site knew — and it would also strand the codes cplot
/// now sets underneath the chart adapters, which cdiagram must pass through.
class Error : public cworks::Error {
public:
    using cworks::Error::Error;
};

} // namespace cdiagram
