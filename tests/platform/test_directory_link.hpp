// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#include <vector>
#endif

inline void createTestDirectoryLink(const std::filesystem::path& destination, const std::filesystem::path& link) {
#ifdef _WIN32
  // Junctions work with an ordinary token; symlink creation may require a privilege.
  std::filesystem::create_directory(link);
  const auto target = L"\\??\\" + destination.wstring();
  struct Junction { DWORD tag; WORD bytes, reserved, substitute_offset, substitute_bytes, print_offset, print_bytes; };
  Junction header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(8 + (target.size() + 2) * 2), 0,
      0, static_cast<WORD>(target.size() * 2), static_cast<WORD>((target.size() + 1) * 2), 0};
  std::vector<unsigned char> data(sizeof(header) + (target.size() + 2) * 2, 0);
  std::memcpy(data.data(), &header, sizeof(header));
  std::memcpy(data.data() + sizeof(header), target.data(), target.size() * 2);
  const auto directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (directory == INVALID_HANDLE_VALUE) throw std::runtime_error("open junction fixture");
  DWORD returned = 0;
  const auto created = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, data.data(),
      static_cast<DWORD>(data.size()), nullptr, 0, &returned, nullptr);
  CloseHandle(directory);
  if (!created) throw std::runtime_error("create junction fixture");
#else
  std::filesystem::create_directory_symlink(destination, link);
#endif
}
