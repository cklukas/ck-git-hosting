// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_remote.hpp"
#include "ckgit/path_utf8.hpp"
#include "ckgit/temp_directory.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/ssh_command.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
namespace ckgit {
namespace {
// A path or token that can be embedded in GIT_SSH_COMMAND, which Git hands to
// a shell: only characters that need no quoting.
bool isShellSafe(std::string_view value) {
  return !value.empty() && value.size() <= 1024 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
                  c == '.' || c == '_' || c == '-' || c == '@' || c == ':' || c == '+' || c == '=';
         });
}

std::filesystem::path selfExecutable() {
#if defined(_WIN32)
  std::wstring path(32768, L'\0');
  const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!size || size >= path.size()) throw std::runtime_error("cannot resolve runner executable");
  path.resize(size);
  return std::filesystem::path(path);
#elif defined(__linux__)
  return std::filesystem::canonical("/proc/self/exe");
#elif defined(__APPLE__)
  char buffer[4096];
  uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) != 0) throw std::runtime_error("executable path too long");
  return std::filesystem::canonical(buffer);
#else
  return "/usr/bin/ck-ci-runnerd";
#endif
}

std::string genericPath(const std::filesystem::path& path) {
  const auto value = path.generic_u8string();
  return {value.begin(), value.end()};
}
// Git invokes GIT_SSH_COMMAND through its POSIX shell, including Git for Windows.
std::string shellQuote(std::string_view value) {
  if (value.empty() || hasControlCharacter(value)) throw std::invalid_argument("invalid runner command path");
  std::string out = "'";
  for (const char c : value) { if (c == '\'') out += "'\\''"; else out += c; }
  return out + "'";
}
std::string quoteSshPath(const std::filesystem::path& path, bool literal_percent = true) {
  const auto value = genericPath(path);
  if (hasControlCharacter(value)) throw std::invalid_argument("invalid SSH path");
  std::string out = "\"";
  for (const char c : value) {
    if (c == '\\' || c == '"') out += '\\';
    if (c == '%' && literal_percent) out += '%';
    out += c;
  }
  return out + "\"";
}
}  // namespace
// --- transport -----------------------------------------------------------------

CiRemoteTransport::CiRemoteTransport(RunnerConfig config, std::filesystem::path config_path, unsigned protocol_version)
    : config_(std::move(config)), config_path_(std::move(config_path)), protocol_version_(protocol_version) {
  if (protocol_version < 1 || protocol_version > kCiRunnerProtocolVersion) {
    throw std::invalid_argument("unsupported runner protocol version");
  }
}

bool CiRemoteTransport::negotiate(std::string_view version_hex, const std::function<bool()>& should_continue) {
  execution_report_ = false;
  for (unsigned version = kCiRunnerProtocolVersion; version > 0; --version) {
    CiRemoteTransport candidate(config_, config_path_, version);
    CiRunnerCommand hello;
    hello.op = CiRunnerOp::kHello;
    hello.version_hex = std::string(version_hex);
    const auto answer = candidate.run(hello, {}, std::chrono::seconds(60), should_continue);
    if (answer.cancelled) return false;
    const std::string expected = "ok\nprotocol=" + std::to_string(version) + "\n";
    if (answer.exit_code == 0 && !answer.timed_out && answer.output.rfind(expected, 0) == 0) {
      protocol_version_ = version;
      execution_report_ = answer.output.find("\nexecution_report=1\n") != std::string::npos;
      return true;
    }
  }
  return false;
}

std::vector<std::string> CiRemoteTransport::sshArguments() const {
  std::vector<std::string> argv = {pathToUtf8(config_.ssh_program),
                                   "-o", "BatchMode=yes",
                                   "-o", "ConnectTimeout=15",
                                   "-o", "ServerAliveInterval=15",
                                   "-o", "ServerAliveCountMax=4"};
#ifndef _WIN32
  argv.insert(argv.end(), {"-o", "ControlMaster=auto", "-o",
      "ControlPath=" + quoteSshPath(config_.build_root.parent_path() / ".ssh-control-%C", false),
      "-o", "ControlPersist=120"});
#else
  argv.insert(argv.end(), {"-o", "ControlMaster=no"});
#endif
  if (config_.identity) {
    argv.insert(argv.end(), {"-o", "IdentitiesOnly=yes", "-i", pathToUtf8(*config_.identity)});
  }
  if (config_.known_hosts) {
    argv.insert(argv.end(), {"-o", "UserKnownHostsFile=" + quoteSshPath(*config_.known_hosts), "-o",
                             "StrictHostKeyChecking=yes"});
  } else {
    argv.insert(argv.end(), {"-o", "StrictHostKeyChecking=accept-new"});
  }
  argv.push_back(config_.server);
  return argv;
}

