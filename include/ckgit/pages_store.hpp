// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ckgit {

// On-disk store for published static sites ("Pages"). Unlike the CI metadata
// store, this tree holds arbitrary build output (any file mode, possibly large),
// so it lives OUTSIDE the private state root, under its own pages_root, and is
// served by the separate ck-pagesd process on its own origin.
//
//   <pages_root>/<project>/versions/<run-id>/...   one published site
//   <pages_root>/<project>/current                 a pointer file: the live run id
//
// Publishing copies a build's site directory into a fresh version, then writes
// the `current` pointer atomically, so a partial publish never replaces a live
// site. Older versions are pruned for rollback headroom.

// Generous ceilings so a project can host large artifacts -- for example a
// multi-hundred-megabyte reference-handbook PDF -- without a publish silently
// failing. Publishing copies files to disk and the pages server reads a whole
// file into memory to serve it, so these bound disk use and per-request memory.
inline constexpr std::size_t kMaximumPagesSiteBytes = static_cast<std::size_t>(8) << 30;   // 8 GiB total site
inline constexpr std::size_t kMaximumPagesFileBytes = static_cast<std::size_t>(1) << 30;    // 1 GiB per file
inline constexpr std::size_t kMaximumPagesEntries = 100000;

// A resolved page ready to serve: its bytes and a content type chosen by
// extension.
struct PageFile {
  std::string content;
  std::string content_type;
};

// Copies `source` (a directory in the build scratch) into
// <pages_root>/<project>/versions/<run_id>, makes it the current site, and prunes
// versions beyond `keep_versions` (never the current one). Symlinks and special
// files in the source are skipped. Throws std::runtime_error on a bad argument or
// if the site exceeds kMaximumPagesSiteBytes / kMaximumPagesEntries.
void publishPagesSite(const std::filesystem::path& pages_root, std::string_view project,
                      std::string_view run_id, const std::filesystem::path& source,
                      std::size_t keep_versions);

// Serves a request path "<rel...>" from <project>'s current site, resolved
// traversal-safe (each component opened O_NOFOLLOW). An empty path or one ending
// in '/' maps to index.html. std::nullopt when the project, the current version,
// or the file is absent, or the request escapes the site. `cap` bounds the file
// read into memory.
std::optional<PageFile> readCurrentPage(const std::filesystem::path& pages_root,
                                        std::string_view project, std::string_view path,
                                        std::size_t cap);

// The traversal-safe half of readCurrentPage, taking the site directory
// directly instead of a <pages_root>/<project>/current lookup: an already
// percent-decoded request path, resolved the identical way (empty or a
// trailing '/' maps to "index.html"; each component opened O_NOFOLLOW, so a
// site file can never redirect the read outside `site`), capped at `cap`
// bytes. Shared by readCurrentPage and by `ckdocs serve`'s own loopback
// preview server, so a reader sees identical behaviour serving from a
// published Pages site or from a plain build directory.
std::optional<PageFile> readSiteFile(const std::filesystem::path& site, std::string_view path, std::size_t cap);

// Percent-decodes an HTTP request target's path (the caller has already
// split off any query string or fragment): %XX escapes only, rejecting a
// malformed escape or one that would decode to a control byte. Shared by
// ck-pagesd and `ckdocs serve`, so a fix to one applies to both.
std::optional<std::string> decodeRequestPath(std::string_view target);

// The run id a project's site currently points at, for tests and status.
std::optional<std::string> currentPagesVersion(const std::filesystem::path& pages_root,
                                               std::string_view project);

// Removes a project's whole pages tree (used when the project is deleted).
void removeProjectPages(const std::filesystem::path& pages_root, std::string_view project);

// The content type for a file name, by extension; a generic type when unknown.
std::string pagesContentType(std::string_view name);

}  // namespace ckgit
