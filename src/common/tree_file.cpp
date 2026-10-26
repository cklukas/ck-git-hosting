// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/tree_file.hpp"
#include <limits>
#include <utility>
#ifdef _WIN32
#include "ckgit/windows.hpp"
#include <windows.h>
#include <winternl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace ckgit::platform {
namespace {
#ifndef _WIN32
class Descriptor {
 public:
  explicit Descriptor(int value) : value_(value) {}
  ~Descriptor() {
    if (value_ >= 0) close(value_);
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
  Descriptor& operator=(Descriptor&& other) noexcept {
    if (this != &other) {
      if (value_ >= 0) close(value_);
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  int get() const { return value_; }
  explicit operator bool() const { return value_ >= 0; }

 private:
  int value_;
};

#endif
}
TreeFile readTreeFile(const std::filesystem::path& root, const std::vector<std::string>& parts,
                      std::uintmax_t limit, bool read, bool follow_root) {
  TreeFile result;
  if (parts.empty()) return result;
  for (const auto& part : parts) {
    if (part.empty() || part == "." || part == ".." || part.find_first_of("/\\") != std::string::npos ||
        part.find('\0') != std::string::npos) return result;
#ifdef _WIN32
    if (part.find(':') != std::string::npos || part.back() == '.' || part.back() == ' ') return result;
#endif
  }
#ifdef _WIN32
  struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
  };
  Handle directory{CreateFileW(root.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | (follow_root ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), nullptr)};
  if (directory.value == INVALID_HANDLE_VALUE) return result;
  FILE_ATTRIBUTE_TAG_INFO info{};
  if (!GetFileInformationByHandleEx(directory.value, FileAttributeTagInfo, &info, sizeof(info)) ||
      !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (!follow_root && (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
    result.status = TreeFileStatus::NotRegular;
    return result;
  }
  // NtCreateFile with RootDirectory is the Windows equivalent of openat.
  // Every name is exactly one component; inspect the returned handle before
  // using it, so even concurrent replacement cannot redirect the next open.
  using Open = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
  const auto open = reinterpret_cast<Open>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
  if (!open) { result.status = TreeFileStatus::Unreadable; return result; }
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto name = utf8ToWide(parts[i]);
    if (name.size() > 32766) return result;
    UNICODE_STRING unicode{};
    unicode.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    unicode.MaximumLength = unicode.Length;
    unicode.Buffer = const_cast<wchar_t*>(name.data());
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = directory.value;
    attributes.ObjectName = &unicode;
    attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
    IO_STATUS_BLOCK status{};
    HANDLE next = nullptr;
    const bool last = i + 1 == parts.size();
    constexpr ULONG open_reparse = 0x00200000, synchronous = 0x20;
    const auto code = open(&next, (last ? FILE_READ_DATA : FILE_LIST_DIRECTORY) | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        &attributes, &status, nullptr, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        1 /* FILE_OPEN */, open_reparse | synchronous, nullptr, 0);
    if (code < 0) {
      result.status = (static_cast<ULONG>(code) == 0xC0000034 || static_cast<ULONG>(code) == 0xC000003A)
                        ? TreeFileStatus::Missing : TreeFileStatus::Unreadable;
      return result;
    }
    CloseHandle(directory.value);
    directory.value = next;
    if (GetFileType(next) != FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(next, FileAttributeTagInfo, &info, sizeof(info)) ||
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (last == static_cast<bool>(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))) {
      result.status = TreeFileStatus::NotRegular;
      return result;
    }
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(directory.value, &size) || size.QuadPart < 0) {
    result.status = TreeFileStatus::Unreadable; return result;
  }
  result.size = static_cast<std::uintmax_t>(size.QuadPart);
  if (result.size > limit || result.size > std::numeric_limits<std::size_t>::max()) {
    result.status = TreeFileStatus::TooLarge; return result;
  }
  if (read) {
    result.content.resize(static_cast<std::size_t>(result.size));
    std::size_t offset = 0;
    while (offset < result.content.size()) {
      DWORD count = 0;
      const auto left = result.content.size() - offset;
      const auto chunk = static_cast<DWORD>(left > 65536 ? 65536 : left);
      if (!ReadFile(directory.value, result.content.data() + offset, chunk, &count, nullptr) || !count) {
        result.content.clear(); result.status = TreeFileStatus::Unreadable; return result;
      }
      offset += count;
    }
  }
  result.status = TreeFileStatus::Ok;
  return result;
#else
  const auto failed = [&result](int code) {
    result.status = code == ENOENT ? TreeFileStatus::Missing
                    : code == ELOOP || code == ENOTDIR || code == EMLINK ? TreeFileStatus::NotRegular
                                                                         : TreeFileStatus::Unreadable;
    return result;
  };
  Descriptor directory(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | (follow_root ? 0 : O_NOFOLLOW)));
  if (!directory) return failed(errno);
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    Descriptor next(::openat(directory.get(), std::string(parts[index]).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!next) return failed(errno);
    directory = std::move(next);
  }
  // O_NONBLOCK: opening a FIFO must not wait for a writer; it is refused
  // below like anything else that is not a regular file.
  Descriptor file(::openat(directory.get(), std::string(parts.back()).c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (!file) return failed(errno);
  struct stat status {};
  if (::fstat(file.get(), &status) != 0) return failed(errno);
  if (!S_ISREG(status.st_mode)) {
    result.status = TreeFileStatus::NotRegular;
    return result;
  }
  result.size = static_cast<std::uintmax_t>(status.st_size);
  if (result.size > limit) {
    result.status = TreeFileStatus::TooLarge;
    return result;
  }
  if (read) {
    result.content.resize(static_cast<std::size_t>(result.size));
    std::size_t done = 0;
    while (done < result.content.size()) {
      const auto count = ::read(file.get(), result.content.data() + done, result.content.size() - done);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) {  // an error, or the file shrank while it was read
        result.content.clear();
        result.status = TreeFileStatus::Unreadable;
        return result;
      }
      done += static_cast<std::size_t>(count);
    }
  }
  result.status = TreeFileStatus::Ok;
  return result;
#endif
}
}
