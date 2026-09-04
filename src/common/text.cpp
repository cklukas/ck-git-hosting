// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/text.hpp"

#include <algorithm>

namespace ckgit {

bool isValidUtf8(std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const unsigned char first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7f) {
      ++index;
      continue;
    }
    std::size_t width = 0;
    unsigned int code_point = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      width = 2;
      code_point = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      width = 3;
      code_point = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      width = 4;
      code_point = first & 0x07;
    } else {
      return false;
    }
    if (index + width > value.size()) {
      return false;
    }
    for (std::size_t continuation = 1; continuation < width; ++continuation) {
      const unsigned char byte = static_cast<unsigned char>(value[index + continuation]);
      if ((byte & 0xc0) != 0x80) {
        return false;
      }
      code_point = (code_point << 6) | (byte & 0x3f);
    }
    if ((width == 3 && code_point < 0x800) || (width == 4 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) || code_point > 0x10ffff) {
      return false;
    }
    index += width;
  }
  return true;
}

bool hasControlCharacter(std::string_view value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 0x20 || character == 0x7f;
  });
}

}  // namespace ckgit
