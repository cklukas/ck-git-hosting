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
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <iostream>
#include <fstream>
#include <functional>
#include <mutex>
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
int serve(const fs::path& config_path, bool once, bool check_only, const std::function<void()>& ready = {}) {
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
  {
    ckgit::WindowsSandbox probe(session.root());
    const auto launcher = probe.tools() / "ckgit-job-launcher.exe";
    fs::copy_file(toolchain.launcher, launcher);
    probe.sealTools();
    ckgit::ProcessOptions options;
    options.timeout = std::chrono::seconds(10);
    options.should_continue = [] { return !stop_requested.load(); };
    const auto checked = ckgit::runWindowsSandboxProcess(probe,
        {ckgit::pathToUtf8(launcher), "--self-check"}, options, {});
    if (checked.exit_code != 0 || checked.timed_out || (checked.output != "LPAC launcher ready\n" && checked.output != "LPAC launcher ready\r\n"))
      throw std::runtime_error("AppContainer startup self-check failed: " + checked.output);
  }
  if (check_only) { std::cout << "Windows runner configuration and AppContainer broker available\n"; return 0; }
  if (ready) ready();
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

// SCM callbacks only signal the same cooperative stop path used by the console.
// No build, network request or filesystem cleanup runs in a control handler.
struct ServiceHost {
  std::wstring name;
  fs::path config, log;
  SERVICE_STATUS_HANDLE handle = nullptr;
  SERVICE_STATUS status{};
  std::mutex mutex;
  int result = 1;
};
ServiceHost service;
void serviceStatus(DWORD state, DWORD error = NO_ERROR) {
  std::lock_guard lock(service.mutex);
  service.status = {};
  service.status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  service.status.dwCurrentState = state;
  service.status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
  service.status.dwWin32ExitCode = error;
  if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
    service.status.dwCheckPoint = 1;
    service.status.dwWaitHint = state == SERVICE_START_PENDING ? 60000 : 10000;
  }
  if (!SetServiceStatus(service.handle, &service.status))
    throw std::runtime_error("cannot report Windows service state");
}
DWORD WINAPI serviceControl(DWORD control, DWORD, void*, void*) {
  if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
    stop_requested = true;
    // Do not let a late control resurrect an already stopped service.
    std::lock_guard lock(service.mutex);
    if (service.status.dwCurrentState == SERVICE_RUNNING) {
      service.status.dwCurrentState = SERVICE_STOP_PENDING;
      service.status.dwControlsAccepted = 0;
      service.status.dwCheckPoint = 1;
      service.status.dwWaitHint = 10000;
      if (!SetServiceStatus(service.handle, &service.status)) return GetLastError();
    }
    return NO_ERROR;
  }
  return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}
std::wstring requireServiceAccount() {
  // A dedicated virtual account avoids passwords and does not grant the
  // broker administrator rights. Never silently run this mode as LocalSystem.
  const auto account = L"NT SERVICE\\" + service.name;
  DWORD sid_bytes = 0, domain_chars = 0;
  SID_NAME_USE use{};
  LookupAccountNameW(nullptr, account.c_str(), nullptr, &sid_bytes, nullptr, &domain_chars, &use);
  if (!sid_bytes) throw std::runtime_error("cannot resolve dedicated virtual service account");
  std::vector<unsigned char> sid(sid_bytes);
  std::wstring domain(domain_chars, L'\0');
  if (!LookupAccountNameW(nullptr, account.c_str(), sid.data(), &sid_bytes, domain.data(), &domain_chars, &use))
    throw std::runtime_error("cannot resolve service identity");
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("cannot inspect service token");
  DWORD bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  std::vector<unsigned char> data(bytes);
  const bool read = bytes && GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes);
  CloseHandle(token);
  if (!read || !EqualSid(sid.data(), reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid))
    throw std::runtime_error("service must run as its own NT SERVICE virtual account");
  BYTE admin_sid[SECURITY_MAX_SID_SIZE];
  DWORD admin_size = sizeof(admin_sid);
  BOOL administrator = TRUE;
  if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_sid, &admin_size) ||
      !CheckTokenMembership(nullptr, admin_sid, &administrator) || administrator)
    throw std::runtime_error("service account must not have administrator rights");
  LPWSTR text = nullptr;
  if (!ConvertSidToStringSidW(sid.data(), &text)) throw std::runtime_error("cannot format service SID");
  std::wstring result(text);
  LocalFree(text);
  return result;
}

