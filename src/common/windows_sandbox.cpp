// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/windows_sandbox.hpp"
#include "ckgit/temp_directory.hpp"
#include "ckgit/windows.hpp"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>
#include <stdexcept>
#include <system_error>
#include <iostream>

namespace ckgit {
namespace {
[[noreturn]] void fail(const char* message, DWORD error = GetLastError()) {
  throw std::system_error(static_cast<int>(error), std::system_category(), message);
}
std::wstring sidText(PSID sid) {
  LPWSTR value = nullptr;
  if (!ConvertSidToStringSidW(sid, &value)) fail("format SID");
  const std::wstring result(value);
  LocalFree(value);
  return result;
}
std::wstring currentUser() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) fail("query user token");
  DWORD bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  std::vector<unsigned char> buffer(bytes);
  const BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes);
  const DWORD error = GetLastError();
  CloseHandle(token);
  if (!ok) fail("query user SID", error);
  return sidText(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
}
void protect(const std::filesystem::path& path, const std::wstring& owner, const std::wstring& app, bool writable) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error("sandbox staging rejects reparse points");
  const std::wstring sddl = L"D:P(A;OICI;FA;;;" + owner + L")(A;OICI;FA;;;SY)(A;OICI;" +
      (writable ? L"0x1301bf" : L"GRGX") + L";;;" + app + L")" + (writable ? L"S:(ML;OICI;NW;;;LW)" : L"");
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) fail("sandbox DACL");
  const BOOL ok = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION |
      (writable ? LABEL_SECURITY_INFORMATION : 0), descriptor);
  const DWORD error = GetLastError();
  LocalFree(descriptor);
  if (!ok) fail("protect sandbox staging", error);
}
std::vector<unsigned char> capability(const wchar_t* name) {
  using Derive = BOOL (WINAPI*)(PCWSTR, PSID**, DWORD*, PSID**, DWORD*);
  const auto derive = reinterpret_cast<Derive>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "DeriveCapabilitySidsFromName"));
  PSID *groups = nullptr, *caps = nullptr;
  DWORD group_count = 0, cap_count = 0;
  if (!derive || !derive(name, &groups, &group_count, &caps, &cap_count)) fail("derive sandbox capability");
  std::vector<unsigned char> result;
  if (cap_count == 1) {
    result.resize(GetLengthSid(caps[0]));
    if (!CopySid(static_cast<DWORD>(result.size()), result.data(), caps[0])) result.clear();
  }
  for (DWORD i = 0; i < group_count; ++i) LocalFree(groups[i]);
  for (DWORD i = 0; i < cap_count; ++i) LocalFree(caps[i]);
  LocalFree(groups); LocalFree(caps);
  if (result.empty()) throw std::runtime_error("sandbox capability unavailable");
  return result;
}
constexpr const wchar_t* recordName = L".ckgit-resource";
bool resourceName(const std::filesystem::path& path, std::wstring_view prefix) {
  const auto name = path.filename().wstring();
  return name.starts_with(prefix) && name.size() == prefix.size() + 32 &&
      name.find_first_not_of(L"0123456789abcdef", prefix.size()) == std::wstring::npos;
}
// Records are created before profile allocation, reserving that random name.
// They contain no paths to follow and are never writable by a job identity.
struct ResourceRecord {
  HANDLE handle = INVALID_HANDLE_VALUE;
  std::filesystem::path root;
  std::string kind, owner;
  bool retained = false;
  ~ResourceRecord() { close(); }
  void close() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); handle = INVALID_HANDLE_VALUE; }
  std::string prefix() const {
    return "ckgit-resource-v1\n" + kind + "\n" + wideToUtf8(root.filename().native()) + "\n" + owner + "\n";
  }
  void write() {
    const auto text = prefix() + (retained ? "retain\n" : "remove\n");
    LARGE_INTEGER zero{};
    DWORD written = 0;
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) ||
        !WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
        written != text.size() || !SetEndOfFile(handle) || !FlushFileBuffers(handle)) fail("persist runner resource record");
  }
  void create(const std::filesystem::path& directory, std::string type, const std::wstring& user) {
    root = directory; kind = std::move(type); owner = wideToUtf8(user);
    const auto sddl = L"O:" + user + L"D:P(A;;FA;;;" + user + L")(A;;FA;;;SY)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) fail("resource record descriptor");
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    handle = CreateFileW((root / recordName).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        &security, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const auto error = GetLastError();
    LocalFree(descriptor);
    if (handle == INVALID_HANDLE_VALUE) fail("create resource record", error);
    write();
  }
  bool open(const std::filesystem::path& directory, std::string type, const std::wstring& user) {
    const auto attrs = GetFileAttributesW(directory.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    root = directory; kind = std::move(type); owner = wideToUtf8(user);
    handle = CreateFileW((root / recordName).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false; // Missing, busy or inaccessible: preserve.
    PSID file_owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto owner_status = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &file_owner, nullptr, nullptr, nullptr, &descriptor);
    const bool owned = owner_status == ERROR_SUCCESS && file_owner && sidText(file_owner) == user;
    if (descriptor) LocalFree(descriptor);
    if (!owned) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nFileSizeHigh || info.nFileSizeLow > 512) return false;
    std::string bytes(info.nFileSizeLow, '\0');
    DWORD count = 0;
    if (!ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size()) return false;
    const auto expected = prefix();
    if (bytes != expected + "remove\n" && bytes != expected + "retain\n") return false;
    retained = bytes == expected + "retain\n";
    return true;
  }
};
// Called only after owning the exclusive record. Never recurse through a
// reparse point, even when it has directory attributes (e.g. a junction).
void removeResourceTree(const std::filesystem::path& path) {
  const auto attrs = GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return;
    fail("inspect abandoned resource");
  }
  const bool directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if (directory && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT))
    for (const auto& entry : std::filesystem::directory_iterator(path)) removeResourceTree(entry.path());
  if ((attrs & FILE_ATTRIBUTE_READONLY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT) &&
      !SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY)) fail("clear owned resource read-only flag");
  if (!(directory ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str()))) fail("remove abandoned resource");
}
void finishResource(ResourceRecord& record) {
  if (record.retained) { record.close(); return; }
  // Keep the durable record until all other entries have been removed, so a
  // partial cleanup can be retried on the next startup.
  for (const auto& entry : std::filesystem::directory_iterator(record.root))
    if (entry.path().filename() != recordName) removeResourceTree(entry.path());
  record.close();
  if (!DeleteFileW((record.root / recordName).c_str())) fail("remove resource record");
  if (!RemoveDirectoryW(record.root.c_str())) fail("remove resource directory");
}
void recoverSandboxes(const std::filesystem::path& parent, const std::wstring& owner) {
  const auto attributes = GetFileAttributesW(parent.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return;
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error("recovery parent is not an ordinary directory");
  for (const auto& entry : std::filesystem::directory_iterator(parent)) {
    if (!resourceName(entry.path(), L"ckgit-lpac-")) continue;
    ResourceRecord record;
    if (!record.open(entry.path(), "sandbox", owner)) continue;
    const auto status = DeleteAppContainerProfile(entry.path().filename().c_str());
    if (FAILED(status)) fail("recover AppContainer profile", static_cast<DWORD>(status));
    finishResource(record);
  }
}
bool hasSandboxDirectories(const std::filesystem::path& parent) {
  if (!std::filesystem::exists(parent)) return false;
  for (const auto& entry : std::filesystem::directory_iterator(parent))
    if (resourceName(entry.path(), L"ckgit-lpac-")) return true;
  return false;
}
void cleanSession(ResourceRecord& record, const std::wstring& owner) {
  recoverSandboxes(record.root, owner);
  recoverSandboxes(record.root / "jobs", owner);
  // Preserve active, retained, unrecorded or invalid sandbox directories.
  if (hasSandboxDirectories(record.root) || hasSandboxDirectories(record.root / "jobs")) return;
  finishResource(record);
}

}
struct WindowsSandbox::State {
  std::filesystem::path root;
  std::wstring profile, owner, app;
  ResourceRecord record;
  PSID sid = nullptr;
  std::vector<std::vector<unsigned char>> capabilities;
  ~State() {
    if (sid) FreeSid(sid);
    try {
      if (!profile.empty()) {
        const auto result = DeleteAppContainerProfile(profile.c_str());
        if (FAILED(result)) fail("delete AppContainer profile", static_cast<DWORD>(result));
      }
      if (record.handle != INVALID_HANDLE_VALUE) finishResource(record);
      else if (!root.empty()) removeResourceTree(root);
    } catch (const std::exception& error) {
      std::cerr << "Preserving sandbox for recovery: " << error.what() << '\n';
    }
  }
};
WindowsSandbox::WindowsSandbox(const std::filesystem::path& parent) : state_(std::make_unique<State>()) {
  state_->root = createTempDirectory(parent, "ckgit-lpac-");
  state_->owner = currentUser();
  const auto name = state_->root.filename().wstring();
  state_->record.create(state_->root, "sandbox", state_->owner);
  PSID sid = nullptr;
  const auto status = CreateAppContainerProfile(name.c_str(), L"ckgit isolated job", L"Temporary ck-git-hosting LPAC job", nullptr, 0, &sid);
  if (FAILED(status)) fail("create AppContainer profile", static_cast<DWORD>(status));
  state_->profile = name;
  state_->sid = sid;
  state_->app = sidText(sid);
  state_->capabilities.push_back(capability(L"registryRead"));
  protect(state_->root, state_->owner, state_->app, false);
  std::filesystem::create_directory(work());
  protect(work(), state_->owner, state_->app, true);
  std::filesystem::create_directory(work() / "tmp");
  std::filesystem::create_directory(work() / "home");
  std::filesystem::create_directory(tools());
  protect(tools(), state_->owner, state_->app, false);
}
WindowsSandbox::~WindowsSandbox() = default;
void WindowsSandbox::retainWorkspace() { state_->record.retained = true; state_->record.write(); }
std::filesystem::path WindowsSandbox::work() const { return state_->root / "work"; }
std::filesystem::path WindowsSandbox::tools() const { return state_->root / "tools"; }
void* WindowsSandbox::sid() const { return state_->sid; }
std::vector<void*> WindowsSandbox::capabilitySids() const {
  std::vector<void*> result;
  for (auto& sid : state_->capabilities) result.push_back(sid.data());
  return result;
}
void WindowsSandbox::sealTools(const std::function<bool()>& should_continue) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(tools())) {
    if (should_continue && !should_continue()) throw std::runtime_error("toolchain staging cancelled");
    protect(entry.path(), state_->owner, state_->app, false);
  }
}
struct WindowsSandboxDesktop::State {
  HWINSTA station = nullptr;
  HDESK desktop = nullptr;
  std::wstring name;
  ~State() {
    if (desktop && !CloseDesktop(desktop))
      std::cerr << "Cannot close sandbox desktop: " << GetLastError() << '\n';
    if (station && !CloseWindowStation(station))
      std::cerr << "Cannot close sandbox window station: " << GetLastError() << '\n';
  }
};
WindowsSandboxDesktop::WindowsSandboxDesktop(const WindowsSandbox& sandbox)
    : state_(std::make_unique<State>()) {
  // Console toolchains can import USER32 through COM. Give each executing
  // step its own noninteractive objects, with only this job's SID admitted.
  const auto& identity = *sandbox.state_;
  const auto sddl = L"D:P(A;;GA;;;" + identity.owner + L")(A;;GA;;;SY)(A;;GA;;;" + identity.app + L")S:(ML;;NW;;;LW)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    fail("sandbox desktop descriptor");
  struct FreeDescriptor {
    PSECURITY_DESCRIPTOR value;
    ~FreeDescriptor() { LocalFree(value); }
  } free_descriptor{descriptor};
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  const std::lock_guard lock(windowsProcessCreationMutex());
  const wchar_t* requested_name = identity.profile.c_str();
  SetLastError(ERROR_SUCCESS);
  state_->station = CreateWindowStationW(requested_name, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, &attributes);
  auto error = GetLastError();
  if (!state_->station && error == ERROR_ACCESS_DENIED) {
    // Windows requires an administrator for a caller-supplied station name.
    // A standard user may create a fresh OS-named station. CREATE_ONLY is
    // essential: reopening the logon's existing station ignores our ACL.
    requested_name = nullptr;
    SetLastError(ERROR_SUCCESS);
    state_->station = CreateWindowStationW(requested_name, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, &attributes);
    error = GetLastError();
  }
  // Windows may retain desktop references briefly after the last process
  // exits. Retry only creation, never opening or changing an existing
  // station. A live conflicting owner still causes a bounded setup error.
  const auto deadline = GetTickCount64() + 2000;
  while (!state_->station && error == ERROR_ALREADY_EXISTS && GetTickCount64() < deadline) {
    Sleep(10);
    SetLastError(ERROR_SUCCESS);
    state_->station = CreateWindowStationW(requested_name, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, &attributes);
    error = GetLastError();
  }
  if (!state_->station || error == ERROR_ALREADY_EXISTS)
    fail("create fresh sandbox window station (another step or the broker may already own this logon's station)", error);
  wchar_t station_name[512]{};
  DWORD bytes = 0;
  USEROBJECTFLAGS flags{};
  if (!GetUserObjectInformationW(state_->station, UOI_NAME, station_name, sizeof(station_name), &bytes) ||
      !GetUserObjectInformationW(state_->station, UOI_FLAGS, &flags, sizeof(flags), &bytes))
    fail("inspect sandbox window station");
  if ((flags.dwFlags & WSF_VISIBLE) || _wcsicmp(station_name, L"WinSta0") == 0)
    throw std::runtime_error("sandbox station must be noninteractive");
  state_->name = std::wstring(station_name) + L"\\Default";
  const auto previous = GetProcessWindowStation();
  const auto previous_desktop = GetThreadDesktop(GetCurrentThreadId());
  if (!previous_desktop) fail("read broker thread desktop");
  if (!SetProcessWindowStation(state_->station)) fail("select sandbox station");
  state_->desktop = CreateDesktopW(L"Default", nullptr, nullptr, 0, GENERIC_ALL, &attributes);
  error = GetLastError();
  const auto restored = SetProcessWindowStation(previous);
  if (!restored) fail("restore broker window station");
  // CreateDesktop can attach the calling thread. Restore both associations
  // so the broker cannot keep a finished step's desktop/station alive.
  if (!SetThreadDesktop(previous_desktop)) fail("restore broker thread desktop");
  if (!state_->desktop) fail("create sandbox desktop", error);
}
WindowsSandboxDesktop::~WindowsSandboxDesktop() = default;
const std::wstring& WindowsSandboxDesktop::name() const { return state_->name; }

struct WindowsRunnerSession::State {
  ResourceRecord record;
  std::wstring owner;
  ~State() {
    if (record.handle == INVALID_HANDLE_VALUE) return;
    try { cleanSession(record, owner); }
    catch (const std::exception& error) {
      std::cerr << "Preserving runner session for recovery: " << error.what() << '\n';
    }
  }
};
WindowsRunnerSession::WindowsRunnerSession(const std::filesystem::path& build_root)
    : state_(std::make_unique<State>()) {
  state_->owner = currentUser();
  for (const auto& entry : std::filesystem::directory_iterator(build_root)) {
    if (!resourceName(entry.path(), L"runner-session-")) continue;
    ResourceRecord record;
    if (!record.open(entry.path(), "session", state_->owner)) continue;
    cleanSession(record, state_->owner);
  }
  const auto root = createTempDirectory(build_root, "runner-session-");
  state_->record.create(root, "session", state_->owner);
}
WindowsRunnerSession::~WindowsRunnerSession() = default;
const std::filesystem::path& WindowsRunnerSession::root() const { return state_->record.root; }

}
