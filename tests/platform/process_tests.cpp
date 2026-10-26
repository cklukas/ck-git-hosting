// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/process.hpp"
#include "ckgit/windows.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/control_rpc.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/ci_workflow.hpp"
#include "ckgit/ci_runner_protocol.hpp"
#include "ckgit/ci_remote.hpp"
#include "ckgit/temp_directory.hpp"
#include "ckgit/path_utf8.hpp"
#include <fstream>
#include <cstdlib>
#include "ckgit/web_renderer.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
int run(const std::vector<std::string>& args) {
  if (args.size() > 1) {
    if (args[1] == "-o") {
#ifdef _WIN32
      _setmode(_fileno(stdin), _O_BINARY);
      _setmode(_fileno(stdout), _O_BINARY);
#endif
      const auto command = ckgit::parseCiRunnerCommand(args.back());
      if (!command) return 2;
      if (command->op == ckgit::CiRunnerOp::kHello) {
        std::cout << "ok\nprotocol=" << command->protocol_version << "\n";
      } else if (command->op == ckgit::CiRunnerOp::kClaim) {
        std::cout << "none\n";
      } else if (command->op == ckgit::CiRunnerOp::kStream || command->op == ckgit::CiRunnerOp::kPut) {
        if (command->op == ckgit::CiRunnerOp::kStream) std::cout << "ready\n" << std::flush;
        std::cout << std::cin.rdbuf();
      } else return 2;
      return 0;
    }
    if (args[1] == "input") {
#ifdef _WIN32
      _setmode(_fileno(stdin), _O_BINARY);
      _setmode(_fileno(stdout), _O_BINARY);
#endif
      // Fill stdout before reading stdin: a pipe writer must not deadlock
      // the parent while the child waits for its output to be drained.
      std::cout << std::string(1024 * 1024, 'o');
      std::cout << std::cin.rdbuf();
      return 0;
    }
    if (args[1] == "echo") {
      for (std::size_t i = 2; i < args.size(); ++i) std::cout << args[i].size() << ':' << args[i] << '\n';
      return 0;
    }
    if (args[1] == "large") { std::cout << std::string(1024 * 1024, 'x'); return 0; }
    if (args[1] == "sleep") { std::this_thread::sleep_for(std::chrono::seconds(10)); return 0; }
    if (args[1] == "exit") return 37;
#ifdef _WIN32
    if (args[1] == "tree") {
      const auto child = ckgit::spawnAttachedProcess({args[0], "sleep"});
      std::cout << child << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(10));
      return 0;
    }
    if (args[1] == "handle") {
      const auto handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(args.at(2))));
      return !SetEvent(handle) && GetLastError() == ERROR_INVALID_HANDLE ? 0 : 1;
    }
