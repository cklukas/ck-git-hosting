// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// The guest is isolated by the Apple VM, not by the guest process. It receives
// no host credentials; the host must validate every message it sends.
#include "ckgit/ci_vm_protocol.hpp"
#include "result_sink.hpp"
#include "staged_source.hpp"
#include "tool_environment.hpp"

#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vsock.h>
#include <unistd.h>

extern char** environ;

namespace {
int connectHost() {
  const int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("cannot create guest Virtio socket");
  sockaddr_vm address{};
  address.svm_len = sizeof(address);
  address.svm_family = AF_VSOCK;
  address.svm_port = 41777;
  address.svm_cid = VMADDR_CID_HOST;
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd);
    throw std::runtime_error("cannot connect to VM host");
  }
  return fd;
}
}
int main(int argc, char** argv) {
  int fd = -1;
  try {
    std::filesystem::path exchange, work_root;
    for (int i = 1; i < argc; ++i) {
      const std::string_view option(argv[i]);
      if (i + 1 >= argc) throw std::invalid_argument("missing VM guest option value");
      const std::string_view value(argv[++i]);
      if (option == "--socket-fd" && fd < 0) {
        const auto result = std::from_chars(value.data(), value.data() + value.size(), fd);
        if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() || fd < 3)
          throw std::invalid_argument("invalid inherited VM socket");
      } else if (option == "--exchange" && exchange.empty()) exchange = value;
      else if (option == "--work-root" && work_root.empty()) work_root = value;
      else throw std::invalid_argument("invalid or duplicate VM guest option");
    }
    if (fd < 0) fd = connectHost();
    const int descriptor_flags = fcntl(fd, F_GETFD);
    if (descriptor_flags < 0 || fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0)
      throw std::runtime_error("cannot protect VM host channel from job processes");
    ckgit::CiVmChannel channel(fd);
    const auto assignment = ckgit::ciVmGuestHandshake(channel, std::chrono::minutes(4));
    try {
    if (exchange.empty() || work_root.empty()) {
      channel.send(ckgit::CiVmFrameType::Error, "VM guest executor is not available", std::chrono::seconds(5));
      close(fd); return 1;
    }
    struct stat directory {};
    if (!exchange.is_absolute() || !work_root.is_absolute())
      throw std::runtime_error("VM guest paths must be absolute");
    if (lstat(work_root.c_str(), &directory) != 0 || !S_ISDIR(directory.st_mode))
      throw std::runtime_error("VM guest work root is missing");
    if (directory.st_uid != geteuid() || (directory.st_mode & 0777) != 0700)
      throw std::runtime_error("VM guest work root ownership or mode is invalid");
    if (!std::filesystem::is_empty(work_root)) throw std::runtime_error("VM guest work root is not empty");
    struct stat shared {};
    if (lstat(exchange.c_str(), &shared) != 0 || !S_ISDIR(shared.st_mode))
      throw std::runtime_error("VM guest source share is missing");
    // macOS can expose a read-only shared directory through an automount whose
    // filesystem flags do not contain MNT_RDONLY. The host must configure the
    // VZSharedDirectory read-only; additionally fail if this guest can write.
    const auto probe = exchange / (".ckgit-write-probe-" + std::to_string(getpid()) + "-" + std::to_string(arc4random()));
    const int writable = open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (writable >= 0) {
      close(writable);
      if (unlink(probe.c_str()) != 0) throw std::runtime_error("cannot remove VM share write probe");
      throw std::runtime_error("VM guest source share permits writes");
    }
    if (errno != EROFS && errno != EACCES && errno != EPERM)
      throw std::runtime_error("cannot verify VM guest source share");
    if (setenv("TMPDIR", work_root.c_str(), 1) != 0) throw std::runtime_error("cannot set VM guest temporary directory");
    if (assignment.policy.max_log_bytes > std::numeric_limits<std::size_t>::max() ||
        assignment.policy.artifact_max_bytes > std::numeric_limits<std::size_t>::max())
      throw std::runtime_error("VM guest limits exceed platform size");
    ckgit::CiExecutionOptions options;
    options.runner = "macos-vm";
    options.build_root = work_root;
    options.timeout_seconds = assignment.policy.timeout_seconds;
    options.max_log_bytes = static_cast<std::size_t>(assignment.policy.max_log_bytes);
    options.artifact_max_bytes = static_cast<std::size_t>(assignment.policy.artifact_max_bytes);
    options.allow_network = false;
    options.extra_env = ckgit::vmGuestToolEnvironment(environ);
    ckgit::validateVmGuestToolEnvironment(assignment.prepared.workflow, options.extra_env);
    ckgit::CiVmGuestSink sink(channel, assignment.prepared, assignment.policy, work_root);
    ckgit::CiVmStagedSource source(assignment.prepared, exchange, [&] {
      sink.tick(); return !sink.cancelRequested() && !sink.leaseLost();
    });
    const auto record = ckgit::executeCiJob(assignment.prepared, options, source, sink);
    close(fd);
    return record.status == ckgit::CiRunStatus::Success ? 0 : 1;
    } catch (const std::exception& error) {
      if (!channel.failed()) {
        try { channel.send(ckgit::CiVmFrameType::Error, std::string(error.what()).substr(0, 200), std::chrono::seconds(5)); }
        catch (...) {}
      }
      throw;
    }
  } catch (const std::exception& error) {
    if (fd >= 0) close(fd);
    std::cerr << "ckgit-macos-vm-guest: " << error.what() << '\n';
    return 1;
  }
}
