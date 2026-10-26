// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/private_file_windows.hpp"
#include "ckgit/windows.hpp"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace ckgit {
namespace {
[[noreturn]] void fail(const char* message, DWORD error = GetLastError()) {
  throw std::system_error(static_cast<int>(error), std::system_category(), message);
}
struct Handle {
  HANDLE value;
  explicit Handle(HANDLE handle) : value(handle) {}
  ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
struct LocalMemory {
  void* value = nullptr;
  ~LocalMemory() { if (value) LocalFree(value); }
};
BY_HANDLE_FILE_INFORMATION regularOwnedFile(HANDLE file, PSID user) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
    throw std::runtime_error("private file must be a regular non-reparse file");
  PSID owner = nullptr;
  LocalMemory security;
  const auto error = GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
      &owner, nullptr, nullptr, nullptr, &security.value);
  if (error != ERROR_SUCCESS) fail("query private file owner", error);
  if (!owner || !EqualSid(owner, user)) throw std::runtime_error("private file is not owned by this user");
  return info;
}
bool sameFile(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
  return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
      a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow &&
      a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
      CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
}
}

void writePrivateWindowsFile(const std::filesystem::path& requested, std::string_view content,
                             bool overwrite, bool create_parents) {
  if (requested.empty() || requested.filename().empty()) throw std::invalid_argument("private file path is required");
  const auto path = std::filesystem::absolute(requested).lexically_normal();
  // Device namespaces bypass normal pathname interpretation. Only ordinary
  // absolute filesystem paths are supported for client-owned state.
  const auto native = path.native();
  if (native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\") || native.size() >= 32767)
    throw std::invalid_argument("unsupported private file namespace");
  for (const auto& component : path.relative_path())
    if (!isWindowsFileComponent(component.native())) throw std::invalid_argument("invalid Windows private file component");

  HANDLE token_raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_raw)) fail("query private file owner");
  Handle token(token_raw);
  DWORD bytes = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
  std::vector<unsigned char> user(bytes);
  if (!GetTokenInformation(token.value, TokenUser, user.data(), bytes, &bytes)) fail("query private file owner SID");
  const auto sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
  LPWSTR sid_string = nullptr;
  if (!ConvertSidToStringSidW(sid, &sid_string)) fail("format private file owner SID");
  LocalMemory sid_guard{sid_string};
  const std::wstring sddl = L"O:" + std::wstring(sid_string) +
      L"D:P(A;OICI;FA;;;" + sid_string + L")(A;OICI;FA;;;SY)";
  LocalMemory descriptor;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor.value, nullptr))
    fail("create private file security descriptor");
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor.value, FALSE};

  // Denying delete-sharing pins every parent so a junction or directory
  // replacement cannot redirect the subsequent path-based opens and rename.
  std::vector<std::unique_ptr<Handle>> parents;
  auto directory = path.root_path();
  const auto pin = [&] {
    auto handle = std::make_unique<Handle>(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle->value == INVALID_HANDLE_VALUE) fail("open private file parent");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle->value, &info) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
      throw std::runtime_error("private file parent must be a non-reparse directory");
    parents.push_back(std::move(handle));
  };
  pin();
  for (const auto& component : path.parent_path().relative_path()) {
    directory /= component;
    if (create_parents && !CreateDirectoryW(directory.c_str(), &security) && GetLastError() != ERROR_ALREADY_EXISTS)
      fail("create private file parent");
    pin();
  }
  Handle previous(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  const bool existed = previous.value != INVALID_HANDLE_VALUE;
  if (!existed && GetLastError() != ERROR_FILE_NOT_FOUND) fail("inspect private file");
  BY_HANDLE_FILE_INFORMATION original{};
  if (existed) {
    original = regularOwnedFile(previous.value, sid);
    if (!overwrite) throw std::runtime_error("configuration already exists; use --overwrite to replace it");
  }
  static std::atomic<unsigned long long> sequence{0};
  std::filesystem::path staging;
  HANDLE staging_raw = INVALID_HANDLE_VALUE;
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    staging = directory / (L".ckgit-new-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence++));
    staging_raw = CreateFileW(staging.c_str(), GENERIC_WRITE | DELETE, 0, &security, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (staging_raw != INVALID_HANDLE_VALUE) break;
    if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) fail("stage private file");
  }
  if (staging_raw == INVALID_HANDLE_VALUE) throw std::runtime_error("could not allocate private file staging name");
  Handle staged(staging_raw);
  try {
    std::size_t offset = 0;
    while (offset < content.size()) {
      DWORD written = 0;
      const auto count = static_cast<DWORD>(std::min<std::size_t>(content.size() - offset, 1024 * 1024));
      if (!WriteFile(staged.value, content.data() + offset, count, &written, nullptr) || !written) fail("write private file");
      offset += written;
    }
    if (!FlushFileBuffers(staged.value)) fail("flush private file");
    if (existed) {
      Handle current(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (current.value == INVALID_HANDLE_VALUE || !sameFile(original, regularOwnedFile(current.value, sid)))
        throw std::runtime_error("configuration changed while it was being saved; review it and retry");
    }
    // FileRenameInfo replacement requires the old target to be closed even
    // when its inspection handle allowed delete-sharing. Both identity
    // checks are complete; the source and all parent handles remain held.
    if (existed) {
      CloseHandle(previous.value);
      previous.value = INVALID_HANDLE_VALUE;
    }
    // Rename the open staged file; never close it and reopen by pathname.
    const auto name_bytes = native.size() * sizeof(wchar_t);
    std::vector<unsigned char> rename_buffer(sizeof(FILE_RENAME_INFO) + name_bytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(rename_buffer.data());
    rename->ReplaceIfExists = existed && overwrite;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(rename->FileName, native.data(), name_bytes);
    if (!SetFileInformationByHandle(staged.value, FileRenameInfo, rename, static_cast<DWORD>(rename_buffer.size())))
      fail(existed ? "replace private file atomically" : "create private file atomically");
  } catch (...) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(staged.value, FileDispositionInfo, &disposition, sizeof(disposition));
    throw;
  }
}
}  // namespace ckgit
