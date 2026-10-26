// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Opt-in native qualification: creates disposable AppContainer profiles.
#include "ckgit/windows_sandbox.hpp"
#include "ckgit/windows_ci_executor.hpp"
#include "ckgit/hash.hpp"
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
class ExecutorSource : public ckgit::CiSourceProvider {
 public:
  unsigned checkouts = 0;
  void checkout(const ckgit::CiCheckoutRequest& request, const fs::path& destination) override {
    require(request.commit == std::string(40, 'a'), "executor must use pinned commit");
    ++checkouts;
    std::ofstream(destination / "probe.c") <<
      "__declspec(dllimport) void __stdcall ExitProcess(unsigned);\n"
      "void mainCRTStartup(void) { ExitProcess(0); }\n";
    fs::create_directory(destination / "public");
    std::ofstream(destination / "public" / "index.html") << "executor site";
  }
};
class ExecutorSink : public ckgit::CiResultSink {
 public:
  bool started = false, artifact = false, pages = false;
  unsigned steps = 0;
  std::string logs;
  void runStarted(const ckgit::CiRunRecord&) override { started = true; }
  void stepStarted(const ckgit::CiRunRecord&, std::size_t index) override { require(index == steps, "executor step order"); }
  void appendLog(std::size_t, std::string_view bytes) override { logs.append(bytes); }
  void stepFinished(const ckgit::CiRunRecord& record, std::size_t) override {
    ++steps; require(record.steps.size() == steps, "executor publishes completed step");
  }
  void heartbeat(const ckgit::CiRunRecord&) override {}
  void progress(const ckgit::CiRunRecord&) override {}
  bool cancelRequested() override { return false; }
  bool leaseLost() override { return false; }
  ckgit::CiArtifactRecord putArtifact(const ckgit::CiArtifact& declared, const fs::path& bundle,
      const std::string& note, bool) override {
    if (!note.empty()) throw std::runtime_error("executor artifact: " + note);
    require(fs::file_size(bundle) > 0, "executor artifact bytes");
    artifact = true;
    ckgit::CiArtifactRecord result;
    result.name = declared.name; result.bytes = fs::file_size(bundle); result.sha256 = ckgit::sha256HexOfFile(bundle);
    return result;
  }
  void putPagesSite(const fs::path& site) override { pages = fs::is_regular_file(site / "index.html"); }
  ckgit::CiRunRecord finish(ckgit::CiRunRecord record) override { return record; }
};
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
    if (argc > 1 && std::wstring_view(argv[1]) == L"--linger") { Sleep(30000); return 0; }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--background") {
      auto command = ckgit::windowsCommandLine({argv[0], L"--linger"});
      STARTUPINFOW startup{};
      startup.cb = sizeof(startup);
      startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
      startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
      startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
      PROCESS_INFORMATION process{};
      require(CreateProcessW(argv[0], command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
          nullptr, nullptr, &startup, &process) != FALSE, "start background helper with inherited output");
      std::cout << process.dwProcessId << std::endl;
      CloseHandle(process.hThread); CloseHandle(process.hProcess);
      return 0;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--executor-child") {
      const auto sister = _wgetenv(L"CKGIT_SISTER_SISTER");
      const auto cache = _wgetenv(L"COMPILER_CACHE");
      const auto command_processor = _wgetenv(L"COMSPEC");
      require(command_processor && fs::path(command_processor).is_absolute(), "executor system command processor");
      require(sister && fs::is_regular_file(fs::path(sister) / "probe.c"), "executor sister environment");
      require(cache && fs::is_regular_file(fs::path(cache) / "seed"), "executor restores persistent cache");
      require(fs::current_path().filename() == L"src", "executor working directory");
      std::ofstream(fs::path(cache) / "written") << "cache output";
      std::cout << "executor environment checked\n";
      return 0;
    }
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
    const auto finished_start = std::chrono::steady_clock::now();
    result = ckgit::runWindowsSandboxProcess(sandbox,
        {ckgit::wideToUtf8(executable.native()), "--background"}, options, {});
    require(result.exit_code == 0 && !result.timed_out &&
        std::chrono::steady_clock::now() - finished_start < std::chrono::seconds(5),
        "completed LPAC step must stop background helpers without waiting for timeout");
    const auto background = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::stoul(result.output)));
    if (background) {
      const auto stopped = WaitForSingleObject(background, 1000);
      CloseHandle(background);
      require(stopped == WAIT_OBJECT_0, "completed LPAC step left a background process");
    } else require(GetLastError() == ERROR_INVALID_PARAMETER, "background termination must be observable");
    std::cout << "LPAC live output, cancellation and background cleanup checks passed\n";
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
    ckgit::CiPreparedJob prepared;
    prepared.job.job_id = "executor-fixture";
    prepared.job.parent_run_id = "parent-fixture";
    prepared.job.workflow_job = "windows";
    prepared.job.project_name = "fixture";
    prepared.job.commit_id = std::string(40, 'a');
    prepared.job.ref = "refs/heads/main";
    prepared.is_default_branch = true;
    prepared.sisters.push_back({"sister", std::string(40, 'a'), {}});
    prepared.workflow.pages_path = "public";
    prepared.workflow.caches.push_back({"compiler", {"COMPILER_CACHE"}});
    ckgit::CiJob ci_job;
    ci_job.name = "windows";
    ci_job.steps.push_back({"environment", {"probe.exe", "--executor-child"}, {}, {}});
    ci_job.steps.push_back({"compile", {"cl.exe", "/nologo", "/GS-", "/Zl", "probe.c", "/Fe:result.exe", "/link",
        "/nodefaultlib", "/entry:mainCRTStartup", "/subsystem:console", "kernel32.lib"}, {}, {}});
    ci_job.steps.push_back({"execute", {"./result.exe"}, {}, {}});
    ci_job.artifact = ckgit::CiArtifact{"native-result", {"result.exe"}, 7};
    prepared.workflow.jobs.push_back(ci_job);
    ckgit::WindowsCiToolchain toolchain;
    toolchain.root = sandbox.tools();
    toolchain.launcher = fs::path(argv[0]).parent_path() / "ckgit-job-launcher.exe";
    toolchain.binary_directories = {"msvc", "cmake/bin", "."};
    toolchain.environment["LIB"] = "${TOOLS}";
    ckgit::CiExecutionOptions execution;
    execution.build_root = sandbox.work() / "executor-build";
    execution.cache_root = sandbox.work() / "executor-cache";
    execution.runner = "windows-fixture";
    execution.timeout_seconds = 60;
    fs::create_directories(execution.cache_root / "windows-x64" / "fixture" / "compiler");
    std::ofstream(execution.cache_root / "windows-x64" / "fixture" / "compiler" / "seed") << "cached input";
    ExecutorSource source;
    ExecutorSink sink;
    const auto record = ckgit::executeWindowsCiJob(prepared, execution, toolchain, source, sink);
    if (record.status != ckgit::CiRunStatus::Success)
      throw std::runtime_error("native executor failed: " + record.detail + "\n" + sink.logs);
    require(source.checkouts == 2 && sink.started && sink.steps == 3 && sink.artifact && sink.pages,
        "native executor checkout, logs, artifacts and Pages lifecycle");
    require(record.parent_run_id == "parent-fixture" && record.workflow_job == "windows" && !sink.logs.empty(),
        "native executor child identity and live logs");
    require(fs::is_regular_file(execution.cache_root / "windows-x64" / "fixture" / "compiler" / "written"), "native executor cache persistence");
    require(fs::is_empty(execution.build_root), "native executor removes scratch");
    std::cout << "LPAC native executor lifecycle passed\n";
    std::cout << "LPAC native build qualification passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
