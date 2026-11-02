// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/macos_vm_process.hpp"

#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
void stopped(int) { _exit(0); }
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
}
int main(int argc, char** argv) {
  if (argc == 5 && std::strcmp(argv[1], "run") == 0) {
    if (std::strcmp(argv[2], "/exit7") == 0) return 7;
    if (std::strcmp(argv[2], "/slow") == 0) usleep(300000);
    struct sigaction action {};
    action.sa_handler = std::strcmp(argv[2], "/ignore") == 0 ? SIG_IGN : stopped;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, nullptr) != 0) return 8;
    sigset_t signals; sigemptyset(&signals); sigaddset(&signals, SIGTERM); sigaddset(&signals, SIGINT);
    if (sigprocmask(SIG_UNBLOCK, &signals, nullptr) != 0) return 8;
    if (std::strcmp(argv[2], "/slow") == 0) { for (;;) pause(); }
    char byte = 0;
    if (read(STDIN_FILENO, &byte, 1) != 1 || byte != 'x' || write(STDOUT_FILENO, "y", 1) != 1) return 9;
    for (;;) pause();
  }
  try {
    const auto executable = std::filesystem::canonical(argv[0]);
    ckgit::MacosVmProcess slow(executable, "/slow", "/exchange");
    require(slow.stop(std::chrono::seconds(3)), "immediate shutdown interrupted helper initialization");
    ckgit::MacosVmProcess child(executable, "/bundle", "/exchange");
    require(send(child.socket(), "x", 1, 0) == 1, "cannot write to child");
    char response = 0;
    require(recv(child.socket(), &response, 1, 0) == 1 && response == 'y', "private duplex channel failed");
    require(child.stop(std::chrono::seconds(3)) && child.stop(std::chrono::seconds(3)),
            "successful shutdown was not confirmed");
    ckgit::MacosVmProcess failed(executable, "/exit7", "/exchange");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(!failed.stop(std::chrono::seconds(3)), "failed helper was treated as confirmed shutdown");
    {
      ckgit::MacosVmProcess stuck(executable, "/ignore", "/exchange");
      require(send(stuck.socket(), "x", 1, 0) == 1 && recv(stuck.socket(), &response, 1, 0) == 1,
              "stuck helper did not start");
      unsigned callbacks = 0;
      require(!stuck.stop(std::chrono::milliseconds(50), [&] { ++callbacks; }) && callbacks > 0,
              "timed-out helper was treated as stopped or wait callback was skipped");
    }
    std::cout << "VM process lifecycle passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
