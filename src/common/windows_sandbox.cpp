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
}
struct WindowsSandbox::State {
  std::filesystem::path root;
  std::wstring profile, owner, app;
  bool retain = false;
  PSID sid = nullptr;
  std::vector<std::vector<unsigned char>> capabilities;
  HWINSTA station = nullptr;
  HDESK desktop = nullptr;
  ~State() {
    if (desktop) CloseDesktop(desktop);
    if (station) CloseWindowStation(station);
    if (sid) FreeSid(sid);
    if (!profile.empty()) DeleteAppContainerProfile(profile.c_str());
    if (!retain && !root.empty()) { std::error_code error; std::filesystem::remove_all(root, error); }
  }
};
WindowsSandbox::WindowsSandbox(const std::filesystem::path& parent) : state_(std::make_unique<State>()) {
  state_->root = createTempDirectory(parent, "ckgit-lpac-");
  state_->owner = currentUser();
  const auto name = state_->root.filename().wstring();
  PSID sid = nullptr;
  const auto status = CreateAppContainerProfile(name.c_str(), L"ckgit isolated job", L"Temporary ck-git-hosting LPAC job", nullptr, 0, &sid);
  if (FAILED(status)) fail("create AppContainer profile", static_cast<DWORD>(status));
  state_->profile = name;
  state_->sid = sid;
  state_->app = sidText(sid);
  state_->capabilities.push_back(capability(L"registryRead"));
  // Native toolchains import USER32 through COM even for console programs.
  // Give them their own noninteractive window station and desktop, never
  // access to the user's desktop. Serialize the temporary broker station
  // switch with all process creation in the platform layer.
  const auto sddl = L"D:P(A;;GA;;;" + state_->owner + L")(A;;GA;;;SY)(A;;GA;;;" + state_->app + L")S:(ML;;NW;;;LW)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) fail("sandbox desktop descriptor");
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  {
    const std::lock_guard lock(windowsProcessCreationMutex());
    // Never reopen an existing object: its descriptor would be ignored by
    // CreateWindowStation, invalidating the per-job isolation contract.
    state_->station = CreateWindowStationW(name.c_str(), CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, &attributes);
    if (!state_->station) {
      const auto error = GetLastError();
      LocalFree(descriptor);
      fail(error == ERROR_ACCESS_DENIED
          ? "create private sandbox window station (Windows requires an administrator token for named window stations)"
          : "create private sandbox window station", error);
    }
    const auto previous = GetProcessWindowStation();
    if (!SetProcessWindowStation(state_->station)) { const auto error = GetLastError(); LocalFree(descriptor); fail("select sandbox station", error); }
    state_->desktop = CreateDesktopW(L"Default", nullptr, nullptr, 0, GENERIC_ALL, &attributes);
    const auto error = GetLastError();
    const auto restored = SetProcessWindowStation(previous);
    LocalFree(descriptor);
    if (!restored) fail("restore broker window station");
    if (!state_->desktop) fail("create sandbox desktop", error);
  }
  protect(state_->root, state_->owner, state_->app, false);
  std::filesystem::create_directory(work());
  protect(work(), state_->owner, state_->app, true);
  std::filesystem::create_directory(work() / "tmp");
  std::filesystem::create_directory(work() / "home");
  std::filesystem::create_directory(tools());
  protect(tools(), state_->owner, state_->app, false);
}
WindowsSandbox::~WindowsSandbox() = default;
void WindowsSandbox::retainWorkspace() { state_->retain = true; }
std::filesystem::path WindowsSandbox::work() const { return state_->root / "work"; }
std::filesystem::path WindowsSandbox::tools() const { return state_->root / "tools"; }
void* WindowsSandbox::sid() const { return state_->sid; }
std::vector<void*> WindowsSandbox::capabilitySids() const {
  std::vector<void*> result;
  for (auto& sid : state_->capabilities) result.push_back(sid.data());
  return result;
}
std::wstring WindowsSandbox::desktopName() const { return state_->profile + L"\\Default"; }
void WindowsSandbox::sealTools(const std::function<bool()>& should_continue) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(tools())) {
    if (should_continue && !should_continue()) throw std::runtime_error("toolchain staging cancelled");
    protect(entry.path(), state_->owner, state_->app, false);
  }
}
}
