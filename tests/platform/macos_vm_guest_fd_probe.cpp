// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Runs as an untrusted job step, not as the guest agent.
#include <sys/socket.h>
#include <sys/vsock.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
  const bool offline = argc == 2 && std::strcmp(argv[1], "--offline") == 0;
  if (argc != 1 && !offline) return 2;
  const int limit = getdtablesize();
  for (int fd = 3; fd < limit; ++fd) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0 && address.ss_family == AF_VSOCK) {
      std::cerr << "VM host socket leaked into a job step\n";
      return 1;
    }
  }
  if (offline) {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) { std::cerr << "cannot inspect guest interfaces\n"; return 1; }
    bool external = false;
    for (const ifaddrs* item = interfaces; item; item = item->ifa_next) {
      if (item->ifa_addr && (item->ifa_flags & IFF_UP) && !(item->ifa_flags & IFF_LOOPBACK) &&
          (item->ifa_addr->sa_family == AF_INET || item->ifa_addr->sa_family == AF_INET6)) external = true;
    }
    freeifaddrs(interfaces);
    if (external) { std::cerr << "guest has an active non-loopback IP interface\n"; return 1; }
  }
  return 0;
}
