// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
#include "ckgit/ci_provisioned_inventory.hpp"
#include "ckgit/hash.hpp"

namespace {
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
template <class F> bool rejects(F f) { try { f(); return false; } catch (const std::exception&) { return true; } }
}
void testCiProvisionedInventory() {
  const char* tmp = std::getenv("TMPDIR"); require(tmp && *tmp, "inventory test needs TMPDIR");
  auto pattern = (std::filesystem::path(tmp) / "inventory-test-XXXXXX").string();
  std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
  require(mkdtemp(name.data()) != nullptr, "inventory test directory");
  const std::filesystem::path root(name.data());
  std::ofstream(root / "tool") << "tool bytes";
  const std::string lock(64, 'a');
  const auto manifest = "ckgit-inventory-v1\nlock=" + lock + "\nplatform=macos\narchitecture=arm64\nabi=arm64-macos\nF\t" +
                        ckgit::sha256Hex("tool bytes") + "\t0644\ttool\n";
  const auto verify = [&](const std::string& text, const std::string& identity, const std::string& platform) {
    return ckgit::verifyCiProvisionedInventory(root, text, identity, lock, platform, "arm64", "arm64-macos");
  };
  require(rejects([&] { (void)verify(manifest, std::string(64, 'b'), "macos"); }), "wrong manifest identity refused");
  require(rejects([&] { (void)verify(manifest, ckgit::sha256Hex(manifest), "windows"); }), "wrong target refused");
  const auto unsafe = manifest + "F\t" + std::string(64, 'a') + "\t0644\t../escape\n";
  require(rejects([&] { (void)verify(unsafe, ckgit::sha256Hex(unsafe), "macos"); }), "unsafe path refused");
  const auto symlink = manifest + "L\t-\t0777\tlink\n";
  require(rejects([&] { (void)verify(symlink, ckgit::sha256Hex(symlink), "macos"); }), "link entry refused");
  require(rejects([&] { (void)verify(manifest, ckgit::sha256Hex(manifest), "macos"); }),
          "user-owned inventory cannot be trusted");
  std::filesystem::remove(root / "tool");
  std::filesystem::remove(root);
}
