// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/macos_native.hpp"
#ifdef __APPLE__
#include "../ci-native-macos/wire.hpp"
#include <sstream>
#include <sys/stat.h>
namespace ckgit {
namespace {
int connectNative(const std::filesystem::path& path) {
  using namespace native;
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  check(fd >= 0, "cannot create native supervisor socket");
  try {
    noSigpipe(fd);
    const auto addr = address(path.string());
    check(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0,
          "native supervisor unavailable; run ckgit-admin runner macos-native setup");
    uid_t uid; gid_t gid;
    check(::getpeereid(fd, &uid, &gid) == 0 && uid == 0, "native supervisor is not root-owned");
    return fd;
  } catch (...) { ::close(fd); throw; }
}
}
void macNativeReady(const std::filesystem::path& socket, const std::filesystem::path& build_root) {
  native::Fd fd(connectNative(socket));
  native::send(fd.value, native::encode({"health", build_root.string()}));
  native::check(native::receive(fd.value) == "ready-v1", "native supervisor not ready or build root mismatch");
}
MacNativeResult runMacNative(const std::filesystem::path& socket,
    const std::filesystem::path& workspace, const std::vector<std::string>& argv,
    const std::vector<std::string>& environment, unsigned timeout_seconds,
    std::size_t log_limit, const std::function<void(std::string_view)>& output,
    const std::function<bool()>& cancel, const std::function<void()>& tick) {
  native::Fd fd(connectNative(socket));
  std::vector<std::string> request{"run", workspace.filename().string(), std::to_string(timeout_seconds),
                                  std::to_string(log_limit), std::to_string(argv.size())};
  request.insert(request.end(), argv.begin(), argv.end());
  request.insert(request.end(), environment.begin(), environment.end());
  native::send(fd.value, native::encode(request));
  bool cancelled = false;
  for (;;) {
    if (tick) tick();
    if (!cancelled && cancel && cancel()) {
      char c = 'C'; native::transfer(fd.value, &c, 1, true); cancelled = true;
    }
    pollfd poller{fd.value, POLLIN, 0};
    const int ready = ::poll(&poller, 1, 100);
    if (ready < 0 && errno == EINTR) continue;
    native::check(ready >= 0, "native supervisor polling failed");
    if (!ready) continue;
    const auto frame = native::receive(fd.value);
    native::check(!frame.empty(), "empty native supervisor response");
    if (frame.front() == 'O') { if (output) output(std::string_view(frame).substr(1)); }
    else if (frame.front() == 'R') {
      MacNativeResult result;
      std::istringstream in(frame.substr(1));
      int timeout = 0, cancellation = 0, truncated = 0;
      native::check(static_cast<bool>(in >> result.exit_code >> timeout >> cancellation >> truncated),
                    "invalid native supervisor result");
      result.timed_out = timeout != 0; result.cancelled = cancellation != 0; result.truncated = truncated != 0;
      return result; // Root sends R only after UID drain and ownership recovery.
    } else throw std::runtime_error("native supervisor: " + frame.substr(1));
  }
}
} // namespace ckgit
#endif
