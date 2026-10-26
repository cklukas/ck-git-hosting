// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/client_state.hpp"
#include "ckgit/process.hpp"
#include "ckgit/temp_directory.hpp"
#include "ckgit/windows.hpp"
#include <iostream>
#include <stdexcept>

namespace {
std::filesystem::path utf8Path(std::string_view value) {
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
int run(const std::vector<std::string>& args) {
  if (args.size() == 3 && args[1] == "child")
    return ckgit::SyncLock::tryAcquire(utf8Path(args[2])) ? 0 : 7;
  require(args.size() == 2, "explicit scratch parent required");
  const auto root = ckgit::createTempDirectory(utf8Path(args[1]), "sync-lock-");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
  } cleanup{root};
  const auto path = root / utf8Path("lock with spaces-\xc3\xbc");
  const auto bytes = path.u8string();
  const std::string encoded(bytes.begin(), bytes.end());
  const std::vector<std::string> child{args[0], "child", encoded};
  {
    auto lock = ckgit::SyncLock::tryAcquire(path);
    require(lock.has_value(), "initial acquisition");
    require(!ckgit::SyncLock::tryAcquire(path), "same-process contention");
    require(ckgit::runProcess(child).exit_code == 7, "cross-process contention");
    auto moved = std::move(*lock);
    lock.reset();
    require(ckgit::runProcess(child).exit_code == 7, "move preserves lock ownership");
  }
  require(std::filesystem::is_regular_file(path), "persistent lock identity");
  require(ckgit::runProcess(child).exit_code == 0, "released lock can be acquired");
  require(ckgit::SyncLock::tryAcquire(path).has_value(), "child releases lock on exit");
  std::cout << "sync lock contention and lifetime tests passed\n";
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