#endif
    return 99;
  }
  require(ckgit::windowsCommandLine({L"tool", L"", L"a\\\"b", L"a b\\"}) ==
          L"\"tool\" \"\" \"a\\\\\\\"b\" \"a b\\\\\"", "Windows argument encoding");
  require(ckgit::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "hash");
  const std::string path = "V:\\project with spaces\\Gr\xc3\xbc\xc3\x9f";
  require(ckgit::decodeCheckoutPathToken(ckgit::encodeCheckoutPath(path)) == path, "opaque checkout path");
  require(ckgit::sameReportedCheckout("V:\\project\\repo\\", "repo"), "Windows full path and private basename");
  require(ckgit::sameReportedCheckout("V:\\project\\repo", "V:/project/repo"), "Windows separators in reported paths");
  require(!ckgit::sameReportedCheckout("/project/name\\repo", "repo"), "POSIX backslash remains literal");
  require(ckgit::parseReleasesControlResponse("ok 0\n").empty(), "portable release reply parser");
  require(ckgit::formatUtcTimestamp(0) == "1970-01-01 00:00 UTC", "portable UTC formatting");
  require(ckgit::formatBytes(1024) == "1.0 KiB", "portable size formatting");
  require(ckgit::parseControlRequest("CKGIT-CONTROL/1 test ping\n").has_value(), "portable control grammar");
  require(!ckgit::parseControlRequest("CKGIT-CONTROL/1 test ping\r\n"), "wire CRLF remains invalid");
  require(ckgit::parseCiWorkflow("version: 1\njobs:\n  - name: test\n    steps:\n      - run: [echo, hello]\n").jobs.size() == 1, "workflow parser");
  {
    ckgit::CiPreparedJob prepared;
    prepared.job.job_id = "child-windows";
    prepared.job.parent_run_id = "parent-workflow";
    prepared.job.workflow_job = "windows";
    prepared.job.project_name = "demo";
    prepared.job.ref = "refs/heads/main";
    prepared.job.commit_id = std::string(40, 'a');
    prepared.workflow_text = "version: 2\njobs:\n"
        "  - name: linux\n    runs_on: [linux]\n    steps:\n      - run: [echo, linux]\n"
        "  - name: windows\n    runs_on: [windows]\n    steps:\n      - run: [cl.exe, /help]\n";
    ckgit::CiLease lease;
    lease.token = std::string(32, 'b');
    const auto wire = ckgit::formatCiClaimResponse(prepared, lease, {}, 2);
    const auto remote = ckgit::parseCiClaimResponse(wire);
    require(remote && remote->prepared.workflow.jobs.size() == 1 &&
        remote->prepared.workflow.jobs.front().name == "windows" &&
        remote->prepared.job.parent_run_id == "parent-workflow", "native distributed claim selects only assigned job");
    ckgit::CiRunnerCommand upload;
    upload.protocol_version = 2;
    upload.op = ckgit::CiRunnerOp::kPut;
    upload.job_id = prepared.job.job_id;
    upload.token = lease.token;
    upload.kind = "asset";
    upload.name = "windows-x64";
    upload.total = 3;
    upload.sha256 = ckgit::sha256Hex("abc");
    const auto parsed_upload = ckgit::parseCiRunnerCommand(ckgit::formatCiRunnerCommand(upload));
    require(parsed_upload && parsed_upload->protocol_version == 2 && parsed_upload->total == 3,
        "native runner artifact upload grammar");
    ckgit::CiFinishReport report;
    report.status = ckgit::CiRunStatus::Success;
    report.detail = "Windows build \xc3\xbc";
    const auto finished = ckgit::parseCiFinishReport(ckgit::formatCiFinishReport(report));
    require(finished.status == report.status && finished.detail == report.detail, "native runner finish report");
    bool refused = false;
    try { ckgit::formatCiClaimResponse(prepared, lease, {}, 1); }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused, "distributed claim cannot be downgraded to protocol 1");
  }
  std::vector<std::string> command{args[0], "echo", "", "space value", "quote\"value", "trailing\\", "\xc3\xbc\xf0\x9f\x98\x80", "&|<>%^$()"};
  std::string expected;
  for (std::size_t i = 2; i < command.size(); ++i) expected += std::to_string(command[i].size()) + ':' + command[i] + '\n';
  auto result = ckgit::runProcess(command);
#ifdef _WIN32
  // The helper's CRT stdout translates LF; process capture preserves bytes.
  std::string windows_expected;
  for (const auto c : expected) { if (c == '\n') windows_expected += '\r'; windows_expected += c; }
  expected = windows_expected;
