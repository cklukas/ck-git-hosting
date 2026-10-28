// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/process.hpp"
#include "process_tools_internal.hpp"
#include "ckgit/windows.hpp"
#include "ckgit/windows_sandbox.hpp"
#include <windows.h>
#include <sddl.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ckgit {
namespace {
[[noreturn]] void fail(const char* message) {
  throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}
struct Handle {
  HANDLE value = nullptr;
  explicit Handle(HANDLE handle = nullptr) : value(handle) {}
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  void close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(value, nullptr)); }
};

void verifyLpacAccess(HANDLE token) {
  // TokenIsLessPrivilegedAppContainer is present in the SDK but the Win32
  // query returns ERROR_INVALID_PARAMETER on supported Windows builds.
  // Verify the defining access semantics instead: ALL_APPLICATION_PACKAGES
  // must contribute no rights, while ALL_RESTRICTED_APPLICATION_PACKAGES
  // contributes its distinct bit. A plain AppContainer receives both bits.
  Handle impersonation;
  if (!DuplicateToken(token, SecurityImpersonation, &impersonation.value)) fail("duplicate sandbox token for access check");
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"O:SYG:SYD:(A;;0x3;;;WD)(A;;0x1;;;AC)(A;;0x2;;;S-1-15-2-2)",
      SDDL_REVISION_1, &descriptor, nullptr)) fail("create LPAC access descriptor");
  GENERIC_MAPPING mapping{1, 2, 0, 3};
  alignas(PRIVILEGE_SET) unsigned char privileges[4096]{};
  DWORD bytes = sizeof(privileges), granted = 0;
  BOOL allowed = FALSE;
  const BOOL checked = AccessCheck(descriptor, impersonation.value, MAXIMUM_ALLOWED, &mapping,
      reinterpret_cast<PPRIVILEGE_SET>(privileges), &bytes, &granted, &allowed);
  const DWORD error = GetLastError();
  LocalFree(descriptor);
  if (!checked) { SetLastError(error); fail("verify LPAC access semantics"); }
  if (!allowed || granted != 2) throw std::runtime_error("child does not enforce LPAC access semantics");
}
struct Attributes {
  std::vector<unsigned char> storage;
  LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
  explicit Attributes(DWORD count = 1) {
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, count, 0, &bytes);
    storage.resize(bytes);
    list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(list, count, 0, &bytes)) fail("initialize process attributes");
  }
  ~Attributes() { DeleteProcThreadAttributeList(list); }
};
std::wstring commandLine(const std::vector<std::string>& arguments) {
  std::vector<std::wstring> wide;
  for (const auto& argument : arguments) wide.push_back(utf8ToWide(argument));
  return windowsCommandLine(wide);
}
// Resolve a concrete executable before CreateProcess so spaces cannot select
// a different executable. Batch files require an explicit shell and are refused.
std::wstring executable(const std::string& name) {
  const auto wide = utf8ToWide(name);
  std::wstring path(32768, L'\0');
  // SearchPath's default search includes the current directory, unlike the
  // POSIX PATH contract. Do not implicitly execute a tool from a checkout.
  const DWORD needed = GetEnvironmentVariableW(L"PATH", nullptr, 0);
  std::wstring search(needed ? needed : 1, L'\0');
  const DWORD copied = GetEnvironmentVariableW(L"PATH", search.data(), static_cast<DWORD>(search.size()));
  search.resize(copied < search.size() ? copied : 0);
  std::wstring safe_search;
  for (std::size_t start = 0; start < search.size();) {
    const auto end = search.find(L';', start);
    auto entry = search.substr(start, end == std::wstring::npos ? end : end - start);
    if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"') entry = entry.substr(1, entry.size() - 2);
    if (!entry.empty()) { if (!safe_search.empty()) safe_search += L';'; safe_search += entry; }
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  if (wide.find_first_of(L"/\\:") == std::wstring::npos && safe_search.empty())
    throw std::runtime_error("cannot resolve executable without PATH");
  const DWORD size = SearchPathW(safe_search.c_str(), wide.c_str(), L".exe", static_cast<DWORD>(path.size()), path.data(), nullptr);
  if (!size || size >= path.size()) fail("resolve executable");
  path.resize(size);
  const auto dot = path.rfind(L'.');
  auto extension = dot == std::wstring::npos ? L"" : path.substr(dot);
  for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
  if (extension != L".exe" && extension != L".com") throw std::invalid_argument("direct execution requires an .exe or .com executable");
  return path;
}
}

