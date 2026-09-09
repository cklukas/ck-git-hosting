// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/http_router.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const auto route = ckgit::parseHttpRoute(input);
  if (route.kind == ckgit::RouteKind::kRaw && !ckgit::isObjectId(route.ref)) std::abort();
  const auto decoded = ckgit::decodePathSegment(input);
  if (decoded && ckgit::encodePathSegment(*decoded) != input) std::abort();
  return 0;
}
