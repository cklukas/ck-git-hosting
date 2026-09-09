// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/http_router.hpp"

#include <algorithm>
#include <charconv>

#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

bool unreserved(unsigned char byte) {
  return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
         (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
         byte == '.' || byte == '~' || byte == '/';
}

int hexDigit(unsigned char byte) {
  if (byte >= '0' && byte <= '9') return byte - '0';
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
  return -1;
}

bool validComponents(std::string_view value, bool allow_empty) {
  if (value.empty()) return allow_empty;
  std::size_t depth = 0;
  for (std::size_t start = 0; start <= value.size();) {
    const auto slash = value.find('/', start);
    const auto component = value.substr(start, slash == std::string_view::npos ?
        value.size() - start : slash - start);
    if (component.empty() || component == "." || component == ".." ||
        component.size() > kMaximumRouteComponentBytes || ++depth > kMaximumRoutePathDepth) {
      return false;
    }
    if (slash == std::string_view::npos) return true;
    start = slash + 1;
  }
  return false;
}

bool validRef(std::string_view value) {
  if (isObjectId(value)) return true;
  if (value.starts_with("heads/")) value.remove_prefix(6);
  else if (value.starts_with("tags/")) value.remove_prefix(5);
  if (!isValidBranchName(value) || value == "@" || !validComponents(value, false)) return false;
  for (std::size_t position = 0; position < value.size(); ++position) {
    if (value[position] == '.' && (position == 0 || value[position - 1] == '/')) return false;
  }
  return true;
}

bool parseDecimal(std::string_view text, std::size_t width, int minimum, int maximum, int& value) {
  if (text.size() != width || !std::all_of(text.begin(), text.end(), [](unsigned char byte) {
        return byte >= '0' && byte <= '9';
      })) return false;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size() &&
         value >= minimum && value <= maximum;
}

bool validDate(int year, int month, int day) {
  static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return day >= 1 && day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}

}  // namespace

bool isObjectId(std::string_view value) {
  return (value.size() == 40 || value.size() == 64) &&
         std::all_of(value.begin(), value.end(), [](unsigned char byte) {
           return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
         });
}

std::string encodePathSegment(std::string_view value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (const unsigned char byte : value) {
    if (unreserved(byte)) encoded += static_cast<char>(byte);
    else {
      encoded += '%';
      encoded += hex[byte >> 4];
      encoded += hex[byte & 15];
    }
  }
  return encoded;
}

std::optional<std::string> decodePathSegment(std::string_view value) {
  if (value.size() > kMaximumRouteBytes) return std::nullopt;
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t position = 0; position < value.size(); ++position) {
    unsigned char byte = value[position];
    if (byte == '%') {
      if (position + 2 >= value.size()) return std::nullopt;
      const int high = hexDigit(value[position + 1]);
      const int low = hexDigit(value[position + 2]);
      if (high < 0 || low < 0) return std::nullopt;
      byte = static_cast<unsigned char>((high << 4) | low);
      if (unreserved(byte)) return std::nullopt;
      position += 2;
    } else if (!unreserved(byte)) return std::nullopt;
    if (byte < 0x20 || byte == 0x7f || byte == '\\') return std::nullopt;
    decoded += static_cast<char>(byte);
  }
  if (!isValidUtf8(decoded)) return std::nullopt;
  // U+0080..U+009F are controls too, although their UTF-8 bytes are printable.
  for (std::size_t position = 1; position < decoded.size(); ++position) {
    if (static_cast<unsigned char>(decoded[position - 1]) == 0xc2 &&
        static_cast<unsigned char>(decoded[position]) >= 0x80 &&
        static_cast<unsigned char>(decoded[position]) <= 0x9f) return std::nullopt;
  }
  return decoded;
}

