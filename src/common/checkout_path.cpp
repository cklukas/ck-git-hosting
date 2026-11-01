// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/metadata_store.hpp"
#include "ckgit/text.hpp"
#include <algorithm>
#include <stdexcept>
namespace ckgit {
namespace {
constexpr std::size_t kMaximumReportedPathBytes = 256;
bool isSafeReportedPath(std::string_view path) {
  return !path.empty() && path.size() <= kMaximumReportedPathBytes && isValidUtf8(path) &&
         std::all_of(path.begin(), path.end(), [](unsigned char character) {
           return character >= 0x20 && character != 0x7f;
         });
}

std::optional<std::string> decodeCheckoutPath(std::string_view token) {
  if (token.empty() || token.size() > kMaximumReportedPathBytes * 2 || token.size() % 2 != 0) {
    return std::nullopt;
  }
  const auto hex_value = [](unsigned char character) -> int {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    return -1;
  };
  std::string decoded;
  decoded.reserve(token.size() / 2);
  for (std::size_t index = 0; index < token.size(); index += 2) {
    const int high = hex_value(static_cast<unsigned char>(token[index]));
    const int low = hex_value(static_cast<unsigned char>(token[index + 1]));
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    decoded += static_cast<char>((high << 4) | low);
  }
  return isSafeReportedPath(decoded) ? std::optional<std::string>(std::move(decoded)) : std::nullopt;
}

}
std::string encodeCheckoutPath(std::string_view path) {
  if (!isSafeReportedPath(path)) {
    throw std::invalid_argument("reported checkout path must be printable, valid UTF-8, and at most 256 bytes");
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(path.size() * 2);
  for (const unsigned char character : path) {
    encoded += hex[character >> 4];
    encoded += hex[character & 0x0f];
  }
  return encoded;
}

bool isValidCheckoutPathToken(std::string_view token) {
  return decodeCheckoutPath(token).has_value();
}

std::optional<std::string> decodeCheckoutPathToken(std::string_view token) {
  return decodeCheckoutPath(token);
}

bool sameReportedCheckout(std::string_view left, std::string_view right) {
  const auto normalized = [](std::string_view input) {
    std::string value(input);
    const bool drive_path = value.size() >= 3 &&
        ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z')) &&
        value[1] == ':' && (value[2] == '\\' || value[2] == '/');
    if (drive_path || value.starts_with("\\\\")) std::replace(value.begin(), value.end(), '\\', '/');
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
  };
  const std::string a = normalized(left);
  const std::string b = normalized(right);
  if (a == b) {
    return true;
  }
  const auto final_component = [](std::string_view value) {
    const std::size_t slash = value.rfind('/');
    return slash == std::string_view::npos ? value : value.substr(slash + 1);
  };
  const bool a_is_name = a.find('/') == std::string_view::npos;
  const bool b_is_name = b.find('/') == std::string_view::npos;
  return (a_is_name != b_is_name) && !a.empty() && !b.empty() && final_component(a) == final_component(b);
}

}
