// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/process.hpp"
#include "ckgit/windows.hpp"
#include "ckgit/windows_sandbox.hpp"
#include <windows.h>
#include <sddl.h>
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
  }
  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (!job.value) fail("create process job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (sandbox) {
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_JOB_MEMORY |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    limits.BasicLimitInformation.ActiveProcessLimit = 64;
    limits.JobMemoryLimit = static_cast<SIZE_T>(4) * 1024 * 1024 * 1024;
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
                      sandbox ? environment_block.data() : nullptr, sandbox ? directory.c_str() : nullptr,
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
  while (true) {
    DWORD available = 0;
    if (pipe_open && !PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr)) {
      if (GetLastError() != ERROR_BROKEN_PIPE) fail("inspect process output");
      pipe_open = false;
    }
    if (available) {
      DWORD received = 0;
      if (!ReadFile(read.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &received, nullptr)) fail("read process output");
      const auto keep = std::min<std::size_t>(received, options.output_limit - result.output.size());
      result.output.append(buffer.data(), keep);
      result.output_truncated |= keep != received;
    }
    const DWORD waited = WaitForSingleObject(process.value, 0);
    if (waited == WAIT_FAILED) fail("wait for process");
    if (!pipe_open && waited == WAIT_OBJECT_0) break;
    if (std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      if (!TerminateJobObject(job.value, 124)) fail("terminate process job");
      WaitForSingleObject(process.value, INFINITE);
      break;
    }
    if (!available) Sleep(10);
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

ProcessResult runProcess(const std::vector<std::string>& arguments, const ProcessOptions& options) {
  return runProcessImpl(arguments, options, nullptr, {});
}

ProcessResult runWindowsSandboxProcess(const WindowsSandbox& sandbox,
    const std::vector<std::string>& arguments, const ProcessOptions& options,
    const std::map<std::wstring, std::wstring>& environment) {
  if (arguments.empty() || !std::filesystem::path(utf8ToWide(arguments.front())).is_absolute())
    throw std::invalid_argument("sandbox requires an absolute executable");
  std::vector<std::string> command{wideToUtf8((sandbox.tools() / "ckgit-job-launcher.exe").native()),
      wideToUtf8((sandbox.work() / "tmp").native())};
  command.insert(command.end(), arguments.begin(), arguments.end());
  return runProcessImpl(command, options, &sandbox, environment);
}

struct ManagedProcess::State {
  Handle process, job;
  DWORD id = 0;
  std::optional<int> exit;
};
ManagedProcess::ManagedProcess(const std::vector<std::string>& arguments) : state_(std::make_unique<State>()) {
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

int spawnAttachedProcess(const std::vector<std::string>& arguments) {
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
}
