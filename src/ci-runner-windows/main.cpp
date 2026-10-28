// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_remote.hpp"
#include "ckgit/windows_ci_executor.hpp"
#include "ckgit/windows_sandbox.hpp"
#include "ckgit/windows.hpp"
#include "ckgit/path_utf8.hpp"
#include "ckgit/cli_help.hpp"
#include "ckgit/text.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
namespace fs = std::filesystem;
std::atomic_bool stop_requested = false;
static_assert(std::atomic_bool::is_always_lock_free);
void signalStop(int) { stop_requested = 1; }
BOOL WINAPI consoleStop(DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
      event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
    stop_requested = true;
    // A returning close handler lets Windows terminate the broker immediately.
    // Keep its cleanup window available to the main thread. Ctrl+C/Break do
    // not terminate the process when this handler returns.
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) Sleep(5000);
    return TRUE;
  }
  return FALSE;
}
fs::path executableDirectory() {
  std::wstring path(32768, L'\0');
  const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!count || count >= path.size()) throw std::runtime_error("cannot locate runner executable");
  path.resize(count);
  return fs::path(path).parent_path();
}
void ordinaryDirectory(const fs::path& path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("runner directory missing or redirected");
}
struct RunLock {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit RunLock(const fs::path& path) {
    value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (value == INVALID_HANDLE_VALUE) throw std::runtime_error("runner already active or build root unavailable");
  }
  ~RunLock() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
int serve(const fs::path& config_path, bool once, bool check_only) {
  auto config = ckgit::loadRunnerConfig(config_path);
  if (!config.toolchain_root) throw std::runtime_error("Windows runner requires ci_toolchain_root");
  ordinaryDirectory(*config.toolchain_root);
  fs::create_directories(config.build_root);
  ordinaryDirectory(config.build_root);
  RunLock lock(config.build_root / "runner.lock");
  ckgit::WindowsRunnerSession session(config.build_root);
  const auto temporary = session.root() / "tmp";
  fs::create_directory(temporary);
  for (const auto* key : {L"TEMP", L"TMP", L"TMPDIR"})
    if (_wputenv_s(key, temporary.c_str())) throw std::runtime_error("cannot set runner temporary directory");
  ckgit::WindowsCiToolchain toolchain;
  toolchain.root = *config.toolchain_root;
  toolchain.launcher = executableDirectory() / "ckgit-job-launcher.exe";
  if (!fs::is_regular_file(toolchain.launcher)) throw std::runtime_error("ckgit-job-launcher.exe is missing beside the runner");
  toolchain.binary_directories = config.toolchain_paths;
  toolchain.environment = config.toolchain_environment;
  for (const auto& relative : toolchain.binary_directories) ordinaryDirectory(toolchain.root / relative);
  // Verify the required broker privileges before claiming a lease. Profiles
  // are disposable; there is no fallback to an unrestricted process.
  { ckgit::WindowsSandbox probe(session.root()); ckgit::WindowsSandboxDesktop desktop(probe); }
  if (check_only) { std::cout << "Windows runner configuration and AppContainer broker available\n"; return 0; }
  config.build_root = session.root();
  ckgit::CiRemoteTransport transport(config, config_path);
  bool negotiated = false;
  unsigned failures = 0;
  while (!stop_requested) {
    if (!negotiated) {
      negotiated = transport.negotiate(ckgit::hexEncode(ckgit::buildVersion()), [] { return !stop_requested.load(); });
      if (negotiated && transport.protocolVersion() < 2)
        throw std::runtime_error("Windows runner requires runner protocol 2 and explicit platform routing; upgrade the server");
      if (negotiated) std::cout << "Connected to " << config.server << " as " << config.name
          << " (protocol " << transport.protocolVersion() << ")\n";
    }
    ckgit::CiRunnerCommand claim;
    claim.op = ckgit::CiRunnerOp::kClaim;
    // Bound shutdown latency while idle, without requiring a second thread to
    // terminate a request while it owns the shared transport.
    claim.wait_seconds = once ? 0 : std::min(config.claim_wait_seconds, 5u);
    const auto answer = negotiated ? transport.run(claim, {}, std::chrono::seconds(claim.wait_seconds + 30), [] { return !stop_requested.load(); })
                                   : ckgit::CiRemoteTransport::Result{};
    std::optional<ckgit::CiRemoteJob> job;
    bool good = answer.exit_code == 0;
    if (good) try { job = ckgit::parseCiClaimResponse(answer.output); }
      catch (const std::exception& error) { std::cerr << "Invalid claim response: " << error.what() << '\n'; good = false; }
    if (!good) {
      negotiated = false;
      if (once && ++failures >= 3) return 1;
      if (!once) ++failures;
      const auto delay = std::min(60u, 1u << std::min(failures, 6u));
      for (unsigned i = 0; i < delay * 10 && !stop_requested; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    failures = 0;
    if (!job) { if (once) return 0; continue; }
    const auto& id = job->prepared.job.job_id;
    std::cout << "Claimed " << job->prepared.job.project_name << ' ' << id << '\n';
    ckgit::CiExecutionOptions execution;
    execution.runner = config.name;
    execution.build_root = session.root() / "jobs";
    execution.timeout_seconds = std::min(job->policy.timeout_seconds, config.timeout_seconds.value_or(86400));
    execution.max_log_bytes = static_cast<std::size_t>(job->policy.max_log_bytes);
    execution.artifact_max_bytes = static_cast<std::size_t>(job->policy.artifact_max_bytes);
    execution.cache_root = config.cache_root.value_or(fs::path{});
    // Windows currently applies the stricter, network-denied LPAC policy even
    // when the host would permit network access.
    execution.allow_network = false;
    ckgit::RemoteCiSource source(transport, id, job->token);
    ckgit::RemoteCiSink sink(transport, *job, session.root() / "relay", [] { return stop_requested.load(); });
    try {
      const auto record = ckgit::executeWindowsCiJob(job->prepared, execution, toolchain, source, sink);
      std::cout << "Finished " << id << ' ' << (sink.abandoned() ? "handed back" : ckgit::ciRunStatusName(record.status)) << '\n';
    } catch (const std::exception& error) {
      std::cerr << "Job " << id << " reporting failed: " << error.what() << '\n';
    }
  }
  return 0;
}
}
int wmain(int argc, wchar_t** argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  try {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(ckgit::wideToUtf8(argv[i]));
    if (argc < 2 || args[1] == "--help" || args[1] == "-h") {
      std::cout << "ck-ci-runnerd serve --remote --config RUNNER.INI [--once]\n"
                   "ck-ci-runnerd check --config RUNNER.INI\n";
      return argc < 2 ? 2 : 0;
    }
    if (args[1] == "--version" || args[1] == "-V") { std::cout << ckgit::versionLine("ck-ci-runnerd"); return 0; }
    fs::path config;
    if (args[1] == "ssh-shim") {
      std::string job, token;
      unsigned protocol = 1;
      std::size_t i = 2;
      for (; i + 1 < args.size(); i += 2) {
        if (args[i] == "--config") config = ckgit::pathFromUtf8(args[i + 1]);
        else if (args[i] == "--job") job = args[i + 1];
        else if (args[i] == "--token") token = args[i + 1];
        else if (args[i] == "--protocol") {
          if (args[i + 1] != "1" && args[i + 1] != "2") throw std::runtime_error("invalid shim protocol");
          protocol = args[i + 1] == "2" ? 2u : 1u;
        } else break;
      }
      return ckgit::runCiSshShim(config, job, token, {args.begin() + static_cast<std::ptrdiff_t>(i), args.end()}, protocol);
    }
    bool once = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--config" && i + 1 < args.size()) config = ckgit::pathFromUtf8(args[++i]);
      else if (args[i] == "--once") once = true;
      else if (args[i] != "--remote") throw std::runtime_error("unknown or incomplete runner option: " + args[i]);
    }
    if (config.empty() || !config.is_absolute()) throw std::runtime_error("supply an absolute --config path");
    if (args[1] != "serve" && args[1] != "check") throw std::runtime_error("unknown runner command");
    std::signal(SIGINT, signalStop);
    std::signal(SIGTERM, signalStop);
    if (!SetConsoleCtrlHandler(consoleStop, TRUE)) throw std::runtime_error("cannot install shutdown handler");
    return serve(config, once, args[1] == "check");
  } catch (const std::exception& error) {
    std::cerr << "ck-ci-runnerd: " << error.what() << '\n';
    return 1;
  }
}
