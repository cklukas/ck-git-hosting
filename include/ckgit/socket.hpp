// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ckgit::net {
#ifdef _WIN32
using Socket = SOCKET;
using Poll = WSAPOLLFD;
using Length = int;
constexpr Socket invalid = INVALID_SOCKET;
struct Runtime {
  Runtime() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Winsock initialization failed"); }
  ~Runtime() { WSACleanup(); }
};
inline int error() { return WSAGetLastError(); }
inline bool interrupted() { return error() == WSAEINTR; }
inline bool wouldBlock() { return error() == WSAEWOULDBLOCK; }
inline bool connecting() { return error() == WSAEWOULDBLOCK || error() == WSAEINPROGRESS; }
inline bool nonblocking(Socket fd, bool enabled) { u_long value = enabled ? 1 : 0; return ioctlsocket(fd, FIONBIO, &value) == 0; }
inline bool connected(Socket fd) {
  int status = 0, size = sizeof(status);
  return getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &size) == 0 && status == 0;
}
inline void close(Socket fd) { closesocket(fd); }
inline int poll(Poll* fds, std::size_t count, int timeout) { return WSAPoll(fds, static_cast<ULONG>(count), timeout); }
inline void timeout(Socket fd, unsigned seconds) {
  const DWORD value = seconds * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
}
inline void reuse(Socket fd) {
  const int enabled = 1;
  setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
}
#else
using Socket = int;
using Poll = pollfd;
using Length = socklen_t;
constexpr Socket invalid = -1;
struct Runtime {};
inline int error() { return errno; }
inline bool interrupted() { return errno == EINTR; }
inline bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
inline bool connecting() { return errno == EINPROGRESS; }
inline bool nonblocking(Socket fd, bool enabled) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0;
}
inline bool connected(Socket fd) {
  int status = 0; socklen_t size = sizeof(status);
  return getsockopt(fd, SOL_SOCKET, SO_ERROR, &status, &size) == 0 && status == 0;
}
inline void close(Socket fd) { ::close(fd); }
inline int poll(Poll* fds, std::size_t count, int timeout) { return ::poll(fds, static_cast<nfds_t>(count), timeout); }
inline void timeout(Socket fd, unsigned seconds) {
  const timeval value{static_cast<time_t>(seconds), 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
}
inline void reuse(Socket fd) {
  const int enabled = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
}
#endif
inline auto read(Socket fd, char* buffer, std::size_t size) {
  return ::recv(fd, buffer, static_cast<int>(std::min<std::size_t>(size, std::numeric_limits<int>::max())), 0);
}
inline auto write(Socket fd, const char* buffer, std::size_t size) {
  return ::send(fd, buffer, static_cast<int>(std::min<std::size_t>(size, std::numeric_limits<int>::max())), 0);
}
}
