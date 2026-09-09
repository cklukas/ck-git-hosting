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
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/file.h>
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

class Descriptor {
 public:
  explicit Descriptor(int value) : value_(value) {}
  ~Descriptor() { if (value_ >= 0) close(value_); }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  operator int() const { return value_; }
 private:
  int value_;
};

void lockMetadata(int descriptor, int operation) {
  while (flock(descriptor, operation) != 0) {
    if (errno != EINTR) throw std::runtime_error("could not lock metadata state");
  }
}

std::vector<std::string> directoryNames(int descriptor) {
  const int copy = dup(descriptor);
  DIR* directory = copy < 0 ? nullptr : fdopendir(copy);
  if (directory == nullptr) {
    if (copy >= 0) close(copy);
    throw std::runtime_error("could not list metadata directory");
  }
  std::vector<std::string> result;
  errno = 0;
  while (dirent* entry = readdir(directory)) {
    const std::string_view name(entry->d_name);
    if (name != "." && name != "..") result.emplace_back(name);
    errno = 0;
  }
  const int scan_error = errno;
  closedir(directory);
  if (scan_error != 0) throw std::runtime_error("could not list metadata directory");
  return result;
}

std::vector<std::string> eventLogNames(int events) {
  std::vector<std::string> names;
  for (auto& name : directoryNames(events)) {
    if (name == "events.log" || (name.starts_with("events.") && name.ends_with(".log"))) {
      struct stat status {};
      if (fstatat(events, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0 ||
          !S_ISREG(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0077) != 0) {
        throw std::runtime_error("server event archive is unsafe");
      }
      names.push_back(std::move(name));
    }
  }
  // Fixed-width sequence suffixes sort after the first archive of a month;
  // the live log is always newest, including after a system-clock adjustment.
  const auto key = [](const std::string& name) {
    if (name == "events.log") return std::string("~");
    if (name.size() == std::string_view("events.2026-09.log").size()) {
      return name.substr(0, name.size() - 4) + ".000000.log";
    }
    return name;
  };
  std::sort(names.begin(), names.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
  return names;
}

std::vector<StateEvent> parseEvents(std::string_view content) {
  std::vector<StateEvent> records;
  std::size_t start = 0;
  while (start < content.size()) {
    const auto newline = content.find('\n', start);
    if (newline == std::string_view::npos) break;  // Ignore an incomplete final append after a crash.
    const auto line = content.substr(start, newline - start);
    start = newline + 1;
    if (line.empty() || line.size() > kMaximumEventLineBytes) {
      throw std::runtime_error("server event log has invalid framing");
    }
    const auto first = line.find('\t');
    const auto second = first == std::string_view::npos ? first : line.find('\t', first + 1);
    const auto third = second == std::string_view::npos ? second : line.find('\t', second + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        third == std::string_view::npos || line.find('\t', third + 1) != std::string_view::npos) {
      throw std::runtime_error("server event log has invalid schema");
    }
    const auto epoch_text = line.substr(0, first);
    const auto kind = line.substr(first + 1, second - first - 1);
    const auto project = line.substr(second + 1, third - second - 1);
    const auto client = line.substr(third + 1);
    std::uint64_t epoch = 0;
    const auto [end, error] = std::from_chars(epoch_text.data(), epoch_text.data() + epoch_text.size(), epoch);
    if (error != std::errc{} || end != epoch_text.data() + epoch_text.size() ||
        !isValidEventKind(kind) || !isValidProjectName(project) || !isValidClientId(client)) {
      throw std::runtime_error("server event log contains invalid data");
    }
    records.push_back({epoch, std::string(kind), std::string(project), std::string(client)});
  }
  return records;
}

std::string serializeEvent(const StateEvent& event) {
  return std::to_string(event.epoch_seconds) + "\t" + event.kind + "\t" +
      event.project_name + "\t" + event.client_id + "\n";
}

void rotateEvents(int events) {
  const auto now = static_cast<std::time_t>(currentEpochSeconds());
  std::tm date {};
  char month[8]{};
  if (gmtime_r(&now, &date) == nullptr || std::strftime(month, sizeof(month), "%Y-%m", &date) == 0) {
    throw std::runtime_error("could not determine event archive month");
  }
  const std::string prefix = "events." + std::string(month);
  for (unsigned int sequence = 0; sequence < 1000000; ++sequence) {
    std::string suffix;
    if (sequence != 0) {
      suffix = std::to_string(sequence);
      suffix = "." + std::string(6 - suffix.size(), '0') + suffix;
    }
    const std::string archive = prefix + suffix + ".log";
    // linkat refuses to replace an existing archive, even across restarts.
    if (linkat(events, "events.log", events, archive.c_str(), 0) == 0) {
      if (fsync(events) != 0 || unlinkat(events, "events.log", 0) != 0 || fsync(events) != 0) {
        throw std::runtime_error("could not finish server event rotation");
      }
      return;
    }
    if (errno != EEXIST) throw std::runtime_error("could not archive server event log");
  }
  throw std::runtime_error("server event archive sequence exhausted");
}

void removeEntryAt(int parent, const std::string& name, bool dry_run, unsigned depth = 0) {
  struct stat status {};
  if (fstatat(parent, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) return;
    throw std::runtime_error("could not inspect project metadata for removal");
  }
  if (S_ISDIR(status.st_mode)) {
    if (depth > 64) throw std::runtime_error("project metadata exceeds maximum directory depth");
    bool missing = false;
    const Descriptor directory(openPrivateDirectoryAt(parent, name, &missing));
    if (missing) return;
    for (const auto& child : directoryNames(directory)) removeEntryAt(directory, child, dry_run, depth + 1);
    if (!dry_run && unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0) {
      throw std::runtime_error("could not remove project metadata directory");
    }
  } else if (!dry_run && unlinkat(parent, name.c_str(), 0) != 0) {
    // Symlinks themselves are unlinked; their destinations are never opened.
    throw std::runtime_error("could not remove project metadata entry");
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

std::optional<std::string> decodeCheckoutPathToken(std::string_view token) {
  return decodeCheckoutPath(token);
}

bool sameReportedCheckout(std::string_view left, std::string_view right) {
  const auto normalized = [](std::string_view value) {
    while (value.size() > 1 && value.back() == '/') {
      value.remove_suffix(1);
    }
    return value;
  };
  const std::string_view a = normalized(left);
  const std::string_view b = normalized(right);
  if (a == b) {
    return true;
  }
  const auto final_component = [](std::string_view value) {
    const std::size_t slash = value.rfind('/');
    return slash == std::string_view::npos ? value : value.substr(slash + 1);
  };
  const bool a_is_name = a.find('/') == std::string_view::npos;
  const bool b_is_name = b.find('/') == std::string_view::npos;
  return (a_is_name != b_is_name) && !a.empty() && !b.empty() && final_component(a) == final_component(b);
}

std::filesystem::path validatedMetadataRoot(const std::filesystem::path& requested_root) {
  if (requested_root.empty()) {
    throw std::invalid_argument("metadata state root is required");
  }
  const int descriptor = openPrivateDirectory(requested_root);
  close(descriptor);
  return requested_root;
}

bool forgetCheckout(const std::filesystem::path& state_root, std::string_view project_name,
                    std::string_view client_id) {
  if (!isValidProjectName(project_name) || !isValidClientId(client_id))
    throw std::invalid_argument("invalid checkout identity");
  const Descriptor root(openPrivateDirectory(validatedMetadataRoot(state_root)));
  lockMetadata(root, LOCK_EX);
  bool missing = false;
  const Descriptor checkouts(openPrivateDirectoryAt(root, "checkouts", &missing));
  if (missing) return false;
  const Descriptor project(openPrivateDirectoryAt(checkouts, project_name, &missing));
  if (missing) return false;
  const auto filename = std::string(client_id) + ".ini";
  struct stat status {};
  if (fstatat(project, filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) return false;
    throw std::runtime_error("could not inspect checkout report");
  }
  if (!S_ISREG(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0077) != 0)
    throw std::runtime_error("checkout report is not a private regular file owned by this user");
  if (unlinkat(project, filename.c_str(), 0) != 0 || fsync(project) != 0)
    throw std::runtime_error("could not remove checkout report");
  // Leave an empty metadata directory in place: another client may register
  // here immediately, and the lifecycle sweep owns project-level removal.
  return true;
}

void registerCheckout(const std::filesystem::path& state_root, std::string_view project_name,
                      std::string_view client_id, std::string_view encoded_path, bool replace) {
  if (!isValidProjectName(project_name) || !isValidClientId(client_id) ||
      !isValidCheckoutPathToken(encoded_path)) {
    throw std::invalid_argument("invalid checkout metadata");
  }
  std::string stored_path(encoded_path);
  if (!replace) {
    for (const auto& existing : loadCheckoutMetadata(state_root, project_name)) {
      const auto requested = decodeCheckoutPath(encoded_path);
      if (existing.client_id != client_id || !requested.has_value()) {
        continue;
      }
      if (!sameReportedCheckout(existing.reported_path, *requested)) {
        throw CheckoutConflict("this host already registered a different checkout for " +
                               std::string(project_name));
      }
      // A name-only report of the same folder must not erase a full path the
      // host reported earlier; the more specific form is kept.
      if (requested->find('/') == std::string::npos && existing.reported_path.find('/') != std::string::npos) {
        stored_path = encodeCheckoutPath(existing.reported_path);
      }
    }
  }
  int root = openPrivateDirectory(validatedMetadataRoot(state_root));
  lockMetadata(root, LOCK_EX);
  int checkouts = -1;
  int project = -1;
  int temporary = -1;
  const std::string client_filename = std::string(client_id) + ".ini";
  std::string temporary_name;
  try {
    checkouts = ensurePrivateDirectoryAt(root, "checkouts");
    project = ensurePrivateDirectoryAt(checkouts, project_name);
    const std::string record = "schema_version=1\nclient_id=" + std::string(client_id) + "\nproject=" +
        std::string(project_name) + "\npath_hex=" + stored_path + "\nlast_seen_epoch=" +
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

std::vector<RegisteredCheckout> loadClientCheckouts(const std::filesystem::path& state_root,
                                                    std::string_view client_id) {
  if (!isValidClientId(client_id)) {
    throw std::invalid_argument("invalid client for checkout metadata");
  }
  const int root = openPrivateDirectory(validatedMetadataRoot(state_root));
  int checkouts = -1;
  DIR* entries = nullptr;
  try {
    bool missing = false;
    checkouts = openPrivateDirectoryAt(root, "checkouts", &missing);
    if (missing) {
      close(root);
      return {};
    }
    const int scan_descriptor = dup(checkouts);
    if (scan_descriptor < 0 || (entries = fdopendir(scan_descriptor)) == nullptr) {
      if (scan_descriptor >= 0) {
        close(scan_descriptor);
      }
      throw std::runtime_error("could not scan checkout metadata");
    }
    std::vector<std::string> projects;
    while (dirent* entry = readdir(entries)) {
      const std::string_view name(entry->d_name);
      if (name == "." || name == "..") {
        continue;
      }
      if (!isValidProjectName(name)) {
        throw std::runtime_error("checkout metadata directory name is invalid");
      }
      projects.emplace_back(name);
    }
    closedir(entries);
    entries = nullptr;
    close(checkouts);
    checkouts = -1;
    close(root);
    std::sort(projects.begin(), projects.end());
    std::vector<RegisteredCheckout> records;
    for (const auto& project : projects) {
      for (const auto& checkout : loadCheckoutMetadata(state_root, project)) {
        if (checkout.client_id == client_id) {
          records.push_back(RegisteredCheckout{project, checkout.reported_path, checkout.last_seen_epoch_seconds});
        }
      }
    }
    return records;
  } catch (...) {
    if (entries != nullptr) {
      closedir(entries);
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
  const std::string record = serializeEvent({currentEpochSeconds(), std::string(kind),
                                             std::string(project_name), std::string(client_id)});
  if (record.size() > kMaximumEventLineBytes) {
    throw std::invalid_argument("server event is too large");
  }
  const Descriptor root(openPrivateDirectory(validatedMetadataRoot(state_root)));
  lockMetadata(root, LOCK_EX);
  const Descriptor events(ensurePrivateDirectoryAt(root, "events"));
  int log = openat(events, "events.log", O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
                   kPrivateFileMode);
  if (log < 0) throw std::runtime_error("could not open server event log");
  try {
    verifyPrivateRegularFile(log, kMaximumEventLogBytes, "server event log");
    struct stat status {};
    if (fstat(log, &status) != 0) throw std::runtime_error("could not inspect server event log");
    if (static_cast<std::size_t>(status.st_size) + record.size() > kMaximumEventLogBytes) {
      close(log);
      log = -1;
      rotateEvents(events);
      log = openat(events, "events.log", O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    kPrivateFileMode);
      if (log < 0) throw std::runtime_error("could not create rotated server event log");
    }
    writeAll(log, record);
    if (fsync(log) != 0 || fsync(events) != 0) throw std::runtime_error("could not sync server event log");
    close(log);
  } catch (...) {
    if (log >= 0) close(log);
    throw;
  }
}

std::vector<StateEvent> loadProjectEvents(const std::filesystem::path& state_root,
                                          std::string_view project_name, std::size_t maximum) {
  if (!isValidProjectName(project_name) || maximum > 128) {
    throw std::invalid_argument("invalid project event request");
  }
  if (maximum == 0) return {};
  const Descriptor root(openPrivateDirectory(validatedMetadataRoot(state_root)));
  lockMetadata(root, LOCK_SH);
  bool missing = false;
  const Descriptor events(openPrivateDirectoryAt(root, "events", &missing));
  if (missing) return {};
  auto names = eventLogNames(events);
  if (names.size() > 2) names.erase(names.begin(), names.end() - 2);
  std::vector<StateEvent> result;
  for (const auto& name : names) {
    for (auto& event : parseEvents(readPrivateFileAt(events, name, kMaximumEventLogBytes, "server event log"))) {
      if (event.project_name == project_name) result.push_back(std::move(event));
    }
  }
  if (result.size() > maximum) result.erase(result.begin(), result.end() - static_cast<std::ptrdiff_t>(maximum));
  return result;
}

std::vector<std::string> listProjectMetadataNames(const std::filesystem::path& state_root) {
  const Descriptor root(openPrivateDirectory(validatedMetadataRoot(state_root)));
  lockMetadata(root, LOCK_SH);
  std::set<std::string> names;
  for (const auto directory_name : {"checkouts", "projects"}) {
    bool missing = false;
    const Descriptor directory(openPrivateDirectoryAt(root, directory_name, &missing));
    if (missing) continue;
    for (const auto& name : directoryNames(directory)) {
      if (!isValidProjectName(name)) throw std::runtime_error("project metadata directory name is invalid");
      names.insert(name);
    }
  }
  bool missing = false;
  const Descriptor events(openPrivateDirectoryAt(root, "events", &missing));
  if (!missing) {
    for (const auto& name : eventLogNames(events)) {
      for (const auto& event : parseEvents(readPrivateFileAt(events, name, kMaximumEventLogBytes, "server event log"))) {
        names.insert(event.project_name);
      }
    }
  }
  return {names.begin(), names.end()};
}

void removeProjectMetadata(const std::filesystem::path& state_root,
                           std::string_view project_name, bool dry_run) {
  if (!isValidProjectName(project_name)) throw std::invalid_argument("invalid project name for metadata removal");
  const Descriptor root(openPrivateDirectory(validatedMetadataRoot(state_root)));
  lockMetadata(root, dry_run ? LOCK_SH : LOCK_EX);
  bool missing = false;
  const Descriptor events(openPrivateDirectoryAt(root, "events", &missing));
  std::vector<std::string> replacements;
  if (!missing) {
    // Validate every archive before making any destructive change.
    for (const auto& name : eventLogNames(events)) {
      const auto content = readPrivateFileAt(events, name, kMaximumEventLogBytes, "server event log");
      bool found = false;
      for (const auto& event : parseEvents(content)) {
        if (event.project_name == project_name) found = true;
      }
      if (found) replacements.push_back(name);
    }
  }
  for (const auto directory_name : {"checkouts", "projects"}) {
    bool absent = false;
    const Descriptor directory(openPrivateDirectoryAt(root, directory_name, &absent));
    if (absent) continue;
    removeEntryAt(directory, std::string(project_name), true);
  }
  if (dry_run) return;
  for (const auto directory_name : {"checkouts", "projects"}) {
    bool absent = false;
    const Descriptor directory(openPrivateDirectoryAt(root, directory_name, &absent));
    if (absent) continue;
    removeEntryAt(directory, std::string(project_name), false);
    if (fsync(directory) != 0) throw std::runtime_error("could not sync removed project metadata");
  }
  for (const auto& name : replacements) {
    std::string retained;
    for (const auto& event : parseEvents(readPrivateFileAt(events, name, kMaximumEventLogBytes, "server event log"))) {
      if (event.project_name != project_name) retained += serializeEvent(event);
    }
    const Descriptor log(openat(events, name.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
    if (log < 0) throw std::runtime_error("could not open server event log for cleanup");
    verifyPrivateRegularFile(log, kMaximumEventLogBytes, "server event log");
    // All readers and writers share the state-directory lock, so an in-place
    // rewrite is never observed partially and needs no temporary file.
    writeAll(log, retained);
    if (ftruncate(log, static_cast<off_t>(retained.size())) != 0 || fsync(log) != 0) {
      throw std::runtime_error("could not scrub project events");
    }
  }
  if (events >= 0 && fsync(events) != 0) throw std::runtime_error("could not sync project event cleanup");
  if (fsync(root) != 0) throw std::runtime_error("could not sync project metadata cleanup");
}

}  // namespace ckgit
