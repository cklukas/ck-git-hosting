// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
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

// Encodes a bounded, printable UTF-8 path for the fixed control-RPC token.
// The encoded form is deliberately not a shell or INI value.
std::string encodeCheckoutPath(std::string_view path);
bool isValidCheckoutPathToken(std::string_view token);

// State roots are pre-created administrative directories. They must be
// private, owned by the daemon user, and free of symlinks.
std::filesystem::path validatedMetadataRoot(const std::filesystem::path& root);

// Replaces one client/project registration through a same-directory staged,
// fsynced, atomic write. `encoded_path` must pass isValidCheckoutPathToken().
void registerCheckout(const std::filesystem::path& state_root,
                      std::string_view project_name,
                      std::string_view client_id,
                      std::string_view encoded_path);

// Reads all strict v1 checkout records for a project. A missing checkout
// directory means no registrations; malformed state fails closed.
std::vector<CheckoutMetadata> loadCheckoutMetadata(const std::filesystem::path& state_root,
                                                    std::string_view project_name);

// Appends a small, fixed-schema server event with a single fsynced O_APPEND
// write. Events never include a checkout path or remote URL.
void appendStateEvent(const std::filesystem::path& state_root,
                      std::string_view kind,
                      std::string_view project_name,
                      std::string_view client_id);

// Returns up to `maximum` newest strict event records for a project. A missing
// event log means no events; malformed event state fails closed.
std::vector<StateEvent> loadProjectEvents(const std::filesystem::path& state_root,
                                          std::string_view project_name,
                                          std::size_t maximum = 16);

}  // namespace ckgit