// Each captured child belongs to a non-inheritable kill-on-close job. Windows
// closes these handles on caller termination, including console termination.
std::string resolveProcessExecutable(const std::string& requested) {
  const auto name = selectedProcessProgram(requested);
  if (name.empty() || name.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid executable name");
  return wideToUtf8(executable(name));
}

void installChildTerminationForwarding() {}

ProcessResult runProcess(const std::vector<std::string>& arguments,
                         std::chrono::milliseconds timeout, std::size_t output_limit) {
  ProcessOptions options;
  options.timeout = timeout;
  options.output_limit = output_limit;
  return runProcess(arguments, options);
}

static ProcessResult runProcessImpl(const std::vector<std::string>& arguments, const ProcessOptions& options,
    const WindowsSandbox* sandbox, const std::map<std::wstring, std::wstring>& supplied_environment) {
  if (options.input_file && (sandbox || !options.input_file->is_absolute()))
    throw std::invalid_argument("host input file requires an absolute path and an unsandboxed transport process");
  if (sandbox && !options.environment.empty())
    throw std::invalid_argument("sandbox environment must use its explicit allowlist");
  auto command = commandLine(arguments);
  if (sandbox && !std::filesystem::path(utf8ToWide(arguments.front())).is_absolute())
    throw std::invalid_argument("sandbox executable must be an absolute host-selected path");
  const auto application = executable(arguments.front());
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  Handle read, write;
  if (!CreatePipe(&read.value, &write.value, &security, 0)) fail("create process pipe");
  if (!SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) fail("protect process pipe");
  Handle input(CreateFileW(options.input_file ? options.input_file->c_str() : L"NUL", GENERIC_READ,
                           options.input_file ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &security, OPEN_EXISTING, options.input_file ? FILE_FLAG_OPEN_REPARSE_POINT : 0, nullptr));
  if (input.value == INVALID_HANDLE_VALUE) fail("open process input");
  if (options.input_file) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileType(input.value) != FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(input.value, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
      throw std::runtime_error("process input must be a readable regular file");
  }
  Handle error;
  HANDLE stderr_handle = write.value;
  if (options.inherit_stderr) {
    if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE), GetCurrentProcess(),
                         &error.value, 0, TRUE, DUPLICATE_SAME_ACCESS)) fail("duplicate stderr");
    stderr_handle = error.value;
  }
  std::vector<HANDLE> inherited{input.value, write.value};
  if (options.inherit_stderr) inherited.push_back(stderr_handle);
  Attributes attributes(sandbox ? 3 : 1);
  if (!UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr)) fail("restrict inherited handles");
  SECURITY_CAPABILITIES capabilities{};
  std::vector<SID_AND_ATTRIBUTES> capability_sids;
  ULONG packages_policy = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;
  std::wstring environment_block;
  if (sandbox) {
    capabilities.AppContainerSid = sandbox->sid();
    for (auto sid : sandbox->capabilitySids()) capability_sids.push_back({sid, SE_GROUP_ENABLED});
    capabilities.Capabilities = capability_sids.data();
    capabilities.CapabilityCount = static_cast<DWORD>(capability_sids.size());
    if (!UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
        &capabilities, sizeof(capabilities), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,
        &packages_policy, sizeof(packages_policy), nullptr, nullptr)) fail("configure LPAC");
    auto environment = supplied_environment;
    std::wstring windows(32768, L'\0');
    const auto count = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (!count || count >= windows.size()) fail("resolve Windows directory");
    windows.resize(count);
    environment[L"SYSTEMROOT"] = windows;
    environment[L"WINDIR"] = windows;
    // NMake and other native build tools invoke the system command processor.
    // Resolve it from Windows, never from the broker's inherited environment.
    environment[L"COMSPEC"] = windows + L"\\System32\\cmd.exe";
    environment[L"SYSTEMDRIVE"] = std::filesystem::path(windows).root_name().wstring();
    environment[L"TEMP"] = (sandbox->work() / "tmp").wstring();
    environment[L"TMP"] = environment[L"TEMP"];
    environment[L"TMPDIR"] = environment[L"TEMP"];
    environment[L"USERPROFILE"] = (sandbox->work() / "home").wstring();
    environment[L"LOCALAPPDATA"] = environment[L"USERPROFILE"];
    environment[L"APPDATA"] = environment[L"USERPROFILE"];
    if (!environment.contains(L"PATH")) environment[L"PATH"] = sandbox->tools().wstring() + L";" + windows + L"\\System32";
    for (const auto& [key, value] : environment) {
      if (key.empty() || key.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::wstring::npos ||
          value.find(L'\0') != std::wstring::npos) throw std::invalid_argument("invalid sandbox environment");
      environment_block += key + L"=" + value;
      environment_block += L'\0';
    }
    environment_block += L'\0';
  } else if (!options.environment.empty()) {
    struct Less {
      bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
      }
    };
    std::map<std::wstring, std::wstring, Less> environment;
    const auto inherited = GetEnvironmentStringsW();
    if (!inherited) fail("read inherited process environment");
    struct Environment { wchar_t* value; ~Environment() { FreeEnvironmentStringsW(value); } } owner{inherited};
    for (const wchar_t* item = inherited; *item; item += wcslen(item) + 1) {
      const std::wstring entry(item);
      const auto equals = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
      if (equals != std::wstring::npos) environment[entry.substr(0, equals)] = entry.substr(equals + 1);
    }
    for (const auto& [key, value] : options.environment) {
      if (key.empty() || key.find('=') != std::string::npos || key.find('\0') != std::string::npos ||
          (value && value->find('\0') != std::string::npos)) throw std::invalid_argument("invalid process environment");
      const auto wide_key = utf8ToWide(key);
      environment.erase(wide_key);
      if (value) environment.emplace(wide_key, utf8ToWide(*value));
    }
    for (const auto& [key, value] : environment) {
      environment_block += key + L"=" + value;
      environment_block += L'\0';
    }
    environment_block += L'\0';
    if (environment.empty()) environment_block += L'\0';
  }
  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (!job.value) fail("create process job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (sandbox) {
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_JOB_MEMORY |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limits.BasicLimitInformation.ActiveProcessLimit = kWindowsSandboxProcessLimit;
    limits.JobMemoryLimit = static_cast<SIZE_T>(kWindowsSandboxMemoryBytes);
  }
  if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) fail("configure process job");
  if (sandbox) {
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
        JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES |
        JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    if (!SetInformationJobObject(job.value, JobObjectBasicUIRestrictions, &ui, sizeof(ui))) fail("restrict sandbox UI");
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.value;
  startup.StartupInfo.hStdOutput = write.value;
  startup.StartupInfo.hStdError = stderr_handle;
  startup.lpAttributeList = attributes.list;
  PROCESS_INFORMATION info{};
  const auto directory = sandbox ? sandbox->work().wstring() : std::wstring();
  auto desktop = sandbox ? sandbox->desktopName() : std::wstring();
  if (sandbox) startup.StartupInfo.lpDesktop = desktop.data();
  {
  const std::lock_guard lock(windowsProcessCreationMutex());
  if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
                      EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                      environment_block.empty() ? nullptr : environment_block.data(), sandbox ? directory.c_str() : nullptr,
                      &startup.StartupInfo, &info)) fail("start process");
  }
  Handle process(info.hProcess), thread(info.hThread);
  if (!AssignProcessToJobObject(job.value, process.value)) {
    const DWORD code = GetLastError();
    TerminateProcess(process.value, 127);
    WaitForSingleObject(process.value, INFINITE);
    SetLastError(code);
    fail("assign process job");
  }
  if (sandbox) {
    Handle token;
    if (!OpenProcessToken(process.value, TOKEN_QUERY | TOKEN_DUPLICATE, &token.value)) fail("inspect sandbox token");
    DWORD app = 0, bytes = 0;
    if (!GetTokenInformation(token.value, TokenIsAppContainer, &app, sizeof(app), &bytes)) fail("query AppContainer flag");
    if (!app) throw std::runtime_error("child is not an AppContainer");
    verifyLpacAccess(token.value);
    GetTokenInformation(token.value, TokenAppContainerSid, nullptr, 0, &bytes);
    std::vector<unsigned char> sid(bytes);
    if (!GetTokenInformation(token.value, TokenAppContainerSid, sid.data(), bytes, &bytes) ||
        !EqualSid(reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(sid.data())->TokenAppContainer, sandbox->sid()))
      throw std::runtime_error("sandbox SID mismatch");
    GetTokenInformation(token.value, TokenCapabilities, nullptr, 0, &bytes);
    std::vector<unsigned char> caps(bytes);
    if (!GetTokenInformation(token.value, TokenCapabilities, caps.data(), bytes, &bytes)) fail("inspect sandbox capabilities");
    const auto groups = reinterpret_cast<TOKEN_GROUPS*>(caps.data());
    if (groups->GroupCount != capability_sids.size()) throw std::runtime_error("sandbox has unexpected capabilities");
    for (DWORD i = 0; i < groups->GroupCount; ++i)
      if (std::none_of(capability_sids.begin(), capability_sids.end(), [&](const auto& expected) {
          return EqualSid(expected.Sid, groups->Groups[i].Sid) != FALSE;
        })) throw std::runtime_error("sandbox has unexpected capabilities");
  }
  if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) fail("resume process");
  write.close();
  error.close();
  input.close();
  ProcessResult result;
  const auto deadline = std::chrono::steady_clock::now() + options.timeout;
  std::array<char, 16384> buffer{};
  bool pipe_open = true;
  bool sandbox_descendants_stopped = false;
  try {
    while (true) {
      DWORD available = 0;
      if (pipe_open && !PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr)) {
        if (GetLastError() != ERROR_BROKEN_PIPE) fail("inspect process output");
        pipe_open = false;
      }
      if (available) {
        DWORD received = 0;
        if (!ReadFile(read.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &received, nullptr)) fail("read process output");
        if (options.on_output) options.on_output(std::string_view(buffer.data(), received));
        const auto keep = std::min<std::size_t>(received, options.output_limit - result.output.size());
        result.output.append(buffer.data(), keep);
        result.output_truncated |= keep != received;
      }
      const DWORD waited = WaitForSingleObject(process.value, 0);
      if (waited == WAIT_FAILED) fail("wait for process");
      if (sandbox && waited == WAIT_OBJECT_0 && !sandbox_descendants_stopped) {
        // A completed step must not leave background helpers holding stdout
        // open or modifying its workspace while the next step/publisher runs.
        if (!TerminateJobObject(job.value, 0)) fail("finish sandbox descendants");
        sandbox_descendants_stopped = true;
      }
      if (!pipe_open && waited == WAIT_OBJECT_0) break;
      const bool cancelled = options.should_continue && !options.should_continue();
      if (cancelled || std::chrono::steady_clock::now() >= deadline) {
        result.cancelled = cancelled;
        result.timed_out = !cancelled;
        if (!TerminateJobObject(job.value, cancelled ? 125 : 124)) fail("terminate process job");
        WaitForSingleObject(process.value, INFINITE);
        break;
      }
      if (!available) Sleep(10);
    }
  } catch (...) {
    TerminateJobObject(job.value, 125);
    WaitForSingleObject(process.value, INFINITE);
    throw;
  }
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process.value, &exit_code)) fail("read process exit code");
  result.exit_code = static_cast<int>(exit_code);
  if (sandbox) {
    if (!TerminateJobObject(job.value, 0)) fail("finish sandbox descendants");
    const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
      JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
      if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) fail("inspect sandbox descendants");
      if (accounting.ActiveProcesses == 0) break;
      if (std::chrono::steady_clock::now() >= cleanup_deadline) throw std::runtime_error("sandbox descendants did not stop");
      Sleep(10);
    }
  }
  return result;
}

