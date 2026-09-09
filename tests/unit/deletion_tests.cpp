// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/control_rpc.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/repository_store.hpp"
#include "ckgit/ssh_command.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void requireFailure(Action action, const char* message) {
  bool failed = false;
  try { action(); } catch (const std::exception&) { failed = true; }
  require(failed, message);
}

void privateDirectory(const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  require(chmod(directory.c_str(), 0700) == 0, "could not make test directory private");
}

void privateFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary);
  output << content;
  output.close();
  require(chmod(path.c_str(), 0600) == 0, "could not make test file private");
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string nearlyFullLog(const std::string& project) {
  const std::string line = "1700000000\tgit-push\t" + project + "\tclient\n";
  std::string result;
  while (result.size() + line.size() <= 64 * 1024) result += line;
  return result;
}

void checkBoundedControlRpc(const std::filesystem::path& directory) {
  const auto socket_path = directory / "s";
  sockaddr_un address{};
  require(socket_path.string().size() < sizeof(address.sun_path), "test control socket path is too long");
  const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  require(listener >= 0, "could not create test control socket");
  address.sun_family = AF_UNIX;
  std::strcpy(address.sun_path, socket_path.c_str());
  require(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
              listen(listener, 1) == 0, "could not bind test control socket");
  std::thread server([&] {
    const int client = accept(listener, nullptr, nullptr);
    if (client >= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      close(client);
    }
  });
  const auto start = std::chrono::steady_clock::now();
  bool failed = false;
  try {
    ckgit::forwardControlRpc(socket_path, "client", "refresh", "project", {}, nullptr,
                              std::chrono::milliseconds(70));
  } catch (const std::exception&) { failed = true; }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  server.join();
  close(listener);
  require(failed && elapsed < std::chrono::milliseconds(220), "control read ignored its overall deadline");
}

}  // namespace

