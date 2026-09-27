// libcworks — shared foundation of the CWorks suite
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// RFC 4648 base64 with the standard alphabet: the encoding a `data:` URI, a
// clipboard payload or a document's embedded bytes travel in. Encoding is
// always padded and never line-wrapped — a data URI is one unbroken token.
// Decoding is strict: no whitespace, no missing or misplaced padding, no
// characters outside the alphabet, and no non-zero bits after the last
// character, so every byte string has exactly one encoding that decodes.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cworks {

/// `bytes` in base64.
std::string encode_base64(std::string_view bytes);

/// The bytes `text` encodes, or nullopt when it is not strict base64.
std::optional<std::string> decode_base64(std::string_view text);

} // namespace cworks