ProcessResult runProcess(const std::vector<std::string>& requested, const ProcessOptions& requested_options) {
  const auto arguments = selectedProcessArguments(requested);
  const auto selected_options = selectedProcessOptions(requested, requested_options);
  const auto& options = selected_options ? *selected_options : requested_options;
  return runProcessImpl(arguments, options, nullptr, {});
}

ProcessResult runWindowsSandboxProcess(const WindowsSandbox& sandbox,
    const std::vector<std::string>& arguments, const ProcessOptions& options,
    const std::map<std::wstring, std::wstring>& environment,
    const std::filesystem::path& working_subdirectory) {
  if (arguments.empty() || !std::filesystem::path(utf8ToWide(arguments.front())).is_absolute())
    throw std::invalid_argument("sandbox requires an absolute executable");
  std::vector<std::string> command{wideToUtf8((sandbox.tools() / "ckgit-job-launcher.exe").native()),
      wideToUtf8((sandbox.work() / "tmp").native())};
  if (!working_subdirectory.empty()) {
    if (working_subdirectory.is_absolute() || working_subdirectory.has_root_path())
      throw std::invalid_argument("sandbox working directory must be relative");
    auto current = sandbox.work();
    for (const auto& part : working_subdirectory) {
      if (part == "..") throw std::invalid_argument("sandbox working directory escapes workspace");
      current /= part;
      const auto attributes = GetFileAttributesW(current.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
          !(attributes & FILE_ATTRIBUTE_DIRECTORY)) throw std::invalid_argument("invalid sandbox working directory");
    }
    command.push_back("--cwd");
    command.push_back(wideToUtf8(current.native()));
  }
  command.insert(command.end(), arguments.begin(), arguments.end());
  return runProcessImpl(command, options, &sandbox, environment);
}

