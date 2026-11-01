// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/client_state.hpp"
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <vector>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ckgit {
namespace {
void closeLock(std::intptr_t descriptor) {
#ifdef _WIN32
  CloseHandle(reinterpret_cast<HANDLE>(descriptor));
#else
  close(static_cast<int>(descriptor));
#endif
}
}
SyncLock::SyncLock(std::intptr_t descriptor) : descriptor_(descriptor) {}

SyncLock::~SyncLock() {
  if (descriptor_ != -1) {
    closeLock(descriptor_);  // Closing releases the kernel lock.
  }
}

SyncLock::SyncLock(SyncLock&& other) noexcept : descriptor_(other.descriptor_) {
  other.descriptor_ = -1;
}

SyncLock& SyncLock::operator=(SyncLock&& other) noexcept {
  if (this != &other) {
    if (descriptor_ != -1) {
      closeLock(descriptor_);
    }
    descriptor_ = other.descriptor_;
    other.descriptor_ = -1;
  }
  return *this;
}

std::optional<SyncLock> SyncLock::tryAcquire(const std::filesystem::path& lock_path) {
  if (lock_path.empty() || lock_path.filename().empty())
    throw std::invalid_argument("sync lock path is required");
#ifdef _WIN32
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    throw std::runtime_error("cannot query sync lock owner");
  struct Handle { HANDLE value; ~Handle() { CloseHandle(value); } } token_guard{token};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> user(size);
  if (!GetTokenInformation(token, TokenUser, user.data(), size, &size))
    throw std::runtime_error("cannot query sync lock owner SID");
  const auto user_sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(user_sid, &sid)) throw std::runtime_error("cannot format sync lock owner");
  const std::wstring sddl = L"O:" + std::wstring(sid) + L"D:P(A;;FA;;;" + sid + L")(A;;FA;;;SY)";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR security = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security, nullptr))
    throw std::runtime_error("cannot create private sync lock ACL");
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), security, FALSE};
  const auto file = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  LocalFree(security);
  if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open sync lock");
  try {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
      throw std::runtime_error("sync lock must be a regular non-reparse file");
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto error = GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, nullptr, nullptr, nullptr, &descriptor);
    const bool owned = error == ERROR_SUCCESS && owner && EqualSid(owner, user_sid);
    if (descriptor) LocalFree(descriptor);
    if (!owned) throw std::runtime_error("sync lock is not owned by this user");
    OVERLAPPED position{};
    if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &position)) {
      const auto lock_error = GetLastError();
      if (lock_error == ERROR_LOCK_VIOLATION) { CloseHandle(file); return std::nullopt; }
      throw std::runtime_error("cannot acquire sync lock");
    }
    return SyncLock(reinterpret_cast<std::intptr_t>(file));
  } catch (...) { CloseHandle(file); throw; }
#else
  const int descriptor = open(lock_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    throw std::runtime_error("could not open sync lock " + lock_path.string());
  }
  struct stat status {};
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid()) {
    close(descriptor);
    throw std::runtime_error("sync lock is not a regular file owned by this user: " + lock_path.string());
  }
  if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int saved_errno = errno;
    close(descriptor);
    if (saved_errno == EWOULDBLOCK) {
      return std::nullopt;
    }
    throw std::runtime_error("could not acquire sync lock " + lock_path.string());
  }
  return SyncLock(descriptor);
#endif
}
}  // namespace ckgit
