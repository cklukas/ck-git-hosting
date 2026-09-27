// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/text.hpp"

#include <algorithm>
#include <cstdio>

namespace ckgit {

std::string formatDuration(std::uint64_t seconds) {
  const std::uint64_t hours = seconds / 3600;
  const std::uint64_t minutes = (seconds % 3600) / 60;
  const std::uint64_t secs = seconds % 60;
  char buffer[32];
  if (hours > 0) {
    std::snprintf(buffer, sizeof(buffer), "%llu:%02llu:%02llu", static_cast<unsigned long long>(hours),
                  static_cast<unsigned long long>(minutes), static_cast<unsigned long long>(secs));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%llu:%02llu", static_cast<unsigned long long>(minutes),
                  static_cast<unsigned long long>(secs));
  }
  return buffer;
}

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

std::string hexEncode(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    encoded.push_back(kDigits[byte >> 4]);
    encoded.push_back(kDigits[byte & 0x0f]);
  }
  return encoded;
}

std::optional<std::string> hexDecode(std::string_view hex, std::size_t maximum_bytes) {
  if (hex.size() % 2 != 0 || hex.size() / 2 > maximum_bytes) return std::nullopt;
  const auto nibble = [](unsigned char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
  };
  std::string decoded;
  decoded.reserve(hex.size() / 2);
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    const int high = nibble(static_cast<unsigned char>(hex[index]));
    const int low = nibble(static_cast<unsigned char>(hex[index + 1]));
    if (high < 0 || low < 0) return std::nullopt;
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  return decoded;
}

bool hasControlCharacter(std::string_view value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 0x20 || character == 0x7f;
  });
}

}  // namespace ckgit