CiRemoteTransport::Result CiRemoteTransport::run(const CiRunnerCommand& command, std::string_view input,
                                                 std::chrono::seconds timeout,
                                                 const std::function<bool()>& should_continue) const {
  std::vector<std::string> argv = sshArguments();
  auto negotiated = command;
  negotiated.protocol_version = protocol_version_;
  argv.push_back(formatCiRunnerCommand(negotiated));
  struct PayloadDirectory {
    std::filesystem::path path;
    ~PayloadDirectory() {
      if (!path.empty()) { std::error_code error; std::filesystem::remove_all(path, error); }
    }
  } payload;
  ProcessOptions options;
  options.timeout = timeout;
  options.should_continue = should_continue;
  options.output_limit = kMaximumCiClaimResponseBytes + 4096;
  options.inherit_stderr = true;
  if (!input.empty()) {
    payload.path = createTempDirectory(config_.build_root, "request-");
    options.input_file = payload.path / "stdin";
    std::ofstream file(*options.input_file, std::ios::binary | std::ios::trunc);
    file.write(input.data(), static_cast<std::streamsize>(input.size()));
    file.close();
    if (!file) throw std::runtime_error("could not stage runner request payload");
  }
  const auto child = runProcess(argv, options);
  return Result{child.output_truncated ? -1 : child.exit_code, child.timed_out, child.output, child.cancelled};
}

CiRemoteTransport::Session CiRemoteTransport::open(const CiRunnerCommand& command) const {
  std::vector<std::string> argv = sshArguments();
  auto negotiated = command;
  negotiated.protocol_version = protocol_version_;
  argv.push_back(formatCiRunnerCommand(negotiated));
  return std::make_unique<ProcessStream>(argv);
}

void CiRemoteTransport::close(Session& session) { session.reset(); }

std::string CiRemoteTransport::gitSshCommand(const std::string& job_id, const std::string& token) const {
  const std::string self = genericPath(selfExecutable());
  const std::string config = genericPath(std::filesystem::absolute(config_path_));
  if (!isValidCiId(job_id) || !isShellSafe(token)) {
    throw std::runtime_error("the runner's paths cannot be passed to git safely");
  }
  return shellQuote(self) + " ssh-shim --config " + shellQuote(config) + " --job " + job_id + " --token " + token +
         " --protocol " + std::to_string(protocol_version_);
}

std::string CiRemoteTransport::fetchUrl(const std::string& project) const {
  return config_.server + ":" + (project.front() == '-' ? "./" : "") + project + ".git";
}

int runCiSshShim(const std::filesystem::path& config_path, const std::string& job_id, const std::string& token,
                 const std::vector<std::string>& git_arguments, unsigned protocol_version) {
  if (git_arguments.empty()) return 2;
  const std::optional<SshCommand> wanted = parseSshOriginalCommand(git_arguments.back());
  if (!wanted.has_value() || wanted->kind != SshCommandKind::kUploadPack) return 2;
  const CiRemoteTransport transport(loadRunnerConfig(config_path), config_path, protocol_version);
  CiRunnerCommand command;
  command.protocol_version = transport.protocolVersion();
  command.op = CiRunnerOp::kUploadPack;
  command.job_id = job_id;
  command.token = token;
  command.project = wanted->project_name;
  std::vector<std::string> argv = transport.sshArguments();
  argv.insert(argv.end() - 1, {"-o", "SendEnv=GIT_PROTOCOL"});
  argv.push_back(formatCiRunnerCommand(command));
  return execProcess(argv);
}

}  // namespace ckgit
