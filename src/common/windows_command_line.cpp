// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/windows.hpp"
#include <stdexcept>

namespace ckgit {
bool isWindowsFileComponent(std::wstring_view component) {
  if (component.empty() || component.size() > 255 || component.back() == L'.' || component.back() == L' ')
    return false;
  for (const auto c : component)
    if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) return false;
  std::wstring base(component.substr(0, component.find(L'.')));
  while (!base.empty() && base.back() == L' ') base.pop_back();
  for (auto& c : base) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
  if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
      base == L"CONIN$" || base == L"CONOUT$") return false;
  if (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT"))) {
    const auto digit = base.back();
    if ((digit >= L'1' && digit <= L'9') || digit == L'\u00b9' || digit == L'\u00b2' || digit == L'\u00b3') return false;
  }
  return true;
}
std::wstring windowsCommandLine(const std::vector<std::wstring>& arguments) {
  if (arguments.empty() || arguments.front().empty()) throw std::invalid_argument("empty command");
  std::wstring result;
  for (const auto& argument : arguments) {
    if (argument.find(L'\0') != std::wstring::npos) throw std::invalid_argument("NUL in command argument");
    if (!result.empty()) result += L' ';
    result += L'"';
    std::size_t slashes = 0;
    for (const auto c : argument) {
      if (c == L'\\') { ++slashes; continue; }
      result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
      slashes = 0;
      result += c;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
  }
  if (result.size() >= 32767) throw std::invalid_argument("Windows command line exceeds 32766 characters");
  return result;
}
}
