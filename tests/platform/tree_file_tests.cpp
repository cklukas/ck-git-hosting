// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/tree_file.hpp"
#include "ckgit/temp_directory.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/pages_store.hpp"
#include "ckgit/process.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include "ckgit/windows.hpp"
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#endif

namespace fs = std::filesystem;
using ckgit::platform::TreeFileStatus;
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
int run(const fs::path& parent) {
  const auto base = ckgit::createTempDirectory(parent, "tree-test-");
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{base};
  fs::create_directory(base / "site");
  fs::create_directory(base / "site" / "nested");
  std::ofstream(base / "site" / "nested" / "file.txt", std::ios::binary) << "hello";
  const auto read = [&](std::vector<std::string> parts, std::uintmax_t cap = 10) {
    return ckgit::platform::readTreeFile(base / "site", parts, cap, true, false);
  };
  auto file = read({"nested", "file.txt"});
  require(file.status == TreeFileStatus::Ok && file.size == 5 && file.content == "hello", "regular file");
  require(read({"nested", "file.txt"}, 4).status == TreeFileStatus::TooLarge, "size cap");
  require(read({"nested"}).status == TreeFileStatus::NotRegular, "directory refused");
  require(read({"absent"}).status == TreeFileStatus::Missing, "missing file");
  require(read({"..", "file"}).status != TreeFileStatus::Ok, "parent traversal refused");
  require(read({"nested/file.txt"}).status != TreeFileStatus::Ok, "multi-component token refused");
  require(read({std::string("bad\0name", 8)}).status != TreeFileStatus::Ok, "NUL refused");
  require(ckgit::sha256HexOfFile(base / "site" / "nested" / "file.txt") == ckgit::sha256Hex("hello"), "file checksum");
  const std::string unicode = "Gr\xc3\xbc\xc3\x9f-\xf0\x9f\x98\x80.txt";
  const std::u8string unicode_path(unicode.begin(), unicode.end());
  std::ofstream(base / "site" / fs::path(unicode_path), std::ios::binary) << "unicode";
  require(read({unicode}).content == "unicode", "Unicode filename");
  // Archive packing must stream from the validated handle, not reopen the
  // pathname after emitting its header. Replace the path in the callback.
  const std::string payload = std::string(70000, 'a') + std::string(80000, 'b');
  const auto streaming_path = base / "site" / "large.bin";
  std::ofstream(streaming_path, std::ios::binary) << payload;
  std::string received;
  std::size_t chunks = 0;
  bool began = false;
  const auto streamed = ckgit::platform::streamTreeFile(base / "site", {"large.bin"}, payload.size(), false, {
    [&](std::uintmax_t size) {
      began = true;
      require(size == payload.size(), "stream header size");
      fs::rename(streaming_path, base / "site" / "opened.bin");
      std::ofstream(streaming_path, std::ios::binary) << "replacement";
    },
    [&](std::string_view chunk) {
      require(began && !chunk.empty() && chunk.size() <= 65536, "bounded ordered stream chunks");
      ++chunks;
      received.append(chunk);
    }
  });
  require(streamed.status == TreeFileStatus::Ok && streamed.content.empty() && chunks > 1 && received == payload,
          "stream remains bound to opened file after path replacement");
  bool called = false;
  const ckgit::platform::TreeFileSink observe{[&](std::uintmax_t) { called = true; },
                                           [&](std::string_view) { called = true; }};
  require(ckgit::platform::streamTreeFile(base / "site", {"opened.bin"}, payload.size() - 1, false, observe).status ==
              TreeFileStatus::TooLarge && !called, "stream cap checked before callbacks");
  bool cancelled = false;
  try {
    ckgit::platform::streamTreeFile(base / "site", {"opened.bin"}, payload.size(), false,
        {{}, [](std::string_view) { throw std::runtime_error("cancel stream"); }});
  } catch (const std::runtime_error& error) { cancelled = std::string(error.what()) == "cancel stream"; }
  require(cancelled, "stream cancellation propagates");
  std::ofstream(base / "site" / "empty.bin", std::ios::binary);
  std::size_t empty_headers = 0;
  require(ckgit::platform::streamTreeFile(base / "site", {"empty.bin"}, 0, false, {
    [&](std::uintmax_t size) { require(size == 0, "empty stream size"); ++empty_headers; },
    [](std::string_view) { throw std::runtime_error("empty file delivered data"); }
  }).status == TreeFileStatus::Ok && empty_headers == 1, "empty stream emits its header");
  fs::create_directory(base / "outside");
  std::ofstream(base / "outside" / "fixture.txt") << "outside";
#ifdef _WIN32
  // Junction creation needs no symlink privilege. Use the native operation
  // so this fixture does not depend on cmd.exe's non-CRT quoting grammar.
  fs::create_directory(base / "site" / "link");
  const auto target = L"\\??\\" + (base / "outside").wstring();
  struct Junction {
    DWORD tag; WORD bytes, reserved, substitute_offset, substitute_bytes, print_offset, print_bytes;
  };
  Junction header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(8 + (target.size() + 2) * 2), 0,
      0, static_cast<WORD>(target.size() * 2), static_cast<WORD>((target.size() + 1) * 2), 0};
  std::vector<unsigned char> data(sizeof(header) + (target.size() + 2) * 2, 0);
  std::memcpy(data.data(), &header, sizeof(header));
  std::memcpy(data.data() + sizeof(header), target.data(), target.size() * 2);
  const auto directory = CreateFileW((base / "site" / "link").c_str(), GENERIC_WRITE, 0, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  require(directory != INVALID_HANDLE_VALUE, "open junction fixture");
  DWORD returned = 0;
  const auto created = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, data.data(),
      static_cast<DWORD>(data.size()), nullptr, 0, &returned, nullptr);
  CloseHandle(directory);
  require(created != FALSE, "create junction fixture");
  require(read({unicode + ":stream"}).status != TreeFileStatus::Ok, "alternate stream refused");
#else
  fs::create_directory_symlink(base / "outside", base / "site" / "link");
#endif
  require(read({"link", "fixture.txt"}).status != TreeFileStatus::Ok, "linked directory refused");
  require(ckgit::platform::streamTreeFile(base / "site", {"link", "fixture.txt"}, 1024, false, observe).status !=
              TreeFileStatus::Ok && !called, "stream refuses linked directory before callbacks");
  require(!ckgit::readSiteFile(base / "site", "link/fixture.txt", 1024), "preview linked directory refused");
  std::cout << "secure tree file tests passed\n";
  return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  try {
    if (argc != 2) throw std::invalid_argument("explicit temporary parent required");
    return run(fs::path(argv[1]));
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
