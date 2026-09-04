// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ckgit {

enum class RemoteTransport { kScpLikeSsh, kSshUrl, kOther };

struct RemoteUrl {
  std::string remote_name;
  std::string raw_url;
  std::string display_url;
  RemoteTransport transport{RemoteTransport::kOther};
  std::string user;
  std::string host;
  std::optional<unsigned short> port;
  std::string path;
};

// Parses common Git SSH URL forms without treating a merely similar URL as a
// paired server.  Non-SSH URLs are retained as kOther for inventory display.
RemoteUrl parseRemoteUrl(std::string remote_name, std::string raw_url);

// Removes a URL password before output reaches a terminal, log, or dashboard.
std::string redactRemoteUrl(std::string_view raw_url);

}  // namespace ckgit
