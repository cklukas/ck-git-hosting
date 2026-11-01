// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/pages_store.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "ckgit/ci_store.hpp"       // isValidCiId
#include "ckgit/validation.hpp"     // isValidProjectName

namespace ckgit {
namespace {

namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("pages store: " + message); }

// Reads the `current` pointer file (a bare run id) for a project.
std::optional<std::string> readCurrent(const fs::path& base) {
  std::error_code error;
  const fs::path pointer = base / "current";
  const auto size = fs::file_size(pointer, error);
  if (error || size == 0 || size > 128) return std::nullopt;
  std::ifstream in(pointer, std::ios::binary);
  if (!in) return std::nullopt;
  std::string id((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) id.pop_back();
  if (!isValidCiId(id)) return std::nullopt;
  return id;
}

}  // namespace

void publishPagesSite(const fs::path& pages_root, std::string_view project, std::string_view run_id,
                      const fs::path& source, std::size_t keep_versions) {
  if (!isValidProjectName(project)) fail("invalid project name for a site");
  if (!isValidCiId(run_id)) fail("invalid run id for a site");
  std::error_code error;
  if (!fs::is_directory(source, error)) fail("the site source is not a directory");

  const fs::path base = pages_root / std::string(project);
  const fs::path versions = base / "versions";
  fs::create_directories(versions, error);
  if (error) fail("could not create the pages directory");
  const fs::path dest = versions / std::string(run_id);
  fs::remove_all(dest, error);
  fs::create_directories(dest, error);
  if (error) fail("could not create the site version directory");

  // Copy regular files and directories only; skip symlinks and special files so
  // a site can never carry a link that escapes it. Bound the total size/count.
  std::size_t total_bytes = 0;
  std::size_t entries = 0;
  for (const auto& entry : fs::recursive_directory_iterator(source, fs::directory_options::none, error)) {
    if (error) fail("could not read the site source");
    if (++entries > kMaximumPagesEntries) {
      fs::remove_all(dest, error);
      fail("the site has too many files");
    }
    const fs::path relative = fs::relative(entry.path(), source, error);
    if (error || relative.empty()) continue;
    const fs::path target = dest / relative;
    const auto status = entry.symlink_status();
    if (fs::is_directory(status)) {
      fs::create_directories(target, error);
    } else if (fs::is_regular_file(status)) {
      const auto size = entry.file_size(error);
      if (error) continue;
      total_bytes += static_cast<std::size_t>(size);
      if (total_bytes > kMaximumPagesSiteBytes) {
        fs::remove_all(dest, error);
        fail("the site exceeds the size limit");
      }
      fs::create_directories(target.parent_path(), error);
      // Not fs::copy_file: its libstdc++ implementation always creates the
      // destination at a deliberately restrictive intermediate mode (no
      // owner-read bit) before widening it once the data is written. Some
      // FUSE-backed mounts (observed with virtiofs) deny creating a file
      // with no read bit at all, even to its owner, which makes that
      // implementation detail fail unconditionally on such a mount. A plain
      // ofstream creates the file at an ordinary, readable mode instead.
      std::ifstream in(entry.path(), std::ios::binary);
      std::ofstream out(target, std::ios::binary | std::ios::trunc);
      if (size > 0) out << in.rdbuf();
      if (!in || !out) {
        fs::remove_all(dest, error);
        fail("could not copy a site file");
      }
    }
    // symlinks and special files are intentionally skipped
  }

  // Atomically point `current` at the new version.
  std::random_device device;
  const fs::path staging = base / (".current-" + std::to_string(::getpid()) + "-" + std::to_string(device()));
  {
    std::ofstream out(staging, std::ios::binary | std::ios::trunc);
    if (!out) {
      fs::remove_all(dest, error);
      fail("could not stage the current pointer");
    }
    out << std::string(run_id) << "\n";
  }
  fs::rename(staging, base / "current", error);
  if (error) {
    fs::remove(staging, error);
    fs::remove_all(dest, error);
    fail("could not update the current pointer");
  }

  // Prune old versions (newest run ids sort last), never removing current.
  if (keep_versions > 0) {
    std::vector<std::string> ids;
    for (const auto& entry : fs::directory_iterator(versions, error)) {
      const std::string id = entry.path().filename().string();
      if (entry.is_directory(error) && isValidCiId(id)) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end(), std::greater<>());
    for (std::size_t index = keep_versions; index < ids.size(); ++index) {
      if (ids[index] == run_id) continue;  // never the just-published current
      fs::remove_all(versions / ids[index], error);
    }
  }
}

std::optional<std::string> currentPagesVersion(const fs::path& pages_root, std::string_view project) {
  if (!isValidProjectName(project)) return std::nullopt;
  return readCurrent(pages_root / std::string(project));
}

std::optional<PageFile> readCurrentPage(const fs::path& pages_root, std::string_view project,
                                        std::string_view path, std::size_t cap) {
  if (!isValidProjectName(project)) return std::nullopt;
  const fs::path base = pages_root / std::string(project);
  const std::optional<std::string> version = readCurrent(base);
  if (!version.has_value()) return std::nullopt;
  return readSiteFile(base / "versions" / *version, path, cap);
}

void removeProjectPages(const fs::path& pages_root, std::string_view project) {
  if (!isValidProjectName(project)) fail("invalid project name for pages removal");
  std::error_code error;
  fs::remove_all(pages_root / std::string(project), error);
}

}  // namespace ckgit
