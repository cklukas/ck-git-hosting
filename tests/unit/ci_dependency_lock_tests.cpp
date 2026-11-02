// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include <stdexcept>
#include <string>

#include "ckgit/ci_dependency_lock.hpp"
#include "ckgit/hash.hpp"

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
std::string lockText() {
  const std::string hash(64, 'a');
  return "version: 1\nplatform: windows\narchitecture: x64\nabi: x64-windows\ninputs:\n"
         "  - path: vcpkg.json\n    sha256: " +
         hash +
         "\npackages:\n"
         "  - name: app\n    version: 1.2.3\n    registry: vcpkg\n    sha256: " +
         hash + "\n    recipe_sha256: " + hash +
         "\n    direct: true\n    needs: [lib]\n"
         "  - name: lib\n    version: 2.0\n    registry: nuget\n    sha256: " +
         hash + "\n    recipe_sha256: " + hash + "\n    direct: false\n    needs: []\n";
}
void replace(std::string& s, const std::string& from, const std::string& to) {
  auto p = s.find(from);
  if (p == std::string::npos) throw std::runtime_error("missing test input");
  s.replace(p, from.size(), to);
}
bool rejects(const std::string& input) {
  try {
    (void)ckgit::parseCiDependencyLock(input);
    return false;
  } catch (const std::exception&) {
    return true;
  }
}
}  // namespace
void testCiDependencyLock() {
  const auto text = lockText();
  const auto lock = ckgit::parseCiDependencyLock(text);
  require(lock.sha256 == ckgit::sha256Hex(text) && lock.packages.size() == 2 && lock.packages[0].direct &&
              lock.packages[0].needs[0] == "lib",
          "dependency lock parse");
  auto changed = text + "# changed\n";
  require(ckgit::parseCiDependencyLock(changed).sha256 != lock.sha256,
          "approval identity covers exact bytes");
  auto bound = text;
  replace(bound, std::string(64, 'a'), ckgit::sha256Hex("manifest contents"));
  const auto bound_lock = ckgit::parseCiDependencyLock(bound);
  ckgit::verifyCiDependencyInputs(bound_lock, [](const std::string& path) {
    if (path != "vcpkg.json") throw std::runtime_error("unexpected path");
    return std::string("manifest contents");
  });
  bool changed_input_rejected = false;
  try {
    ckgit::verifyCiDependencyInputs(bound_lock, [](const std::string&) { return std::string("changed"); });
  } catch (const std::exception&) {
    changed_input_rejected = true;
  }
  require(changed_input_rejected, "changed manifest cannot reuse approval");
  bool missing_input_rejected = false;
  try {
    ckgit::verifyCiDependencyInputs(bound_lock, [](const std::string&) -> std::string {
      throw std::runtime_error("missing input");
    });
  } catch (const std::exception&) {
    missing_input_rejected = true;
  }
  require(missing_input_rejected, "missing manifest cannot reuse approval");
  auto missing = text;
  replace(missing, "needs: [lib]", "needs: [missing]");
  require(rejects(missing), "unresolved package rejected");
  auto cycle = text;
  replace(cycle, "needs: []", "needs: [app]");
  require(rejects(cycle), "dependency cycle rejected");
  auto unreachable = text;
  replace(unreachable, "needs: [lib]", "needs: []");
  require(rejects(unreachable), "unreachable package rejected");
  auto unsafe = text;
  replace(unsafe, "vcpkg.json", "../vcpkg.json");
  require(rejects(unsafe), "unsafe manifest path rejected");
  auto deceptive_path = text;
  replace(deceptive_path, "vcpkg.json", "vcpkg\xe2\x80\xae.json");
  require(rejects(deceptive_path), "terminal-bidi manifest path rejected");
  auto nonhex = text;
  replace(nonhex, std::string(64, 'a'), std::string(64, 'A'));
  require(rejects(nonhex), "noncanonical hash rejected");
  auto url = text;
  replace(url, "registry: vcpkg", "registry: https://evil.test");
  require(rejects(url), "untrusted URL cannot name a registry");
  require(rejects(text + "executable: injected\n"), "unknown keys rejected");
}
