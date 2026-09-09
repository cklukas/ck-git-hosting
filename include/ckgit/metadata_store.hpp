// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ckgit {

struct CheckoutMetadata {
  std::string client_id;
  std::string reported_path;
  std::uint64_t last_seen_epoch_seconds{};
};

struct StateEvent {
  std::uint64_t epoch_seconds{};
  std::string kind;
  std::string project_name;
  std::string client_id;
};

struct RegisteredCheckout {
  std::string project_name;
  std::string reported_path;
  std::uint64_t last_seen_epoch_seconds{};
};

// Thrown when a host already registered a different checkout for a project
// and the caller did not ask to replace it.
class CheckoutConflict : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Encodes a bounded, printable UTF-8 path for the fixed control-RPC token.
// The encoded form is deliberately not a shell or INI value.
std::string encodeCheckoutPath(std::string_view path);
bool isValidCheckoutPathToken(std::string_view token);
std::optional<std::string> decodeCheckoutPathToken(std::string_view token);

// Two reported checkouts name the same folder when they are equal, or when
// one is a bare folder name (basename privacy mode) that matches the final
// component of the other.  Switching the privacy mode therefore never turns a
// host's own main checkout into a conflict.
bool sameReportedCheckout(std::string_view left, std::string_view right);

// State roots are pre-created administrative directories. They must be
// private, owned by the daemon user, and free of symlinks.
std::filesystem::path validatedMetadataRoot(const std::filesystem::path& root);

// Writes one client/project registration through a same-directory staged,
// fsynced, atomic write. `encoded_path` must pass isValidCheckoutPathToken().
// A host keeps one main checkout per project: when a record with a different
// path exists, the call throws CheckoutConflict unless `replace` is set.
void registerCheckout(const std::filesystem::path& state_root,
                      std::string_view project_name,
                      std::string_view client_id,
                      std::string_view encoded_path,
                      bool replace = false);

// Removes only the authenticated client's report, idempotently. No checkout or
// repository data is touched, and missing metadata is never created.
bool forgetCheckout(const std::filesystem::path& state_root,
                    std::string_view project_name, std::string_view client_id);

// Every project this client registered, sorted by project name.  Only the
// requesting client's own records are read, so one host never learns
// another host's paths.
std::vector<RegisteredCheckout> loadClientCheckouts(const std::filesystem::path& state_root,
                                                    std::string_view client_id);

// Reads all strict v1 checkout records for a project. A missing checkout
// directory means no registrations; malformed state fails closed.
std::vector<CheckoutMetadata> loadCheckoutMetadata(const std::filesystem::path& state_root,
                                                    std::string_view project_name);

// Appends a small, fixed-schema server event under a cross-process lock.
// At 64 KiB the log is archived without replacing prior rotations.
// Events never include a checkout path or remote URL.
void appendStateEvent(const std::filesystem::path& state_root,
                      std::string_view kind,
                      std::string_view project_name,
                      std::string_view client_id);

// Returns up to `maximum` newest strict event records for a project. A missing
// event log means no events; malformed event state fails closed. Reads only
// the newest two logs, in chronological order.
std::vector<StateEvent> loadProjectEvents(const std::filesystem::path& state_root,
                                          std::string_view project_name,
                                          std::size_t maximum = 16);

// Administrative cleanup: remove a project's checkout/derived directories
// without following symlinks, and scrub its events from every log archive.
// A dry run validates the operation without writing anything.
void removeProjectMetadata(const std::filesystem::path& state_root,
                           std::string_view project_name, bool dry_run = false);

// Includes project names appearing only in archived events, so an orphan
// sweep also removes their historical metadata.
std::vector<std::string> listProjectMetadataNames(const std::filesystem::path& state_root);

}  // namespace ckgit
