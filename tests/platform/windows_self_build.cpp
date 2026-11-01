// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Opt-in full repository qualification using the production Windows executor.
#include "ckgit/windows_ci_executor.hpp"
#include "ckgit/runner_config.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/windows.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
class Source : public ckgit::CiSourceProvider {
 public:
  explicit Source(fs::path path) : root(std::move(path)) {}
  void checkout(const ckgit::CiCheckoutRequest&, const fs::path& destination) override {
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
      const auto attributes = GetFileAttributesW(it->path().c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("self-build snapshot contains a redirected path");
      if (it->is_directory() && it->path().filename() == ".git") { it.disable_recursion_pending(); continue; }
      const auto target = destination / it->path().lexically_relative(root);
      if (it->is_directory()) fs::create_directory(target);
      else if (it->is_regular_file()) fs::copy_file(it->path(), target);
      else throw std::runtime_error("self-build snapshot contains a special file");
    }
  }
 private:
  fs::path root;
};
class Sink : public ckgit::CiResultSink {
 public:
  explicit Sink(fs::path path) : root(std::move(path)) { fs::create_directories(root); }
  void runStarted(const ckgit::CiRunRecord&) override { std::cout << "Self-build started\n"; }
  void stepStarted(const ckgit::CiRunRecord&, std::size_t index) override {
    log.open(root / ("step-" + std::to_string(index) + ".log"), std::ios::binary);
    if (!log) throw std::runtime_error("cannot create qualification log");
  }
  void appendLog(std::size_t, std::string_view bytes) override {
    log.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    log.flush();
    if (!log) throw std::runtime_error("cannot write qualification log");
  }
  void stepFinished(const ckgit::CiRunRecord&, std::size_t) override { log.close(); }
  void heartbeat(const ckgit::CiRunRecord&) override {}
  void progress(const ckgit::CiRunRecord&) override {}
  bool cancelRequested() override { return false; }
  bool leaseLost() override { return false; }
  ckgit::CiArtifactRecord putArtifact(const ckgit::CiArtifact& artifact, const fs::path& bundle,
      const std::string& note, bool) override {
    if (!note.empty()) throw std::runtime_error("artifact: " + note);
    fs::copy_file(bundle, root / (artifact.name + ".tar"));
    ckgit::CiArtifactRecord record;
    record.name = artifact.name; record.bytes = fs::file_size(bundle); record.sha256 = ckgit::sha256HexOfFile(bundle);
    std::ofstream(root / "artifact.sha256") << record.sha256 << "  " << artifact.name << ".tar\n";
    return record;
  }
  void putPagesSite(const fs::path&) override { throw std::runtime_error("Windows self-build must not publish Pages"); }
  ckgit::CiRunRecord finish(ckgit::CiRunRecord record) override {
    if (log.is_open()) log.close();
    std::ofstream(root / "result.txt") << "status=" << ckgit::ciRunStatusName(record.status)
        << "\ndetail=" << record.detail << "\ncommit=" << record.commit_id << '\n';
    std::cout << ckgit::ciRunStatusName(record.status) << ": " << record.detail << '\n';
    return record;
  }
 private:
  fs::path root;
  std::ofstream log;
};
int wmain(int argc, wchar_t** argv) {
  std::cout << std::unitbuf;
  try {
    if (argc != 5) throw std::runtime_error("usage: self-build RUNNER.INI SOURCE OUTPUT COMMIT");
    const auto config = ckgit::loadRunnerConfig(fs::path(argv[1]));
    if (!config.toolchain_root) throw std::runtime_error("toolchain root is required");
    if (fs::exists(fs::path(argv[3]))) throw std::runtime_error("qualification output already exists");
    const auto commit = ckgit::wideToUtf8(argv[4]);
    if (!ckgit::isValidObjectId(commit)) throw std::runtime_error("invalid source commit");
    std::ifstream workflow_file(fs::path(argv[2]) / ".ckgit" / "ci.yml", std::ios::binary);
    const std::string workflow_text(std::istreambuf_iterator<char>(workflow_file), {});
    ckgit::CiPreparedJob prepared;
    prepared.workflow = ckgit::selectCiWorkflowJob(ckgit::parseCiWorkflow(workflow_text), "windows-installers");
    prepared.job.project_name = "ck-git-hosting";
    prepared.job.job_id = "native-self-build";
    prepared.job.parent_run_id = "self-build-parent";
    prepared.job.workflow_job = "windows-installers";
    prepared.job.commit_id = commit;
    prepared.job.ref = "refs/heads/windows-port-ci";
    ckgit::WindowsCiToolchain toolchain;
    toolchain.root = *config.toolchain_root;
    toolchain.launcher = fs::path(argv[0]).parent_path() / "ckgit-job-launcher.exe";
    toolchain.binary_directories = config.toolchain_paths;
    toolchain.environment = config.toolchain_environment;
    ckgit::CiExecutionOptions options;
    options.runner = config.name; options.build_root = config.build_root;
    options.timeout_seconds = config.timeout_seconds.value_or(1800);
    options.max_log_bytes = 4u << 20;
    options.keep_scratch = true;  // retain the current qualification build for diagnosis
    Source source{fs::path(argv[2])};
    Sink sink{fs::path(argv[3])};
    const auto record = ckgit::executeWindowsCiJob(prepared, options, toolchain, source, sink);
    return record.status == ckgit::CiRunStatus::Success ? 0 : 1;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
