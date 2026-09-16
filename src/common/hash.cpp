// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/hash.hpp"

#include <bit>
#include <cerrno>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace ckgit {

void Sha256::add(const unsigned char* bytes, std::size_t length) {
  total_ += length;
  for (std::size_t index = 0; index < length; ++index) {
    block_[used_++] = bytes[index];
    if (used_ == block_.size()) {
      compress();
      used_ = 0;
    }
  }
}

void Sha256::compress() {
  static constexpr std::array<std::uint32_t, 64> constants{
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::array<std::uint32_t, 64> words{};
  for (unsigned index = 0; index < 16; ++index) {
    for (unsigned byte = 0; byte < 4; ++byte) words[index] = (words[index] << 8) | block_[index * 4 + byte];
  }
  for (unsigned index = 16; index < 64; ++index) {
    const auto a = words[index - 15], b = words[index - 2];
    words[index] = words[index - 16] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3)) + words[index - 7] +
                   (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10));
  }
  auto a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5],
       g = state_[6], h = state_[7];
  for (unsigned index = 0; index < 64; ++index) {
    const auto first = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                       constants[index] + words[index];
    const auto second = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
                        ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + first; d = c; c = b; b = a; a = first + second;
  }
  state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
  state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

std::string Sha256::finish() {
  const auto bits = total_ * 8;
  const unsigned char one = 0x80, zero = 0;
  add(&one, 1);
  while (used_ != 56) add(&zero, 1);
  unsigned char length[8];
  for (unsigned index = 0; index < 8; ++index) {
    length[index] = static_cast<unsigned char>(bits >> ((7 - index) * 8));
  }
  add(length, 8);
  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (const auto word : state_) result << std::setw(8) << word;
  return result.str();
}

std::string sha256Hex(std::string_view data) {
  Sha256 hash;
  hash.add(data);
  return hash.finish();
}

std::string sha256HexOfFile(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat status {};
  if (fd < 0 || ::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
    if (fd >= 0) ::close(fd);
    throw std::runtime_error("could not checksum regular file: " + path.string());
  }
  Sha256 hash;
  std::array<unsigned char, 65536> buffer{};
  while (true) {
    const auto count = ::read(fd, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      ::close(fd);
      throw std::runtime_error("could not checksum " + path.string());
    }
    if (count == 0) break;
    hash.add(buffer.data(), static_cast<std::size_t>(count));
  }
  ::close(fd);
  return hash.finish();
}

}  // namespace ckgit
