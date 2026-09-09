// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/client_state.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/ssh_command.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(std::string("client management: ") + message);
  }
}

struct Fixture {
  std::filesystem::path root;
  Fixture() {
    const char* temporary = std::getenv("TMPDIR");
    const char* configured_root = std::getenv("CKGIT_TEST_ROOT");
    require(temporary != nullptr && *temporary != '\0', "TMPDIR must be explicitly selected");
    const auto allowed = std::filesystem::canonical(configured_root != nullptr && *configured_root
        ? configured_root : "/Volumes/PRO-BLADE/tmp");
    const auto directory = std::filesystem::canonical(temporary);
    const auto relative = directory.lexically_relative(allowed);
    require(!relative.empty() && *relative.begin() != "..", "TMPDIR must be under the approved root");
    const auto pattern = (directory / "ckgit-management-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const auto created = mkdtemp(writable.data());
    require(created != nullptr, "could not create fixture");
    root = created;
  }
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

}  // namespace

void testClientManagement() {
  Fixture fixture;
  const auto file = fixture.root / "canonical.ini";
  const auto first = fixture.root / "one" / "same-name";
  const auto replacement = fixture.root / "two" / "same-name";
  require(ckgit::loadCanonicalCheckouts(file).empty(), "a fresh installation starts with no private management");
  ckgit::saveCanonicalCheckout(file, "hosted", first);
  ckgit::saveCanonicalCheckout(file, "other", fixture.root / "other");
  auto inventory = ckgit::loadCanonicalCheckouts(file);
  require(inventory.at("hosted") == first, "absolute path is retained independently of a reported basename");
  struct stat status {};
  require(stat(file.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600,
          "managed absolute paths are written with private permissions");
  ckgit::saveCanonicalCheckout(file, "hosted", replacement);
  inventory = ckgit::loadCanonicalCheckouts(file);
  require(inventory.at("hosted") == replacement && inventory.size() == 2,
          "replacement selects the exact path without losing unrelated projects");
  require(ckgit::forgetCanonicalCheckout(file, "hosted"), "forget removes a selected missing checkout");
  inventory = ckgit::loadCanonicalCheckouts(file);
  require(inventory.size() == 1 && inventory.contains("other"), "forget preserves every other selection");
  require(!ckgit::forgetCanonicalCheckout(file, "hosted"), "repeated forgetting is idempotent");
  const auto state = fixture.root / "state";
  std::filesystem::create_directory(state);
  chmod(state.c_str(), 0700);
  ckgit::registerCheckout(state, "hosted", "first", ckgit::encodeCheckoutPath("/first/hosted"));
  ckgit::registerCheckout(state, "hosted", "second", ckgit::encodeCheckoutPath("/second/hosted"));
  require(ckgit::forgetCheckout(state, "hosted", "first"), "forget removes the authenticated device record");
  require(ckgit::loadClientCheckouts(state, "first").empty() &&
              ckgit::loadCheckoutMetadata(state, "hosted").size() == 1 &&
              ckgit::loadClientCheckouts(state, "second").size() == 1,
          "forget never removes another device's registration");
  require(!ckgit::forgetCheckout(state, "hosted", "first"), "server forgetting is idempotent");
  std::filesystem::create_symlink(state / "checkouts/hosted/second.ini", state / "checkouts/hosted/first.ini");
  bool unsafe_record = false;
  try { ckgit::forgetCheckout(state, "hosted", "first"); } catch (const std::exception&) { unsafe_record = true; }
  require(unsafe_record && std::filesystem::is_symlink(state / "checkouts/hosted/first.ini") &&
              std::filesystem::is_regular_file(state / "checkouts/hosted/second.ini"),
          "forget refuses a symlink record and retains its target");
  std::filesystem::remove(state / "checkouts/hosted/first.ini");
  require(ckgit::loadClientCheckouts(state, "second").at(0).reported_path == "/second/hosted",
          "the other device's retained record still parses with its original path");
  require(ckgit::parseSshOriginalCommand("ckgit-rpc 1 forget-checkout hosted").has_value(), "SSH accepts fixed forget operation");
  require(!ckgit::parseSshOriginalCommand("ckgit-rpc 1 forget-checkout hosted second").has_value(),
          "forget cannot select another device's identity");
  require(!ckgit::parseSshOriginalCommand("ckgit-rpc 1 forget-checkout ../hosted").has_value(),
          "forget rejects unsafe project names");
  const auto dash_upload = ckgit::parseSshOriginalCommand("git-upload-pack './--config.git'");
  require(dash_upload && dash_upload->project_name == "--config", "SSH recognizes safe option-looking transport path");
  for (const auto* request : {"git-upload-pack './../outside.git'", "git-upload-pack './normal.git'",
                             "git-upload-pack './--nested/outside.git'", "git-upload-pack '././--config.git'"}) {
    require(!ckgit::parseSshOriginalCommand(request), "transport prefix does not permit arbitrary relative paths");
  }

  // A failed extension at the declared capacity must leave a readable prior
  // inventory. Previously a 1025th short record could be saved but never loaded.
  std::ofstream populated(file, std::ios::binary | std::ios::trunc);
  populated << "schema_version=1\n";
  for (unsigned int index = 0; index < 1024; ++index) {
    populated << "p" << index << "=/r/p" << index << '\n';
  }
  populated.close();
  require(ckgit::loadCanonicalCheckouts(file).size() == 1024, "the declared inventory capacity is readable");
  bool rejected = false;
  try {
    ckgit::saveCanonicalCheckout(file, "overflow", "/r/overflow");
  } catch (const std::exception&) {
    rejected = true;
  }
  require(rejected, "adding beyond capacity fails before replacing the inventory");
  inventory = ckgit::loadCanonicalCheckouts(file);
  require(inventory.size() == 1024 && !inventory.contains("overflow"),
          "capacity rejection preserves every earlier managed path");
}
