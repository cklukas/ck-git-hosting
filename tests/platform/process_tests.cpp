// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/process.hpp"
#include "ckgit/windows.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/control_rpc.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/ci_workflow.hpp"
#include "ckgit/web_renderer.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
int run(const std::vector<std::string>& args) {
  if (args.size() > 1) {
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