struct ServiceHandle {
  HANDLE value = nullptr;
  explicit ServiceHandle(HANDLE handle = nullptr) : value(handle) {}
  ~ServiceHandle() { if (value) CloseHandle(value); }
  ServiceHandle(const ServiceHandle&) = delete;
  ServiceHandle& operator=(const ServiceHandle&) = delete;
};
struct ServiceWorkerProcess {
  HANDLE process = nullptr, thread = nullptr, job = nullptr;
  bool assigned = false;
  ~ServiceWorkerProcess() {
    // The supervisor owns every worker descendant, including its SSH clients.
    // On exceptional exit no child may escape the outer kill-on-close job.
    if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
      if (assigned) TerminateJobObject(job, 1);
      else TerminateProcess(process, 1);
      WaitForSingleObject(process, 5000);
    }
    if (job) CloseHandle(job);
    if (thread) CloseHandle(thread);
    if (process) CloseHandle(process);
  }
};
struct ServiceLog {
  std::ofstream stream;
  std::streambuf *out = nullptr, *err = nullptr;
  explicit ServiceLog(const fs::path& path) {
    ordinaryDirectory(path.parent_path());
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
      throw std::runtime_error("service log must be an ordinary file");
    stream.open(path, std::ios::app);
    if (!stream) throw std::runtime_error("cannot open service log");
    out = std::cout.rdbuf(stream.rdbuf());
    err = std::cerr.rdbuf(stream.rdbuf());
  }
  ~ServiceLog() { std::cout.rdbuf(out); std::cerr.rdbuf(err); }
};
int superviseServiceWorker() {
  const auto sid = requireServiceAccount();
  auto config = ckgit::loadRunnerConfig(service.config);
  fs::create_directories(config.build_root);
  ordinaryDirectory(config.build_root);
  // A recorded session gives the supervisor its own temporary directory and
  // crash recovery. The worker skips this live record during its own recovery.
  ckgit::WindowsRunnerSession supervisor(config.build_root);
  const auto temporary = supervisor.root() / "tmp";
  fs::create_directory(temporary);
  for (const auto* key : {L"TEMP", L"TMP", L"TMPDIR"})
    if (_wputenv_s(key, temporary.c_str())) throw std::runtime_error("cannot set service temporary directory");
  const auto nonce = supervisor.root().filename().native();
  const auto ready_name = L"Local\\ckgit-service-" + nonce + L"-ready";
  const auto stop_name = L"Local\\ckgit-service-" + nonce + L"-stop";
  const auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    throw std::runtime_error("cannot create service event descriptor");
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  ServiceHandle ready(CreateEventW(&attributes, TRUE, FALSE, ready_name.c_str()));
  const auto ready_error = GetLastError();
  ServiceHandle stop(CreateEventW(&attributes, TRUE, FALSE, stop_name.c_str()));
  const auto stop_error = GetLastError();
  LocalFree(descriptor);
  if (!ready.value || !stop.value || ready_error == ERROR_ALREADY_EXISTS || stop_error == ERROR_ALREADY_EXISTS)
    throw std::runtime_error("cannot reserve private service control events");
  ServiceHandle token;
  // NEW_CREDENTIALS preserves the local virtual-account identity but gives
  // this worker a fresh logon ID. Explicitly retain the parent's desktop so
  // the worker's OS-named station remains available for per-step creation.
  // No real network password is supplied: all remote access uses runner SSH.
  const auto unused_password = nonce + L"-no-network-logon";
  if (!LogonUserW(L"ckgit-no-network", L".", unused_password.c_str(), LOGON32_LOGON_NEW_CREDENTIALS,
      LOGON32_PROVIDER_WINNT50, &token.value))
    throw std::runtime_error("cannot create service worker logon: " + std::to_string(GetLastError()));
  wchar_t station[512]{}, desktop[512]{};
  DWORD bytes = 0;
  USEROBJECTFLAGS flags{};
  const auto parent_station = GetProcessWindowStation();
  if (!GetUserObjectInformationW(parent_station, UOI_NAME, station, sizeof(station), &bytes) ||
      !GetUserObjectInformationW(parent_station, UOI_FLAGS, &flags, sizeof(flags), &bytes) ||
      !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &bytes) ||
      (flags.dwFlags & WSF_VISIBLE)) throw std::runtime_error("service requires its own noninteractive desktop");
  auto desktop_path = std::wstring(station) + L"\\" + desktop;
  const auto executable = executableDirectory() / "ck-ci-runnerd.exe";
  auto command = ckgit::windowsCommandLine({executable.native(), L"service-worker", service.config.native(),
      service.log.native(), service.name, ready_name, stop_name});
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.lpDesktop = desktop_path.data();
  PROCESS_INFORMATION info{};
  ServiceWorkerProcess child;
  child.job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  // Only trusted broker-created LPAC roots explicitly break away. They are
  // born in their own UI-restricted, non-breakaway kill-on-close job.
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
  if (!child.job || !SetInformationJobObject(child.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    throw std::runtime_error("cannot create service worker process-tree guard");
  if (!CreateProcessWithTokenW(token.value, 0, executable.c_str(), command.data(), CREATE_SUSPENDED | CREATE_NO_WINDOW,
      nullptr, service.config.parent_path().c_str(), &startup, &info))
    throw std::runtime_error("cannot launch service worker: " + std::to_string(GetLastError()));
  child.process = info.hProcess;
  child.thread = info.hThread;
  if (!AssignProcessToJobObject(child.job, child.process)) throw std::runtime_error("cannot guard service worker process tree");
  child.assigned = true;
  if (ResumeThread(child.thread) == static_cast<DWORD>(-1)) throw std::runtime_error("cannot start service worker");
  std::cout << "Service worker pid=" << info.dwProcessId << '\n';
  bool running = false, stopping = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  for (;;) {
    const auto exited = WaitForSingleObject(child.process, 50);
    if (exited == WAIT_OBJECT_0) {
      DWORD code = 1;
      if (!GetExitCodeProcess(child.process, &code)) throw std::runtime_error("cannot read service worker exit");
      std::cout << "Service worker exit=" << code << '\n';
      return stopping && code == 0 ? 0 : 1;
    }
    if (exited != WAIT_TIMEOUT) throw std::runtime_error("cannot wait for service worker");
    if (stop_requested && !stopping) {
      if (!SetEvent(stop.value)) throw std::runtime_error("cannot stop service worker");
      stopping = true;
      deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    if (!running && !stopping && WaitForSingleObject(ready.value, 0) == WAIT_OBJECT_0) {
      serviceStatus(SERVICE_RUNNING);
      running = true;
      std::cout << "Service worker ready\n";
    }
    if ((!running || stopping) && std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error(stopping ? "service worker stop timed out; terminating process tree" : "service worker startup timed out");
  }
}
int runServiceWorker(const std::vector<std::string>& args) {
  service.name = ckgit::utf8ToWide(args[4]);
  requireServiceAccount();
  const auto config = ckgit::pathFromUtf8(args[2]);
  const auto log = ckgit::pathFromUtf8(args[3]);
  if (!config.is_absolute() || !log.is_absolute()) throw std::runtime_error("service worker paths must be absolute");
  ServiceLog output(log);
  ServiceHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ckgit::utf8ToWide(args[5]).c_str()));
  ServiceHandle stop(OpenEventW(SYNCHRONIZE, FALSE, ckgit::utf8ToWide(args[6]).c_str()));
  if (!ready.value || !stop.value) throw std::runtime_error("service worker control events unavailable");
  std::jthread monitor([&](std::stop_token request) {
    while (!request.stop_requested()) {
      const auto result = WaitForSingleObject(stop.value, 50);
      if (result != WAIT_TIMEOUT) { stop_requested = true; return; }
    }
  });
  try {
    return serve(config, false, false, [&] {
      if (!SetEvent(ready.value)) throw std::runtime_error("cannot report worker readiness");
    });
  } catch (const std::exception& error) {
    std::cerr << "service worker: " << error.what() << '\n';
    return 1;
  }
}
void WINAPI serviceMain(DWORD, wchar_t**) {
  service.handle = RegisterServiceCtrlHandlerExW(service.name.c_str(), serviceControl, nullptr);
  if (!service.handle) return;
  try {
    serviceStatus(SERVICE_START_PENDING);
    requireServiceAccount();
    auto host_log = service.log;
    host_log += L".host.log";
    ServiceLog log(host_log);
    std::cout << "Starting dedicated Windows runner service\n";
    try { service.result = superviseServiceWorker(); }
    catch (const std::exception& error) {
      std::cerr << "ck-ci-runnerd service: " << error.what() << '\n';
      service.result = 1;
    }
  } catch (const std::exception&) { service.result = 1; }
  try { serviceStatus(SERVICE_STOPPED, service.result ? ERROR_PROCESS_ABORTED : NO_ERROR); }
  catch (...) { service.result = 1; }
}
int runService(const fs::path& config, const fs::path& log, const std::string& name) {
  if (name.empty() || name.size() > 80 || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
      })) throw std::runtime_error("invalid service name");
  if (log.empty() || !log.is_absolute()) throw std::runtime_error("service requires an absolute --log path");
  service.name = ckgit::utf8ToWide(name);
  service.config = config;
  service.log = log;
  SERVICE_TABLE_ENTRYW entries[] = {{service.name.data(), serviceMain}, {nullptr, nullptr}};
  if (!StartServiceCtrlDispatcherW(entries)) throw std::runtime_error("service command must be started by Windows Service Control Manager");
  return service.result;
}
}
int wmain(int argc, wchar_t** argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  try {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(ckgit::wideToUtf8(argv[i]));
    if (argc == 7 && args[1] == "service-worker") return runServiceWorker(args);
    if (argc < 2 || args[1] == "--help" || args[1] == "-h") {
      std::cout << "ck-ci-runnerd serve --remote --config RUNNER.INI [--once]\n"
                   "ck-ci-runnerd check --config RUNNER.INI\n"
                   "ck-ci-runnerd service --config RUNNER.INI --log LOGFILE [--service-name NAME]\n";
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
    fs::path service_log;
    std::string service_name = "ckgit-runner";
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--config" && i + 1 < args.size()) config = ckgit::pathFromUtf8(args[++i]);
      else if (args[1] == "service" && args[i] == "--log" && i + 1 < args.size()) service_log = ckgit::pathFromUtf8(args[++i]);
      else if (args[1] == "service" && args[i] == "--service-name" && i + 1 < args.size()) service_name = args[++i];
      else if (args[i] == "--once") once = true;
      else if (args[i] != "--remote") throw std::runtime_error("unknown or incomplete runner option: " + args[i]);
    }
    if (config.empty() || !config.is_absolute()) throw std::runtime_error("supply an absolute --config path");
    if (args[1] == "service") {
      if (once) throw std::runtime_error("service does not support --once");
      return runService(config, service_log, service_name);
    }
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
