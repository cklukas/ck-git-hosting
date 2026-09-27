// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ckgit {

// Strict UTF-8: overlong encodings, surrogates, and code points above
// U+10FFFF are rejected so a value is safe to store and render later.
bool isValidUtf8(std::string_view value);

// True when any byte is an ASCII control character, including DEL.
bool hasControlCharacter(std::string_view value);

// A compact clock rendering of a duration for CI timings: "M:SS" under an hour
// (e.g. "3:20"), "H:MM:SS" from an hour up (e.g. "1:02:03"). Shared by the web
// dashboard and the CLI so a run's elapsed or total time reads the same
// everywhere. Callers add any unit suffix ("min") themselves.
std::string formatDuration(std::uint64_t seconds);

// Lowercase hexadecimal, two digits per byte: the encoding every stored record
// and control response uses for free text (refs, details, names, notes), so
// arbitrary bytes travel as a single safe token.
std::string hexEncode(std::string_view bytes);
// Strict inverse of hexEncode: lowercase digits only, an even length, and at
// most `maximum_bytes` decoded bytes. std::nullopt for anything else.
std::optional<std::string> hexDecode(std::string_view hex, std::size_t maximum_bytes);

}  // namespace ckgit
