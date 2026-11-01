// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "process_tools_internal.hpp"
#include "ckgit/path_utf8.hpp"
#include <utility>

namespace ckgit {
namespace {
struct Tools { std::string git, ssh; };
thread_local const Tools* active_tools = nullptr;
void validateProgram(const std::string& program) {
  if (!program.empty() && !pathFromUtf8(program).is_absolute())
    throw std::invalid_argument("selected executable must be an absolute path");
  for (const unsigned char character : program)
    if (character < 32 || character == 127) throw std::invalid_argument("control character in executable path");
}
bool namedTool(const std::string& program, std::string_view tool) {
  if (program == tool) return true;
#ifdef _WIN32
  return program == std::string(tool) + ".exe";
#else
  return false;
#endif
}
std::string gitShellQuote(std::string path) {
#ifdef _WIN32
  // Git for Windows executes GIT_SSH_COMMAND with its POSIX shell.
  for (auto& character : path) if (character == '\\') character = '/';
#endif
  std::string quoted = "'";
  for (const char character : path) quoted += character == '\'' ? "'\\''" : std::string(1, character);
  return quoted + "'";
}
}
struct ProcessToolScope::State { Tools tools; const Tools* previous; };
ProcessToolScope::ProcessToolScope(std::string git, std::string ssh) {
  validateProgram(git); validateProgram(ssh);
  state_ = std::make_unique<State>(State{{std::move(git), std::move(ssh)}, active_tools});
  active_tools = &state_->tools;
}
ProcessToolScope::~ProcessToolScope() { active_tools = state_->previous; }
std::string selectedProcessProgram(const std::string& program) {
  if (active_tools) {
    if (namedTool(program, "git") && !active_tools->git.empty()) return active_tools->git;
    if (namedTool(program, "ssh") && !active_tools->ssh.empty()) return active_tools->ssh;
  }
  return program;
}
std::vector<std::string> selectedProcessArguments(const std::vector<std::string>& arguments) {
  auto selected = arguments;
  if (!selected.empty()) selected.front() = selectedProcessProgram(selected.front());
  return selected;
}
std::optional<ProcessOptions> selectedProcessOptions(const std::vector<std::string>& arguments, const ProcessOptions& requested) {
  if (active_tools && !active_tools->ssh.empty() && !arguments.empty() &&
      (namedTool(arguments.front(), "git") || (!active_tools->git.empty() && arguments.front() == active_tools->git))) {
    auto options = requested;
    options.environment["GIT_SSH_COMMAND"] = gitShellQuote(active_tools->ssh) +
        " -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30 -o ServerAliveCountMax=6";
    options.environment["GIT_SSH_VARIANT"] = "ssh";
    options.environment["GIT_SSH"] = std::nullopt;
    return options;
  }
  return std::nullopt;
}
}
