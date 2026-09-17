// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
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

}  // namespace ckgit
