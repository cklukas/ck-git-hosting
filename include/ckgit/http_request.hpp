// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ckgit {

enum class HttpMethod { kGet, kHead };

struct HttpRequest {
  HttpMethod method;
  std::string target;
};

// Parses the complete header block for the deliberately small, one-request
// HTTP/1.1 surface. Only a canonical, bodyless GET or HEAD request is valid;
// callers must pass exactly one header block, including its final CRLFCRLF.
std::optional<HttpRequest> parseReadOnlyHttpRequest(std::string_view request);

}  // namespace ckgit
