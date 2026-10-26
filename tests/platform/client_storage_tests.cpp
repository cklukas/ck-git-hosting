// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/client_config.hpp"
#include "ckgit/client_state.hpp"
#include "ckgit/directory_identity.hpp"
#include "ckgit/path_utf8.hpp"
#include "ckgit/temp_directory.hpp"
#include "ckgit/windows.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F&& action, const char* message) {
  bool rejected = false;
  try { action(); } catch (const std::exception&) { rejected = true; }
  require(rejected, message);
}
void verifyPrivate(const std::filesystem::path& path) {
#ifdef _WIN32
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PSID owner = nullptr;
  PACL acl = nullptr;
  require(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner, nullptr, &acl, nullptr, &descriptor) == ERROR_SUCCESS, "read private ACL");
  struct Cleanup { void* value; ~Cleanup() { LocalFree(value); } } cleanup{descriptor};
  SECURITY_DESCRIPTOR_CONTROL control{};
  DWORD revision = 0;
  require(GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED), "protected DACL");
  require(acl && acl->AceCount == 2, "only owner and SYSTEM grants");
  char system[SECURITY_MAX_SID_SIZE];
  DWORD size = sizeof(system);
  require(CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size), "SYSTEM SID");
  for (DWORD i = 0; i < acl->AceCount; ++i) {
    void* raw = nullptr;
    require(GetAce(acl, i, &raw), "read ACL grant");
    auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    require(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
        (EqualSid(&ace->SidStart, owner) || EqualSid(&ace->SidStart, system)), "private ACL principals");
  }
#else
  struct stat status{};
  require(stat(path.c_str(), &status) == 0 && (status.st_mode & 077) == 0, "private POSIX permissions");
#endif
}
int run(const std::vector<std::string>& args) {
  require(args.size() == 2, "explicit scratch parent required");
  const auto root = ckgit::createTempDirectory(ckgit::pathFromUtf8(args[1]), "client-storage-");
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{root};
  const auto directory = root / ckgit::pathFromUtf8("config with spaces-\xc3\xbc");
  const auto identity_path = root / "identity";
  std::filesystem::create_directory(identity_path);
  {
    ckgit::DirectoryIdentity identity(identity_path);
    require(identity.matches(identity_path), "held directory identity");
    std::filesystem::rename(identity_path, root / "moved");
    std::filesystem::create_directory(identity_path);
    require(!identity.matches(identity_path), "replacement directory is detected");
    require(identity.matches(root / "moved"), "identity follows original directory");
  }
  const auto file = directory / "client.ini";
  ckgit::ClientConfig config;
  config.client_id = "windows-test";
  config.display_name = "Unicode \xc3\xbc";
  config.server = "ckgit@example.test";
  config.remote_name = "ckgit";
  config.scan_roots = {root / ckgit::pathFromUtf8("checkout \xf0\x9f\x98\x80")};
  ckgit::saveClientConfig(file, config);
  verifyPrivate(directory);
  verifyPrivate(file);
  auto loaded = ckgit::loadClientConfig(file);
  require(loaded.scan_roots == config.scan_roots && loaded.display_name == config.display_name, "UTF-8 configuration roundtrip");
  config.display_name = "changed";
  rejects([&] { ckgit::saveClientConfig(file, config); }, "existing config requires overwrite");
  require(ckgit::loadClientConfig(file).display_name == loaded.display_name, "refused overwrite preserves bytes");
  ckgit::saveClientConfig(file, config, true);
  require(ckgit::loadClientConfig(file).display_name == "changed", "explicit replacement");
  verifyPrivate(file);
  const auto selections = directory / "canonical.ini";
  ckgit::saveCanonicalCheckout(selections, "first", config.scan_roots[0]);
  ckgit::saveCanonicalCheckout(selections, "second", root / "other");
  require(ckgit::loadCanonicalCheckouts(selections).at("first") == config.scan_roots[0], "canonical Unicode path preserved");
  require(ckgit::forgetCanonicalCheckout(selections, "second"), "forget selection");
  require(!ckgit::forgetCanonicalCheckout(selections, "second"), "forget absent selection");
  require(ckgit::loadCanonicalCheckouts(selections).size() == 1, "other selection preserved");
  rejects([&] { ckgit::saveCanonicalCheckout(selections, "first", "relative"); }, "relative checkout rejected");
  verifyPrivate(selections);
  const auto race = directory / "race.ini";
  std::atomic<unsigned> saved{0};
  const auto create = [&] { try { ckgit::saveClientConfig(race, config); ++saved; } catch (const std::exception&) {} };
  std::thread first(create), second(create);
  first.join(); second.join();
  require(saved == 1 && ckgit::loadClientConfig(race).client_id == config.client_id, "one concurrent creator succeeds");
  for (auto name : {L"NUL.txt", L"aux", L"COM1", L"LPT\u00b2.log", L"stream:data", L"trailing.", L"trailing ", L".."})
    require(!ckgit::isWindowsFileComponent(name), "Windows reserved component rejected");
  require(ckgit::isWindowsFileComponent(L"Gr\u00fc\u00dfe.txt"), "Unicode filename accepted");
#ifdef _WIN32
  rejects([&] { ckgit::saveClientConfig(directory / L"NUL.ini", config); }, "device filename rejected");
  rejects([&] { ckgit::saveClientConfig(directory / L"client.ini:stream", config); }, "alternate stream rejected");
  config.scan_roots = {L"V:relative"};
  rejects([&] { ckgit::renderClientConfig(config); }, "drive-relative root rejected");
#endif
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    require(!entry.path().filename().string().starts_with(".ckgit-new-"), "staging files cleaned");
  std::cout << "client configuration, selection and private storage tests passed\n";
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
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
