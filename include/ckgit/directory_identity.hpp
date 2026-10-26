// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <memory>
namespace ckgit {
// Holds the opened directory identity while Git transfers run. A later path
// lookup must still name this directory, not a replacement or reparse point.
class DirectoryIdentity {
 public:
  explicit DirectoryIdentity(const std::filesystem::path& path);
  ~DirectoryIdentity();
  DirectoryIdentity(const DirectoryIdentity&) = delete;
  DirectoryIdentity& operator=(const DirectoryIdentity&) = delete;
  bool matches(const std::filesystem::path& path) const;
 private:
  struct State;
  std::unique_ptr<State> state_;
};
}
