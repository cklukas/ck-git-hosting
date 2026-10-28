// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/windows_ci_executor.hpp"
#include "ckgit/archive.hpp"
#ifdef _WIN32
#include "ckgit/windows_sandbox.hpp"
#include "ckgit/windows.hpp"
#include "ckgit/path_utf8.hpp"
#include "ckgit/hash.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace ckgit {
namespace {
namespace fs = std::filesystem;
std::uint64_t now() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}
void ordinary(const fs::path& path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error("missing path or reparse point in job data: " + pathToUtf8(path));
}
void inspectTree(const fs::path& path) {
  ordinary(path);
  if (fs::is_directory(path)) for (const auto& item : fs::recursive_directory_iterator(path)) ordinary(item.path());
}
void copyTree(const fs::path& source, const fs::path& destination, const std::function<bool()>& monitor) {
  ordinary(source);
  fs::create_directories(destination);
  for (const auto& item : fs::recursive_directory_iterator(source)) {
    if (!monitor()) throw std::runtime_error("copy cancelled");
    ordinary(item.path());
    const auto target = destination / item.path().lexically_relative(source);
    if (item.is_directory()) fs::create_directory(target);
    else if (item.is_regular_file()) fs::copy_file(item.path(), target);
    else throw std::runtime_error("special file in job data");
  }
}
fs::path below(const fs::path& root, const fs::path& relative) {
  if (relative.is_absolute() || relative.has_root_path()) throw std::runtime_error("expected a relative job path");
  auto result = root;
  for (const auto& part : relative) {
    if (part == "..") throw std::runtime_error("job path escapes workspace");
    result /= part;
    ordinary(result);
  }
  return result;
}
std::string upper(std::string value) {
  for (auto& c : value) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return value;
}
std::string variable(const std::string& prefix, const std::string& name) {
  auto result = upper(name);
  for (auto& c : result) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
  return prefix + result;
}
struct Cache {
  fs::path stored, work;
  HANDLE lock = INVALID_HANDLE_VALUE;
  ~Cache() { if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock); }
};
}
std::string windowsCiToolchainFingerprint(const WindowsCiToolchain& staged,
    const std::function<bool()>& monitor) {
  Sha256 hash;
  const auto field = [&](std::string_view value) {
    hash.add(std::to_string(value.size())); hash.add(":"); hash.add(value);
  };
  field("ckgit-windows-toolchain-v1");
  ordinary(staged.root);
  std::vector<fs::path> entries;
  for (const auto& entry : fs::recursive_directory_iterator(staged.root)) {
    if (monitor && !monitor()) throw std::runtime_error("toolchain fingerprint cancelled");
    ordinary(entry.path());
    entries.push_back(entry.path().lexically_relative(staged.root));
  }
  std::sort(entries.begin(), entries.end());
  for (const auto& relative : entries) {
    if (monitor && !monitor()) throw std::runtime_error("toolchain fingerprint cancelled");
    const auto path = staged.root / relative;
    field(pathToUtf8(relative));
    if (fs::is_directory(path)) field("directory");
    else { field("file"); field(sha256HexOfFile(path)); }
  }
  field("paths");
  for (const auto& path : staged.binary_directories) field(pathToUtf8(path));
  field("environment");
  for (const auto& [key, value] : staged.environment) { field(key); field(value); }
  return hash.finish();
}
fs::path windowsCiCacheDirectory(const fs::path& root, std::string_view project,
    std::string_view job, std::string_view toolchain, std::string_view cache) {
  // Hash names as well: Windows case folding must not merge distinct CI names.
  Sha256 scope;
  for (const auto value : {project, job, toolchain}) {
    scope.add(std::to_string(value.size())); scope.add(":"); scope.add(value);
  }
  return root / "windows-x64-v2" / scope.finish() / sha256Hex(cache);
}
CiRunRecord executeWindowsCiJob(const CiPreparedJob& prepared, const CiExecutionOptions& options,
    const WindowsCiToolchain& toolchain, CiSourceProvider& source, CiResultSink& sink, CiSandboxReport* report) {
  CiRunRecord record;
  const auto& job = prepared.job;
  record.run_id = job.job_id;
  record.parent_run_id = job.parent_run_id;
  record.workflow_job = job.workflow_job;
  record.project_name = job.project_name;
  record.ref = job.ref;
  record.commit_id = job.commit_id;
  record.runner = options.runner;
  record.attempt = job.attempt;
  record.status = CiRunStatus::Running;
  record.started_epoch_seconds = now();
  std::unique_ptr<WindowsSandbox> sandbox;
  std::vector<std::unique_ptr<Cache>> caches;
  const auto finish = [&](CiRunStatus status, std::string detail) {
    record.status = status;
    detail.resize(std::min(detail.find('\n'), detail.size()));
    if (detail.size() > kMaximumCiDetailBytes) detail.resize(kMaximumCiDetailBytes);
    record.detail = std::move(detail);
    record.finished_epoch_seconds = now();
    record.heartbeat_epoch_seconds = record.finished_epoch_seconds;
    return sink.finish(record);
  };
  bool interrupted = false;
  auto heartbeat = std::chrono::steady_clock::now();
  const auto continuing = [&]() {
    sink.tick();
    const auto current = std::chrono::steady_clock::now();
    if (current - heartbeat >= std::chrono::seconds(1)) {
      sink.heartbeat(record);
      heartbeat = current;
    }
    interrupted = interrupted || sink.leaseLost() || sink.cancelRequested();
    return !interrupted;
  };
  try {
    if (!continuing()) return finish(CiRunStatus::Cancelled, "cancelled before setup");
    sink.runStarted(record);
    fs::create_directories(options.build_root);
    sandbox = std::make_unique<WindowsSandbox>(options.build_root);
    if (options.keep_scratch) sandbox->retainWorkspace();
    if (report) {
      *report = {};
      report->probed = true;
      report->network_isolated = true;
      report->filesystem_masked = true;
    }
    copyTree(toolchain.root, sandbox->tools(), continuing);
    fs::copy_file(toolchain.launcher, sandbox->tools() / "ckgit-job-launcher.exe", fs::copy_options::overwrite_existing);
    sandbox->sealTools(continuing);
    const auto work = sandbox->work() / "src";
    fs::create_directory(work);
    source.checkout({job.project_name, job.commit_id, prepared.workflow.checkout, job.ref}, work, continuing);
    inspectTree(work);
    std::map<std::string, std::string> run_environment;
    for (const auto& sister : prepared.sisters) {
      if (!continuing()) return finish(CiRunStatus::Cancelled, "cancelled during checkout");
      const auto destination = sandbox->work() / sister.name;
      fs::create_directory(destination);
      source.checkout({sister.name, sister.commit, sister.checkout, {}}, destination, continuing);
      inspectTree(destination);
      run_environment[variable("CKGIT_SISTER_", sister.name)] = pathToUtf8(destination);
    }
    std::string toolchain_identity;
    if (!options.cache_root.empty() && !prepared.workflow.caches.empty()) {
      auto staged = toolchain;
      staged.root = sandbox->tools();
      toolchain_identity = windowsCiToolchainFingerprint(staged, continuing);
    }
    std::vector<fs::path> bins;
    for (const auto& relative : toolchain.binary_directories) bins.push_back(below(sandbox->tools(), relative));
    const auto resolve = [&](const std::string& executable) {
      auto relative = pathFromUtf8(executable);
      if (relative.is_absolute()) throw std::runtime_error("job executables must be staged tool names or checkout-relative paths");
      if (relative.has_parent_path()) return below(work, relative);
      for (const auto& bin : bins) {
        auto path = bin / relative;
        if (!path.has_extension()) path += L".exe";
        if (fs::is_regular_file(path)) { ordinary(path); return path; }
      }
      throw std::runtime_error("tool is not staged: " + executable);
    };
    std::size_t index = 0;
    for (const auto& ci_job : prepared.workflow.jobs) {
      auto job_environment = run_environment;
      for (const auto& declared : prepared.workflow.caches) {
        auto cache = std::make_unique<Cache>();
        cache->work = sandbox->work() / ".cache" /
            sha256Hex(std::to_string(ci_job.name.size()) + ":" + ci_job.name + declared.name);
        if (!options.cache_root.empty()) {
          cache->stored = windowsCiCacheDirectory(options.cache_root, job.project_name,
              ci_job.name, toolchain_identity, declared.name);
          fs::create_directories(cache->stored.parent_path());
          auto lock_path = cache->stored;
          lock_path += L".lock";
          cache->lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
              0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
          if (cache->lock == INVALID_HANDLE_VALUE) throw std::runtime_error("build cache is busy or unavailable");
          if (fs::exists(cache->stored)) copyTree(cache->stored, cache->work, continuing);
        }
        fs::create_directories(cache->work);
        job_environment[variable("CKGIT_CACHE_", declared.name)] = pathToUtf8(cache->work);
        for (const auto& name : declared.env) job_environment[upper(name)] = pathToUtf8(cache->work);
        caches.push_back(std::move(cache));
      }
      std::map<std::string, std::string> environment;
      for (const auto& [key, raw] : toolchain.environment) {
        auto value = raw;
        const auto replacement = pathToUtf8(sandbox->tools());
        std::size_t offset = 0;
        while ((offset = value.find("${TOOLS}", offset)) != std::string::npos) {
          value.replace(offset, 8, replacement); offset += replacement.size();
        }
        environment[upper(key)] = value;
      }
      std::string path;
      for (const auto& bin : bins) { if (!path.empty()) path += ';'; path += pathToUtf8(bin); }
      environment["PATH"] = path;
      for (const auto& [key, value] : prepared.workflow.env) environment[upper(key)] = value;
      for (const auto& [key, value] : ci_job.env) environment[upper(key)] = value;
      for (const auto& assignment : options.extra_env) {
        const auto equal = assignment.find('=');
        if (equal != std::string::npos) environment[upper(assignment.substr(0, equal))] = assignment.substr(equal + 1);
      }
      for (const auto& [key, value] : job_environment) environment[key] = value;
      environment["CI"] = "true"; environment["CKGIT_CI"] = "1";
      environment["CKGIT_COMMIT"] = job.commit_id; environment["CKGIT_REF"] = job.ref;
      environment["HOME"] = pathToUtf8(sandbox->work() / "home");
      if (job.ref.starts_with("refs/heads/")) environment["CKGIT_BRANCH"] = job.ref.substr(11);
      if (job.ref.starts_with("refs/tags/")) environment["CKGIT_TAG"] = job.ref.substr(10);
      std::map<std::wstring, std::wstring> wide_environment;
      for (const auto& [key, value] : environment) wide_environment[utf8ToWide(key)] = utf8ToWide(value);
      for (const auto& step : ci_job.steps) {
        if (!continuing()) return finish(CiRunStatus::Cancelled, "cancelled before step");
        std::vector<std::string> command = step.argv;
        if (step.usesShell()) {
          const auto shell = step.shell.empty() ? "pwsh" : step.shell;
          if (shell == "sh") command = {shell, "-ec", step.script};
          else command = {shell, "-NoProfile", "-NonInteractive", "-Command",
              "$ErrorActionPreference='Stop'; $PSNativeCommandUseErrorActionPreference=$true; & {\n" + step.script +
              "\n}; if (!$?) { exit 1 }; if ($null -ne $LASTEXITCODE) { exit $LASTEXITCODE }"};
        }
        command.at(0) = pathToUtf8(resolve(command.at(0)));
        sink.stepStarted(record, index);
        ProcessOptions process;
        process.timeout = std::chrono::seconds(options.timeout_seconds);
        process.output_limit = 0;
        std::size_t logged = 0;
        bool truncated = false;
        process.on_output = [&](std::string_view bytes) {
          const auto count = std::min(bytes.size(), options.max_log_bytes - logged);
          if (count) sink.appendLog(index, bytes.substr(0, count));
          logged += count; truncated |= count != bytes.size();
        };
        process.should_continue = continuing;
        const auto result = runWindowsSandboxProcess(*sandbox, command, process, wide_environment, "src");
        // This call returns only after process creation with LPAC attributes
        // and Job Object limits succeeded. Setup failures throw and must not
        // be represented as a successfully applied isolation policy.
#if defined(_M_ARM64) || defined(__aarch64__)
        const std::string architecture = "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
        const std::string architecture = "x64";
#else
        const std::string architecture = "x86";
#endif
        record.execution = CiExecutionReport{"windows", architecture, "lpac", "denied",
            kWindowsSandboxMemoryBytes, kWindowsSandboxProcessLimit, options.timeout_seconds};
        CiStepResult step_result;
        step_result.name = step.name.empty() ? ci_job.name : step.name;
        step_result.exit_code = result.exit_code;
        step_result.timed_out = result.timed_out;
        step_result.output_truncated = truncated;
        record.steps.push_back(step_result);
        sink.stepFinished(record, index++);
        if (result.cancelled) return finish(CiRunStatus::Cancelled, "cancelled during step");
        if (result.timed_out) return finish(CiRunStatus::Timeout, "step exceeded its time budget");
        if (result.exit_code) return finish(CiRunStatus::Failure, "step exited " + std::to_string(result.exit_code));
      }
      if (ci_job.artifact) {
        const auto bundle = sandbox->tools().parent_path() / (ci_job.artifact->name + ".tar");
        std::string note;
        try {
          for (const auto& name : ci_job.artifact->paths) inspectTree(below(work, pathFromUtf8(name)));
          TarPackOptions pack;
          pack.max_bytes = options.artifact_max_bytes;
          pack.should_continue = continuing;
          packRegularTreeTar(work, ci_job.artifact->paths, bundle, pack);
        } catch (const std::exception& error) { note = error.what(); }
        record.artifacts.push_back(sink.putArtifact(*ci_job.artifact, bundle, note, prepared.is_release));
        std::error_code ignored; fs::remove(bundle, ignored);
      }
    }
    if (!continuing()) return finish(CiRunStatus::Cancelled, "cancelled before publication");
    if (!prepared.is_release && prepared.is_default_branch && prepared.workflow.pages_path) {
      const auto site = below(work, pathFromUtf8(*prepared.workflow.pages_path));
      inspectTree(site);
      sink.putPagesSite(site);
    }
    for (const auto& cache : caches) if (!cache->stored.empty()) {
      inspectTree(cache->work);
      fs::remove_all(cache->stored);
      copyTree(cache->work, cache->stored, continuing);
    }
    return finish(CiRunStatus::Success, "all steps passed");
  } catch (const std::exception& error) {
    return finish(interrupted || sink.cancelRequested() ? CiRunStatus::Cancelled : CiRunStatus::Error, error.what());
  }
}
}
#endif
