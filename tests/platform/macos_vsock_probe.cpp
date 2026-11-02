// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Run inside a macOS guest. The host must explicitly enable its diagnostic
// listener; this fixed-message probe does not transfer credentials or files.
#include <sys/socket.h>
#include <sys/vsock.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
  const bool require_offline = argc == 2 && std::strcmp(argv[1], "--require-offline") == 0;
  const char* request = require_offline ? "ckgit-vsock-offline-v1\n" : "ckgit-vsock-probe-v1\n";
  bool offline_failed = false;
  if (require_offline) {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
      std::cerr << "cannot inspect guest interfaces\n";
      offline_failed = true;
    }
    bool external = false;
    for (const ifaddrs* item = interfaces; item; item = item->ifa_next) {
      if (item->ifa_addr && (item->ifa_flags & IFF_UP) && !(item->ifa_flags & IFF_LOOPBACK) &&
          (item->ifa_addr->sa_family == AF_INET || item->ifa_addr->sa_family == AF_INET6)) external = true;
    }
    if (interfaces) freeifaddrs(interfaces);
    if (external) { std::cerr << "guest has an active non-loopback IP interface\n"; offline_failed = true; }
    if (offline_failed) request = "ckgit-vsock-network-v1\n";
  } else if (argc != 1) {
    std::cerr << "usage: ckgit-macos-vsock-probe [--require-offline]\n";
    return 2;
  }
  const auto request_size = std::strlen(request);
  constexpr char response[] = "ckgit-vsock-reply-v1\n";
  const int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
  if (fd < 0) { std::cerr << "cannot create guest Virtio socket\n"; return 1; }
  sockaddr_vm address{};
  address.svm_len = sizeof(address);
  address.svm_family = AF_VSOCK;
  address.svm_port = 41777;
  address.svm_cid = VMADDR_CID_HOST;
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      write(fd, request, request_size) != static_cast<ssize_t>(request_size)) {
    close(fd); std::cerr << "guest Virtio connection failed\n"; return 1;
  }
  std::array<char, sizeof(response) - 1> received{};
  std::size_t count = 0;
  while (count < received.size()) {
    const auto n = read(fd, received.data() + count, received.size() - count);
    if (n <= 0) { close(fd); std::cerr << "guest Virtio response was incomplete\n"; return 1; }
    count += static_cast<std::size_t>(n);
  }
  close(fd);
  if (std::memcmp(received.data(), response, received.size()) != 0) {
    std::cerr << "unexpected guest Virtio response\n"; return 1;
  }
  if (offline_failed) return 1;
  std::cout << "Apple Virtio socket host/guest probe passed\n";
  return 0;
}
