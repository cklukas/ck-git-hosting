// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

namespace ckgit {

// Strict UTF-8: overlong encodings, surrogates, and code points above
// U+10FFFF are rejected so a value is safe to store and render later.
bool isValidUtf8(std::string_view value);

// True when any byte is an ASCII control character, including DEL.
bool hasControlCharacter(std::string_view value);

}  // namespace ckgit
