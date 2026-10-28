// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/archive.hpp"
#include "ckgit/temp_directory.hpp"
#include "test_directory_link.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int run(const fs::path& parent) {
  const auto base = ckgit::createTempDirectory(parent, "archive-test-");
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); } } cleanup{base};
  const auto source = base / "source", bundle = base / "files.tar";
  fs::create_directories(source / "out");
  const std::string name = "Gr\xc3\xbc\xc3\x9f" "e.txt";
  const std::string payload(150000, 'x');
  std::ofstream(source / "out" / fs::path(std::u8string(name.begin(), name.end())), std::ios::binary) << payload;
  std::ofstream(source / "out" / "empty.txt", std::ios::binary);
  ckgit::TarPackOptions options;
  options.max_bytes = 1024 * 1024;
  const std::string original_locale = setlocale(LC_CTYPE, nullptr);
  ckgit::packRegularTreeTar(source, {"out"}, bundle, options);
  require(original_locale == setlocale(LC_CTYPE, nullptr), "packer changed caller locale");
  require(fs::file_size(bundle) <= options.max_bytes, "bundle exceeds cap");
#ifdef _WIN32
  require(setlocale(LC_CTYPE, ".UTF8") != nullptr, "reader UTF-8 locale");
#else
  require(setlocale(LC_CTYPE, "C.UTF-8") || setlocale(LC_CTYPE, "en_US.UTF-8"), "reader UTF-8 locale");
#endif
  const std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
  require(reader != nullptr, "reader allocation");
  require(archive_read_support_filter_none(reader.get()) == ARCHIVE_OK, "reader filter");
  require(archive_read_support_format_tar(reader.get()) == ARCHIVE_OK, "reader format");
#ifdef _WIN32
  require(archive_read_open_filename_w(reader.get(), bundle.c_str(), 65536) == ARCHIVE_OK, "open TAR");
#else
  require(archive_read_open_filename(reader.get(), bundle.c_str(), 65536) == ARCHIVE_OK, "open TAR");
#endif
  archive_entry* entry = nullptr;
  int status = 0;
  bool found_unicode = false, found_empty = false;
  while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
    if (archive_entry_filetype(entry) != AE_IFREG) continue;
    const char* raw = archive_entry_pathname_utf8(entry);
    require(raw != nullptr, "UTF-8 entry name");
    const std::string received_name(raw);
    if (received_name == "out/empty.txt") {
      found_empty = true;
      require(archive_entry_size(entry) == 0, "empty file size");
      continue;
    }
#ifdef __APPLE__
    const std::string expected = "out/Gru\xcc\x88\xc3\x9f" "e.txt";
#else
    const std::string expected = "out/" + name;
#endif
    require(received_name == expected, "unexpected archive member");
    require(archive_entry_size(entry) == static_cast<la_int64_t>(payload.size()), "payload size");
    std::string data(payload.size(), '\0');
    require(archive_read_data(reader.get(), data.data(), data.size()) == static_cast<la_ssize_t>(data.size()), "payload read");
    require(data == payload, "payload bytes");
    found_unicode = true;
  }
  require(status == ARCHIVE_EOF && found_unicode && found_empty, "complete TAR roundtrip");
  require(archive_read_close(reader.get()) == ARCHIVE_OK, "close reader");
  const auto must_fail = [&](const std::vector<std::string>& paths, const ckgit::TarPackOptions& limits) {
    fs::remove(bundle);
    bool failed = false;
    try { ckgit::packRegularTreeTar(source, paths, bundle, limits); }
    catch (const std::exception&) { failed = true; }
    require(failed && !fs::exists(bundle), "rejected archive left partial output");
  };
  auto small = options;
  small.max_bytes = 512;
  must_fail({"out"}, small);
  small = options; small.max_entries = 1;
  must_fail({"out"}, small);
  small = options;
  unsigned checks = 0;
  small.should_continue = [&] { return ++checks < 6; };
  must_fail({"out"}, small);
  require(checks >= 6, "cancellation hook not exercised");
  must_fail({"../source/out"}, options);
  must_fail({""}, options);
  must_fail({"missing"}, options);
  createTestDirectoryLink(source / "out", source / "link");
  must_fail({"link"}, options);
  must_fail({"link/empty.txt"}, options);
  auto pages = options;
  pages.skip_non_regular = true;
  ckgit::packRegularTreeTar(source, {"."}, bundle, pages);
  const std::unique_ptr<struct archive, decltype(&archive_read_free)> filtered(archive_read_new(), archive_read_free);
  require(filtered != nullptr, "filtered reader allocation");
  require(archive_read_support_filter_none(filtered.get()) == ARCHIVE_OK, "filtered reader filter");
  require(archive_read_support_format_tar(filtered.get()) == ARCHIVE_OK, "filtered reader format");