void testDeletion() {
  const char* approved_root = std::getenv("CKGIT_TEST_ROOT");
  const auto root = std::filesystem::path(approved_root == nullptr ? "/Volumes/PRO-BLADE/tmp" : approved_root);
  require(std::filesystem::is_directory(root), "approved test root is not mounted");
  const auto directory = root / ("ckgit-del-" + std::to_string(getpid()) + "-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  privateDirectory(directory);
  try {
    const auto repos = directory / "repos";
    const auto state = directory / "state";
    privateDirectory(repos);
    privateDirectory(state);
    privateDirectory(state / "events");
    privateFile(state / "events" / "events.log", nearlyFullLog("old"));
    ckgit::appendStateEvent(state, "checkout-registered", "recent", "client");
    require(ckgit::loadProjectEvents(state, "old").size() == 16, "rotation lost the previous log");
    privateFile(state / "events" / "events.log", nearlyFullLog("middle"));
    ckgit::appendStateEvent(state, "checkout-registered", "recent", "client");
    std::size_t logs = 0;
    for (const auto& entry : std::filesystem::directory_iterator(state / "events")) {
      if (entry.path().extension() == ".log") ++logs;
    }
    require(logs == 3, "same-month event rotation overwrote an earlier archive");
    require(ckgit::loadProjectEvents(state, "old").empty(), "reader scanned beyond newest two logs");
    require(ckgit::loadProjectEvents(state, "middle").size() == 16, "reader omitted newest archived log");
    privateFile(state / "events" / "events.log", nearlyFullLog("recent"));
    std::mutex failures_mutex;
    std::exception_ptr failure;
    std::vector<std::thread> writers;
    for (int worker = 0; worker < 4; ++worker) {
      writers.emplace_back([&, worker] {
        try {
          for (int index = 0; index < 60; ++index) {
            ckgit::appendStateEvent(state, "git-push", "parallel", "client-" + std::to_string(worker));
          }
        } catch (...) {
          std::lock_guard lock(failures_mutex);
          failure = std::current_exception();
        }
      });
    }
    for (auto& writer : writers) writer.join();
    if (failure) std::rethrow_exception(failure);
    const auto parallel = ckgit::loadProjectEvents(state, "parallel", 128);
    require(parallel.size() == 128, "concurrent event appends were lost or malformed");
    std::size_t parallel_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(state / "events")) {
      const auto content = readFile(entry.path());
      std::size_t position = 0;
      while ((position = content.find("\tparallel\t", position)) != std::string::npos) {
        ++parallel_count;
        ++position;
      }
      require(content.size() <= 64 * 1024, "concurrent rotation exceeded the log cap");
    }
    require(parallel_count == 240, "concurrent rotation lost or duplicated events");

    ckgit::createBareRepository(repos, "project", "main");
    ckgit::registerCheckout(state, "project", "client", ckgit::encodeCheckoutPath("/work/project"));
    privateDirectory(state / "projects");
    privateDirectory(state / "projects" / "project");
    privateFile(state / "projects" / "project" / "future-cache", "derived");
    ckgit::appendStateEvent(state, "git-push", "project", "client");
    const auto proposed = ckgit::removeProject(repos, state, "project", true);
    require(!std::filesystem::exists(repos / ".trash") && std::filesystem::exists(repos / "project.git") &&
                !std::filesystem::exists(proposed), "removal dry run changed repository storage");
    require(!ckgit::loadCheckoutMetadata(state, "project").empty(), "removal dry run erased checkouts");
    const auto trashed = ckgit::removeProject(repos, state, "project");
    require(std::filesystem::exists(trashed / "HEAD") && !std::filesystem::exists(repos / "project.git"),
            "removed bare repository was not retained in trash");
    require(!std::filesystem::exists(state / "checkouts" / "project") &&
                !std::filesystem::exists(state / "projects" / "project") &&
                ckgit::loadProjectEvents(state, "project").empty(), "project metadata outlived removal");
    require(ckgit::loadProjectEvents(state, "parallel", 128).size() == 128,
            "project removal scrubbed another project's events");
    ckgit::createBareRepository(repos, "project", "main");
    const auto second_trash = ckgit::removeProject(repos, state, "project");
    require(second_trash != trashed && std::filesystem::exists(trashed / "HEAD"), "repeated removal replaced trash");

    const auto outside = directory / "outside";
    privateDirectory(outside);
    privateFile(outside / "keep", "untouched");
    ckgit::registerCheckout(state, "orphan", "client", ckgit::encodeCheckoutPath("/work/orphan"));
    privateDirectory(state / "projects" / "orphan");
    std::filesystem::create_directory_symlink(outside, state / "projects" / "orphan" / "link");
    std::filesystem::create_directory_symlink(outside, state / "checkouts" / "linked");
    const auto swept = ckgit::sweepOrphanProjectMetadata(repos, state);
    require(std::find(swept.begin(), swept.end(), "orphan") != swept.end() &&
                std::find(swept.begin(), swept.end(), "old") != swept.end(), "sweep missed directory or archived-only orphan");
    require(readFile(outside / "keep") == "untouched", "metadata cleanup followed a symlink");
    require(ckgit::listProjectMetadataNames(state).empty(), "orphan metadata survived cleanup");
    for (const auto& entry : std::filesystem::directory_iterator(state / "events")) {
      require(readFile(entry.path()).empty(), "archived events survived orphan cleanup");
    }
    ckgit::createBareRepository(repos, "protected", "main");
    ckgit::registerHostedCheckout(repos, state, "protected", "client", ckgit::encodeCheckoutPath("/work/protected"));
    ckgit::sweepOrphanProjectMetadata(repos, state);
    require(ckgit::loadCheckoutMetadata(state, "protected").size() == 1 &&
                ckgit::loadProjectEvents(state, "protected").size() == 1,
            "orphan sweep removed a hosted project's registration");
    ckgit::removeProject(repos, state, "protected");
    require(!ckgit::appendHostedStateEvent(repos, state, "git-push", "protected", "client") &&
                ckgit::loadProjectEvents(state, "protected").empty(),
            "late push event recreated removed project metadata");
    for (int iteration = 0; iteration < 8; ++iteration) {
      ckgit::createBareRepository(repos, "race", "main");
      std::exception_ptr removal_failure;
      std::exception_ptr event_failure;
      std::thread registration([&] {
        try {
          ckgit::registerHostedCheckout(repos, state, "race", "client", ckgit::encodeCheckoutPath("/work/race"));
        } catch (const std::exception&) {
          // Removal may win the lifecycle lock; registration must then fail.
        }
      });
      std::thread removal([&] {
        try { ckgit::removeProject(repos, state, "race"); }
        catch (...) { removal_failure = std::current_exception(); }
      });
      std::thread event([&] {
        try { ckgit::appendHostedStateEvent(repos, state, "git-push", "race", "client"); }
        catch (...) { event_failure = std::current_exception(); }
      });
      registration.join();
      removal.join();
      event.join();
      if (removal_failure) std::rethrow_exception(removal_failure);
      if (event_failure) std::rethrow_exception(event_failure);
      require(ckgit::loadCheckoutMetadata(state, "race").empty() &&
                  ckgit::loadProjectEvents(state, "race").empty(),
              "simultaneous registration recreated metadata after project removal");
    }
    requireFailure([&] {
      ckgit::registerHostedCheckout(repos, state, "race", "client", ckgit::encodeCheckoutPath("/work/race"));
    }, "registration accepted an absent hosted repository");
    std::filesystem::create_directory_symlink(outside, repos / "linked.git");
    requireFailure([&] { ckgit::removeProject(repos, state, "linked"); }, "removal accepted a repository symlink");
    requireFailure([&] { ckgit::removeProject(repos, state, "../outside"); }, "removal accepted project traversal");
    ckgit::createBareRepository(repos, "unsafe", "main");
    const auto unsafe_state = directory / "unsafe-state";
    privateDirectory(unsafe_state);
    std::filesystem::create_directory_symlink(outside, unsafe_state / "checkouts");
    requireFailure([&] { ckgit::removeProject(repos, unsafe_state, "unsafe"); }, "removal accepted symlink metadata parent");
    require(std::filesystem::exists(repos / "unsafe.git"), "metadata validation failure already moved repository");
    const auto refresh = ckgit::parseSshOriginalCommand("ckgit-rpc 1 refresh project");
    require(refresh.has_value() && refresh->rpc_operation == "refresh", "refresh is missing from SSH grammar");
    require(!ckgit::parseSshOriginalCommand("ckgit-rpc 1 refresh ../project").has_value(), "refresh accepted traversal");
    checkBoundedControlRpc(directory);
    std::filesystem::remove_all(directory);
  } catch (...) {
    std::filesystem::remove_all(directory);
    throw;
  }
}
