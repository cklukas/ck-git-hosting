// libcworks — a located region of user-written text
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>

namespace cworks {

/// A half-open region of a text, in UTF-8 bytes from its start.
///
/// One type for the whole suite because the same region travels: a token
/// carries one out of the tokenizer, a syntax error carries it through an
/// engine's exception and across an application boundary (AppError), and an
/// editor finally puts its caret on it. Bytes rather than characters, because
/// bytes are the only unit every layer and every language on the far side of a
/// C boundary agrees on; a frontend converts to its own text index at the edge.
///
/// A zero `length` is a position rather than a range — the place where
/// something was expected and nothing stood, such as the end of `1+`.
struct SourceSpan {
    std::size_t offset = 0;
    std::size_t length = 0;

    bool operator==(const SourceSpan&) const = default;
};

} // namespace cworks
