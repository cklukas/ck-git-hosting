// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Opt-in native qualification: creates disposable AppContainer profiles.
#include "ckgit/windows_sandbox.hpp"
#include "ckgit/windows.hpp"
#include "ckgit/socket.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void copyTools(const fs::path& source, const fs::path& destination) {
  fs::create_directories(destination);
  for (const auto& item : fs::recursive_directory_iterator(source)) {
    const auto attrs = GetFileAttributesW(item.path().c_str());
    require(attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT), "reparse point in toolchain source");
    const auto target = destination / item.path().lexically_relative(source);
    if (item.is_directory()) fs::create_directory(target);
    else if (item.is_regular_file()) fs::copy_file(item.path(), target);
    else throw std::runtime_error("special file in toolchain source");
  }
}
int child(int argc, wchar_t** argv) {
  require(argc == 5, "child arguments");
  const auto denied = CreateFileW(argv[2], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  require(denied == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED, "other job must be unreadable");
  const auto modified = CreateFileW(argv[0], GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  require(modified == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED, "tool must be read-only");
  std::ofstream("workspace-write.txt") << "job output";
  require(fs::is_regular_file("workspace-write.txt"), "workspace must be writable");
  wchar_t temp[32768]{};
  require(GetEnvironmentVariableW(L"TEMP", temp, 32768) > 0 && fs::path(temp) == fs::path(argv[4]), "job TEMP must match host-selected path");
  ckgit::net::Runtime network;
  const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == INVALID_SOCKET) {
    require(WSAGetLastError() == WSAEACCES, "LPAC socket denial must be WSAEACCES");
  } else {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(std::stoul(argv[3])));
    const auto connected = connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const auto error = WSAGetLastError();
    closesocket(socket);
    require(connected == SOCKET_ERROR && error == WSAEACCES, "LPAC must deny loopback network");
  }
  std::cout << "LPAC filesystem, write protection, TEMP and network checks passed\n";
  return 0;
}
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc > 1 && std::wstring_view(argv[1]) == L"--wait") {
      std::cout << "ready" << std::flush;
      Sleep(30000);
      return 0;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--child") return child(argc, argv);
    require(argc == 5, "usage: probe TEMP_PARENT MSVC_HOSTX64_X64 CMAKE_ROOT KERNEL32_LIB");
    ckgit::WindowsSandbox sandbox{fs::path(argv[1])};
    ckgit::WindowsSandbox other{fs::path(argv[1])};
    std::ofstream(other.work() / "other-job.txt") << "fixture";
    const auto executable = sandbox.tools() / "probe.exe";
    fs::copy_file(fs::path(argv[0]), executable);
    fs::copy_file(fs::path(argv[0]).parent_path() / "ckgit-job-launcher.exe", sandbox.tools() / "ckgit-job-launcher.exe");
    copyTools(fs::path(argv[2]), sandbox.tools() / "msvc");
    copyTools(fs::path(argv[3]), sandbox.tools() / "cmake");
    fs::copy_file(fs::path(argv[4]), sandbox.tools() / "kernel32.lib");
    sandbox.sealTools();
    ckgit::net::Runtime network;
    const auto listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "host loopback listener");
    struct Listener { SOCKET socket; ~Listener() { closesocket(socket); } } cleanup{listener};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0, "host loopback bind");
    int size = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0, "host loopback port");
    ckgit::ProcessOptions options;
    options.timeout = std::chrono::seconds(60);
    auto result = ckgit::runWindowsSandboxProcess(sandbox, {ckgit::wideToUtf8(executable.native()), "--child",
        ckgit::wideToUtf8((other.work() / "other-job.txt").native()), std::to_string(ntohs(address.sin_port)),
        ckgit::wideToUtf8((sandbox.work() / "tmp").native())}, options, {});
    std::cout << result.output;
    require(result.exit_code == 0 && !result.timed_out, "LPAC child qualification failed");
    std::string streamed;
    options.output_limit = 2;
    options.on_output = [&](std::string_view bytes) { streamed.append(bytes); };
    options.should_continue = [&] { return streamed != "ready"; };
    result = ckgit::runWindowsSandboxProcess(sandbox,
        {ckgit::wideToUtf8(executable.native()), "--wait"}, options, {});
    require(result.cancelled && !result.timed_out && streamed == "ready" && result.output_truncated,
        "LPAC live output and cancellation failed");
    options.on_output = {};
    options.should_continue = {};
    options.output_limit = 256 * 1024;
    std::cout << "LPAC live output and cancellation checks passed\n";
    // Real compiler -> linker -> produced executable, driven by CMake/NMake.
    // No CRT/SDK headers are needed by this minimal console executable.
    std::ofstream(sandbox.work() / "probe.c") <<
      "__declspec(dllimport) void __stdcall ExitProcess(unsigned);\n"
      "void mainCRTStartup(void) { ExitProcess(0); }\n";
    std::ofstream(sandbox.work() / "CMakeLists.txt") <<
      "cmake_minimum_required(VERSION 3.24)\nproject(lpac_probe NONE)\n"
      "add_custom_target(probe ALL COMMAND cl.exe /nologo /GS- /Zl "
      "${CMAKE_SOURCE_DIR}/probe.c /Fe:probe.exe /link /nodefaultlib "
      "/entry:mainCRTStartup /subsystem:console \"" << (sandbox.tools() / "kernel32.lib").generic_string() << "\")\n";
    const auto cmake = ckgit::wideToUtf8((sandbox.tools() / "cmake" / "bin" / "cmake.exe").native());
    std::map<std::wstring, std::wstring> env;
    wchar_t system[32768]{};
    require(GetSystemDirectoryW(system, 32768) > 0, "system directory");
    env[L"PATH"] = (sandbox.tools() / "msvc").wstring() + L";" + (sandbox.tools() / "cmake" / "bin").wstring() + L";" + system;
    for (const auto& command : std::vector<std::vector<std::string>>{
        {ckgit::wideToUtf8((sandbox.tools() / "msvc" / "cl.exe").native()), "/nologo", "/GS-", "/Zl", "probe.c",
          "/Fe:direct.exe", "/link", "/nodefaultlib", "/entry:mainCRTStartup", "/subsystem:console",
          ckgit::wideToUtf8((sandbox.tools() / "kernel32.lib").native())},
        {ckgit::wideToUtf8((sandbox.work() / "direct.exe").native())},
        {cmake, "--version"},
        {cmake, "-S", ".", "-B", "build", "-G", "NMake Makefiles"},
        {cmake, "--build", "build"},
        {ckgit::wideToUtf8((sandbox.work() / "build" / "probe.exe").native())}}) {
      result = ckgit::runWindowsSandboxProcess(sandbox, command, options, env);
      std::cout << result.output;
      if (result.exit_code != 0 || result.timed_out)
        throw std::runtime_error("LPAC tool failed: " + command.front() + " exit=" + std::to_string(result.exit_code) +
            (result.timed_out ? " timeout" : ""));
    }
    std::cout << "LPAC native build qualification passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
