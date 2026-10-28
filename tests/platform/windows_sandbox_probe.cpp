// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Opt-in native qualification: creates disposable AppContainer profiles.
#include "ckgit/windows_sandbox.hpp"
#include "ckgit/windows_ci_executor.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/temp_directory.hpp"
#include "ckgit/windows.hpp"
#include "ckgit/socket.hpp"
#include <windows.h>
#include <userenv.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>

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
    if (argc == 2 && std::wstring_view(argv[1]) == L"--recovery-wait") {
      std::cout << GetCurrentProcessId() << '\n' << std::flush;
      Sleep(60000);
      return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--recovery-fixture") {
      const fs::path parent(argv[2]);
      ckgit::WindowsRunnerSession session(parent);
      fs::create_directory(session.root() / "jobs");
      ckgit::WindowsSandbox sandbox(session.root() / "jobs");
      const auto executable = sandbox.tools() / "probe.exe";
      fs::copy_file(fs::path(argv[0]), executable);
      fs::copy_file(fs::path(argv[0]).parent_path() / "ckgit-job-launcher.exe", sandbox.tools() / "ckgit-job-launcher.exe");
      sandbox.sealTools();
      require(CreateSymbolicLinkW((sandbox.work() / "outside-link").c_str(), (parent / "outside").c_str(),
          SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE,
          "create recovery reparse-point fixture");
      ckgit::ProcessOptions options;
      options.timeout = std::chrono::seconds(60);
      std::string output;
      options.on_output = [&](std::string_view part) {
        output += part;
        if (output.find('\n') == std::string::npos || fs::exists(parent / "ready")) return;
        std::ofstream manifest(parent / "ready.new");
        manifest << ckgit::wideToUtf8(session.root().filename().native()) << '\n'
                 << ckgit::wideToUtf8(sandbox.work().parent_path().filename().native()) << '\n' << output;
        manifest.close();
        fs::rename(parent / "ready.new", parent / "ready");
      };
      const auto result = ckgit::runWindowsSandboxProcess(sandbox,
          {ckgit::wideToUtf8(executable.native()), "--recovery-wait"}, options, {});
      throw std::runtime_error("recovery fixture must be killed; child exit=" + std::to_string(result.exit_code));
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--recovery-test") {
      const auto parent = ckgit::createTempDirectory(fs::path(argv[2]), "recovery-test-");
      struct Cleanup {
        fs::path path;
        ~Cleanup() {
          try {
            { ckgit::WindowsRunnerSession recovery(path); }
            fs::remove_all(path);
          } catch (const std::exception& error) {
            std::cerr << "Preserving recovery fixture " << path << ": " << error.what() << '\n';
          }
        }
      } cleanup{parent};
      const auto unknown = parent / (L"runner-session-" + std::wstring(32, L'a'));
      fs::create_directory(unknown);
      std::ofstream(unknown / "unowned") << "preserve";
      const auto malformed = parent / (L"runner-session-" + std::wstring(32, L'b'));
      fs::create_directory(malformed);
      std::ofstream(malformed / ".ckgit-resource") << "not a resource record\n";
      fs::create_directory(parent / "outside");
      std::ofstream(parent / "outside" / "sentinel") << "must survive cleanup";
      ckgit::WindowsRunnerSession live(parent);
      auto command = ckgit::windowsCommandLine({argv[0], L"--recovery-fixture", parent.native()});
      STARTUPINFOW startup{}; startup.cb = sizeof(startup);
      PROCESS_INFORMATION process{};
      require(CreateProcessW(argv[0], command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
          nullptr, parent.c_str(), &startup, &process) != FALSE, "start recovery broker fixture");
      CloseHandle(process.hThread);
      struct Broker {
        HANDLE handle;
        ~Broker() { TerminateProcess(handle, 99); WaitForSingleObject(handle, 5000); CloseHandle(handle); }
      } broker{process.hProcess};
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (!fs::exists(parent / "ready") && std::chrono::steady_clock::now() < deadline &&
          WaitForSingleObject(broker.handle, 50) == WAIT_TIMEOUT) {}
      require(fs::exists(parent / "ready"), "LPAC child readiness before crash");
      std::ifstream manifest(parent / "ready");
      std::string session_name, profile_name, pid;
      std::getline(manifest, session_name); std::getline(manifest, profile_name); std::getline(manifest, pid);
      manifest.close();
      const auto abandoned = parent / ckgit::utf8ToWide(session_name);
      const auto child = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::stoul(pid)));
      require(child != nullptr && WaitForSingleObject(child, 0) == WAIT_TIMEOUT, "LPAC child is live before broker crash");
      require(TerminateProcess(broker.handle, 99) != FALSE && WaitForSingleObject(broker.handle, 5000) == WAIT_OBJECT_0,
          "terminate fixture broker without destructors");
      const auto killed = WaitForSingleObject(child, 5000); CloseHandle(child);
      require(killed == WAIT_OBJECT_0, "broker crash kills LPAC child");
      require(fs::exists(abandoned / "jobs" / ckgit::utf8ToWide(profile_name) / ".ckgit-resource"), "crash leaves durable profile record");
      const auto locked_file = abandoned / "jobs" / ckgit::utf8ToWide(profile_name) / "work" / "held-open";
      const auto held = CreateFileW(locked_file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      require(held != INVALID_HANDLE_VALUE, "hold abandoned output open during cleanup");
      bool cleanup_failed = false;
      try { ckgit::WindowsRunnerSession blocked(parent); }
      catch (const std::exception&) { cleanup_failed = true; }
      CloseHandle(held);
      require(cleanup_failed && fs::exists(abandoned / ".ckgit-resource") &&
          fs::exists(abandoned / "jobs" / ckgit::utf8ToWide(profile_name) / ".ckgit-resource"),
          "partial cleanup keeps session and sandbox records for retry");
      {
        ckgit::WindowsRunnerSession restart(parent);
        require(!fs::exists(abandoned), "restart removes recorded abandoned session and sandbox");
        require(fs::is_regular_file(parent / "outside" / "sentinel"), "recovery must not traverse a directory reparse point");
        require(fs::exists(live.root() / ".ckgit-resource"), "restart preserves exclusively locked live session");
        require(fs::exists(unknown / "unowned") && fs::exists(malformed / ".ckgit-resource"), "restart preserves unknown and invalid records");
        PSID sid = nullptr;
        const auto recreated = CreateAppContainerProfile(ckgit::utf8ToWide(profile_name).c_str(), L"Recovery verification", L"Recovery verification", nullptr, 0, &sid);
        require(SUCCEEDED(recreated), "recovery removed OS profile, allowing the name to be recreated");
        FreeSid(sid);
        require(SUCCEEDED(DeleteAppContainerProfile(ckgit::utf8ToWide(profile_name).c_str())), "remove verification profile");
      }
      { ckgit::WindowsRunnerSession repeated(parent); }
      fs::path retained_root;
      {
        ckgit::WindowsRunnerSession retained_session(parent);
        fs::create_directory(retained_session.root() / "jobs");
        ckgit::WindowsSandbox retained(retained_session.root() / "jobs");
        retained.retainWorkspace();
        retained_root = retained.work().parent_path();
        std::ofstream(retained.work() / "evidence") << "keep scratch";
      }
      {
        ckgit::WindowsRunnerSession restart(parent);
        require(fs::is_regular_file(retained_root / "work" / "evidence"), "restart honors retained workspace records");
      }
      std::cout << "Broker crash, LPAC child termination, recorded profile/workspace recovery, live/unknown/retained preservation, reparse boundaries and repeat startup passed\n";
      return 0;
    }
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
    if (argc > 1 && std::wstring_view(argv[1]) == L"--empty-cache-child") {
      const auto cache = _wgetenv(L"COMPILER_CACHE");
      require(cache && fs::is_empty(fs::path(cache)), "another job must start with an independent cache");
      std::ofstream(fs::path(cache) / "second-job") << "independent output";
      return 0;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--wait") {
      std::cout << "ready" << std::flush;
      Sleep(30000);
      return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--path-child") {
      // Diagnose absolute-path compatibility without granting ancestor access.
      // A successful directory change does not imply GetLongPathName can walk
      // the same path: tools may use both APIs before starting a compiler.
      const fs::path original = fs::current_path();
      for (fs::path path(argv[2]); !path.empty();) {
        wchar_t buffer[32768]{};
        SetLastError(ERROR_SUCCESS);
        const auto attributes = GetFileAttributesW(path.c_str());
        const auto attribute_error = GetLastError();
        SetLastError(ERROR_SUCCESS);
        const auto full = GetFullPathNameW(path.c_str(), 32768, buffer, nullptr);
        const auto full_error = GetLastError();
        SetLastError(ERROR_SUCCESS);
        const auto long_path = GetLongPathNameW(path.c_str(), buffer, 32768);
        const auto long_error = GetLastError();
        SetLastError(ERROR_SUCCESS);
        const auto changed = SetCurrentDirectoryW(path.c_str());
        const auto change_error = GetLastError();
        require(SetCurrentDirectoryW(original.c_str()) != FALSE, "restore diagnostic working directory");
        std::cout << ckgit::wideToUtf8(path.native())
                  << " attributes=" << attributes << "/" << attribute_error
                  << " full=" << full << "/" << full_error
                  << " long=" << long_path << "/" << long_error
                  << " chdir=" << changed << "/" << change_error << "\n";
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
      }
      return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--path-diagnostics") {
      ckgit::WindowsSandbox sandbox{fs::path(argv[2])};
      const auto broker_station = GetProcessWindowStation();
      const auto broker_desktop = GetThreadDesktop(GetCurrentThreadId());
      {
        ckgit::WindowsSandboxDesktop first(sandbox);
        require(GetProcessWindowStation() == broker_station &&
                    GetThreadDesktop(GetCurrentThreadId()) == broker_desktop,
                "desktop allocation must restore broker process and thread associations");
        bool collision = false;
        try { ckgit::WindowsSandboxDesktop duplicate(sandbox); }
        catch (const std::system_error& error) { collision = error.code().value() == ERROR_ALREADY_EXISTS; }
        require(collision, "desktop allocation must never reopen an existing station");
      }
      { ckgit::WindowsSandboxDesktop reused_after_close(sandbox); }
      const auto executable = sandbox.tools() / "path-probe.exe";
      fs::copy_file(fs::path(argv[0]), executable);
      fs::copy_file(fs::path(argv[0]).parent_path() / "ckgit-job-launcher.exe",
                    sandbox.tools() / "ckgit-job-launcher.exe");
      sandbox.sealTools();
      const auto result = ckgit::runWindowsSandboxProcess(sandbox,
          {ckgit::wideToUtf8(executable.native()), "--path-child", ckgit::wideToUtf8(sandbox.work().native())},
          ckgit::ProcessOptions{}, {});
      std::cout << result.output << "diagnostic child exit=" << result.exit_code
                << " timeout=" << result.timed_out << "\n";
      require(result.exit_code == 0 && !result.timed_out, "LPAC path diagnostics failed");
      return 0;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--child") return child(argc, argv);
    require(argc == 5, "usage: probe TEMP_PARENT MSVC_HOSTX64_X64 CMAKE_ROOT KERNEL32_LIB");
    ckgit::WindowsSandbox sandbox{fs::path(argv[1])};
    ckgit::WindowsSandbox other{fs::path(argv[1])};
    ckgit::WindowsCiToolchain identity_fixture;
    identity_fixture.root = sandbox.work() / "fingerprint";
    fs::create_directories(identity_fixture.root / "bin");
    std::ofstream(identity_fixture.root / "bin" / "compiler.exe") << "first compiler";
    const auto first_identity = ckgit::windowsCiToolchainFingerprint(identity_fixture);
    std::ofstream(identity_fixture.root / "bin" / "compiler.exe") << "other compiler";
    require(first_identity != ckgit::windowsCiToolchainFingerprint(identity_fixture),
            "toolchain identity includes tool contents");
    std::ofstream(identity_fixture.root / "bin" / "compiler.exe") << "first compiler";
    identity_fixture.environment["SDK"] = "v2";
    require(first_identity != ckgit::windowsCiToolchainFingerprint(identity_fixture),
            "toolchain identity includes configured environment");
    identity_fixture.environment.clear();
    identity_fixture.binary_directories = {".", "bin"};
    const auto search_identity = ckgit::windowsCiToolchainFingerprint(identity_fixture);
    identity_fixture.binary_directories = {"bin", "."};
    require(search_identity != ckgit::windowsCiToolchainFingerprint(identity_fixture),
            "toolchain identity includes executable search order");
    identity_fixture.binary_directories = {"bin"};
    const auto relocated = sandbox.work() / "fingerprint-relocated";
    fs::rename(identity_fixture.root, relocated);
    identity_fixture.root = relocated;
    require(first_identity == ckgit::windowsCiToolchainFingerprint(identity_fixture),
            "identical toolchains can reuse caches after staging relocation");
    const auto cache_path = [&](std::string_view project, std::string_view job,
                                std::string_view tools, std::string_view name) {
      return ckgit::windowsCiCacheDirectory(sandbox.work(), project, job, tools, name);
    };
    const auto base_cache = cache_path("project", "job", first_identity, "compiler");
    require(base_cache != cache_path("Project", "job", first_identity, "compiler") &&
            base_cache != cache_path("project", "Job", first_identity, "compiler") &&
            base_cache != cache_path("project", "job", search_identity, "compiler") &&
            base_cache != cache_path("project", "job", first_identity, "Compiler"),
            "persistent cache identity separates projects, jobs, toolchains and case-sensitive cache names");
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
    ckgit::CiJob second_job;
    second_job.name = "other-job";
    second_job.steps.push_back({"separate cache", {"probe.exe", "--empty-cache-child"}, {}, {}});
    prepared.workflow.jobs.push_back(second_job);
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
    const auto toolchain_identity = ckgit::windowsCiToolchainFingerprint(toolchain);
    const auto stored_cache = ckgit::windowsCiCacheDirectory(execution.cache_root,
        "fixture", "windows", toolchain_identity, "compiler");
    fs::create_directories(stored_cache);
    std::ofstream(stored_cache / "seed") << "cached input";
    ExecutorSource source;
    ExecutorSink sink;
    const auto record = ckgit::executeWindowsCiJob(prepared, execution, toolchain, source, sink);
    if (record.status != ckgit::CiRunStatus::Success)
      throw std::runtime_error("native executor failed: " + record.detail + "\n" + sink.logs);
    require(record.execution && record.execution->isolation == "lpac" && record.execution->network == "denied" &&
        record.execution->memory_bytes == ckgit::kWindowsSandboxMemoryBytes &&
        record.execution->process_limit == ckgit::kWindowsSandboxProcessLimit && record.execution->timeout_seconds == 60,
        "executor reports actually applied LPAC limits");
    require(source.checkouts == 2 && sink.started && sink.steps == 4 && sink.artifact && sink.pages,
        "native executor checkout, logs, artifacts and Pages lifecycle");
    require(record.parent_run_id == "parent-fixture" && record.workflow_job == "windows" && !sink.logs.empty(),
        "native executor child identity and live logs");
    require(fs::is_regular_file(stored_cache / "written"), "native executor cache persistence");
    const auto other_cache = ckgit::windowsCiCacheDirectory(execution.cache_root,
        "fixture", "other-job", toolchain_identity, "compiler");
    require(fs::is_regular_file(other_cache / "second-job") && !fs::exists(other_cache / "written") &&
            !fs::exists(stored_cache / "second-job"), "native executor stores each job cache independently");
    require(fs::is_empty(execution.build_root), "native executor removes scratch");
    // This fixture deliberately stages no PowerShell. A default script must
    // request pwsh, fail clearly, and never fall back to a host shell.
    prepared.workflow.jobs[0].steps = {{"script", {}, "exit 0", {}}};
    ExecutorSource missing_shell_source;
    ExecutorSink missing_shell_sink;
    const auto missing_shell = ckgit::executeWindowsCiJob(
        prepared, execution, toolchain, missing_shell_source, missing_shell_sink);
    require(!missing_shell.execution, "setup failure must not claim applied process isolation");
    require(missing_shell.status == ckgit::CiRunStatus::Error &&
            missing_shell.detail == "tool is not staged: pwsh",
            "Windows default shell must be staged PowerShell 7");
    require(missing_shell_sink.steps == 0 && !missing_shell_sink.artifact && !missing_shell_sink.pages,
            "missing shell cannot execute or publish outputs");
    require(fs::is_empty(execution.build_root), "missing shell removes scratch");
    std::cout << "LPAC native executor lifecycle passed\n";
    std::cout << "LPAC native build qualification passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
