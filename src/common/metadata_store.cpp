// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/metadata_store.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "ckgit/text.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

constexpr std::size_t kMaximumReportedPathBytes = 256;
constexpr std::size_t kMaximumRecordBytes = 2048;
constexpr std::size_t kMaximumEventLogBytes = 64 * 1024;
constexpr std::size_t kMaximumEventLineBytes = 256;
constexpr mode_t kPrivateDirectoryMode = 0700;
constexpr mode_t kPrivateFileMode = 0600;

bool isSafeReportedPath(std::string_view path) {
  return !path.empty() && path.size() <= kMaximumReportedPathBytes && isValidUtf8(path) &&
         std::all_of(path.begin(), path.end(), [](unsigned char character) {
           return character >= 0x20 && character != 0x7f;
         });
}

bool isValidEventKind(std::string_view kind) {
  return !kind.empty() && kind.size() <= 48 &&
         std::all_of(kind.begin(), kind.end(), [](unsigned char character) {
           return (character >= 'a' && character <= 'z') || character == '-';
         });
}

std::optional<std::string> decodeCheckoutPath(std::string_view token) {
  if (token.empty() || token.size() > kMaximumReportedPathBytes * 2 || token.size() % 2 != 0) {
    return std::nullopt;
  }
  const auto hex_value = [](unsigned char character) -> int {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    return -1;
  };
  std::string decoded;
  decoded.reserve(token.size() / 2);
  for (std::size_t index = 0; index < token.size(); index += 2) {
    const int high = hex_value(static_cast<unsigned char>(token[index]));
    const int low = hex_value(static_cast<unsigned char>(token[index + 1]));
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    decoded += static_cast<char>((high << 4) | low);
  }
  return isSafeReportedPath(decoded) ? std::optional<std::string>(std::move(decoded)) : std::nullopt;
}

int openPrivateDirectory(const std::filesystem::path& path) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::runtime_error("could not open metadata state directory");
  }
  struct stat status {};
  if (fstat(descriptor, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & 0077) != 0) {
    close(descriptor);
    throw std::runtime_error("metadata state directory is not private and daemon-owned");
  }
  return descriptor;
}

int ensurePrivateDirectoryAt(int parent, std::string_view name) {
  const std::string directory_name(name);
  if (mkdirat(parent, directory_name.c_str(), kPrivateDirectoryMode) != 0 && errno != EEXIST) {
    throw std::runtime_error("could not create metadata directory");
  }
  const int descriptor = openat(parent, directory_name.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::runtime_error("could not open metadata directory");
  }
  struct stat status {};
  if (fstat(descriptor, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & 0077) != 0) {
    close(descriptor);
    throw std::runtime_error("metadata directory is not private and daemon-owned");
  }
  return descriptor;
}

int openPrivateDirectoryAt(int parent, std::string_view name, bool* missing) {
  *missing = false;
  const std::string directory_name(name);
  const int descriptor = openat(parent, directory_name.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      *missing = true;
      return -1;
    }
    throw std::runtime_error("could not open metadata directory");
  }
  struct stat status {};
  if (fstat(descriptor, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & 0077) != 0) {
    close(descriptor);
    throw std::runtime_error("metadata directory is not private and daemon-owned");
  }
  return descriptor;
}

void writeAll(int descriptor, std::string_view content) {
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t written = write(descriptor, content.data() + offset, content.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      throw std::runtime_error("could not write checkout metadata");
    }
    offset += static_cast<std::size_t>(written);
  }
}

std::string readRecordAt(int directory, std::string_view name) {
  const std::string filename(name);
  const int descriptor = openat(directory, filename.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::runtime_error("could not open checkout metadata record");
  }
  try {
    struct stat status {};
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0077) != 0 || status.st_size < 0 ||
        static_cast<std::size_t>(status.st_size) > kMaximumRecordBytes) {
      throw std::runtime_error("checkout metadata record is unsafe");
    }
    std::string content;
    content.resize(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < content.size()) {
      const ssize_t received = read(descriptor, content.data() + offset, content.size() - offset);
      if (received < 0 && errno == EINTR) {
        continue;
      }
      if (received <= 0) {
        throw std::runtime_error("could not read checkout metadata record");
      }
      offset += static_cast<std::size_t>(received);
    }
    close(descriptor);
    return content;
  } catch (...) {
    close(descriptor);
    throw;
  }
}

