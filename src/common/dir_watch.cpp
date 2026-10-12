// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/dir_watch.hpp"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>

#if defined(__linux__)
#include <sys/inotify.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#endif

namespace ckgit {

DirectoryWatch::DirectoryWatch() {
#if defined(__linux__)
  descriptor_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#elif defined(__APPLE__)
  descriptor_ = ::kqueue();
  if (descriptor_ >= 0) ::fcntl(descriptor_, F_SETFD, FD_CLOEXEC);
#endif
}

DirectoryWatch::~DirectoryWatch() {
#if defined(__APPLE__)
  for (const int watched : watched_) ::close(watched);
#endif
  if (descriptor_ >= 0) ::close(descriptor_);
}

bool DirectoryWatch::add(const std::filesystem::path& directory) {
  if (descriptor_ < 0) return false;
#if defined(__linux__)
  const int watch = ::inotify_add_watch(
      descriptor_, directory.c_str(),
      IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_CLOSE_WRITE | IN_DELETE | IN_ONLYDIR);
  if (watch < 0) return false;
  active_ = true;
  return true;
#elif defined(__APPLE__)
  const int watched = ::open(directory.c_str(), O_EVTONLY | O_DIRECTORY | O_CLOEXEC);
  if (watched < 0) return false;
  struct kevent change;
  EV_SET(&change, static_cast<uintptr_t>(watched), EVFILT_VNODE, EV_ADD | EV_CLEAR,
         NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME, 0, nullptr);
  if (::kevent(descriptor_, &change, 1, nullptr, 0, nullptr) != 0) {
    ::close(watched);
    return false;
  }
  watched_.push_back(watched);
  active_ = true;
  return true;
#else
  (void)directory;
  return false;
#endif
}

bool DirectoryWatch::active() const { return active_; }

bool DirectoryWatch::wait(std::chrono::milliseconds timeout) {
  if (timeout < std::chrono::milliseconds::zero()) timeout = std::chrono::milliseconds::zero();
  if (!active_) {
    std::this_thread::sleep_for(timeout);
    return false;
  }
#if defined(__linux__)
  pollfd watch{descriptor_, POLLIN, 0};
  const int ready = ::poll(&watch, 1, static_cast<int>(timeout.count()));
  if (ready <= 0) return false;
  // Drain everything queued, so a burst of renames wakes the caller once. The
  // content of the events does not matter (see the header): any event,
  // including IN_Q_OVERFLOW, means "rescan".
  alignas(inotify_event) char buffer[4096];
  bool changed = false;
  for (;;) {
    const ssize_t received = ::read(descriptor_, buffer, sizeof(buffer));
    if (received > 0) {
      changed = true;
      continue;
    }
    if (received < 0 && errno == EINTR) continue;
    break;  // EAGAIN: drained
  }
  return changed;
#elif defined(__APPLE__)
  timespec wait_for{static_cast<time_t>(timeout.count() / 1000),
                    static_cast<long>((timeout.count() % 1000) * 1000000)};
  struct kevent events[16];
  int ready = ::kevent(descriptor_, nullptr, 0, events, 16, &wait_for);
  if (ready <= 0) return false;
  const timespec immediately{0, 0};
  while (::kevent(descriptor_, nullptr, 0, events, 16, &immediately) > 0) {
  }
  return true;
#else
  std::this_thread::sleep_for(timeout);
  return false;
#endif
}

}  // namespace ckgit
