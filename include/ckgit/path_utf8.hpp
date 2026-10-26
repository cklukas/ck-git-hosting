// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <stdexcept>
#include "ckgit/text.hpp"

namespace ckgit {
inline std::filesystem::path pathFromUtf8(std::string_view value) {
  if (!isValidUtf8(value) || value.find('\0') != std::string_view::npos)
    throw std::invalid_argument("invalid UTF-8 filesystem path");
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
inline std::string pathToUtf8(const std::filesystem::path& value) {
  const auto bytes = value.u8string();
  return std::string(bytes.begin(), bytes.end());
}
}
