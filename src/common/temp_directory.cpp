// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/temp_directory.hpp"
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <array>
#else
#include <cstdlib>
#include <unistd.h>
#endif
namespace ckgit {
std::filesystem::path createTempDirectory(const std::filesystem::path& parent, std::string_view prefix) {
  if (prefix.empty() || prefix.find_first_of("/\\:") != std::string_view::npos || prefix.find('\0') != std::string_view::npos)
    throw std::invalid_argument("invalid temporary directory prefix");
#ifdef _WIN32
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("cannot query temporary directory owner");
  struct Token { HANDLE value; ~Token() { CloseHandle(value); } } token_owner{token};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> user(size);
  if (!GetTokenInformation(token, TokenUser, user.data(), size, &size)) throw std::runtime_error("cannot query temporary directory owner SID");
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) throw std::runtime_error("cannot format temporary directory owner SID");
  const std::wstring sddl = L"D:P(A;OICI;FA;;;" + std::wstring(sid) + L")(A;OICI;FA;;;SY)";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) throw std::runtime_error("cannot build private directory ACL");
  struct Descriptor { PSECURITY_DESCRIPTOR value; ~Descriptor() { LocalFree(value); } } descriptor_owner{descriptor};
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  for (int attempt = 0; attempt != 32; ++attempt) {
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
      throw std::runtime_error("cannot generate temporary directory name");
    std::string suffix;
    for (const auto byte : random) { suffix += "0123456789abcdef"[byte >> 4]; suffix += "0123456789abcdef"[byte & 15]; }
    const auto path = parent / (std::string(prefix) + suffix);
    if (CreateDirectoryW(path.c_str(), &security)) return path;
    if (GetLastError() != ERROR_ALREADY_EXISTS) throw std::runtime_error("cannot create temporary directory");
  }
  throw std::runtime_error("temporary directory name collisions");
#else
  const auto pattern = (parent / (std::string(prefix) + "XXXXXX")).string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  if (!mkdtemp(writable.data())) throw std::runtime_error("cannot create temporary directory");
  return std::filesystem::path(writable.data());
#endif
}
}