struct ManagedProcess::State {
  Handle process, job;
  DWORD id = 0;
  std::optional<int> exit;
};
ManagedProcess::ManagedProcess(const std::vector<std::string>& requested) : state_(std::make_unique<State>()) {
  const auto arguments = selectedProcessArguments(requested);
  auto command = commandLine(arguments);
  const auto application = executable(arguments.front());
  state_->job.value = CreateJobObjectW(nullptr, nullptr);
  if (!state_->job.value) fail("create background process job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(state_->job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    fail("configure background process job");
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &security, OPEN_EXISTING, 0, nullptr));
  Handle output, error;
  const auto duplicate = [&](DWORD which, Handle& target) {
    const auto source = GetStdHandle(which);
    if (source && source != INVALID_HANDLE_VALUE && DuplicateHandle(GetCurrentProcess(), source,
        GetCurrentProcess(), &target.value, 0, TRUE, DUPLICATE_SAME_ACCESS)) return;
    target.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, 0, nullptr);
    if (target.value == INVALID_HANDLE_VALUE) fail("open background process output");
  };
  if (input.value == INVALID_HANDLE_VALUE) fail("open background process input");
  duplicate(STD_OUTPUT_HANDLE, output);
  duplicate(STD_ERROR_HANDLE, error);
  std::array<HANDLE, 3> handles{input.value, output.value, error.value};
  Attributes attributes;
  if (!UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
      handles.data(), sizeof(handles), nullptr, nullptr)) fail("restrict background process handles");
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.value;
  startup.StartupInfo.hStdOutput = output.value;
  startup.StartupInfo.hStdError = error.value;
  startup.lpAttributeList = attributes.list;
  PROCESS_INFORMATION info{};
  const std::lock_guard lock(windowsProcessCreationMutex());
  if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
      nullptr, nullptr, &startup.StartupInfo, &info)) fail("start background process");
  state_->process.value = info.hProcess;
  state_->id = info.dwProcessId;
  Handle thread(info.hThread);
  if (!AssignProcessToJobObject(state_->job.value, info.hProcess) || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
    const auto saved = GetLastError();
    TerminateProcess(info.hProcess, 127);
    WaitForSingleObject(info.hProcess, 5000);
    SetLastError(saved);
    fail("contain background process");
  }
}
ManagedProcess::~ManagedProcess() { stop(); }
unsigned long ManagedProcess::id() const { return state_->id; }
std::optional<int> ManagedProcess::poll() {
  if (state_->exit) return state_->exit;
  const auto result = WaitForSingleObject(state_->process.value, 0);
  if (result == WAIT_TIMEOUT) return std::nullopt;
  if (result != WAIT_OBJECT_0) fail("inspect background process");
  DWORD code = 0;
  if (!GetExitCodeProcess(state_->process.value, &code)) fail("read background process exit");
  state_->exit = static_cast<int>(code);
  TerminateJobObject(state_->job.value, code);
  return state_->exit;
}
void ManagedProcess::stop() noexcept {
  if (!state_ || state_->exit) return;
  TerminateJobObject(state_->job.value, 1);
  WaitForSingleObject(state_->process.value, 5000);
  DWORD code = 1;
  GetExitCodeProcess(state_->process.value, &code);
  state_->exit = static_cast<int>(code);
}

