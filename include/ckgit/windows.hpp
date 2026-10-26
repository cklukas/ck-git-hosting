// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <mutex>

namespace ckgit {
// MS CRT command-line encoding, including empty arguments and trailing
// backslashes. This is not shell quoting: never pass its output to cmd.exe.
std::wstring windowsCommandLine(const std::vector<std::wstring>& arguments);
bool isWindowsFileComponent(std::wstring_view component);
#ifdef _WIN32
std::wstring utf8ToWide(std::string_view value);
std::string wideToUtf8(std::wstring_view value);
std::mutex& windowsProcessCreationMutex();
#endif
}