#ifdef _WIN32
  require(archive_read_open_filename_w(filtered.get(), bundle.c_str(), 65536) == ARCHIVE_OK, "open filtered TAR");
#else
  require(archive_read_open_filename(filtered.get(), bundle.c_str(), 65536) == ARCHIVE_OK, "open filtered TAR");
#endif
  unsigned entries = 0;
  while ((status = archive_read_next_header(filtered.get(), &entry)) == ARCHIVE_OK) {
    const char* path = archive_entry_pathname_utf8(entry);
    require(path && std::string(path).find("link") == std::string::npos, "Pages included a linked path");
    require(archive_entry_filetype(entry) == AE_IFREG || archive_entry_filetype(entry) == AE_IFDIR, "Pages included a special entry");
    ++entries;
  }
  require(status == ARCHIVE_EOF && entries == 4, "Pages must retain exactly root, out, and two regular files");
  require(archive_read_close(filtered.get()) == ARCHIVE_OK, "close filtered reader");

  ckgit::TarExtractOptions extraction;
  extraction.max_archive_bytes = options.max_bytes;
  extraction.max_bytes = options.max_bytes;
  extraction.max_file_bytes = options.max_bytes;
  const auto extracted = base / "extracted";
  ckgit::extractRegularTreeTar(bundle, extracted, extraction);
  std::ifstream restored(extracted / "out" / fs::path(std::u8string(name.begin(), name.end())), std::ios::binary);
  require(std::string(std::istreambuf_iterator<char>(restored), {}) == payload, "extracted Unicode file bytes");
  restored.close();
  require(fs::file_size(extracted / "out/empty.txt") == 0, "extracted empty file");
  bool refused = false;
  try { ckgit::extractRegularTreeTar(bundle, extracted, extraction); }
  catch (const std::exception&) { refused = true; }
  require(refused && fs::exists(extracted / "out/empty.txt"), "existing destination must remain untouched");
  fs::remove_all(extracted);
  unsigned rejection_case = 0;
  const auto reject_extract = [&](const ckgit::TarExtractOptions& limits) {
    ++rejection_case;
    bool rejected = false;
    std::string reason;
    try { ckgit::extractRegularTreeTar(bundle, extracted, limits); }
    catch (const std::exception& error) { rejected = true; reason = error.what(); }
    if (!rejected) throw std::runtime_error("extraction rejection case " + std::to_string(rejection_case) + " was accepted");
    if (fs::exists(extracted)) throw std::runtime_error("extraction rejection case " + std::to_string(rejection_case) +
        " left partial tree after: " + reason);
  };
  auto bounded = extraction;
  bounded.max_archive_bytes = 512;
  reject_extract(bounded);
  bounded = extraction; bounded.max_bytes = 100;
  reject_extract(bounded);
  bounded = extraction; bounded.max_file_bytes = 100;
  reject_extract(bounded);
  bounded = extraction; bounded.max_entries = 1;
  reject_extract(bounded);
  bounded = extraction;
  unsigned extraction_checks = 0;
  bounded.should_continue = [&] { return ++extraction_checks < 8; };
  reject_extract(bounded);
  require(extraction_checks >= 8, "extraction cancellation not exercised");

  // Small, local parser fixtures: invalid paths must never produce output
  // outside the fresh extraction directory, including when a link is skipped.
  const auto fixture = [&](const std::string& path, bool link, bool hardlink = false, bool duplicate = false) {
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    require(writer != nullptr, "fixture writer allocation");
    require(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK, "fixture format");
    require(archive_write_set_bytes_per_block(writer.get(), 512) == ARCHIVE_OK, "fixture blocks");
#ifdef _WIN32
    require(archive_write_open_filename_w(writer.get(), bundle.c_str()) == ARCHIVE_OK, "fixture open");
#else
    require(archive_write_open_filename(writer.get(), bundle.c_str()) == ARCHIVE_OK, "fixture open");
#endif
    const std::unique_ptr<archive_entry, decltype(&archive_entry_free)> member(archive_entry_new(), archive_entry_free);
    require(member != nullptr, "fixture entry allocation");
    archive_entry_set_pathname_utf8(member.get(), path.c_str());
    archive_entry_set_filetype(member.get(), link ? AE_IFLNK : AE_IFREG);
    archive_entry_set_size(member.get(), link || hardlink ? 0 : 1);
    if (link) archive_entry_set_symlink(member.get(), "../source");
    if (hardlink) archive_entry_set_hardlink(member.get(), "../source/out/empty.txt");
    require(archive_write_header(writer.get(), member.get()) == ARCHIVE_OK, "fixture header");
    if (!link && !hardlink) require(archive_write_data(writer.get(), "x", 1) == 1, "fixture data");
    if (duplicate) {
      require(archive_write_header(writer.get(), member.get()) == ARCHIVE_OK, "duplicate fixture header");
      require(archive_write_data(writer.get(), "y", 1) == 1, "duplicate fixture data");
    }
    require(archive_write_close(writer.get()) == ARCHIVE_OK, "fixture close");
    if (path.find('\\') != std::string::npos) {
      // libarchive's Windows writer converts backslashes to slashes. Restore
      // the exact test pathname in this small regular-file header so the
      // rejection test exercises identical input bytes on both platforms.
      require(path.size() < 100 && !link && !hardlink, "raw pathname fixture shape");
      std::fstream bytes(bundle, std::ios::in | std::ios::out | std::ios::binary);
      char header[512];
      bytes.read(header, sizeof(header));
      require(bytes.gcount() == sizeof(header) && header[156] == '0', "raw pathname fixture header");
      std::fill_n(header, 100, '\0');
      std::copy(path.begin(), path.end(), header);
      std::fill_n(header + 148, 8, ' ');
      unsigned checksum = 0;
      for (unsigned char byte : header) checksum += byte;
      require(std::snprintf(header + 148, 7, "%06o", checksum) == 6, "fixture checksum");
      bytes.seekp(0);
      bytes.write(header, sizeof(header));
      require(static_cast<bool>(bytes), "raw pathname fixture write");
    }
  };
  for (const auto* invalid : {"../escaped.txt", "/absolute.txt", "C:/drive.txt", "folder\\escape.txt"}) {
    fixture(invalid, false);
    reject_extract(extraction);
  }
  require(!fs::exists(base / "escaped.txt"), "extraction escaped destination");
  fixture("redirect", true);
  reject_extract(extraction);
  fixture("redirect", false, true);
  reject_extract(extraction);
  bounded = extraction; bounded.skip_non_regular = true;
  ckgit::extractRegularTreeTar(bundle, extracted, bounded);
  require(fs::is_empty(extracted), "skipped link was materialized");
  fs::remove_all(extracted);
  fixture("duplicate.txt", false, false, true);
  reject_extract(extraction);
#ifdef _WIN32
  for (const auto* alias : {"nul.txt", "CON .txt", "COM1", "LPT\xc2\xb9", "name.", "name ", "bad?.txt"}) {
    fixture(alias, false);
    reject_extract(extraction);
  }
#endif
  fixture("ordinary.txt", false);
  fs::resize_file(bundle, 513);  // one header and only part of its padded data
  reject_extract(extraction);
  std::cout << "bounded TAR packing and extraction tests passed\n";
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
