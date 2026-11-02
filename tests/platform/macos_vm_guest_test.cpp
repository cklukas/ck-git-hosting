// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_vm_protocol.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 1;
  const pid_t child = fork();
  if (child < 0) return 1;
  if (child == 0) {
    close(pair[0]);
    const auto fd = std::to_string(pair[1]);
    execl(argv[1], argv[1], "--socket-fd", fd.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  close(pair[1]);
  bool passed = false;
  try {
    ckgit::CiPreparedJob job;
    job.job.job_id = "00000000000000000001-abcdabcd";
    job.job.project_name = "project";
    job.job.commit_id = std::string(40, 'a');
    job.job.ref = "refs/heads/main";
    job.workflow_text = "version: 1\njobs:\n  - name: build\n    steps:\n      - run: [true]\n";
    job.workflow = ckgit::parseCiWorkflow(job.workflow_text);
    ckgit::CiVmChannel channel(pair[0]);
    ckgit::ciVmHostHandshake(channel, job, {}, std::chrono::seconds(3));
    const auto result = channel.receive(std::chrono::seconds(3));
    passed = result.type == ckgit::CiVmFrameType::Error && result.payload == "VM guest executor is not available";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
  close(pair[0]);
  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 1) passed = false;
  if (!passed) return 1;
  std::cout << "VM guest handshake failed closed as expected\n";
  return 0;
}
