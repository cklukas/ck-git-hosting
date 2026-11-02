// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
namespace ckgit::native {
struct Fd {
  int value = -1;
  explicit Fd(int n = -1) : value(n) {}
  ~Fd() { if (value >= 0) ::close(value); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};
inline void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
inline void transfer(int fd, void* buffer, std::size_t size, bool writing) {
  auto* p = static_cast<char*>(buffer);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (size) {
    check(std::chrono::steady_clock::now() < deadline, "native supervisor I/O deadline");
    pollfd poller{fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
    const int ready = ::poll(&poller, 1, 100);
    if (ready < 0 && errno == EINTR) continue;
    check(ready >= 0, "native supervisor poll failed");
    if (!ready) continue;
    const auto n = writing ? ::send(fd, p, size, MSG_DONTWAIT) : ::recv(fd, p, size, MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    check(n > 0, "native supervisor disconnected");
    p += n; size -= static_cast<std::size_t>(n);
  }
}
inline void send(int fd, const std::string& text) {
  check(text.size() <= 1024 * 1024, "native supervisor frame too large");
  uint32_t size = htonl(static_cast<uint32_t>(text.size()));
  transfer(fd, &size, sizeof(size), true);
  transfer(fd, const_cast<char*>(text.data()), text.size(), true);
}
inline std::string receive(int fd) {
  uint32_t size = 0; transfer(fd, &size, sizeof(size), false); size = ntohl(size);
  check(size <= 1024 * 1024, "native supervisor frame too large");
  std::string text(size, '\0'); transfer(fd, text.data(), size, false); return text;
}
inline std::string encode(const std::vector<std::string>& fields) {
  std::string out;
  for (const auto& s : fields) {
    check(s.find('\0') == std::string::npos, "NUL in native request");
    out += s; out += '\0';
  }
  check(out.size() <= 1024 * 1024, "native request too large"); return out;
}
inline std::vector<std::string> decode(const std::string& text) {
  check(!text.empty() && text.back() == '\0', "invalid native request");
  std::vector<std::string> fields;
  for (std::size_t p = 0; p < text.size();) {
    const auto end = text.find('\0', p); fields.push_back(text.substr(p, end-p)); p = end+1;
    check(fields.size() <= 4096, "too many native fields");
  }
  return fields;
}
inline sockaddr_un address(const std::string& path) {
  sockaddr_un addr{}; addr.sun_family = AF_UNIX;
  check(!path.empty() && path.size() < sizeof(addr.sun_path), "native socket path too long");
  std::memcpy(addr.sun_path, path.c_str(), path.size()+1); return addr;
}
inline void noSigpipe(int fd) {
  int one = 1; check(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == 0, "socket setup failed");
}
} // namespace ckgit::native