CheckoutMetadata parseRecord(std::string_view content, std::string_view project,
                             std::string_view expected_client) {
  std::array<std::string_view, 5> lines{};
  std::size_t start = 0;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::size_t newline = content.find('\n', start);
    if (newline == std::string_view::npos) {
      throw std::runtime_error("checkout metadata record has invalid framing");
    }
    lines[index] = content.substr(start, newline - start);
    start = newline + 1;
  }
  if (start != content.size() || lines[0] != "schema_version=1" ||
      lines[1].rfind("client_id=", 0) != 0 || lines[2].rfind("project=", 0) != 0 ||
      lines[3].rfind("path_hex=", 0) != 0 || lines[4].rfind("last_seen_epoch=", 0) != 0) {
    throw std::runtime_error("checkout metadata record has invalid schema");
  }
  const std::string_view client = lines[1].substr(std::string_view("client_id=").size());
  const std::string_view stored_project = lines[2].substr(std::string_view("project=").size());
  const std::string_view encoded_path = lines[3].substr(std::string_view("path_hex=").size());
  const std::string_view epoch_text = lines[4].substr(std::string_view("last_seen_epoch=").size());
  const auto decoded_path = decodeCheckoutPath(encoded_path);
  std::uint64_t epoch = 0;
  const auto [end, parse_error] = std::from_chars(epoch_text.data(), epoch_text.data() + epoch_text.size(), epoch);
  if (!isValidClientId(client) || client != expected_client || stored_project != project ||
      !decoded_path.has_value() || parse_error != std::errc{} || end != epoch_text.data() + epoch_text.size()) {
    throw std::runtime_error("checkout metadata record contains invalid data");
  }
  return CheckoutMetadata{std::string(client), *decoded_path, epoch};
}

std::uint64_t currentEpochSeconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
  if (seconds < 0) {
    throw std::runtime_error("system clock is before the Unix epoch");
  }
  return static_cast<std::uint64_t>(seconds);
}

void verifyPrivateRegularFile(int descriptor, std::size_t maximum_size, std::string_view description) {
  struct stat status {};
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & 0077) != 0 || status.st_size < 0 ||
      static_cast<std::size_t>(status.st_size) > maximum_size) {
    throw std::runtime_error(std::string(description) + " is unsafe");
  }
}

std::string readPrivateFileAt(int directory, std::string_view filename,
                              std::size_t maximum_size, std::string_view description) {
  const std::string name(filename);
  const int descriptor = openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::runtime_error("could not open " + std::string(description));
  }
  try {
    verifyPrivateRegularFile(descriptor, maximum_size, description);
    struct stat status {};
    if (fstat(descriptor, &status) != 0) {
      throw std::runtime_error("could not inspect " + std::string(description));
    }
    std::string content(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0;
    while (offset < content.size()) {
      const ssize_t received = read(descriptor, content.data() + offset, content.size() - offset);
      if (received < 0 && errno == EINTR) {
        continue;
      }
      if (received <= 0) {
        throw std::runtime_error("could not read " + std::string(description));
      }
      offset += static_cast<std::size_t>(received);
    }
    close(descriptor);
    return content;
  } catch (...) {
    close(descriptor);
    throw;
  }
}

}  // namespace

