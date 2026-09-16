// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ckgit {

// Streaming SHA-256, so the codebase needs no platform-specific checksum
// executable or third-party library. Feed bytes with add(), then call finish()
// once to obtain the 64-character lowercase hex digest. An instance is
// single-use: do not call add() after finish().
class Sha256 {
 public:
  void add(const unsigned char* bytes, std::size_t length);
  void add(std::string_view data) {
    add(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  }
  std::string finish();

 private:
  void compress();
  std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<unsigned char, 64> block_{};
  std::size_t used_{};
  std::uint64_t total_{};
};

// One-shot digest of an in-memory buffer.
std::string sha256Hex(std::string_view data);

// One-shot digest of a regular file, read through an O_NOFOLLOW descriptor.
// Throws std::runtime_error if the path is not a readable regular file.
std::string sha256HexOfFile(const std::filesystem::path& path);

}  // namespace ckgit
