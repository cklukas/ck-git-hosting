// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/remote_url.hpp"

#include <charconv>
#include <cctype>

namespace ckgit {
namespace {

bool containsWhitespaceOrControl(std::string_view value) {
  for (const unsigned char character : value) {
    if (std::iscntrl(character) != 0 || std::isspace(character) != 0) {
      return true;
    }
  }
  return false;
}

bool parsePort(std::string_view value, std::optional<unsigned short>* port) {
  if (value.empty()) {
    return false;
  }
  unsigned int parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 ||
      parsed > 65535) {
    return false;
  }
  *port = static_cast<unsigned short>(parsed);
  return true;
}

void parseUserHost(std::string_view authority, RemoteUrl* result) {
  const auto at = authority.rfind('@');
  std::string_view host_port = authority;
  if (at != std::string_view::npos) {
    result->user = std::string(authority.substr(0, at));
    host_port = authority.substr(at + 1);
  }
  if (host_port.empty() || containsWhitespaceOrControl(host_port)) {
    return;
  }
  if (host_port.front() == '[') {
    const auto close = host_port.find(']');
    if (close == std::string_view::npos || close == 1) {
      return;
    }
    result->host = std::string(host_port.substr(1, close - 1));
    if (close + 1 < host_port.size()) {
      if (host_port[close + 1] != ':' ||
          !parsePort(host_port.substr(close + 2), &result->port)) {
        result->host.clear();
      }
    }
    return;
  }
  const auto colon = host_port.rfind(':');
  if (colon != std::string_view::npos) {
    if (!parsePort(host_port.substr(colon + 1), &result->port)) {
      return;
    }
    host_port = host_port.substr(0, colon);
  }
  if (!host_port.empty()) {
    result->host = std::string(host_port);
  }
}

}  // namespace

std::string redactRemoteUrl(std::string_view raw_url) {
  // URI-style userinfo is the only Git remote form that can include a password.
  const auto scheme = raw_url.find("://");
  if (scheme == std::string_view::npos) {
    return std::string(raw_url);
  }
  const auto authority_start = scheme + 3;
  const auto authority_end = raw_url.find_first_of("/?#", authority_start);
  const auto at = raw_url.find('@', authority_start);
  if (at == std::string_view::npos ||
      (authority_end != std::string_view::npos && at > authority_end)) {
    return std::string(raw_url);
  }
  const auto colon = raw_url.find(':', authority_start);
  if (colon == std::string_view::npos || colon > at) {
    return std::string(raw_url);
  }
  std::string redacted(raw_url.substr(0, colon + 1));
  redacted += "***";
  redacted.append(raw_url.substr(at));
  return redacted;
}

RemoteUrl parseRemoteUrl(std::string remote_name, std::string raw_url) {
  RemoteUrl result;
  result.remote_name = std::move(remote_name);
  result.raw_url = std::move(raw_url);
  result.display_url = redactRemoteUrl(result.raw_url);

  constexpr std::string_view kSshPrefix{"ssh://"};
  if (result.raw_url.rfind(kSshPrefix, 0) == 0) {
    const std::string_view remainder =
        std::string_view(result.raw_url).substr(kSshPrefix.size());
    const auto slash = remainder.find('/');
    if (slash == std::string_view::npos || slash + 1 == remainder.size()) {
      return result;
    }
    parseUserHost(remainder.substr(0, slash), &result);
    if (result.host.empty() || containsWhitespaceOrControl(result.user)) {
      result.user.clear();
      result.port.reset();
      return result;
    }
    result.transport = RemoteTransport::kSshUrl;
    result.path = std::string(remainder.substr(slash + 1));
    return result;
  }

  const std::string_view raw{result.raw_url};
  const auto colon = raw.find(':');
  const auto slash = raw.find('/');
  if (colon == std::string_view::npos || (slash != std::string_view::npos && slash < colon) ||
      colon == 0 || colon + 1 == raw.size() || containsWhitespaceOrControl(raw)) {
    return result;
  }
  const std::string_view authority = raw.substr(0, colon);
  // SCP-like syntax needs a host (and normally user@host); a colon alone must
  // not turn a local path or Windows-like path into a server identity.
  if (authority.find('@') == std::string_view::npos || authority.find('[') != std::string_view::npos ||
      authority.find(']') != std::string_view::npos) {
    return result;
  }
  parseUserHost(authority, &result);
  if (result.host.empty() || result.port.has_value()) {
    result.user.clear();
    result.host.clear();
    result.port.reset();
    return result;
  }
  result.transport = RemoteTransport::kScpLikeSsh;
  result.path = std::string(raw.substr(colon + 1));
  return result;
}

}  // namespace ckgit
