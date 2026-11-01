// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/pages_store.hpp"
#include "ckgit/tree_file.hpp"
#include <vector>
namespace ckgit {
namespace fs = std::filesystem;
namespace {
// A single request path component that is safe to resolve within a site.
bool safeComponent(std::string_view component) {
  if (component.empty() || component == "." || component == "..") return false;
  for (const unsigned char byte : component) {
    if (byte < 0x20 || byte == 0x7f || byte == '/' || byte == '\\') return false;
  }
  return true;
}

}
std::string pagesContentType(std::string_view name) {
  const auto dot = name.rfind('.');
  const std::string_view ext = dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
  if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
  if (ext == "css") return "text/css; charset=utf-8";
  if (ext == "js" || ext == "mjs") return "text/javascript; charset=utf-8";
  if (ext == "json" || ext == "map") return "application/json";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gif") return "image/gif";
  if (ext == "webp") return "image/webp";
  if (ext == "ico") return "image/x-icon";
  if (ext == "txt" || ext == "md") return "text/plain; charset=utf-8";
  if (ext == "xml") return "application/xml";
  if (ext == "pdf") return "application/pdf";
  if (ext == "wasm") return "application/wasm";
  if (ext == "woff2") return "font/woff2";
  if (ext == "woff") return "font/woff";
  if (ext == "ttf") return "font/ttf";
  return "application/octet-stream";
}

std::optional<PageFile> readSiteFile(const fs::path& site, std::string_view path, std::size_t cap) {
  // Split the request path into components; an empty path or a trailing slash
  // resolves to index.html.
  std::vector<std::string> components;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string_view part =
        path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (!part.empty()) {
      if (!safeComponent(part)) return std::nullopt;
      components.emplace_back(part);
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  if (components.empty() || (!path.empty() && path.back() == '/')) components.emplace_back("index.html");

  const auto file = platform::readTreeFile(site, components, cap, true, false);
  if (file.status != platform::TreeFileStatus::Ok) return std::nullopt;
  return PageFile{file.content, pagesContentType(components.back())};
}

namespace {

int hexNibble(unsigned char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

}  // namespace

std::optional<std::string> decodeRequestPath(std::string_view target) {
  std::string out;
  for (std::size_t index = 0; index < target.size(); ++index) {
    if (target[index] == '%') {
      if (index + 2 >= target.size()) return std::nullopt;
      const int high = hexNibble(static_cast<unsigned char>(target[index + 1]));
      const int low = hexNibble(static_cast<unsigned char>(target[index + 2]));
      if (high < 0 || low < 0) return std::nullopt;
      const unsigned char byte = static_cast<unsigned char>((high << 4) | low);
      if (byte < 0x20 || byte == 0x7f) return std::nullopt;
      out.push_back(static_cast<char>(byte));
      index += 2;
    } else {
      out.push_back(target[index]);
    }
  }
  return out;
}

}
