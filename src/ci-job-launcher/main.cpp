// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Windows substitutes package environment paths for the first lowbox process.
// Reset them before starting a tool; descendants inherit its token and job.
#include "ckgit/windows.hpp"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc < 3) throw std::invalid_argument("job launcher requires TEMP and an absolute executable");
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("query launcher token");
    DWORD isolated = 0, bytes = 0;
    const auto queried = GetTokenInformation(token, TokenIsAppContainer, &isolated, sizeof(isolated), &bytes);
    CloseHandle(token);
    if (!queried || !isolated) throw std::runtime_error("job launcher requires an AppContainer");
    const auto work = std::filesystem::current_path();
    const bool selected_directory = argc >= 5 && std::wstring_view(argv[2]) == L"--cwd";
    const int executable_index = selected_directory ? 4 : 2;
    const auto directory = selected_directory ? std::filesystem::path(argv[3]) : work;
    if (std::filesystem::path(argv[1]) != work / "tmp" || !std::filesystem::path(argv[executable_index]).is_absolute())
      throw std::invalid_argument("invalid job launcher paths");
    for (const auto name : {L"TEMP", L"TMP", L"TMPDIR"})
      if (_wputenv_s(name, argv[1])) throw std::runtime_error("set job temporary directory");
    for (const auto name : {L"USERPROFILE", L"APPDATA", L"LOCALAPPDATA"})
      if (_wputenv_s(name, (work / "home").c_str())) throw std::runtime_error("set job home directory");
    std::vector<std::wstring> arguments;
    for (int i = executable_index; i < argc; ++i) arguments.emplace_back(argv[i]);
    auto command = ckgit::windowsCommandLine(arguments);
    std::vector<HANDLE> handles;
    for (const auto which : {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
      const auto handle = GetStdHandle(which);
      if (!handle || handle == INVALID_HANDLE_VALUE) throw std::runtime_error("missing launcher standard handle");
      if (std::find(handles.begin(), handles.end(), handle) == handles.end()) handles.push_back(handle);
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> buffer(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) throw std::runtime_error("initialize launcher attributes");
    struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } owner{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
        handles.size() * sizeof(HANDLE), nullptr, nullptr)) throw std::runtime_error("restrict launcher handles");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(argv[executable_index], command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        nullptr, directory.c_str(), &startup.StartupInfo, &process))
      throw std::runtime_error("start job tool: Windows error " + std::to_string(GetLastError()));
    CloseHandle(process.hThread);
    const auto waited = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    const auto obtained = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (waited != WAIT_OBJECT_0 || !obtained) throw std::runtime_error("wait for job tool");
    return static_cast<int>(code);
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 125; }
}