int spawnAttachedProcess(const std::vector<std::string>& requested) {
  const auto arguments = selectedProcessArguments(requested);
  auto command = commandLine(arguments);
  const auto application = executable(arguments.front());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  const std::lock_guard lock(windowsProcessCreationMutex());
  if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &info)) fail("start attached process");
  Handle process(info.hProcess), thread(info.hThread);
  return static_cast<int>(info.dwProcessId);
}

int execProcess(const std::vector<std::string>& requested) {
  const auto arguments = selectedProcessArguments(requested);
  if (arguments.empty()) throw std::invalid_argument("empty exec command");
  auto command = commandLine(arguments);
  const auto application = executable(arguments.front());
  std::array<Handle, 3> standard;
  const std::array<DWORD, 3> identifiers{STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  std::array<HANDLE, 3> inherited{};
  for (std::size_t i = 0; i < standard.size(); ++i) {
    if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(identifiers[i]), GetCurrentProcess(),
        &standard[i].value, 0, TRUE, DUPLICATE_SAME_ACCESS)) fail("duplicate shim standard handle");
    inherited[i] = standard[i].value;
  }
  Attributes attributes;
  if (!UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
      inherited.data(), sizeof(inherited), nullptr, nullptr)) fail("restrict shim handles");
  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (!job.value) fail("create shim process job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    fail("configure shim process job");
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = inherited[0];
  startup.StartupInfo.hStdOutput = inherited[1];
  startup.StartupInfo.hStdError = inherited[2];
  startup.lpAttributeList = attributes.list;
  PROCESS_INFORMATION info{};
  {
    const std::lock_guard lock(windowsProcessCreationMutex());
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &startup.StartupInfo, &info)) fail("start shim child");
  }
  Handle process(info.hProcess), thread(info.hThread);
  if (!AssignProcessToJobObject(job.value, process.value)) {
    const auto error = GetLastError();
    TerminateProcess(process.value, 127);
    WaitForSingleObject(process.value, INFINITE);
    SetLastError(error); fail("contain shim child");
  }
  if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) fail("resume shim child");
  if (WaitForSingleObject(process.value, INFINITE) != WAIT_OBJECT_0) fail("wait for shim child");
  DWORD code = 0;
  if (!GetExitCodeProcess(process.value, &code)) fail("read shim exit status");
  return static_cast<int>(code);
}

