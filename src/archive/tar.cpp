// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/archive.hpp"
#include "ckgit/tree_file.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <limits>
#include <locale.h>
#include <memory>
#include <set>
#include <stdexcept>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

namespace ckgit {
namespace {
namespace fs = std::filesystem;
class Utf8ThreadLocale {
 public:
  Utf8ThreadLocale() {
#ifdef _WIN32
    prior_mode_ = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
    if (prior_mode_ == -1) throw std::runtime_error("cannot select thread locale");
    prior_name_ = setlocale(LC_CTYPE, nullptr);
    if (!setlocale(LC_CTYPE, ".UTF8")) {
      _configthreadlocale(prior_mode_);
      throw std::runtime_error("UTF-8 locale unavailable");
    }
#else
    const auto base = duplocale(uselocale(nullptr));
    if (!base) throw std::runtime_error("cannot copy thread locale");
    locale_ = newlocale(LC_CTYPE_MASK, "C.UTF-8", base);
    if (!locale_) locale_ = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", base);
    if (!locale_) { freelocale(base); throw std::runtime_error("UTF-8 locale unavailable"); }
    prior_ = uselocale(locale_);
    if (!prior_) { freelocale(locale_); throw std::runtime_error("cannot select thread locale"); }
#endif
  }
  ~Utf8ThreadLocale() {
#ifdef _WIN32
    setlocale(LC_CTYPE, prior_name_.c_str());
    _configthreadlocale(prior_mode_);
#else
    uselocale(prior_);
    freelocale(locale_);
#endif
  }
  Utf8ThreadLocale(const Utf8ThreadLocale&) = delete;
  Utf8ThreadLocale& operator=(const Utf8ThreadLocale&) = delete;
 private:
#ifdef _WIN32
  int prior_mode_ = 0;
  std::string prior_name_;
#else
  locale_t locale_ = nullptr, prior_ = nullptr;
#endif
};

std::string utf8(const fs::path& path) {
  const auto bytes = path.generic_u8string();
  return {bytes.begin(), bytes.end()};
}
std::vector<std::string> components(const fs::path& path) {
  std::vector<std::string> result;
  if (path.is_absolute() || path.has_root_name()) throw std::runtime_error("archive path must be relative");
  for (const auto& part : path) {
    const auto name = utf8(part);
    if (name == "." || name.empty()) continue;
    if (name == ".." || name.find_first_of("\\:") != std::string::npos || name.find('\0') != std::string::npos)
      throw std::runtime_error("unsafe archive path");
    result.push_back(name);
  }
  return result;
}
bool regularTreeNode(const fs::path& path, const fs::file_status& status) {
  if (!fs::is_directory(status) && !fs::is_regular_file(status))
    return false;
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("archive input is unreadable");
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
#else
  (void)path;
#endif
  return true;
}
void requireRegularTreeNode(const fs::path& path, const fs::file_status& status) {
  if (!regularTreeNode(path, status)) throw std::runtime_error("archive input is not a regular file or directory");
}
struct Output {
  std::ofstream file;
  const TarPackOptions& options;
  std::chrono::steady_clock::time_point deadline;
  std::uintmax_t bytes = 0;
  std::exception_ptr failure;
  void check() const {
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("artifact packing timed out");
    if (options.should_continue && !options.should_continue()) throw std::runtime_error("artifact packing cancelled");
  }
  static la_ssize_t write(struct archive* writer, void* opaque, const void* data, size_t size) noexcept {
    auto& out = *static_cast<Output*>(opaque);
    try {
      out.check();
      if (size > out.options.max_bytes - out.bytes) throw std::runtime_error("artifact exceeds the size cap");
      out.file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
      if (!out.file) throw std::runtime_error("cannot write artifact bundle");
      out.bytes += size;
      return static_cast<la_ssize_t>(size);
    } catch (...) {
      out.failure = std::current_exception();
      archive_set_error(writer, 0, "artifact output failed");
      return -1;
    }
  }
};
void checked(int status, struct archive* writer, const Output& output) {
  if (output.failure) std::rethrow_exception(output.failure);
  if (status != ARCHIVE_OK) {
    const char* error = archive_error_string(writer);
    throw std::runtime_error(error ? error : "archive operation failed");
  }
}

struct Input {
  std::ifstream file;
  const TarExtractOptions& options;
  std::chrono::steady_clock::time_point deadline;
  std::array<char, 65536> buffer{};
  std::uintmax_t bytes = 0;
  std::exception_ptr failure;
  void check() const {
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("archive extraction timed out");
    if (options.should_continue && !options.should_continue()) throw std::runtime_error("archive extraction cancelled");
  }
  static la_ssize_t read(struct archive* reader, void* opaque, const void** data) noexcept {
    auto& in = *static_cast<Input*>(opaque);
    try {
      in.check();
      in.file.read(in.buffer.data(), static_cast<std::streamsize>(in.buffer.size()));
      if (in.file.bad() || (in.file.fail() && !in.file.eof())) throw std::runtime_error("cannot read archive");
      const auto count = static_cast<std::uintmax_t>(in.file.gcount());
      if (count > in.options.max_archive_bytes - in.bytes) throw std::runtime_error("archive exceeds input size cap");
      in.bytes += count;
      *data = in.buffer.data();
      return static_cast<la_ssize_t>(count);
    } catch (...) {
      in.failure = std::current_exception();
      archive_set_error(reader, 0, "archive input failed");
      return -1;
    }
  }
};
void checkedRead(int status, struct archive* reader, const Input& input) {
  if (input.failure) std::rethrow_exception(input.failure);
  if (status != ARCHIVE_OK) {
    const char* error = archive_error_string(reader);
    throw std::runtime_error(error ? error : "archive read failed");
  }
}
}

void packRegularTreeTar(const fs::path& root, const std::vector<std::string>& paths,
                        const fs::path& destination, const TarPackOptions& options) {
  if (paths.empty() || !options.max_bytes || !options.max_entries || options.timeout.count() <= 0)
    throw std::invalid_argument("archive paths and positive limits required");
  requireRegularTreeNode(root, fs::symlink_status(root));
  if (!fs::is_directory(root)) throw std::invalid_argument("archive root must be a directory");
  const auto relative_output = fs::absolute(destination).lexically_normal().lexically_relative(fs::absolute(root).lexically_normal());
  if (!relative_output.empty() && *relative_output.begin() != "..")
    throw std::invalid_argument("archive output must be outside source tree");
  const Utf8ThreadLocale locale;
  try {
    Output output{std::ofstream(destination, std::ios::binary), options, std::chrono::steady_clock::now() + options.timeout,
                  0, {}};
    if (!output.file) throw std::runtime_error("cannot create artifact bundle");
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    if (!writer) throw std::bad_alloc();
    checked(archive_write_set_format_pax_restricted(writer.get()), writer.get(), output);
    checked(archive_write_add_filter_none(writer.get()), writer.get(), output);
    checked(archive_write_set_bytes_per_block(writer.get(), 512), writer.get(), output);
    checked(archive_write_open(writer.get(), &output, nullptr, Output::write, nullptr), writer.get(), output);
    std::set<std::string> written;
    const auto add = [&](const fs::path& relative) {
      output.check();
      const auto parts = components(relative);
      const auto name = parts.empty() ? std::string(".") : utf8(relative.lexically_normal());
      if (name.size() > 4096) throw std::runtime_error("archive pathname exceeds limit");
      if (!written.insert(name).second) return;
      if (written.size() > options.max_entries) throw std::runtime_error("archive entry count exceeds limit");
      const auto full = root / relative;
      auto ancestor = root;
      for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
        ancestor /= fs::path(std::u8string(parts[index].begin(), parts[index].end()));
        const auto parent_status = fs::symlink_status(ancestor);
        requireRegularTreeNode(ancestor, parent_status);
        if (!fs::is_directory(parent_status)) throw std::runtime_error("archive parent is not a directory");
      }
      const auto status = fs::symlink_status(full);
      if (options.skip_non_regular && fs::exists(status) && !regularTreeNode(full, status)) return;
      requireRegularTreeNode(full, status);
      const std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
      if (!entry) throw std::bad_alloc();
      archive_entry_set_pathname_utf8(entry.get(), name.c_str());
      const bool directory = fs::is_directory(status);
      const bool executable = (status.permissions() & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
      archive_entry_set_perm(entry.get(), directory || executable ? 0755 : 0644);
      archive_entry_set_filetype(entry.get(), directory ? AE_IFDIR : AE_IFREG);
      if (directory) {
        archive_entry_set_size(entry.get(), 0);
        checked(archive_write_header(writer.get(), entry.get()), writer.get(), output);
      } else {
        const auto result = platform::streamTreeFile(root, parts,
            std::min<std::uintmax_t>(options.max_bytes, std::numeric_limits<la_int64_t>::max()), false, {
          [&](std::uintmax_t size) {
            output.check();
            archive_entry_set_size(entry.get(), static_cast<la_int64_t>(size));
            checked(archive_write_header(writer.get(), entry.get()), writer.get(), output);
          },
          [&](std::string_view chunk) {
            output.check();
            const auto count = archive_write_data(writer.get(), chunk.data(), chunk.size());
            if (output.failure) std::rethrow_exception(output.failure);
            if (count < 0 || static_cast<std::size_t>(count) != chunk.size()) throw std::runtime_error("archive data write failed");
          }
        });
        if (result.status != platform::TreeFileStatus::Ok) throw std::runtime_error("cannot read archive input within size cap");
      }
      checked(archive_write_finish_entry(writer.get()), writer.get(), output);
    };
    for (const auto& requested : paths) {
      if (requested.empty()) throw std::invalid_argument("empty archive path");
      const fs::path relative(std::u8string(requested.begin(), requested.end()));
      components(relative);
      add(relative);
      const auto requested_status = fs::symlink_status(root / relative);
      if (fs::is_directory(requested_status) && regularTreeNode(root / relative, requested_status)) {
        for (auto iterator = fs::recursive_directory_iterator(root / relative); iterator != fs::recursive_directory_iterator(); ++iterator) {
          if (!regularTreeNode(iterator->path(), iterator->symlink_status())) iterator.disable_recursion_pending();
          add(iterator->path().lexically_relative(root));
        }
      }
    }
    checked(archive_write_close(writer.get()), writer.get(), output);
    output.file.close();
    if (!output.file) throw std::runtime_error("cannot finish artifact bundle");
  } catch (...) {
    std::error_code ignored;
    fs::remove(destination, ignored);
    throw;
  }
}

void extractRegularTreeTar(const fs::path& source, const fs::path& destination, const TarExtractOptions& options) {
  if (!options.max_archive_bytes || !options.max_bytes || !options.max_file_bytes || !options.max_entries ||
      options.timeout.count() <= 0) throw std::invalid_argument("positive archive extraction limits required");
  requireRegularTreeNode(source, fs::symlink_status(source));
  if (!fs::is_regular_file(source) || fs::file_size(source) > options.max_archive_bytes)
    throw std::runtime_error("archive exceeds input size cap or is not a file");
  if (fs::file_size(source) % 512 != 0) throw std::runtime_error("archive has a partial TAR block");
  const Utf8ThreadLocale locale;
  // Only clean up a directory this invocation created, never caller data.
  if (!fs::create_directory(destination)) throw std::runtime_error("archive destination already exists");
  try {
    Input input{std::ifstream(source, std::ios::binary), options,
                std::chrono::steady_clock::now() + options.timeout, {}, 0, {}};
    if (!input.file) throw std::runtime_error("cannot open archive");
    const std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    if (!reader) throw std::bad_alloc();
    checkedRead(archive_read_support_filter_none(reader.get()), reader.get(), input);
    checkedRead(archive_read_support_format_tar(reader.get()), reader.get(), input);
    checkedRead(archive_read_open(reader.get(), &input, nullptr, Input::read, nullptr), reader.get(), input);
    std::uintmax_t total = 0;
    std::size_t count = 0;
    for (;;) {
      input.check();
      archive_entry* entry = nullptr;
      const auto status = archive_read_next_header(reader.get(), &entry);
      if (status == ARCHIVE_EOF) break;
      checkedRead(status, reader.get(), input);
      if (++count > options.max_entries) throw std::runtime_error("archive entry count exceeds limit");
      const char* raw = archive_entry_pathname_utf8(entry);
      if (!raw || !*raw || std::char_traits<char>::length(raw) > 4096)
        throw std::runtime_error("invalid archive pathname");
      const std::string name(raw);
      // Interpret member names identically on POSIX and Windows. Do not let
      // std::filesystem treat a backslash as a platform-specific separator.
      if (name.find('\\') != std::string::npos) throw std::runtime_error("unsafe archive path separator");
      const auto parts = components(fs::path(std::u8string(name.begin(), name.end())));
      if (parts.size() > 64) throw std::runtime_error("archive pathname is too deep");
      const bool directory = archive_entry_filetype(entry) == AE_IFDIR;
      const bool regular = archive_entry_filetype(entry) == AE_IFREG;
      if ((!directory && !regular) || archive_entry_hardlink(entry) || archive_entry_symlink(entry)) {
        if (options.skip_non_regular) {
          checkedRead(archive_read_data_skip(reader.get()), reader.get(), input);
          continue;
        }
        throw std::runtime_error("archive contains a link or special entry");
      }
      if ((!directory && parts.empty()) || archive_entry_sparse_count(entry) != 0)
        throw std::runtime_error("unsupported archive entry");
      auto path = destination;
      for (std::size_t index = 0; index < parts.size(); ++index) {
        input.check();
        const auto& part = parts[index];
#ifdef _WIN32
        // Win32 path aliases must not turn a member into a device or collide
        // with another member after trailing-dot/space normalization.
        auto base = part.substr(0, part.find('.'));
        std::transform(base.begin(), base.end(), base.begin(), [](unsigned char ch) {
          return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : static_cast<char>(ch);
        });
        const auto suffix = base.size() > 3 ? base.substr(3) : std::string();
        const bool numbered_device = (base.starts_with("COM") || base.starts_with("LPT")) &&
            ((suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') ||
             suffix == "\xc2\xb9" || suffix == "\xc2\xb2" || suffix == "\xc2\xb3");
        if (part.back() == '.' || part.back() == ' ' || base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            base == "CONIN$" || base == "CONOUT$" || numbered_device)
          throw std::runtime_error("archive pathname is a Windows alias");
#endif
        path /= fs::path(std::u8string(part.begin(), part.end()));
        if (directory || index + 1 < parts.size()) {
          if (!fs::create_directory(path)) {
            const auto state = fs::symlink_status(path);
            requireRegularTreeNode(path, state);
            if (!fs::is_directory(state)) throw std::runtime_error("archive parent is not a directory");
          }
        }
      }
      if (directory) {
        if (archive_entry_size(entry) != 0) throw std::runtime_error("archive directory contains data");
        continue;
      }
      const auto declared = archive_entry_size(entry);
      if (declared < 0 || static_cast<std::uintmax_t>(declared) > options.max_file_bytes ||
          static_cast<std::uintmax_t>(declared) > options.max_bytes - total)
        throw std::runtime_error("archive content exceeds size cap");
      if (fs::exists(fs::symlink_status(path))) throw std::runtime_error("duplicate archive file");
      std::ofstream output(path, std::ios::binary);
      if (!output) throw std::runtime_error("cannot create extracted file");
      std::array<char, 65536> buffer{};
      std::uintmax_t written = 0;
      for (;;) {
        input.check();
        const auto received = archive_read_data(reader.get(), buffer.data(), buffer.size());
        if (input.failure) std::rethrow_exception(input.failure);
        if (received < 0) throw std::runtime_error("archive file data is invalid");
        if (!received) break;
        const auto bytes = static_cast<std::uintmax_t>(received);
        if (bytes > static_cast<std::uintmax_t>(declared) - written) throw std::runtime_error("archive file exceeds declared size");
        output.write(buffer.data(), static_cast<std::streamsize>(received));
        if (!output) throw std::runtime_error("cannot write extracted file");
        written += bytes;
      }
      if (written != static_cast<std::uintmax_t>(declared)) throw std::runtime_error("archive file is truncated");
      output.close();
      if (!output) throw std::runtime_error("cannot finish extracted file");
      total += written;
    }
    checkedRead(archive_read_close(reader.get()), reader.get(), input);
  } catch (...) {
    std::error_code ignored;
    fs::remove_all(destination, ignored);
    throw;
  }
}
}
