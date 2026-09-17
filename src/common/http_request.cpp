// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/http_request.hpp"

#include "ckgit/http_router.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>

namespace ckgit {
namespace {

constexpr std::size_t kMaximumRequestBytes = 16 * 1024;
constexpr std::size_t kMaximumRequestLineBytes = kMaximumRouteBytes + 32;
constexpr std::size_t kMaximumHeaderCount = 32;

bool isTokenCharacter(unsigned char character) {
  return (character >= '0' && character <= '9') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z') ||
         character == '!' || character == '#' || character == '$' || character == '%' ||
         character == '&' || character == '\'' || character == '*' || character == '+' ||
         character == '-' || character == '.' || character == '^' || character == '_' ||
         character == '`' || character == '|' || character == '~';
}

bool equalsIgnoreCase(std::string_view left, std::string_view right) {
  return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
      [](unsigned char lhs, unsigned char rhs) {
        const auto lower = [](unsigned char character) {
          return character >= 'A' && character <= 'Z'
                     ? static_cast<unsigned char>(character - 'A' + 'a')
                     : character;
        };
        return lower(lhs) == lower(rhs);
      });
}

bool isSafeHeaderValue(std::string_view value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return character == '\t' || (character >= 0x20 && character != 0x7f);
  });
}

std::string_view trimOptionalWhitespace(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

bool isSafeTarget(std::string_view target) {
  return !target.empty() && target.size() <= kMaximumRouteBytes && target.front() == '/' &&
         std::all_of(target.begin(), target.end(), [](unsigned char character) {
           return character >= 0x21 && character <= 0x7e && character != '\\' &&
                  character != '#' && character != '?';
         }) && (target.find('%') == std::string_view::npos ||
                 parseHttpRoute(target).kind != RouteKind::kNotFound);
}

}  // namespace

std::optional<HttpRequest> parseReadOnlyHttpRequest(std::string_view request) {
  if (request.size() < 4 || request.size() > kMaximumRequestBytes ||
      request.substr(request.size() - 4) != "\r\n\r\n" ||
      request.find("\r\n\r\n") != request.size() - 4) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < request.size(); ++index) {
    const unsigned char character = request[index];
    if (character == '\r') {
      if (index + 1 >= request.size() || request[index + 1] != '\n') {
        return std::nullopt;
      }
    } else if (character == '\n' && (index == 0 || request[index - 1] != '\r')) {
      return std::nullopt;
    } else if (character < 0x20 && character != '\r' && character != '\n' && character != '\t') {
      return std::nullopt;
    } else if (character == 0x7f) {
      return std::nullopt;
    }
  }

  const std::size_t request_line_end = request.find("\r\n");
  if (request_line_end == std::string_view::npos || request_line_end > kMaximumRequestLineBytes) {
    return std::nullopt;
  }
  const std::string_view request_line = request.substr(0, request_line_end);
  const std::size_t first_space = request_line.find(' ');
  const std::size_t second_space = first_space == std::string_view::npos ? std::string_view::npos :
                                   request_line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
      request_line.find(' ', second_space + 1) != std::string_view::npos ||
      request_line.substr(second_space + 1) != "HTTP/1.1") {
    return std::nullopt;
  }
  const std::string_view method_text = request_line.substr(0, first_space);
  std::string_view target = request_line.substr(first_space + 1, second_space - first_space - 1);
  // A read-only server routes and serves by path alone; drop any ?query or
  // #fragment. Static sites request assets with a cache-busting query
  // (theme.css?digest=..., pygments.css?v=...), so rejecting those would leave
  // every generated site unstyled. Stripping here keeps both the dashboard
  // router and the pages server working on the path.
  const std::size_t query_or_fragment = target.find_first_of("?#");
  if (query_or_fragment != std::string_view::npos) target = target.substr(0, query_or_fragment);
  if (!isSafeTarget(target)) {
    return std::nullopt;
  }
  const std::optional<HttpMethod> method = method_text == "GET" ? std::optional<HttpMethod>(HttpMethod::kGet)
                                       : method_text == "HEAD" ? std::optional<HttpMethod>(HttpMethod::kHead)
                                       : method_text == "POST" ? std::optional<HttpMethod>(HttpMethod::kPost)
                                                               : std::nullopt;
  if (!method.has_value()) {
    return std::nullopt;
  }

  bool host_seen = false;
  bool content_length_seen = false;
  std::string origin;
  std::string last_event_id;
  std::string host;
  std::size_t header_count = 0;
  for (std::size_t position = request_line_end + 2; position < request.size() - 2;) {
    const std::size_t line_end = request.find("\r\n", position);
    if (line_end == std::string_view::npos || line_end == position) {
      return std::nullopt;
    }
    if (++header_count > kMaximumHeaderCount) {
      return std::nullopt;
    }
    const std::string_view line = request.substr(position, line_end - position);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0 ||
        !std::all_of(line.begin(), line.begin() + static_cast<std::ptrdiff_t>(colon), isTokenCharacter)) {
      return std::nullopt;
    }
    const std::string_view name = line.substr(0, colon);
    const std::string_view value = trimOptionalWhitespace(line.substr(colon + 1));
    if (!isSafeHeaderValue(value)) {
      return std::nullopt;
    }
    if (equalsIgnoreCase(name, "host")) {
      if (host_seen || value.empty()) {
        return std::nullopt;
      }
      host_seen = true;
      host = std::string(value);
    } else if (equalsIgnoreCase(name, "transfer-encoding")) {
      return std::nullopt;
    } else if (equalsIgnoreCase(name, "content-length")) {
      if (content_length_seen || value.empty()) {
        return std::nullopt;
      }
      std::size_t length = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), length);
      if (error != std::errc{} || end != value.data() + value.size() || length != 0) {
        return std::nullopt;
      }
      content_length_seen = true;
    } else if (equalsIgnoreCase(name, "origin")) {
      origin = std::string(value);
    } else if (equalsIgnoreCase(name, "last-event-id")) {
      last_event_id = std::string(value);
    }
    position = line_end + 2;
  }
  if (!host_seen) {
    return std::nullopt;
  }
  return HttpRequest{*method, std::string(target), origin, last_event_id, host};
}

}  // namespace ckgit
