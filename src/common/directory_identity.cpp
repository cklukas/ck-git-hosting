// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/directory_identity.hpp"
#include <stdexcept>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace ckgit {
struct DirectoryIdentity::State {
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  FILE_ID_INFO identity{};
  ~State() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
  int handle = -1;
  struct stat identity{};
  ~State() { if (handle >= 0) close(handle); }
#endif
};
DirectoryIdentity::DirectoryIdentity(const std::filesystem::path& path) : state_(std::make_unique<State>()) {
#ifdef _WIN32
  state_->handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (state_->handle == INVALID_HANDLE_VALUE ||
      !GetFileInformationByHandleEx(state_->handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
      !(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      !GetFileInformationByHandleEx(state_->handle, FileIdInfo, &state_->identity, sizeof(state_->identity)))
    throw std::runtime_error("cannot retain a regular directory identity");
#else
  state_->handle = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (state_->handle < 0 || fstat(state_->handle, &state_->identity) != 0)
    throw std::runtime_error("cannot retain a regular directory identity");
#endif
}
DirectoryIdentity::~DirectoryIdentity() = default;
bool DirectoryIdentity::matches(const std::filesystem::path& path) const {
#ifdef _WIN32
  try {
    const DirectoryIdentity current(path);
    return state_->identity.VolumeSerialNumber == current.state_->identity.VolumeSerialNumber &&
        std::memcmp(&state_->identity.FileId, &current.state_->identity.FileId, sizeof(FILE_ID_128)) == 0;
  } catch (const std::exception&) { return false; }
#else
  struct stat current{};
  return lstat(path.c_str(), &current) == 0 && S_ISDIR(current.st_mode) &&
      state_->identity.st_dev == current.st_dev && state_->identity.st_ino == current.st_ino;
#endif
}
}
