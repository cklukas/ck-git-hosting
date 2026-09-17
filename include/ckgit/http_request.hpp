// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ckgit {

enum class HttpMethod { kGet, kHead, kPost };

struct HttpRequest {
  HttpMethod method;
  std::string target;
  // The Origin header value, or empty when absent. The dashboard is otherwise
  // read-only; the single mutating route (CI cancel) uses this to refuse a
  // cross-origin POST, since it carries no credentials to protect otherwise.
  std::string origin;
  // The Last-Event-ID header, or empty. An SSE client sends it on reconnect so
  // the log-stream endpoint can resume tailing from that byte offset instead of
  // replaying the whole log.
  std::string last_event_id;
};

// Parses the complete header block for the deliberately small, one-request
// HTTP/1.1 surface. Only a canonical, bodyless GET, HEAD, or POST request is
// valid (a POST must still declare Content-Length: 0 — the surface carries no
// request bodies); callers pass exactly one header block, including its final
// CRLFCRLF.
std::optional<HttpRequest> parseReadOnlyHttpRequest(std::string_view request);

}  // namespace ckgit