#endif
  require(result.exit_code == 0 && result.output == expected, "literal UTF-8 argument roundtrip");
  {
    const char* temporary = std::getenv("TMPDIR");
    require(temporary && *temporary, "explicit TMPDIR required for process input tests");
    const auto root = ckgit::createTempDirectory(ckgit::pathFromUtf8(temporary), "process-input-");
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    const auto file = root / ckgit::pathFromUtf8("payload \xc3\xbc.bin");
    std::string payload(1024 * 1024, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i % 256);
    { std::ofstream stream(file, std::ios::binary); stream.write(payload.data(), payload.size()); }
    {
      ckgit::RunnerConfig config;
      config.server = "runner@example.test";
      config.name = "native-test";
      config.build_root = root;
      config.ssh_program = ckgit::pathFromUtf8(args[0]);
      config.known_hosts = root / ckgit::pathFromUtf8("known hosts \xc3\xbc");
      ckgit::CiRemoteTransport transport(config, root / "runner's config.ini");
      require(transport.negotiate("31") && transport.protocolVersion() == 2, "native SSH transport negotiates v2");
      const auto shim = transport.gitSshCommand("job-test", std::string(32, 'a'));
      require(shim.find("runner'\\''s config.ini") != std::string::npos, "Git shim quotes paths for the Git shell");
      ckgit::CiRunnerCommand command;
      command.op = ckgit::CiRunnerOp::kClaim;
      const auto claimed = transport.run(command, {}, std::chrono::seconds(3));
      require(claimed.exit_code == 0 && claimed.output == "none\n", "native SSH transport receives claim response");
      command.op = ckgit::CiRunnerOp::kPut;
      command.job_id = "job-test";
      command.token = std::string(32, 'a');
      command.kind = "artifact";
      command.name = "binary";
      const std::string input("abc\0def", 7);
      command.total = input.size();
      command.sha256 = ckgit::sha256Hex(input);
      const auto uploaded = transport.run(command, input, std::chrono::seconds(3));
      require(uploaded.exit_code == 0 && uploaded.output == input, "native SSH transport preserves binary payload");
      command.op = ckgit::CiRunnerOp::kStream;
      auto session = transport.open(command);
      require(session->write("heartbeat\n") == 10, "native SSH transport accepts stream frames");
      std::string output;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (std::chrono::steady_clock::now() < deadline) {
        session->closeInput();
        char buffer[128];
        const auto count = session->read(buffer, sizeof(buffer));
        if (count < 0) break;
        if (count > 0) output.append(buffer, static_cast<std::size_t>(count));
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      require(output == "ready\nheartbeat\n", "native SSH transport carries bidirectional stream frames");
      ckgit::CiRemoteTransport::close(session);
      require(!session, "native SSH transport closes session ownership");
    }
    ckgit::ProcessOptions options;
    options.input_file = file;
    options.output_limit = 3 * 1024 * 1024;
    result = ckgit::runProcess({args[0], "input"}, options);
    require(result.exit_code == 0 && result.output == std::string(1024 * 1024, 'o') + payload,
        "binary stdin and output larger than pipe buffers survive without deadlock");
    options.output_limit = 127;
    result = ckgit::runProcess({args[0], "input"}, options);
    require(result.exit_code == 0 && result.output.size() == 127 && result.output_truncated,
        "file input preserves bounded output draining");
    options.timeout = std::chrono::milliseconds(100);
    result = ckgit::runProcess({args[0], "sleep"}, options);
    require(result.timed_out, "file input preserves process timeout");
    for (const auto& invalid : {std::filesystem::path("relative"), root}) {
      options.input_file = invalid;
      bool refused = false;
      try { ckgit::runProcess({args[0], "input"}, options); }
      catch (const std::exception&) { refused = true; }
      require(refused, "non-regular or relative process input refused");
    }
    {
      ckgit::ProcessStream stream({args[0], "input"});
      std::size_t sent = 0;
      std::string received;
      bool closed = false;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (std::chrono::steady_clock::now() < deadline) {
        if (sent < payload.size()) {
          const auto count = stream.write(std::string_view(payload).substr(sent));
          require(count >= 0, "stream stays open while transmitting binary input");
          sent += static_cast<std::size_t>(count);
        } else stream.closeInput();
        char buffer[16384];
        const auto count = stream.read(buffer, sizeof(buffer));
        if (count > 0) received.append(buffer, static_cast<std::size_t>(count));
        if (count < 0) closed = true;
        if (closed && stream.poll()) break;
        if (!count) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      require(closed && stream.poll() == 0 && sent == payload.size(), "stream completes bidirectional transfer");
      require(received == std::string(1024 * 1024, 'o') + payload, "stream preserves binary data under backpressure");
    }
    {
      ckgit::ProcessStream stream({args[0], "sleep"});
      std::size_t accepted = 0;
      bool blocked = false;
      while (accepted < 4 * 1024 * 1024) {
        const auto count = stream.write(payload);
        require(count >= 0, "live sleeper stream accepts input");
        if (!count) { blocked = true; break; }
        accepted += static_cast<std::size_t>(count);
      }
      require(blocked, "stream input is bounded when child does not read");
      const auto before = std::chrono::steady_clock::now();
      stream.stop();
      require(std::chrono::steady_clock::now() - before < std::chrono::seconds(3), "pending stream write cancels promptly");
      require(stream.poll().has_value() && stream.poll() != 0, "stopped stream has terminal status");
      require(stream.write("late") < 0, "closed stream refuses more input");
    }
  }
  result = ckgit::runProcess({args[0], "large"}, std::chrono::seconds(5), 127);
  require(result.exit_code == 0 && result.output.size() == 127 && result.output_truncated, "bounded pipe drain");
  result = ckgit::runProcess({args[0], "sleep"}, std::chrono::milliseconds(100));
  require(result.timed_out && result.exit_code != 0, "timeout");
  require(ckgit::runProcess({args[0], "exit"}).exit_code == 37, "exit status");
  {
    ckgit::ManagedProcess child({args[0], "exit"});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!child.poll() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(child.poll() == 37, "managed exit status");
    child.stop();
    require(child.poll() == 37, "managed repeated polling preserves exit status");
  }
  {
    ckgit::ManagedProcess child({args[0], "sleep"});
    require(!child.poll(), "managed child starts live");
    child.stop();
    require(child.poll().has_value() && child.poll() != 0, "managed child stop is observed");
  }
#ifdef _WIN32
  result = ckgit::runProcess({args[0], "tree"}, std::chrono::milliseconds(500));
  require(result.timed_out && !result.output.empty(), "descendant test started");
  const auto child_id = static_cast<DWORD>(std::stoul(result.output));
  const auto descendant = OpenProcess(SYNCHRONIZE, FALSE, child_id);
  if (descendant) {
    const auto waited = WaitForSingleObject(descendant, 2000);
    CloseHandle(descendant);
    require(waited == WAIT_OBJECT_0, "timeout terminates descendants");
  } else require(GetLastError() == ERROR_INVALID_PARAMETER, "descendant termination observable");
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  const auto event = CreateEventW(&security, TRUE, FALSE, nullptr);
  require(event != nullptr, "create inherited-handle fixture");
  result = ckgit::runProcess({args[0], "handle", std::to_string(reinterpret_cast<std::uintptr_t>(event))});
  {
    ckgit::ProcessStream stream({args[0], "handle", std::to_string(reinterpret_cast<std::uintptr_t>(event))});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!stream.poll() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(stream.poll() == 0, "stream does not inherit unlisted handles");
    require(stream.write("after-exit") < 0, "stream detects closed child input");
  }
  const auto event_status = WaitForSingleObject(event, 0);
  CloseHandle(event);
  require(result.exit_code == 0 && event_status == WAIT_TIMEOUT, "unlisted handle not inherited");
#endif
  bool rejected = false;
  try { ckgit::runProcess({}); } catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "empty command");
  std::cout << "process and portable protocol tests passed\n";
  return 0;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  try {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
#ifdef _WIN32
      args.push_back(ckgit::wideToUtf8(argv[i]));
#else
      args.emplace_back(argv[i]);
#endif
    }
    return run(args);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