Route parseHttpRoute(std::string_view target) {
  Route route;
  if (target.empty() || target.size() > kMaximumRouteBytes ||
      std::any_of(target.begin(), target.end(), [](unsigned char byte) {
        return byte < 0x21 || byte > 0x7e || byte == '?' || byte == '#' || byte == '\\';
      })) return {};
  if (target == "/" || target == "/by-name") {
    route.kind = RouteKind::kTable;
    route.sort_by_name = target == "/by-name";
    return route;
  }
  if (!target.starts_with("/project/")) return {};
  target.remove_prefix(9);
  const auto project_end = target.find('/');
  const auto project = target.substr(0, project_end);
  if (!isValidProjectName(project)) return {};
  route.project = project;
  if (project_end == std::string_view::npos) {
    route.kind = RouteKind::kOverview;
    return route;
  }
  target.remove_prefix(project_end + 1);
  const auto operation_end = target.find('/');
  if (operation_end == std::string_view::npos) return {};
  const auto operation = target.substr(0, operation_end);
  target.remove_prefix(operation_end + 1);
  if (operation == "commit") {
    if (!isObjectId(target)) return {};
    route.kind = RouteKind::kCommit;
    route.ref = target;
    return route;
  }
  if (operation == "tree" || operation == "blob" || operation == "source" || operation == "raw") {
    const auto colon = target.find(':');
    if (colon == std::string_view::npos || target.find(':', colon + 1) != std::string_view::npos) return {};
    const auto ref = decodePathSegment(target.substr(0, colon));
    const auto path = decodePathSegment(target.substr(colon + 1));
    if (!ref || !validRef(*ref) || !path || !validComponents(*path, operation == "tree")) return {};
    if (operation == "raw" && !isObjectId(*ref)) return {};
    route.kind = operation == "tree" ? RouteKind::kTree :
                 operation == "blob" ? RouteKind::kBlob :
                 operation == "source" ? RouteKind::kSource : RouteKind::kRaw;
    route.ref = *ref;
    route.path = *path;
    return route;
  }
  // Git refs may themselves end in /YYYY/MM or /before/<object>. An explicit
  // colon separates those ref names from an optional calendar/paging suffix.
  // Existing unambiguous routes retain their original spelling below.
  const auto ref_boundary = target.find(':');
  if (ref_boundary != std::string_view::npos &&
      (operation == "calendar" || operation == "commits" || operation == "graph")) {
    const auto ref = decodePathSegment(target.substr(0, ref_boundary));
    const auto suffix = target.substr(ref_boundary + 1);
    if (!ref || !validRef(*ref)) return {};
    route.kind = operation == "calendar" ? RouteKind::kCalendar :
                 operation == "commits" ? RouteKind::kCommits : RouteKind::kGraph;
    route.ref = *ref;
    if (suffix.empty()) return route;
    if (operation == "calendar") {
      if (suffix.size() != 8 || suffix[0] != '/' || suffix[5] != '/' ||
          !parseDecimal(suffix.substr(1, 4), 4, 1, 9999, route.year) ||
          !parseDecimal(suffix.substr(6), 2, 1, 12, route.month)) return {};
    } else {
      if (!suffix.starts_with("/before/") || !isObjectId(suffix.substr(8))) return {};
      route.cursor = suffix.substr(8);
    }
    return route;
  }
  if (operation == "overview") {
    route.kind = RouteKind::kOverview;
  } else if (operation == "commits" || operation == "graph") {
    const auto cursor_start = target.rfind("/before/");
    if (cursor_start != std::string_view::npos) {
      const auto cursor = target.substr(cursor_start + 8);
      if (!isObjectId(cursor)) return {};
      route.cursor = cursor;
      target = target.substr(0, cursor_start);
    }
    route.kind = operation == "commits" ? RouteKind::kCommits : RouteKind::kGraph;
  } else if (operation == "calendar") {
    // A YYYY/MM suffix selects a month; slash-bearing branch names otherwise
    // remain intact. Years are deliberately limited to four positive digits.
    if (target.size() >= 8 && target[target.size() - 3] == '/' && target[target.size() - 8] == '/') {
      const auto suffix = target.substr(target.size() - 7);
      if (!parseDecimal(suffix.substr(0, 4), 4, 1, 9999, route.year) ||
          !parseDecimal(suffix.substr(5), 2, 1, 12, route.month)) return {};
      target.remove_suffix(8);
    }
    route.kind = RouteKind::kCalendar;
  } else if (operation == "day") {
    const auto slash = target.rfind('/');
    if (slash == std::string_view::npos) return {};
    const auto date = target.substr(slash + 1);
    if (date.size() != 10 || date[4] != '-' || date[7] != '-' ||
        !parseDecimal(date.substr(0, 4), 4, 1, 9999, route.year) ||
        !parseDecimal(date.substr(5, 2), 2, 1, 12, route.month) ||
        !parseDecimal(date.substr(8, 2), 2, 1, 31, route.day) ||
        !validDate(route.year, route.month, route.day)) return {};
    target = target.substr(0, slash);
    route.kind = RouteKind::kDay;
  } else return {};
  const auto ref = decodePathSegment(target);
  if (!ref || !validRef(*ref)) return {};
  route.ref = *ref;
  return route;
}

}  // namespace ckgit
