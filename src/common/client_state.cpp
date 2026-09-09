// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/client_state.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ckgit/text.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

constexpr std::size_t kMaximumSelectionBytes = 64 * 1024;
constexpr std::size_t kMaximumLineBytes = 2048;
constexpr std::size_t kMaximumSelections = 1024;
constexpr mode_t kPrivateFileMode = 0600;

bool isAcceptableCheckoutPath(std::string_view value) {
  return !value.empty() && value.size() <= 1024 && value.front() == '/' && isValidUtf8(value) &&
         !hasControlCharacter(value) &&
         std::isspace(static_cast<unsigned char>(value.back())) == 0;
}

void writeAll(int descriptor, std::string_view content) {
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t written = write(descriptor, content.data() + offset, content.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      throw std::runtime_error("could not write canonical checkout selection");
    }
    offset += static_cast<std::size_t>(written);
  }
}

std::string renderSelections(const std::map<std::string, std::filesystem::path>& selections) {
  std::string rendered = "schema_version=1\n";
  for (const auto& [project, checkout] : selections) {
    rendered += project;
    rendered += '=';
    rendered += checkout.string();
    rendered += '\n';
  }
  return rendered;
}

}  // namespace

SyncLock::SyncLock(int descriptor) : descriptor_(descriptor) {}

SyncLock::~SyncLock() {
  if (descriptor_ >= 0) {
    close(descriptor_);  // Closing releases the flock() lock.
  }
}

SyncLock::SyncLock(SyncLock&& other) noexcept : descriptor_(other.descriptor_) {
  other.descriptor_ = -1;
}

SyncLock& SyncLock::operator=(SyncLock&& other) noexcept {
  if (this != &other) {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
    descriptor_ = other.descriptor_;
    other.descriptor_ = -1;
  }
  return *this;
}

std::optional<SyncLock> SyncLock::tryAcquire(const std::filesystem::path& lock_path) {
  if (lock_path.empty() || lock_path.filename().empty()) {
    throw std::invalid_argument("sync lock path is required");
  }
  const int descriptor = open(lock_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, kPrivateFileMode);
  if (descriptor < 0) {
    throw std::runtime_error("could not open sync lock " + lock_path.string());
  }
  struct stat status {};
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid()) {
    close(descriptor);
    throw std::runtime_error("sync lock is not a regular file owned by this user: " + lock_path.string());
  }
  if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int saved_errno = errno;
    close(descriptor);
    if (saved_errno == EWOULDBLOCK) {
      return std::nullopt;
    }
    throw std::runtime_error("could not acquire sync lock " + lock_path.string());
  }
  return SyncLock(descriptor);
}

std::map<std::string, std::filesystem::path> loadCanonicalCheckouts(const std::filesystem::path& file) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(file, error);
  if (status.type() == std::filesystem::file_type::not_found ||
      error == std::errc::no_such_file_or_directory) {
    return {};
  }
  if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
    throw std::runtime_error("canonical checkout selection must be a regular non-symlink file: " +
                             file.string());
  }
  const auto size = std::filesystem::file_size(file, error);
  if (error || size > kMaximumSelectionBytes) {
    throw std::runtime_error("canonical checkout selection is too large or unavailable: " + file.string());
  }
  std::ifstream stream(file, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot open canonical checkout selection: " + file.string());
  }
  std::map<std::string, std::filesystem::path> selections;
  bool has_schema = false;
  std::size_t line_number = 0;
  for (std::string line; std::getline(stream, line);) {
    ++line_number;
    const std::string location = file.string() + ":" + std::to_string(line_number);
    if (line.size() > kMaximumLineBytes || (!line.empty() && line.back() == '\r') || hasControlCharacter(line)) {
      throw std::runtime_error("invalid canonical checkout selection " + location + ": malformed line");
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto equals = line.find('=');
    if (equals == std::string::npos || equals == 0) {
      throw std::runtime_error("invalid canonical checkout selection " + location + ": expected project=path");
    }
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (!has_schema) {
      if (key != "schema_version" || value != "1") {
        throw std::runtime_error("invalid canonical checkout selection " + location +
                                 ": schema_version=1 must come first");
      }
      has_schema = true;
      continue;
    }
    if (key == "schema_version" || !isValidProjectName(key) || !isAcceptableCheckoutPath(value) ||
        selections.size() == kMaximumSelections || !selections.emplace(key, value).second) {
      throw std::runtime_error("invalid canonical checkout selection " + location +
                               ": expected a unique project and an absolute path");
    }
  }
  if (!stream.eof()) {
    throw std::runtime_error("could not read canonical checkout selection: " + file.string());
  }
  if (!has_schema) {
    throw std::runtime_error("canonical checkout selection is missing schema_version: " + file.string());
  }
  return selections;
}

