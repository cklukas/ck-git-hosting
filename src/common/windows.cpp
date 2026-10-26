// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/windows.hpp"
#include <windows.h>
#include <limits>
#include <stdexcept>

namespace ckgit {
std::mutex& windowsProcessCreationMutex() { static std::mutex mutex; return mutex; }
std::wstring utf8ToWide(std::string_view value) {
  if (value.empty()) return {};
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      value.find('\0') != std::string_view::npos) throw std::invalid_argument("invalid UTF-8 argument");
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!size) throw std::invalid_argument("invalid UTF-8 argument");
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size))
    throw std::runtime_error("UTF-8 conversion failed");
  return result;
}
std::string wideToUtf8(std::wstring_view value) {
  if (value.empty()) return {};
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      value.find(L'\0') != std::wstring_view::npos) throw std::invalid_argument("invalid UTF-16 argument");
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (!size) throw std::invalid_argument("invalid UTF-16 argument");
  std::string result(static_cast<std::size_t>(size), '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr))
    throw std::runtime_error("UTF-16 conversion failed");
  return result;
}
}
