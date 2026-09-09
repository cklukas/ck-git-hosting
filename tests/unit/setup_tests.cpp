// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/client_config.hpp"
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
  if (!condition) throw std::runtime_error(std::string("setup configuration: ") + message);
}
template <typename Action> void rejects(Action action, const char* message) {
  bool failed = false;
  try { action(); } catch (const std::exception&) { failed = true; }
  require(failed, message);
}
std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct Fixture {
  std::filesystem::path root;
  Fixture() {
    const char* temporary = std::getenv("TMPDIR"), *configured = std::getenv("CKGIT_TEST_ROOT");
    require(temporary && *temporary, "TMPDIR must be explicitly selected");
    const auto allowed = std::filesystem::canonical(configured && *configured ? configured : "/Volumes/PRO-BLADE/tmp");
    const auto parent = std::filesystem::canonical(temporary);
    const auto relative = parent.lexically_relative(allowed);
    require(!relative.empty() && *relative.begin() != "..", "TMPDIR must be under the approved root");
    const auto pattern = (parent / "setup-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end()); writable.push_back('\0');
    const auto created = mkdtemp(writable.data());
    require(created, "fixture creation failed"); root = created;
  }
  ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};
}

void testSetupConfig() {
  Fixture fixture;
  ckgit::ClientConfig config;
  config.client_id = "workstation"; config.display_name = "Work Station";
  config.server = "ckgit@server"; config.remote_name = "ckgit";
  config.web_host = "admin@web-server"; config.web_port = 8421;
  config.scan_roots = {fixture.root / "Projects with spaces", fixture.root / "other"};
  config.exclusions = {"*/node_modules/*", "*/Archive/*"};
  const auto path = fixture.root / "new" / "private" / "client.ini";
  const auto serialized = ckgit::renderClientConfig(config);
  require(!std::filesystem::exists(path.parent_path()), "serialization must not create directories");
  require(serialized.find("public_path_mode=basename\n") != std::string::npos &&
          serialized.find("web_host=admin@web-server\nweb_port=8421\n") != std::string::npos,
          "new configuration preserves privacy and separates dashboard access");
  ckgit::saveClientConfig(path, config);
  auto loaded = ckgit::loadClientConfig(path);
  require(loaded.server == config.server && loaded.web_host == config.web_host && loaded.web_port == config.web_port &&
          loaded.scan_roots == config.scan_roots && loaded.exclusions == config.exclusions && !loaded.expose_full_paths,
          "saved configuration must round-trip every setup field");
  struct stat status{};
  require(stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600, "saved configuration must be private");
  require(stat(path.parent_path().c_str(), &status) == 0 && (status.st_mode & 0777) == 0700,
          "new configuration directories must be private");
  config.display_name = "Changed";
  rejects([&] { ckgit::saveClientConfig(path, config); }, "existing files must require explicit overwrite");
  require(read(path) == serialized, "refused overwrite must preserve the existing bytes");
  ckgit::saveClientConfig(path, config, true);
  require(ckgit::loadClientConfig(path).display_name == "Changed", "explicit overwrite installs the complete new configuration");
  for (const auto& entry : std::filesystem::directory_iterator(path.parent_path()))
    require(entry.path() == path, "atomic save must clean up its staging files");
  const auto current = read(path);
  config.display_name = "injected\nserver=bad@host";
  rejects([&] { ckgit::saveClientConfig(path, config, true); }, "newline injection must be rejected before writing");
  require(read(path) == current, "invalid overwrite must preserve the existing configuration");
  config.display_name = "Changed";
  for (const auto* server : {"-option@server", "ckgit@-option", "bad host", "ckgit@server:22"}) {
    config.server = server;
    rejects([&] { static_cast<void>(ckgit::renderClientConfig(config)); }, "unsafe SSH targets must be rejected");
  }
  config.server = "ckgit@server";
  for (const auto* web_host : {"-option", "bad host", "a@@host", "host;command"}) {
    config.web_host = web_host;
    rejects([&] { static_cast<void>(ckgit::renderClientConfig(config)); }, "unsafe dashboard targets must be rejected");
  }
  config.web_host = "web-alias";
  config.scan_roots = {"relative"};
  rejects([&] { static_cast<void>(ckgit::renderClientConfig(config)); }, "relative scan roots cannot be serialized");
  config.scan_roots = {fixture.root};
  config.web_port = 0;
  rejects([&] { static_cast<void>(ckgit::renderClientConfig(config)); }, "a disabled port cannot be used for dashboard access");
  config.web_port = 8420;
  const auto symlink = fixture.root / "alias.ini";
  std::filesystem::create_symlink(path, symlink);
  rejects([&] { ckgit::saveClientConfig(symlink, config, true); }, "save must reject a symlink target");
  const auto alias_directory = fixture.root / "directory-link";
  std::filesystem::create_directory_symlink(path.parent_path(), alias_directory);
  rejects([&] { ckgit::saveClientConfig(alias_directory / "other.ini", config); }, "save must reject symlink parents");
  const auto legacy = fixture.root / "legacy.ini";
  std::ofstream(legacy) << "schema_version=1\nclient_id=device\ndisplay_name=Device\nserver=ckgit@server\nremote_name=ckgit\n";
  loaded = ckgit::loadClientConfig(legacy);
  require(loaded.web_host.empty() && loaded.web_port == 8420, "legacy configurations retain existing dashboard defaults");
  std::ofstream(legacy, std::ios::app) << "web_port=65536\n";
  rejects([&] { static_cast<void>(ckgit::loadClientConfig(legacy)); }, "invalid dashboard ports are rejected on load too");
}