namespace {
void writeCanonicalSelections(const std::filesystem::path& file,
                              const std::map<std::string, std::filesystem::path>& selections) {
  const std::string content = renderSelections(selections);
  if (selections.size() > kMaximumSelections || content.size() > kMaximumSelectionBytes) {
    throw std::runtime_error("canonical checkout selection would exceed its size limit");
  }

  const std::filesystem::path directory = file.has_parent_path() ? file.parent_path() : ".";
  const int directory_descriptor = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_descriptor < 0) {
    throw std::runtime_error("could not open configuration directory " + directory.string());
  }
  const std::string final_name = file.filename().string();
  std::string staging_name;
  int staging = -1;
  try {
    for (unsigned int attempt = 0; attempt < 32 && staging < 0; ++attempt) {
      staging_name = "." + final_name + ".staging-" + std::to_string(getpid()) + "-" + std::to_string(attempt);
      staging = openat(directory_descriptor, staging_name.c_str(),
                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, kPrivateFileMode);
      if (staging < 0 && errno != EEXIST) {
        throw std::runtime_error("could not create canonical checkout staging file in " + directory.string());
      }
    }
    if (staging < 0) {
      throw std::runtime_error("could not allocate canonical checkout staging file");
    }
    writeAll(staging, content);
    if (fsync(staging) != 0 || close(staging) != 0) {
      staging = -1;
      throw std::runtime_error("could not sync canonical checkout staging file");
    }
    staging = -1;
    if (renameat(directory_descriptor, staging_name.c_str(), directory_descriptor, final_name.c_str()) != 0) {
      throw std::runtime_error("could not replace canonical checkout selection " + file.string());
    }
    fsync(directory_descriptor);  // Best effort: the rename itself is already atomic.
    close(directory_descriptor);
  } catch (...) {
    if (staging >= 0) {
      close(staging);
    }
    if (!staging_name.empty()) {
      unlinkat(directory_descriptor, staging_name.c_str(), 0);
    }
    close(directory_descriptor);
    throw;
  }
}

}  // namespace

void saveCanonicalCheckout(const std::filesystem::path& file, std::string_view project,
                           const std::filesystem::path& checkout) {
  if (!isValidProjectName(project) || !isAcceptableCheckoutPath(checkout.string()) || file.filename().empty())
    throw std::invalid_argument("canonical checkout selection needs a valid project and an absolute path");
  auto selections = loadCanonicalCheckouts(file);
  selections[std::string(project)] = checkout;
  writeCanonicalSelections(file, selections);
}

bool forgetCanonicalCheckout(const std::filesystem::path& file, std::string_view project) {
  if (!isValidProjectName(project) || file.filename().empty())
    throw std::invalid_argument("forget checkout requires a valid project and selection file");
  auto selections = loadCanonicalCheckouts(file);
  if (selections.erase(std::string(project)) == 0) return false;
  writeCanonicalSelections(file, selections);
  return true;
}

bool looksEphemeralCheckoutPath(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  const std::size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || name.size() - dot - 1 != 6) {
    return false;
  }
  const std::string_view suffix = std::string_view(name).substr(dot + 1);
  bool has_upper = false;
  bool has_lower_or_digit = false;
  for (const unsigned char character : suffix) {
    if (std::isalnum(character) == 0) {
      return false;
    }
    has_upper = has_upper || std::isupper(character) != 0;
    has_lower_or_digit = has_lower_or_digit || std::islower(character) != 0 || std::isdigit(character) != 0;
  }
  return has_upper && has_lower_or_digit;
}

}  // namespace ckgit