struct ProcessStream::State {
  Handle process, job, input, output, event;
  OVERLAPPED write_operation{};
  std::array<char, 65536> outgoing{};
  DWORD queued = 0;
  bool pending = false;
  std::optional<int> exit;
  int finishWrite() {
    if (!pending) return 1;
    DWORD transferred = 0;
    if (!GetOverlappedResult(input.value, &write_operation, &transferred, FALSE)) {
      if (GetLastError() == ERROR_IO_INCOMPLETE) return 0;
      pending = false;
      input.close();
      return -1;
    }
    pending = false;
    if (transferred != queued) { input.close(); return -1; }
    return 1;
  }
};
ProcessStream::ProcessStream(const std::vector<std::string>& requested) : state_(std::make_unique<State>()) {
  const auto arguments = selectedProcessArguments(requested);
  auto command = commandLine(arguments);
  if (arguments.empty()) throw std::invalid_argument("empty stream command");
  const auto application = executable(arguments.front());
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) fail("query stream owner");
  DWORD size = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> user(size);
  if (!GetTokenInformation(token.value, TokenUser, user.data(), size, &size)) fail("query stream owner SID");
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) fail("format stream owner SID");
  const auto sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")(A;;GA;;;SY)";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    fail("create private stream ACL");
  struct Descriptor { PSECURITY_DESCRIPTOR value; ~Descriptor() { LocalFree(value); } } descriptor_owner{descriptor};
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  std::array<unsigned char, 16> random{};
  if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    throw std::runtime_error("cannot generate private stream name");
  std::wstring name = LR"(\\.\pipe\ckgit-stream-)";
  for (const auto byte : random) { name += L"0123456789abcdef"[byte >> 4]; name += L"0123456789abcdef"[byte & 15]; }
  state_->input.value = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED |
      FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1, 65536, 65536, 0, &security);
  if (state_->input.value == INVALID_HANDLE_VALUE) fail("create private stream input");
  security.bInheritHandle = TRUE;
  Handle child_input(CreateFileW(name.c_str(), GENERIC_READ, 0, &security, OPEN_EXISTING, 0, nullptr));
  if (child_input.value == INVALID_HANDLE_VALUE) fail("open child stream input");
  state_->event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!state_->event.value) fail("create stream write event");
  state_->write_operation.hEvent = state_->event.value;
  // The synchronous client is already connected before the server starts I/O.
  if (!ConnectNamedPipe(state_->input.value, &state_->write_operation) && GetLastError() != ERROR_PIPE_CONNECTED)
    fail("connect private stream input");
  Handle child_output, child_error;
  if (!CreatePipe(&state_->output.value, &child_output.value, &security, 0) ||
      !SetHandleInformation(state_->output.value, HANDLE_FLAG_INHERIT, 0)) fail("create stream output");
  if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE), GetCurrentProcess(),
      &child_error.value, 0, TRUE, DUPLICATE_SAME_ACCESS)) fail("duplicate stream stderr");
  std::array<HANDLE, 3> inherited{child_input.value, child_output.value, child_error.value};
  Attributes attributes;
  if (!UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
      inherited.data(), sizeof(inherited), nullptr, nullptr)) fail("restrict stream inherited handles");
  state_->job.value = CreateJobObjectW(nullptr, nullptr);
  if (!state_->job.value) fail("create stream process job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(state_->job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    fail("configure stream process job");
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = child_input.value;
  startup.StartupInfo.hStdOutput = child_output.value;
  startup.StartupInfo.hStdError = child_error.value;
  startup.lpAttributeList = attributes.list;
  PROCESS_INFORMATION info{};
  {
    const std::lock_guard lock(windowsProcessCreationMutex());
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &startup.StartupInfo, &info)) fail("start stream process");
  }
  state_->process.value = info.hProcess;
  Handle thread(info.hThread);
  if (!AssignProcessToJobObject(state_->job.value, state_->process.value)) {
    const auto error = GetLastError();
    TerminateProcess(state_->process.value, 127);
    WaitForSingleObject(state_->process.value, INFINITE);
    SetLastError(error); fail("assign stream process job");
  }
  if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) fail("resume stream process");
}
ProcessStream::~ProcessStream() { stop(); }
std::ptrdiff_t ProcessStream::read(char* buffer, std::size_t capacity) {
  if (!capacity) return 0;
  if (!state_->output.value) return -1;
  DWORD available = 0;
  if (!PeekNamedPipe(state_->output.value, nullptr, 0, nullptr, &available, nullptr)) {
    state_->output.close(); return -1;
  }
  if (!available) return 0;
  DWORD received = 0;
  if (!ReadFile(state_->output.value, buffer, static_cast<DWORD>(std::min<std::size_t>(capacity, available)),
      &received, nullptr)) { state_->output.close(); return -1; }
  return received;
}
std::ptrdiff_t ProcessStream::write(std::string_view bytes) {
  if (!state_->input.value) return -1;
  const int ready = state_->finishWrite();
  if (ready <= 0) return ready;
  if (bytes.empty()) return 0;
  state_->queued = static_cast<DWORD>(std::min(bytes.size(), state_->outgoing.size()));
  std::copy_n(bytes.data(), state_->queued, state_->outgoing.data());
  state_->write_operation = {};
  state_->write_operation.hEvent = state_->event.value;
  ResetEvent(state_->event.value);
  DWORD written = 0;
  if (WriteFile(state_->input.value, state_->outgoing.data(), state_->queued, &written, &state_->write_operation))
    return written;
  if (GetLastError() == ERROR_IO_PENDING) { state_->pending = true; return state_->queued; }
  state_->input.close(); return -1;
}
bool ProcessStream::closeInput() {
  if (!state_->input.value) return true;
  if (state_->finishWrite() == 0) return false;
  state_->input.close(); return true;
}
std::optional<int> ProcessStream::poll() {
  if (state_->exit || !state_->process.value) return state_->exit;
  const auto waited = WaitForSingleObject(state_->process.value, 0);
  if (waited == WAIT_OBJECT_0) {
    DWORD code = 0;
    if (!GetExitCodeProcess(state_->process.value, &code)) fail("read stream exit status");
    state_->exit = static_cast<int>(code);
  } else if (waited == WAIT_FAILED) fail("poll stream process");
  return state_->exit;
}
void ProcessStream::stop() noexcept {
  if (state_->job.value) TerminateJobObject(state_->job.value, 137);
  if (state_->process.value) {
    WaitForSingleObject(state_->process.value, INFINITE);
    DWORD code = 137;
    GetExitCodeProcess(state_->process.value, &code);
    state_->exit = static_cast<int>(code);
  }
  if (state_->pending) {
    CancelIoEx(state_->input.value, &state_->write_operation);
    DWORD ignored = 0;
    GetOverlappedResult(state_->input.value, &state_->write_operation, &ignored, TRUE);
    state_->pending = false;
  }
  state_->input.close(); state_->output.close(); state_->job.close(); state_->process.close();
}

}