std::string encodeCheckoutPath(std::string_view path) {
  if (!isSafeReportedPath(path)) {
    throw std::invalid_argument("reported checkout path must be printable, valid UTF-8, and at most 256 bytes");
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(path.size() * 2);
  for (const unsigned char character : path) {
    encoded += hex[character >> 4];
    encoded += hex[character & 0x0f];
  }
  return encoded;
}

bool isValidCheckoutPathToken(std::string_view token) {
  return decodeCheckoutPath(token).has_value();
}

std::filesystem::path validatedMetadataRoot(const std::filesystem::path& requested_root) {
  if (requested_root.empty()) {
    throw std::invalid_argument("metadata state root is required");
  }
  const int descriptor = openPrivateDirectory(requested_root);
  close(descriptor);
  return requested_root;
}

void registerCheckout(const std::filesystem::path& state_root, std::string_view project_name,
                      std::string_view client_id, std::string_view encoded_path) {
  if (!isValidProjectName(project_name) || !isValidClientId(client_id) ||
      !isValidCheckoutPathToken(encoded_path)) {
    throw std::invalid_argument("invalid checkout metadata");
  }
  int root = openPrivateDirectory(validatedMetadataRoot(state_root));
  int checkouts = -1;
  int project = -1;
  int temporary = -1;
  const std::string client_filename = std::string(client_id) + ".ini";
  std::string temporary_name;
  try {
    checkouts = ensurePrivateDirectoryAt(root, "checkouts");
    project = ensurePrivateDirectoryAt(checkouts, project_name);
    const std::string record = "schema_version=1\nclient_id=" + std::string(client_id) + "\nproject=" +
        std::string(project_name) + "\npath_hex=" + std::string(encoded_path) + "\nlast_seen_epoch=" +
        std::to_string(currentEpochSeconds()) + "\n";
    for (unsigned int attempt = 0; attempt < 32; ++attempt) {
      temporary_name = ".staging-" + std::to_string(getpid()) + "-" + std::to_string(attempt);
      temporary = openat(project, temporary_name.c_str(),
                         O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, kPrivateFileMode);
      if (temporary >= 0) {
        break;
      }
      if (errno != EEXIST) {
        throw std::runtime_error("could not create checkout metadata staging file");
      }
    }
    if (temporary < 0) {
      throw std::runtime_error("could not allocate checkout metadata staging file");
    }
    writeAll(temporary, record);
    if (fsync(temporary) != 0) {
      throw std::runtime_error("could not sync checkout metadata staging file");
    }
    if (close(temporary) != 0) {
      temporary = -1;
      throw std::runtime_error("could not sync checkout metadata staging file");
    }
    temporary = -1;
    if (renameat(project, temporary_name.c_str(), project, client_filename.c_str()) != 0 || fsync(project) != 0) {
      throw std::runtime_error("could not atomically publish checkout metadata");
    }
    close(project);
    close(checkouts);
    close(root);
  } catch (...) {
    if (temporary >= 0) {
      close(temporary);
    }
    if (project >= 0 && !temporary_name.empty()) {
      unlinkat(project, temporary_name.c_str(), 0);
    }
    if (project >= 0) {
      close(project);
    }
    if (checkouts >= 0) {
      close(checkouts);
    }
    close(root);
    throw;
  }
}

std::vector<CheckoutMetadata> loadCheckoutMetadata(const std::filesystem::path& state_root,
                                                    std::string_view project_name) {
  if (!isValidProjectName(project_name)) {
    throw std::invalid_argument("invalid project name for checkout metadata");
  }
  const int root = openPrivateDirectory(validatedMetadataRoot(state_root));
  int checkouts = -1;
  int project = -1;
  DIR* entries = nullptr;
  try {
    bool missing = false;
    checkouts = openPrivateDirectoryAt(root, "checkouts", &missing);
    if (missing) {
      close(root);
      return {};
    }
    project = openPrivateDirectoryAt(checkouts, project_name, &missing);
    if (missing) {
      close(checkouts);
      close(root);
      return {};
    }
    const int scan_descriptor = dup(project);
    if (scan_descriptor < 0 || (entries = fdopendir(scan_descriptor)) == nullptr) {
      if (scan_descriptor >= 0) {
        close(scan_descriptor);
      }
      throw std::runtime_error("could not scan checkout metadata");
    }
    std::vector<CheckoutMetadata> records;
    while (dirent* entry = readdir(entries)) {
      const std::string_view filename(entry->d_name);
      constexpr std::string_view suffix{".ini"};
      if (filename.size() <= suffix.size() || filename.substr(filename.size() - suffix.size()) != suffix) {
        continue;
      }
      const std::string_view client = filename.substr(0, filename.size() - suffix.size());
      if (!isValidClientId(client)) {
        throw std::runtime_error("checkout metadata filename is invalid");
      }
      records.push_back(parseRecord(readRecordAt(project, filename), project_name, client));
    }
    closedir(entries);
    entries = nullptr;
    close(project);
    close(checkouts);
    close(root);
    std::sort(records.begin(), records.end(), [](const CheckoutMetadata& left, const CheckoutMetadata& right) {
      return left.client_id < right.client_id;
    });
    return records;
  } catch (...) {
    if (entries != nullptr) {
      closedir(entries);
    }
    if (project >= 0) {
      close(project);
    }
    if (checkouts >= 0) {
      close(checkouts);
    }
    close(root);
    throw;
  }
}

void appendStateEvent(const std::filesystem::path& state_root, std::string_view kind,
                      std::string_view project_name, std::string_view client_id) {
  if (!isValidEventKind(kind) || !isValidProjectName(project_name) || !isValidClientId(client_id)) {
    throw std::invalid_argument("invalid server event");
  }
  const std::string record = std::to_string(currentEpochSeconds()) + "\t" + std::string(kind) + "\t" +
      std::string(project_name) + "\t" + std::string(client_id) + "\n";
  if (record.size() > kMaximumEventLogBytes) {
    throw std::invalid_argument("server event is too large");
  }
  int root = openPrivateDirectory(validatedMetadataRoot(state_root));
  int events = -1;
  int log = -1;
  try {
    events = ensurePrivateDirectoryAt(root, "events");
    log = openat(events, "events.log", O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
                 kPrivateFileMode);
    if (log < 0) {
      throw std::runtime_error("could not open server event log");
    }
    verifyPrivateRegularFile(log, kMaximumEventLogBytes - record.size(), "server event log");
    writeAll(log, record);
    if (fsync(log) != 0) {
      throw std::runtime_error("could not sync server event log");
    }
    if (close(log) != 0) {
      log = -1;
      throw std::runtime_error("could not close server event log");
    }
    log = -1;
    if (fsync(events) != 0) {
      throw std::runtime_error("could not sync server event directory");
    }
    close(events);
    events = -1;
    close(root);
    root = -1;
  } catch (...) {
    if (log >= 0) {
      close(log);
    }
    if (events >= 0) {
      close(events);
    }
    if (root >= 0) {
      close(root);
    }
    throw;
  }
}

std::vector<StateEvent> loadProjectEvents(const std::filesystem::path& state_root,
                                          std::string_view project_name, std::size_t maximum) {
  if (!isValidProjectName(project_name) || maximum > 128) {
    throw std::invalid_argument("invalid project event request");
  }
  if (maximum == 0) {
    return {};
  }
  int root = openPrivateDirectory(validatedMetadataRoot(state_root));
  int events = -1;
  try {
    bool missing = false;
    events = openPrivateDirectoryAt(root, "events", &missing);
    if (missing) {
      close(root);
      return {};
    }
    const int log = openat(events, "events.log", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (log < 0) {
      if (errno == ENOENT) {
        close(events);
        close(root);
        return {};
      }
      throw std::runtime_error("could not open server event log");
    }
    close(log);
    const std::string content = readPrivateFileAt(events, "events.log", kMaximumEventLogBytes,
                                                  "server event log");
    close(events);
    events = -1;
    close(root);
    root = -1;

    std::vector<StateEvent> records;
    std::size_t start = 0;
    while (start < content.size()) {
      const std::size_t newline = content.find('\n', start);
      if (newline == std::string::npos) {
        break;  // A crash may leave only the final, incomplete append unreadable.
      }
      const std::string_view line(content.data() + start, newline - start);
      start = newline + 1;
      if (line.empty() || line.size() > kMaximumEventLineBytes) {
        throw std::runtime_error("server event log has invalid framing");
      }
      const std::size_t first_tab = line.find('\t');
      const std::size_t second_tab = first_tab == std::string_view::npos ? std::string_view::npos :
          line.find('\t', first_tab + 1);
      const std::size_t third_tab = second_tab == std::string_view::npos ? std::string_view::npos :
          line.find('\t', second_tab + 1);
      if (first_tab == std::string_view::npos || second_tab == std::string_view::npos ||
          third_tab == std::string_view::npos || line.find('\t', third_tab + 1) != std::string_view::npos) {
        throw std::runtime_error("server event log has invalid schema");
      }
      const std::string_view epoch_text = line.substr(0, first_tab);
      const std::string_view kind = line.substr(first_tab + 1, second_tab - first_tab - 1);
      const std::string_view project = line.substr(second_tab + 1, third_tab - second_tab - 1);
      const std::string_view client = line.substr(third_tab + 1);
      std::uint64_t epoch = 0;
      const auto [end, parse_error] = std::from_chars(epoch_text.data(), epoch_text.data() + epoch_text.size(), epoch);
      if (parse_error != std::errc{} || end != epoch_text.data() + epoch_text.size() ||
          !isValidEventKind(kind) || !isValidProjectName(project) || !isValidClientId(client)) {
        throw std::runtime_error("server event log contains invalid data");
      }
      if (project == project_name) {
        records.push_back(StateEvent{epoch, std::string(kind), std::string(project), std::string(client)});
      }
    }
    if (records.size() > maximum) {
      records.erase(records.begin(), records.end() - static_cast<std::ptrdiff_t>(maximum));
    }
    return records;
  } catch (...) {
    if (events >= 0) {
      close(events);
    }
    if (root >= 0) {
      close(root);
    }
    throw;
  }
}

}  // namespace ckgit
