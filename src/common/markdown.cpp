// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/markdown.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <cctype>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ckgit/emoji.hpp"
#include "ckgit/highlight.hpp"
#include "ckgit/math.hpp"
#include "ckgit/http_router.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/yaml_subset.hpp"

namespace ckgit {
namespace {

// Where one line of the text being rendered came from: its 1-based line in
// the document and, for a line spliced in by `<!--@include:-->`, the include
// it belongs to (an index into Renderer::include_directories_; 0 is the
// document itself). An included line carries the line of its directive, so a
// finding inside a part still points at a line of the page.
struct SourceLine {
  std::size_t line = 0;
  std::size_t origin = 0;
};

using Lines = std::vector<std::string>;
using LineNumbers = std::vector<SourceLine>;

constexpr std::size_t kMaximumIncludeDepth = 4;     // an include inside an include inside ...
constexpr std::size_t kMaximumIncludes = 256;       // includes of every kind in one document
constexpr std::size_t kMaximumTabs = 16;            // tabs in one code group or tab set
constexpr std::size_t kMaximumHighlightRanges = 64;  // `{1,3-5,…}` ranges on one code block

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
  return value;
}

std::string_view trimLeft(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  return value;
}

void append(std::string& output, std::string_view value) {
  if (value.size() > kMaximumMarkdownOutputBytes - output.size()) {
    throw std::length_error("Markdown output exceeds 4 MiB");
  }
  output.append(value);
}

void escape(std::string& output, std::string_view value) {
  for (const unsigned char byte : value) {
    switch (byte) {
      case '&': append(output, "&amp;"); break;
      case '<': append(output, "&lt;"); break;
      case '>': append(output, "&gt;"); break;
      case '"': append(output, "&quot;"); break;
      case '\'': append(output, "&#39;"); break;
      default:
        if ((byte < 0x20 && byte != '\n' && byte != '\t') || byte == 0x7f) append(output, "\xef\xbf\xbd");
        else append(output, std::string_view(reinterpret_cast<const char*>(&byte), 1));
    }
  }
}

std::string escaped(std::string_view value) {
  std::string output;
  escape(output, value);
  return output;
}

std::size_t indentation(std::string_view line) {
  std::size_t columns = 0;
  for (char byte : line) {
    if (byte == ' ') ++columns;
    else if (byte == '\t') columns += 4 - columns % 4;
    else break;
  }
  return columns;
}

std::string removeIndent(std::string_view line, std::size_t columns) {
  std::size_t removed = 0;
  while (!line.empty() && removed < columns) {
    if (line.front() == ' ') ++removed;
    else if (line.front() == '\t') removed += 4 - removed % 4;
    else break;
    line.remove_prefix(1);
  }
  return std::string(removed > columns ? removed - columns : 0, ' ') + std::string(line);
}

bool punctuation(unsigned char byte) {
  return byte >= 0x21 && byte <= 0x7e && std::ispunct(byte) != 0;
}

std::string unescape(std::string_view value) {
  std::string result;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '\\' && index + 1 < value.size() && punctuation(value[index + 1])) ++index;
    result += value[index];
  }
  return result;
}

std::optional<std::string> decodeUrl(std::string_view value) {
  auto hex = [](unsigned char byte) -> int {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
  };
  std::string result;
  for (std::size_t index = 0; index < value.size(); ++index) {
    unsigned char byte = value[index];
    if (byte == '%') {
      if (index + 2 >= value.size() || hex(value[index + 1]) < 0 || hex(value[index + 2]) < 0) return {};
      byte = static_cast<unsigned char>(hex(value[index + 1]) * 16 + hex(value[index + 2]));
      index += 2;
    }
    if (byte < 0x20 || byte == 0x7f || byte == '\\') return {};
    result += static_cast<char>(byte);
  }
  if (!isValidUtf8(result)) return {};
  return result;
}

// `directory` is where the text holding the link lives relative to the
// document ("" for the document itself, else an included part's directory);
// a relative target is taken from there.
std::optional<std::string> linkTarget(std::string_view original, bool image, const LinkContext& context,
                                      std::size_t line, std::string_view directory = {}) {
  const auto value = unescape(trim(original));
  if (value.empty() || std::any_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte < 0x20 || byte == 0x7f || byte == '\\';
      })) return {};
  const auto colon = value.find(':');
  const auto first_separator = value.find_first_of("/#?");
  if (colon != std::string::npos && (first_separator == std::string::npos || colon < first_separator)) {
    auto scheme = value.substr(0, colon);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char byte) {
      return static_cast<char>(std::tolower(byte));
    });
    if (scheme == "http" || scheme == "https" || (!image && scheme == "mailto")) return value;
    return {};
  }
  if (value.starts_with("//") || value.find('?') != std::string::npos) return {};
  const auto hash = value.find('#');
  std::string fragment;
  if (hash != std::string::npos) {
    const auto decoded = decodeUrl(std::string_view(value).substr(hash + 1));
    if (!decoded) return {};
    fragment = "#" + encodePathSegment(*decoded);
  }
  if (hash == 0 && !image) return fragment;
  auto decoded = decodeUrl(std::string_view(value).substr(0, hash));
  if (!decoded || decoded->empty()) return {};
  if (!directory.empty() && decoded->front() != '/') *decoded = std::string(directory) + "/" + *decoded;
  if (context.resolver) {
    const auto resolved = context.resolver(*decoded, image, line);
    if (!resolved) return {};
    return *resolved + fragment;
  }
  if (!isValidProjectName(context.project) || !isObjectId(context.commit_id)) return {};
  std::string path = decoded->front() == '/' ? decoded->substr(1) :
      (context.directory.empty() ? "" : context.directory + "/") + *decoded;
  std::vector<std::string> components;
  for (std::size_t start = 0; start <= path.size();) {
    const auto slash = path.find('/', start);
    const auto component = path.substr(start, slash == std::string::npos ? path.size() - start : slash - start);
    if (component == "..") {
      if (components.empty()) return {};
      components.pop_back();
    } else if (!component.empty() && component != ".") components.push_back(component);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  path.clear();
  for (const auto& component : components) {
    if (!path.empty()) path += '/';
    path += component;
  }
  const bool tree = path.empty() || decoded->back() == '/' || *decoded == "." || *decoded == ".." ||
                    decoded->ends_with("/.") || decoded->ends_with("/..");
  const auto route = image ? "/raw/" : tree ? "/tree/" : "/blob/";
  const auto& ref = !image && !context.ref.empty() ? context.ref : context.commit_id;
  auto result = "/project/" + context.project + route + encodePathSegment(ref) + ":" + encodePathSegment(path);
  if (parseHttpRoute(result).kind == RouteKind::kNotFound) return {};
  return result + fragment;
}

struct ListMarker {
  bool ordered = false;
  std::size_t indent = 0;
  std::size_t content_offset = 0;
  std::size_t content_indent = 0;
  std::string start;
};

std::optional<ListMarker> listMarker(std::string_view line) {
  const auto indent = indentation(line);
  if (indent > 3) return {};
  std::size_t index = 0;
  while (index < line.size() && line[index] == ' ') ++index;
  const auto marker_start = index;
  if (index >= line.size()) return {};
  bool ordered = false;
  std::string start;
  if (line[index] == '-' || line[index] == '+' || line[index] == '*') ++index;
  else {
    while (index < line.size() && line[index] >= '0' && line[index] <= '9') ++index;
    if (index == marker_start || index - marker_start > 9 || index == line.size() ||
        (line[index] != '.' && line[index] != ')')) return {};
    ordered = true;
    start = line.substr(marker_start, index - marker_start);
    ++index;
  }
  if (index < line.size() && line[index] != ' ' && line[index] != '\t') return {};
  if (index < line.size()) ++index;
  return ListMarker{ordered, indent, index, index, start};
}

bool thematic(std::string_view line) {
  line = trim(line);
  if (line.empty() || (line.front() != '-' && line.front() != '*' && line.front() != '_')) return false;
  const char marker = line.front();
  std::size_t count = 0;
  for (char byte : line) {
    if (byte == marker) ++count;
    else if (byte != ' ' && byte != '\t') return false;
  }
  return count >= 3;
}

// What a fence's info string (or a `<<<` snippet's `{…}`) says about the
// code: the language that names its highlighter and class, a title
// (`title="…"` or `[…]`), the lines to highlight (`{1,3-5}`), and whether to
// number the lines (`:line-numbers[=N]` / `showLineNumbers[=N]`, or
// `:no-line-numbers`). Anything else in the info string is ignored, as
// CommonMark leaves it to the application.
struct CodeInfo {
  std::string language;
  std::string title;
  std::vector<std::pair<std::size_t, std::size_t>> highlight;  // 1-based, inclusive
  std::optional<bool> line_numbers;
  std::size_t first_number = 1;
};

bool languageByte(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-' ||
         byte == '_' || byte == '+' || byte == '#' || byte == '.';
}

std::optional<std::size_t> smallNumber(std::string_view text) {
  if (text.empty() || text.size() > 7) return std::nullopt;
  std::size_t value = 0;
  for (const char byte : text) {
    if (byte < '0' || byte > '9') return std::nullopt;
    value = value * 10 + static_cast<std::size_t>(byte - '0');
  }
  return value;
}

// `1,3-5, 9` into ranges; nothing when the text is anything else.
std::optional<std::vector<std::pair<std::size_t, std::size_t>>> lineRanges(std::string_view text) {
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  for (std::size_t start = 0; start <= text.size();) {
    const auto comma = text.find(',', start);
    const auto part = trim(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
    const auto dash = part.find('-');
    const auto first = smallNumber(trim(part.substr(0, dash)));
    const auto last = dash == std::string_view::npos ? first : smallNumber(trim(part.substr(dash + 1)));
    if (!first || !last || *first == 0 || *last < *first || ranges.size() >= kMaximumHighlightRanges) return std::nullopt;
    ranges.emplace_back(*first, *last);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return ranges;
}

void applyLineNumbersOption(CodeInfo& info, std::string_view value, bool on) {
  info.line_numbers = on;
  if (on && !value.empty()) {
    if (const auto first = smallNumber(value)) info.first_number = std::max<std::size_t>(*first, 1);
  }
}

CodeInfo parseCodeInfo(std::string_view text) {
  CodeInfo info;
  text = trim(text);
  // The first word names the language -- up to a space or the start of an
  // option (`ts{1}`, `js:line-numbers`, `sh[npm]`) -- and only when every
  // byte of it could be one; anything else names no language at all.
  std::size_t index = std::min(text.find_first_of(" \t{[:"), text.size());
  const auto word = text.substr(0, index);
  if (word.size() <= 32 && std::all_of(word.begin(), word.end(), [](unsigned char byte) { return languageByte(byte); })) {
    info.language = std::string(word);
  }
  while (index < text.size()) {
    const char byte = text[index];
    if (byte == ' ' || byte == '\t') {
      ++index;
    } else if (byte == '{') {
      const auto close = text.find('}', index);
      if (close == std::string_view::npos) break;
      if (auto ranges = lineRanges(text.substr(index + 1, close - index - 1))) info.highlight = std::move(*ranges);
      index = close + 1;
    } else if (byte == '[') {
      const auto close = text.find(']', index);
      if (close == std::string_view::npos) break;
      info.title = std::string(trim(text.substr(index + 1, close - index - 1)));
      index = close + 1;
    } else {
      auto end = text.find_first_of(" \t{[", index);
      if (end == std::string_view::npos) end = text.size();
      auto word = text.substr(index, end - index);
      if (word.starts_with("title=") && word.size() > 6 && (word[6] == '"' || word[6] == '\'')) {
        // A quoted title may hold spaces: it runs to its closing quote.
        const auto close = text.find(word[6], index + 7);
        if (close == std::string_view::npos) break;
        info.title = std::string(text.substr(index + 7, close - index - 7));
        end = close + 1;
      } else {
        const auto equals = word.find('=');
        const auto name = word.substr(0, equals);
        const auto value = equals == std::string_view::npos ? std::string_view() : word.substr(equals + 1);
        if (name == ":line-numbers" || name == "showLineNumbers") applyLineNumbersOption(info, value, true);
        else if (name == ":no-line-numbers") applyLineNumbersOption(info, value, false);
      }
      index = end;
    }
  }
  return info;
}

struct Fence { char marker; std::size_t length; std::string info; };

std::optional<Fence> fenceStart(std::string_view line) {
  if (indentation(line) > 3) return {};
  line = trim(line);
  if (line.empty() || (line.front() != '`' && line.front() != '~')) return {};
  std::size_t count = 0;
  while (count < line.size() && line[count] == line.front()) ++count;
  if (count < 3) return {};
  auto info = trim(line.substr(count));
  if (line.front() == '`' && info.find('`') != std::string_view::npos) return {};
  return Fence{line.front(), count, std::string(info)};
}

// The line that closes `fence`: its marker, at least as long, and nothing else.
bool fenceCloses(const Fence& fence, std::string_view line) {
  const auto closing = trim(line);
  std::size_t count = 0;
  while (count < closing.size() && closing[count] == fence.marker) ++count;
  return indentation(line) <= 3 && count >= fence.length && trim(closing.substr(count)).empty();
}

// ---- marked lines inside code ----------------------------------------------

// How a line of code is marked: from the block's `{…}` ranges, or from a
// marker comment on or before the line.
struct CodeLine {
  std::string text;
  bool highlighted = false;
  bool added = false;
  bool removed = false;
  bool focused = false;
  bool warning = false;
  bool error = false;
};

// The comment openers a marker may follow and the closers that may follow it.
constexpr std::array<std::string_view, 9> kMarkerOpeners{"<!--", "//", "/*", "--", "#", ";", "%", "'", "REM"};
constexpr std::array<std::string_view, 2> kMarkerClosers{"-->", "*/"};

// `text` with a trailing comment opener (and the spaces before it) removed.
std::string_view withoutTrailingOpener(std::string_view text) {
  text = trim(text);
  for (const auto opener : kMarkerOpeners) {
    if (text.ends_with(opener)) {
      text.remove_suffix(opener.size());
      return trim(text);
    }
  }
  return text;
}

std::string_view withoutLeadingCloser(std::string_view text) {
  text = trim(text);
  for (const auto closer : kMarkerClosers) {
    if (text.starts_with(closer)) return trim(text.substr(closer.size()));
  }
  return text;
}

// A line that is nothing but a comment holding `word` (Docusaurus's magic
// comments: `// highlight-next-line`, `# highlight-start`, `<!-- highlight-end -->`).
bool magicComment(std::string_view line, std::string_view word) {
  const auto content = trim(line);
  const auto at = content.find(word);
  if (at == std::string_view::npos) return false;
  const auto before = trim(content.substr(0, at));
  if (before.empty() || withoutTrailingOpener(before).size() != 0) return false;
  return withoutLeadingCloser(content.substr(at + word.size())).empty();
}

// Applies the markers a block's lines carry -- VitePress's `[!code …]` at the
// end of a line, Docusaurus's highlight comments on lines of their own -- and
// the block's highlighted ranges, removing the marker comments themselves.
std::vector<CodeLine> markCodeLines(std::string_view code, const CodeInfo& info) {
  std::vector<CodeLine> lines;
  bool next_highlighted = false;
  bool in_highlight = false;
  std::size_t pending_highlight = 0;  // from `[!code highlight:N]`, lines still to mark
  std::size_t pending_focus = 0;
  for (std::size_t start = 0; start < code.size();) {
    const auto newline = code.find('\n', start);
    const auto raw = code.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start);
    start = newline == std::string_view::npos ? code.size() : newline + 1;
    if (magicComment(raw, "highlight-next-line")) { next_highlighted = true; continue; }
    if (magicComment(raw, "highlight-start")) { in_highlight = true; continue; }
    if (magicComment(raw, "highlight-end")) { in_highlight = false; continue; }
    CodeLine line;
    line.text = std::string(raw);
    line.highlighted = next_highlighted || in_highlight;
    next_highlighted = false;
    // `[!code kind]` or `[!code kind:N]`, possibly several, at the line's end.
    for (std::size_t guard = 0; guard < 4; ++guard) {
      const auto at = line.text.rfind("[!code ");
      if (at == std::string::npos) break;
      const auto close = line.text.find(']', at);
      if (close == std::string::npos || !trim(withoutLeadingCloser(std::string_view(line.text).substr(close + 1))).empty()) break;
      const auto spec = std::string_view(line.text).substr(at + 7, close - at - 7);
      const auto colon = spec.find(':');
      const auto kind = spec.substr(0, colon);
      std::size_t count = 1;
      if (colon != std::string_view::npos) {
        const auto parsed = smallNumber(spec.substr(colon + 1));
        if (!parsed || *parsed == 0) break;
        count = *parsed;
      }
      if (kind == "highlight" || kind == "hl") pending_highlight = std::max(pending_highlight, count);
      else if (kind == "focus") pending_focus = std::max(pending_focus, count);
      else if (kind == "++") line.added = true;
      else if (kind == "--") line.removed = true;
      else if (kind == "warning") line.warning = true;
      else if (kind == "error") line.error = true;
      else break;
      // The comment that held only this marker goes with it.
      line.text = std::string(withoutTrailingOpener(std::string_view(line.text).substr(0, at)));
      const auto indent = raw.substr(0, raw.size() - trimLeft(raw).size());
      if (!line.text.starts_with(indent)) line.text = std::string(indent) + line.text;
    }
    if (pending_highlight > 0) { line.highlighted = true; --pending_highlight; }
    if (pending_focus > 0) { line.focused = true; --pending_focus; }
    lines.push_back(std::move(line));
  }
  for (const auto& [first, last] : info.highlight) {
    for (std::size_t number = first; number <= last && number <= lines.size(); ++number) lines[number - 1].highlighted = true;
  }
  return lines;
}

// ---- regions and line ranges of included files -------------------------------

// A line that marks a region: `#region name` / `#endregion [name]` inside
// whatever comment the file's language uses (`// #region a`, `# region` is
// not one, `<!-- #endregion -->`). `name` receives the name, possibly empty.
std::optional<bool> regionMarker(std::string_view line, std::string& name) {
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
  const auto content = trim(line);
  for (const bool start : {true, false}) {
    const std::string_view word = start ? "#region" : "#endregion";
    const auto at = content.find(word);
    if (at == std::string_view::npos) continue;
    const auto before = content.substr(0, at);
    if (!std::all_of(before.begin(), before.end(), [](char byte) {
          return byte == ' ' || byte == '\t' || std::string_view("/*<!-#;%'").find(byte) != std::string_view::npos;
        })) continue;
    auto rest = content.substr(at + word.size());
    if (!rest.empty() && rest.front() != ' ' && rest.front() != '\t' && rest.front() != '-' && rest.front() != '*') continue;
    rest = trim(rest);
    for (const auto closer : kMarkerClosers) {
      if (rest.ends_with(closer)) rest = trim(rest.substr(0, rest.size() - closer.size()));
    }
    name = std::string(rest);
    return start;
  }
  return std::nullopt;
}

// The lines between `#region name` and its `#endregion`, without any region
// markers inside; nothing when the region does not exist.
std::optional<std::string> extractRegion(std::string_view content, std::string_view wanted) {
  std::string result;
  bool inside = false;
  std::size_t nested = 0;
  for (std::size_t start = 0; start < content.size();) {
    const auto newline = content.find('\n', start);
    const auto line = content.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start + 1);
    start = newline == std::string_view::npos ? content.size() : newline + 1;
    std::string name;
    const auto marker = regionMarker(line, name);
    if (!inside) {
      if (marker == true && name == wanted) inside = true;
      continue;
    }
    if (marker.has_value()) {
      if (*marker) {
        ++nested;
      } else if (nested > 0) {
        --nested;
      } else {
        return result;
      }
      continue;
    }
    result.append(line);
  }
  return inside ? std::optional<std::string>(result) : std::nullopt;
}

// Lines `first` to `last` (1-based, inclusive; 0 = from the start / to the end).
std::string lineRange(std::string_view content, std::size_t first, std::size_t last) {
  std::string result;
  std::size_t number = 0;
  for (std::size_t start = 0; start < content.size();) {
    const auto newline = content.find('\n', start);
    const auto line = content.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start + 1);
    start = newline == std::string_view::npos ? content.size() : newline + 1;
    ++number;
    if ((first == 0 || number >= first) && (last == 0 || number <= last)) result.append(line);
  }
  return result;
}

// Removes the indentation every non-blank line shares, so a region taken from
// inside a function reads as a block of its own.
std::string dedent(std::string_view content) {
  std::optional<std::string_view> common;
  for (std::size_t start = 0; start < content.size();) {
    const auto newline = content.find('\n', start);
    auto line = content.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start);
    start = newline == std::string_view::npos ? content.size() : newline + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (trim(line).empty()) continue;
    const auto indent = line.substr(0, line.size() - trimLeft(line).size());
    if (!common) {
      common = indent;
    } else {
      std::size_t shared = 0;
      while (shared < common->size() && shared < indent.size() && (*common)[shared] == indent[shared]) ++shared;
      common = common->substr(0, shared);
    }
  }
  if (!common || common->empty()) return std::string(content);
  std::string result;
  for (std::size_t start = 0; start < content.size();) {
    const auto newline = content.find('\n', start);
    const auto line = content.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start + 1);
    start = newline == std::string_view::npos ? content.size() : newline + 1;
    result.append(line.starts_with(*common) ? line.substr(common->size()) : trimLeft(line));
  }
  return result;
}

// ---- containers ----------------------------------------------------------------

// A `:::` line: the opener of a container (with its kind and the rest of the
// line) or, with an empty kind, a closer. `colons` is the marker's length.
struct ContainerLine {
  std::size_t colons = 0;
  std::string kind;
  std::string title;
  bool open = false;  // `{open}` after a details title
};

constexpr std::array<std::string_view, 10> kContainerKinds{"info",    "note",   "tip",    "important",  "warning",
                                                           "caution", "danger", "details", "code-group", "tabs"};

std::optional<ContainerLine> containerLine(std::string_view line) {
  if (indentation(line) > 3) return std::nullopt;
  auto text = trim(line);
  std::size_t colons = 0;
  while (colons < text.size() && text[colons] == ':') ++colons;
  if (colons < 3) return std::nullopt;
  text = trim(text.substr(colons));
  ContainerLine result;
  result.colons = colons;
  if (text.empty()) return result;
  std::size_t end = 0;
  while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) != 0 || text[end] == '-')) ++end;
  auto kind = std::string(text.substr(0, end));
  std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
  if (std::find(kContainerKinds.begin(), kContainerKinds.end(), kind) == kContainerKinds.end()) return std::nullopt;
  auto rest = text.substr(end);
  if (!rest.empty() && rest.front() == '[') {
    // Docusaurus spells the title right after the kind: `:::tip[Title]`.
    const auto close = rest.rfind(']');
    if (close == std::string_view::npos) return std::nullopt;
    result.title = std::string(trim(rest.substr(1, close - 1)));
    rest = rest.substr(close + 1);
  } else if (!rest.empty() && rest.front() != ' ' && rest.front() != '\t') {
    return std::nullopt;
  }
  rest = trim(rest);
  if (rest.ends_with("{open}")) {
    result.open = true;
    rest = trim(rest.substr(0, rest.size() - 6));
  }
  if (result.title.empty()) result.title = std::string(rest);
  result.kind = std::move(kind);
  return result;
}

// `== Label` starting a tab inside `::: tabs`.
std::optional<std::string_view> tabLabel(std::string_view line) {
  if (indentation(line) > 3) return std::nullopt;
  const auto text = trim(line);
  if (!text.starts_with("== ") || trim(text.substr(3)).empty()) return std::nullopt;
  return trim(text.substr(3));
}

// `[^label]: ` at the start of a footnote definition; the label as written.
std::optional<std::string_view> footnoteDefinition(std::string_view line, std::size_t* content = nullptr) {
  if (indentation(line) > 3) return std::nullopt;
  const auto text = trimLeft(line);
  if (!text.starts_with("[^")) return std::nullopt;
  const auto close = text.find("]:");
  if (close == std::string_view::npos || close <= 2) return std::nullopt;
  const auto label = text.substr(2, close - 2);
  if (label.size() > 64 || label.find_first_of(" \t[]") != std::string_view::npos) return std::nullopt;
  if (content != nullptr) *content = (line.size() - text.size()) + close + 2;
  return label;
}

std::string footnoteKey(std::string_view label) {
  std::string key(label);
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
  return key;
}

// `: ` opening a definition in a definition list; the definition's content offset.
std::optional<std::size_t> definitionMarker(std::string_view line) {
  if (indentation(line) > 3) return std::nullopt;
  const auto text = trimLeft(line);
  if (text.size() < 2 || text.front() != ':' || (text[1] != ' ' && text[1] != '\t') || trim(text.substr(1)).empty()) {
    return std::nullopt;
  }
  return line.size() - text.size() + 2;
}

// `<<< path…`: a code snippet include.
bool snippetLine(std::string_view line) {
  return indentation(line) <= 3 && trimLeft(line).starts_with("<<< ") && trim(trimLeft(line).substr(4)).size() > 0;
}

// `<!--@include: target-->` alone on a line: the target as written.
std::optional<std::string_view> markdownIncludeLine(std::string_view line) {
  if (indentation(line) > 3) return std::nullopt;
  auto text = trim(line);
  if (!text.starts_with("<!--") || !text.ends_with("-->") || text.size() < 7) return std::nullopt;
  text = trim(text.substr(4, text.size() - 7));
  if (!text.starts_with("@include:")) return std::nullopt;
  text = trim(text.substr(9));
  if (text.empty()) return std::nullopt;
  return text;
}

std::size_t headingLevel(std::string_view line) {
  if (indentation(line) > 3) return 0;
  line = trim(line);
  std::size_t count = 0;
  while (count < line.size() && line[count] == '#') ++count;
  return count >= 1 && count <= 6 && (count == line.size() || line[count] == ' ' || line[count] == '\t') ? count : 0;
}

std::vector<std::string> tableCells(std::string_view line) {
  line = trim(line);
  if (!line.empty() && line.front() == '|') line.remove_prefix(1);
  if (!line.empty() && line.back() == '|' && (line.size() == 1 || line[line.size() - 2] != '\\')) line.remove_suffix(1);
  std::vector<std::string> cells;
  std::string cell;
  std::size_t code_run = 0;
  for (std::size_t index = 0; index < line.size(); ++index) {
    if (line[index] == '\\' && index + 1 < line.size()) {
      cell += line[index++];
      cell += line[index];
    } else if (line[index] == '`') {
      std::size_t count = 1;
      while (index + count < line.size() && line[index + count] == '`') ++count;
      if (code_run == 0) code_run = count;
      else if (code_run == count) code_run = 0;
      cell.append(count, '`');
      index += count - 1;
    } else if (line[index] == '|' && code_run == 0) {
      cells.emplace_back(trim(cell));
      cell.clear();
    } else cell += line[index];
  }
  cells.emplace_back(trim(cell));
  return cells;
}

std::optional<std::vector<std::string>> tableAlignment(std::string_view line) {
  if (line.find('|') == std::string_view::npos) return {};
  const auto cells = tableCells(line);
  if (cells.size() > 128) return {};
  std::vector<std::string> alignment;
  for (auto cell : cells) {
    const bool left = !cell.empty() && cell.front() == ':';
    const bool right = !cell.empty() && cell.back() == ':';
    if (left) cell.erase(0, 1);
    if (right && !cell.empty()) cell.pop_back();
    if (cell.empty() || !std::all_of(cell.begin(), cell.end(), [](char byte) { return byte == '-'; })) return {};
    alignment.push_back(left && right ? "center" : right ? "right" : "left");
  }
  return alignment;
}

// GitHub's alerts: a blockquote whose first line is exactly one of these
// markers (case-sensitive, nothing else on the line) and which has content
// below it. Any other first line, or a marker with no body, is a blockquote.
struct AlertKind {
  std::string_view marker;
  std::string_view kind;   // the class suffix
  std::string_view title;  // the visible title
};

constexpr AlertKind kAlertKinds[] = {
    {"[!NOTE]", "note", "Note"},
    {"[!TIP]", "tip", "Tip"},
    {"[!IMPORTANT]", "important", "Important"},
    {"[!WARNING]", "warning", "Warning"},
    {"[!CAUTION]", "caution", "Caution"},
};

const AlertKind* alertKind(const Lines& quoted) {
  if (quoted.empty()) return nullptr;
  const auto first = trim(quoted.front());
  const bool has_body = std::any_of(quoted.begin() + 1, quoted.end(),
                                    [](const std::string& line) { return !trim(line).empty(); });
  if (!has_body) return nullptr;
  for (const auto& kind : kAlertKinds) {
    if (first == kind.marker) return &kind;
  }
  return nullptr;
}

// A task-list marker at the start of a list item's first line: `[ ]`, `[x]`
// or `[X]` after at most three spaces, then whitespace, then the item's text
// on the same line. `rest` is that text.
struct TaskMarker {
  bool checked = false;
  std::string rest;
};

std::optional<TaskMarker> taskMarker(std::string_view line) {
  std::size_t index = 0;
  while (index < line.size() && index < 3 && line[index] == ' ') ++index;
  if (line.size() < index + 4 || line[index] != '[' || line[index + 2] != ']') return {};
  const char state = line[index + 1];
  if (state != ' ' && state != 'x' && state != 'X') return {};
  if (line[index + 3] != ' ' && line[index + 3] != '\t') return {};
  const auto rest = trimLeft(line.substr(index + 4));
  if (rest.empty()) return {};
  return TaskMarker{state != ' ', std::string(rest)};
}

// The raw HTML a document may write: a few attribute-free inline tags and the
// <details>/<summary> disclosure block. Without attributes none of them can
// carry a URL, a style, or a script, so they are as safe on the dashboard's
// origin as on a docs site; every other tag stays escaped text.
constexpr std::array<std::string_view, 6> kInlineHtmlTags{"kbd", "sub", "sup", "mark", "ins", "small"};
constexpr std::array<std::string_view, 2> kVoidHtmlTags{"br", "wbr"};

struct InlineHtmlTag {
  std::string_view name;
  bool closing = false;
  bool empty = false;  // <br>, <wbr>: no closing tag
  std::size_t length = 0;
};

// An allowed tag at the start of `text`, in exactly one spelling: lowercase,
// no attributes, `<br>`/`<br/>`/`<br />` for the void ones.
std::optional<InlineHtmlTag> inlineHtmlTag(std::string_view text) {
  if (!text.starts_with('<')) return std::nullopt;
  const bool closing = text.size() > 1 && text[1] == '/';
  const std::size_t name_start = closing ? 2 : 1;
  for (const auto name : kInlineHtmlTags) {
    if (text.substr(name_start).starts_with(name) && text.size() > name_start + name.size() &&
        text[name_start + name.size()] == '>') {
      return InlineHtmlTag{name, closing, false, name_start + name.size() + 1};
    }
  }
  if (closing) return std::nullopt;
  for (const auto name : kVoidHtmlTags) {
    const auto rest = text.substr(1);
    if (!rest.starts_with(name)) continue;
    for (const std::string_view ending : {">", "/>", " />"}) {
      if (rest.substr(name.size()).starts_with(ending)) return InlineHtmlTag{name, false, true, 1 + name.size() + ending.size()};
    }
  }
  return std::nullopt;
}

// `<details>` or `<details open>` alone on a line opens a disclosure block.
std::optional<bool> detailsOpener(std::string_view line) {
  const auto text = trim(line);
  if (text == "<details>") return false;
  if (text == "<details open>") return true;
  return std::nullopt;
}

bool detailsCloser(std::string_view line) { return trim(line) == "</details>"; }

// Whether one UTF-8 sequence is a pictograph or symbol a heading id leaves
// out: emoji and their joiners, selectors and tags, arrows, technical and
// geometric symbols, dingbats, and the other emoji-presentation symbols.
bool symbolSequence(std::string_view sequence) {
  std::uint32_t code = 0;
  const auto lead = static_cast<unsigned char>(sequence.front());
  if (sequence.size() == 2) code = lead & 0x1fu;
  else if (sequence.size() == 3) code = lead & 0x0fu;
  else if (sequence.size() == 4) code = lead & 0x07u;
  else return false;
  for (std::size_t at = 1; at < sequence.size(); ++at) code = (code << 6) | (static_cast<unsigned char>(sequence[at]) & 0x3fu);
  return code == 0xa9 || code == 0xae || code == 0x200d || code == 0x203c || code == 0x2049 || code == 0x20e3 ||
         code == 0x2122 || code == 0x2139 || (code >= 0x2190 && code <= 0x21ff) || (code >= 0x2300 && code <= 0x23ff) ||
         (code >= 0x24c2 && code <= 0x24c2) || (code >= 0x25a0 && code <= 0x27bf) || (code >= 0x2900 && code <= 0x297f) ||
         (code >= 0x2b00 && code <= 0x2bff) || code == 0x3030 || code == 0x303d || code == 0x3297 || code == 0x3299 ||
         (code >= 0xfe00 && code <= 0xfe0f) || (code >= 0x1f000 && code <= 0x1faff) || (code >= 0xe0000 && code <= 0xe007f);
}

class Renderer {
 public:
  Renderer(const LinkContext& context, std::size_t input_size, std::vector<MarkdownHeading>* outline,
           const MarkdownCodeRenderer& code_renderer)
      : context_(context), work_(input_size * 64 + 4096), outline_(outline), code_renderer_(code_renderer) {}

  // The whole document: its includes spliced in, its blocks, and the
  // footnotes it references.
  std::string render(Lines lines, LineNumbers numbers) {
    include_directories_.assign(1, std::string());
    if (context_.include_reader) expandIncludes(lines, numbers, 0, {});
    collectFootnoteLabels(lines);
    std::string output = blocks(std::move(lines), std::move(numbers));
    append(output, footnoteSection());
    return output;
  }

  // `numbers` holds the source of each entry of `lines`. `lead` is HTML placed
  // at the start of the first paragraph (a task item's checkbox); it is
  // dropped if the first block is not a paragraph.
  std::string blocks(Lines lines, LineNumbers numbers, std::size_t depth = 0, bool tight = false,
                     std::string_view lead = {}) {
    std::string output;
    if (depth >= kMaximumMarkdownDepth) {
      append(output, "<pre>");
      for (const auto& line : lines) { escape(output, line); append(output, "\n"); }
      append(output, "</pre>\n");
      return output;
    }
    for (std::size_t index = 0; index < lines.size();) {
      spend(lines[index].size() + 1);
      if (comment_open_) {
        // An inline comment may continue across paragraph or block boundaries.
        // Ignore Markdown-looking content inside it before identifying blocks.
        const auto closing = lines[index].find("-->");
        if (closing == std::string::npos) { ++index; continue; }
        lines[index].erase(0, closing + 3);
        comment_open_ = false;
      }
      if (trim(lines[index]).empty()) { ++index; continue; }
      if (const auto fence = fenceStart(lines[index])) {
        const auto first = index;
        const auto indent = indentation(lines[index++]);
        std::string code;
        while (index < lines.size()) {
          if (fenceCloses(*fence, lines[index])) { ++index; break; }
          append(code, removeIndent(lines[index++], indent));
          append(code, "\n");
        }
        const CodeInfo info = parseCodeInfo(fence->info);
        append(output, codeBlock(info, code, languageForName(info.language), numbers[first], true));
      } else if (const auto container = containerLine(lines[index]); container && !container->kind.empty()) {
        const auto end = containerEnd(lines, index);
        const auto from = static_cast<std::ptrdiff_t>(index + 1);
        const auto to = static_cast<std::ptrdiff_t>(end);
        append(output, renderContainer(*container, Lines(lines.begin() + from, lines.begin() + to),
                                       LineNumbers(numbers.begin() + from, numbers.begin() + to), depth, numbers[index]));
        index = end < lines.size() ? end + 1 : end;
      } else if (context_.include_reader && snippetLine(lines[index])) {
        append(output, snippet(trim(lines[index]).substr(4), numbers[index], true));
        ++index;
      } else if (indentation(lines[index]) >= 4) {
        append(output, "<pre><code>");
        std::string code;
        while (index < lines.size() && (indentation(lines[index]) >= 4 || trim(lines[index]).empty())) {
          append(code, removeIndent(lines[index++], 4));
          append(code, "\n");
        }
        while (code.ends_with("\n\n")) code.pop_back();
        escape(output, code);
        append(output, "</code></pre>\n");
      } else if (const auto level = headingLevel(lines[index])) {
        enterBlock(numbers, index, index + 1);
        auto text = trim(lines[index++]);
        text = trim(text.substr(level));
        std::size_t closing = text.size();
        while (closing > 0 && text[closing - 1] == '#') --closing;
        if (closing < text.size() && (closing == 0 || text[closing - 1] == ' ' || text[closing - 1] == '\t')) text = trim(text.substr(0, closing));
        const auto visible = withoutComments(text);
        const auto rendered = inlineText(visible, 0);
        const auto [id, plain] = headingId(rendered);
        append(output, "<h" + std::to_string(level) + " id=\"" + escaped(id) + "\">");
        append(output, rendered);
        append(output, "</h" + std::to_string(level) + ">\n");
        if (depth == 0 && outline_ != nullptr) outline_->push_back({static_cast<int>(level), id, plain});
      } else if (thematic(lines[index])) {
        append(output, "<hr>\n");
        ++index;
      } else if (const auto end = detailsEnd(lines, index); end.has_value()) {
        // <details>, an optional <summary>…</summary> line, Markdown blocks,
        // then </details> on a line of its own, nested ones counted.
        const bool open = *detailsOpener(lines[index]);
        std::size_t inner = index + 1;
        while (inner < *end && trim(lines[inner]).empty()) ++inner;
        std::string summary;
        if (inner < *end) {
          const auto text = trim(lines[inner]);
          if (text.starts_with("<summary>") && text.ends_with("</summary>") && text.size() >= 19) {
            enterBlock(numbers, inner, inner + 1);
            summary = inlineText(withoutComments(text.substr(9, text.size() - 19)), 0);
            ++inner;
          }
        }
        append(output, open ? "<details open>" : "<details>");
        if (!summary.empty()) append(output, "<summary>" + summary + "</summary>");
        append(output, "\n");
        append(output, blocks(Lines(lines.begin() + static_cast<std::ptrdiff_t>(inner),
                                    lines.begin() + static_cast<std::ptrdiff_t>(*end)),
                              LineNumbers(numbers.begin() + static_cast<std::ptrdiff_t>(inner),
                                          numbers.begin() + static_cast<std::ptrdiff_t>(*end)),
                              depth + 1));
        append(output, "</details>\n");
        index = *end + 1;
      } else if (trim(lines[index]).starts_with('>')) {
        Lines quoted;
        LineNumbers quoted_numbers;
        while (index < lines.size() && indentation(lines[index]) <= 3 && trim(lines[index]).starts_with('>')) {
          quoted_numbers.push_back(numbers[index]);
          auto line = trimLeft(lines[index++]);
          line.remove_prefix(1);
          if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
          quoted.emplace_back(line);
        }
        if (const auto* alert = alertKind(quoted)) {
          quoted.erase(quoted.begin());
          quoted_numbers.erase(quoted_numbers.begin());
          append(output, "<div class=\"alert alert-" + std::string(alert->kind) + "\"><p class=\"alert-title\">" +
                             escaped(label(alert->kind)) + "</p>\n");
          append(output, blocks(quoted, quoted_numbers, depth + 1));
          append(output, "</div>\n");
        } else {
          append(output, "<blockquote>\n");
          append(output, blocks(quoted, quoted_numbers, depth + 1));
          append(output, "</blockquote>\n");
        }
      } else if (const auto marker = listMarker(lines[index])) {
        const bool ordered = marker->ordered;
        std::string items;
        bool tasks = false;
        while (index < lines.size()) {
          const auto current = listMarker(lines[index]);
          if (!current || current->ordered != ordered || current->indent != marker->indent) break;
          LineNumbers item_numbers{numbers[index]};
          Lines item{lines[index++].substr(current->content_offset)};
          bool loose = false;
          while (index < lines.size()) {
            if (trim(lines[index]).empty()) {
              if (index + 1 >= lines.size()) { ++index; break; }
              const auto next = listMarker(lines[index + 1]);
              if (next && next->indent == marker->indent) { ++index; break; }
              if (indentation(lines[index + 1]) <= marker->indent) break;
              loose = true;
              item.emplace_back();
              item_numbers.push_back(numbers[index]);
              ++index;
              continue;
            }
            const auto next = listMarker(lines[index]);
            if (next && next->indent == marker->indent) break;
            if (indentation(lines[index]) <= marker->indent && beginsBlock(lines[index])) break;
            item_numbers.push_back(numbers[index]);
            item.push_back(removeIndent(lines[index++], current->content_indent));
          }
          // A task marker counts only when what follows it on the line is
          // paragraph text, so `[ ] # x` or `[ ] > x` stay literal.
          std::string_view item_lead;
          if (const auto task = taskMarker(item.front())) {
            const bool table = task->rest.find('|') != std::string::npos && item.size() > 1 && tableAlignment(item[1]);
            if (!beginsBlock(task->rest) && !table) {
              item.front() = task->rest;
              item_lead = task->checked ? "<input type=\"checkbox\" disabled checked> " : "<input type=\"checkbox\" disabled> ";
              tasks = true;
            }
          }
          append(items, item_lead.empty() ? "<li>" : "<li class=\"task-list-item\">");
          append(items, blocks(item, item_numbers, depth + 1, !loose, item_lead));
          append(items, "</li>\n");
        }
        append(output, ordered ? "<ol" : "<ul");
        if (tasks) append(output, " class=\"contains-task-list\"");
        if (ordered && marker->start != "1") append(output, " start=\"" + marker->start + "\"");
        append(output, ">\n");
        append(output, items);
        append(output, ordered ? "</ol>\n" : "</ul>\n");
      } else if (index + 1 < lines.size() && lines[index].find('|') != std::string::npos && tableAlignment(lines[index + 1])) {
        const auto alignments = *tableAlignment(lines[index + 1]);
        const auto headers = tableCells(lines[index]);
        enterBlock(numbers, index, index + 1);
        if (headers.size() != alignments.size()) {
          append(output, "<p>"); append(output, inlineText(withoutComments(lines[index++]), 0)); append(output, "</p>\n");
          continue;
        }
        append(output, "<table><thead><tr>");
        tableRow(output, headers, alignments, true);
        append(output, "</tr></thead><tbody>\n");
        index += 2;
        while (index < lines.size() && !trim(lines[index]).empty() && lines[index].find('|') != std::string::npos && !beginsBlock(lines[index])) {
          append(output, "<tr>");
          enterBlock(numbers, index, index + 1);
          tableRow(output, tableCells(lines[index++]), alignments, false);
          append(output, "</tr>\n");
        }
        append(output, "</tbody></table>\n");
      } else if (const auto formula = displayMath(lines, index)) {
        append(output, math(formula->first, true, numbers[index]) + "\n");
        index = formula->second;
      } else if (const auto label = footnoteDefinition(lines[index]); label && footnote_labels_.count(footnoteKey(*label)) != 0) {
        index = collectFootnote(lines, numbers, index);
      } else {
        const std::size_t first = index;
        std::string paragraph = lines[index++];
        while (index < lines.size() && !trim(lines[index]).empty() && !beginsBlock(lines[index]) &&
               !definitionMarker(lines[index])) {
          if (index + 1 < lines.size() && lines[index].find('|') != std::string::npos && tableAlignment(lines[index + 1])) break;
          append(paragraph, "\n");
          append(paragraph, lines[index++]);
        }
        // A term line (or several) right above `: definition` is a definition list.
        std::size_t probe = index;
        if (probe < lines.size() && trim(lines[probe]).empty()) ++probe;
        if (probe < lines.size() && definitionMarker(lines[probe]) && !definitionMarker(lines[first])) {
          index = definitionList(output, lines, numbers, first, depth);
          continue;
        }
        const auto visible = withoutComments(paragraph);
        enterBlock(numbers, first, index, visible);
        if (!trim(visible).empty()) {
          if (!tight) append(output, "<p>");
          if (!lead.empty()) {
            append(output, lead);
            lead = {};
          }
          append(output, inlineText(visible, 0));
          append(output, tight ? "\n" : "</p>\n");
        }
      }
    }
    return output;
  }

 private:
  const LinkContext& context_;
  std::size_t work_;
  std::vector<MarkdownHeading>* outline_;
  const MarkdownCodeRenderer& code_renderer_;
  std::map<std::string, std::size_t> heading_counts_;
  std::set<std::string> heading_ids_;
  bool comment_open_{false};
  // Where the block whose inline text is being rendered came from: the
  // source line of each of its lines, and, for a paragraph, the joined text
  // the inline renderer reads, so a link is placed on the line it is written
  // on. Newlines are counted incrementally, as links are met in order.
  LineNumbers block_numbers_;
  std::string_view block_text_;
  std::size_t counted_offset_{0};
  std::size_t counted_newlines_{0};

  // Includes: the directory of each spliced-in part relative to the document
  // (index 0 is the document itself), and how many files were included.
  std::vector<std::string> include_directories_;
  std::size_t includes_{0};
  std::size_t included_bytes_{0};
  std::size_t tab_sets_{0};
  // Footnotes: the labels defined at the top level (found before rendering,
  // so a reference may precede its definition), each definition's text, and
  // the order of first reference, which numbers them.
  struct Footnote {
    Lines lines;
    LineNumbers numbers;
    std::size_t number = 0;      // 0 until first referenced
    std::size_t references = 0;
    bool collected = false;
  };
  std::set<std::string> footnote_labels_;
  std::map<std::string, Footnote> footnotes_;
  std::vector<std::string> footnote_order_;

  void report(const std::string& message, SourceLine where) const {
    if (context_.reporter) context_.reporter(message, where.line);
  }

  // The words an alert or container of `kind` is titled with by default.
  const std::string& label(std::string_view kind) const {
    const auto& labels = context_.labels;
    if (kind == "note") return labels.note;
    if (kind == "tip") return labels.tip;
    if (kind == "important") return labels.important;
    if (kind == "warning") return labels.warning;
    if (kind == "caution") return labels.caution;
    if (kind == "danger") return labels.danger;
    if (kind == "details") return labels.details;
    return labels.info;
  }

  // ---- includes ----

  // The path an include names, as the reader takes it: relative to the
  // document. `@/x` is the repository root's `x`, like `/x`; a relative path
  // written inside an included part is relative to that part.
  std::string includeTarget(std::string_view written, SourceLine where) const {
    if (written.starts_with("@/")) return "/" + std::string(written.substr(2));
    const auto& directory = include_directories_[where.origin];
    if (written.starts_with('/') || directory.empty()) return std::string(written);
    return directory + "/" + std::string(written);
  }

  // Keeps an include's budget: how many there are and how much they add.
  bool admitInclude(std::size_t bytes, SourceLine where) {
    if (++includes_ > kMaximumIncludes) {
      report("more than " + std::to_string(kMaximumIncludes) + " includes; the rest are left as text", where);
      return false;
    }
    if (bytes > kMaximumMarkdownInputBytes - std::min(included_bytes_, kMaximumMarkdownInputBytes)) {
      throw std::length_error("included content exceeds 512 KiB");
    }
    included_bytes_ += bytes;
    work_ += bytes * 64;
    return true;
  }

  // Splices `<!--@include: path[#region][{from,to}]-->` lines (outside fenced
  // code) into `lines`, recursively, each included line carrying the
  // directive's line and its part's directory. An include that cannot be
  // made stays visible as text, with the problem reported.
  void expandIncludes(Lines& lines, LineNumbers& numbers, std::size_t depth, std::vector<std::string> chain) {
    std::optional<Fence> fence;
    for (std::size_t index = 0; index < lines.size(); ++index) {
      spend(lines[index].size() + 1);
      if (fence) {
        if (fenceCloses(*fence, lines[index])) fence.reset();
        continue;
      }
      if ((fence = fenceStart(lines[index]))) continue;
      const auto written = markdownIncludeLine(lines[index]);
      if (!written) continue;
      const SourceLine where = numbers[index];
      const auto visible = [&](const std::string& problem) {
        report(problem, where);
        lines[index] = "\\" + std::string(trim(lines[index]));
      };
      std::string_view spec = *written;
      std::size_t from = 0, to = 0;
      if (spec.ends_with('}')) {
        const auto open = spec.rfind('{');
        const auto comma = open == std::string_view::npos ? std::string_view::npos : spec.find(',', open);
        if (comma == std::string_view::npos) { visible("include '" + std::string(*written) + "': a line range is written {from,to}"); continue; }
        const auto first = trim(spec.substr(open + 1, comma - open - 1));
        const auto last = trim(spec.substr(comma + 1, spec.size() - comma - 2));
        const auto first_number = first.empty() ? std::optional<std::size_t>(0) : smallNumber(first);
        const auto last_number = last.empty() ? std::optional<std::size_t>(0) : smallNumber(last);
        if (!first_number || !last_number || (*last_number != 0 && *last_number < *first_number)) {
          visible("include '" + std::string(*written) + "': invalid line range");
          continue;
        }
        from = *first_number;
        to = *last_number;
        spec = trim(spec.substr(0, open));
      }
      std::string region;
      if (const auto hash = spec.find('#'); hash != std::string_view::npos) {
        region = std::string(trim(spec.substr(hash + 1)));
        spec = trim(spec.substr(0, hash));
      }
      if (depth >= kMaximumIncludeDepth) { visible("include '" + std::string(spec) + "' is nested more than 4 levels deep"); continue; }
      const auto target = includeTarget(spec, where);
      if (std::find(chain.begin(), chain.end(), target) != chain.end()) {
        visible("include '" + std::string(spec) + "' would include itself (directly or through other includes)");
        continue;
      }
      const auto included = context_.include_reader(target, true, where.line);
      if (!included) { lines[index] = "\\" + std::string(trim(lines[index])); continue; }
      std::string content(markdownBody(included->content));
      if (!region.empty()) {
        auto part = extractRegion(content, region);
        if (!part) { visible("include '" + std::string(spec) + "' has no region '" + region + "'"); continue; }
        content = std::move(*part);
      }
      if (from != 0 || to != 0) content = lineRange(content, from, to);
      if (!admitInclude(content.size(), where)) { lines[index] = "\\" + std::string(trim(lines[index])); continue; }
      include_directories_.push_back(included->directory);
      const std::size_t origin = include_directories_.size() - 1;
      Lines part;
      LineNumbers part_numbers;
      for (std::size_t start = 0; start < content.size();) {
        const auto newline = content.find('\n', start);
        auto line = std::string_view(content).substr(start, newline == std::string::npos ? std::string::npos : newline - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        part.emplace_back(line);
        part_numbers.push_back({where.line, origin});
        if (newline == std::string::npos) break;
        start = newline + 1;
      }
      auto inner_chain = chain;
      inner_chain.push_back(target);
      expandIncludes(part, part_numbers, depth + 1, std::move(inner_chain));
      const auto at = static_cast<std::ptrdiff_t>(index);
      lines.erase(lines.begin() + at);
      numbers.erase(numbers.begin() + at);
      lines.insert(lines.begin() + at, part.begin(), part.end());
      numbers.insert(numbers.begin() + at, part_numbers.begin(), part_numbers.end());
      index += part.size();
      --index;  // the loop's ++ moves past the spliced part
    }
  }

  // `<<< path[#region|#Lfrom-Lto][{lines lang[:line-numbers]}][ [title]]`:
  // the file (or part of it) as a code block. `chrome` adds the title bar.
  std::string snippet(std::string_view spec, SourceLine where, bool chrome, CodeInfo* parsed = nullptr) {
    const std::string written(trim(spec));
    const auto asText = [&](const std::string& problem) {
      if (!problem.empty()) report(problem, where);
      return "<p>" + escaped("<<< " + written) + "</p>\n";
    };
    std::string_view rest = written;
    std::string title;
    if (rest.ends_with(']')) {
      const auto open = rest.rfind(" [");
      if (open != std::string_view::npos) {
        title = std::string(trim(rest.substr(open + 2, rest.size() - open - 3)));
        rest = trim(rest.substr(0, open));
      }
    }
    std::string meta;
    if (rest.ends_with('}')) {
      const auto open = rest.rfind('{');
      if (open == std::string_view::npos) return asText("snippet '" + written + "': unbalanced {");
      meta = std::string(trim(rest.substr(open + 1, rest.size() - open - 2)));
      rest = trim(rest.substr(0, open));
    }
    std::string region;
    if (const auto hash = rest.find('#'); hash != std::string_view::npos) {
      region = std::string(rest.substr(hash + 1));
      rest = rest.substr(0, hash);
    }
    const std::string path(trim(rest));
    if (path.empty()) return asText("snippet '" + written + "' names no file");
    // The braces hold line ranges and/or a language with options, in either
    // order: `{1,3}`, `{js}`, `{2-4 ts:line-numbers}`.
    std::string language;
    std::string options;
    std::string ranges;
    for (std::size_t start = 0; start < meta.size();) {
      auto end = meta.find_first_of(" \t", start);
      if (end == std::string::npos) end = meta.size();
      const auto word = std::string_view(meta).substr(start, end - start);
      if (!word.empty()) {
        if (lineRanges(word)) ranges = std::string(word);
        else if (word.starts_with(':')) options += " " + std::string(word);
        else {
          const auto colon = word.find(':');
          language = std::string(word.substr(0, colon));
          if (colon != std::string_view::npos) options += " " + std::string(word.substr(colon));
        }
      }
      start = end + 1;
    }
    if (includes_ >= kMaximumIncludes) return asText("more than " + std::to_string(kMaximumIncludes) + " includes");
    const auto included = context_.include_reader(includeTarget(path, where), false, where.line);
    if (!included) return asText({});
    std::string code = included->content;
    if (!region.empty()) {
      std::size_t first = 0, last = 0;
      if (region.size() > 1 && region.front() == 'L') {
        // GitHub's line anchors: #L10 or #L10-L20.
        const auto dash = region.find('-');
        const auto first_number = smallNumber(std::string_view(region).substr(1, dash == std::string::npos ? std::string::npos : dash - 1));
        std::optional<std::size_t> last_number = first_number;
        if (dash != std::string::npos) {
          const auto end = std::string_view(region).substr(dash + 1);
          last_number = end.starts_with('L') ? smallNumber(end.substr(1)) : std::nullopt;
        }
        if (!first_number || !last_number || *first_number == 0 || *last_number < *first_number) {
          return asText("snippet '" + written + "': invalid line range '#" + region + "'");
        }
        first = *first_number;
        last = *last_number;
        code = dedent(lineRange(code, first, last));
      } else {
        auto part = extractRegion(code, region);
        if (!part) return asText("snippet '" + path + "' has no region '" + region + "'");
        code = dedent(*part);
      }
    }
    if (!isValidUtf8(code)) return asText("snippet '" + path + "' is not valid UTF-8 text");
    if (!admitInclude(code.size(), where)) return asText({});
    if (!code.empty() && code.back() != '\n') code.push_back('\n');
    CodeInfo info = parseCodeInfo((language.empty() ? std::string() : language) + options +
                                  (ranges.empty() ? std::string() : " {" + ranges + "}"));
    info.title = title;
    const auto slash = path.rfind('/');
    const auto dot = path.rfind('.');
    if (info.language.empty() && dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) {
      info.language = path.substr(dot + 1);
      if (!std::all_of(info.language.begin(), info.language.end(), [](unsigned char byte) { return languageByte(byte); })) {
        info.language.clear();
      }
    }
    const Language highlighter = language.empty() ? detectLanguage(path, code) : languageForName(language);
    if (parsed != nullptr) {
      *parsed = info;
      if (parsed->title.empty()) parsed->title = path.substr(slash == std::string::npos ? 0 : slash + 1);
    }
    return codeBlock(info, code, highlighter, where, chrome);
  }

  // ---- math ----

  // A formula as MathML; what the converter could not understand is marked
  // in place and reported.
  std::string math(std::string_view tex, bool display, SourceLine where) {
    spend(tex.size() * 8 + 64);
    auto rendered = renderTexMath(tex, display);
    for (const auto& error : rendered.errors) report("math: " + error, where);
    return std::move(rendered.html);
  }

  // `$$` starting a display formula at `index`: the formula and the index
  // after its closing `$$` (on the same line, or on a later one ending with
  // it); nothing when it is never closed.
  std::optional<std::pair<std::string, std::size_t>> displayMath(const Lines& lines, std::size_t index) {
    if (indentation(lines[index]) > 3) return std::nullopt;
    const auto first = trim(lines[index]);
    if (!first.starts_with("$$")) return std::nullopt;
    const auto rest = first.substr(2);
    // `$$ … $$` alone on its line is a display; a line holding more than one
    // formula, or text after one, is a paragraph with inline math.
    if (const auto closing = rest.find("$$"); closing != std::string_view::npos) {
      if (closing + 2 == rest.size() && !trim(rest.substr(0, closing)).empty()) {
        return std::pair<std::string, std::size_t>{std::string(rest.substr(0, closing)), index + 1};
      }
      return std::nullopt;
    }
    std::string tex(rest);
    for (std::size_t at = index + 1; at < lines.size(); ++at) {
      spend(lines[at].size() + 1);
      const auto line = trim(lines[at]);
      if (line.ends_with("$$")) {
        tex += "\n";
        tex += line.substr(0, line.size() - 2);
        return std::pair<std::string, std::size_t>{tex, at + 1};
      }
      if (line.empty()) return std::nullopt;  // a formula does not span a blank line
      tex += "\n";
      tex += lines[at];
    }
    return std::nullopt;
  }

  // An inline formula at value[index] (`$…$`, or `$$…$$` shown as a
  // display): its markup and the bytes it spans. As on GitHub and in Pandoc,
  // dollar signs that are not math stay text: the opening `$` is followed by
  // a non-space, the closing one follows a non-space and is not followed by a
  // digit, and the formula holds no other unescaped `$` -- so `$5 and $10`
  // is text, and each search ends at the next dollar sign (linear time).
  std::optional<std::pair<std::string, std::size_t>> inlineMath(std::string_view value, std::size_t index) {
    const bool display = index + 1 < value.size() && value[index + 1] == '$';
    const std::size_t open = display ? 2 : 1;
    if (index + open >= value.size() || std::isspace(static_cast<unsigned char>(value[index + open])) != 0) return std::nullopt;
    for (std::size_t at = index + open; at < value.size(); ++at) {
      spend(1);
      if (value[at] == '\\' && at + 1 < value.size()) {
        ++at;
        continue;
      }
      if (value[at] != '$') continue;
      if (display) {
        if (at + 1 >= value.size() || value[at + 1] != '$') return std::nullopt;
        return std::pair<std::string, std::size_t>{math(value.substr(index + 2, at - index - 2), true, lineOf(value.substr(index))),
                                                   at + 2 - index};
      }
      if (std::isspace(static_cast<unsigned char>(value[at - 1])) != 0) return std::nullopt;
      if (at + 1 < value.size() && std::isdigit(static_cast<unsigned char>(value[at + 1])) != 0) return std::nullopt;
      return std::pair<std::string, std::size_t>{math(value.substr(index + 1, at - index - 1), false, lineOf(value.substr(index))),
                                                 at + 1 - index};
    }
    return std::nullopt;
  }

  // ---- code blocks ----

  // A fenced or included block of code: what the application's renderer makes
  // of it (a diagram), else highlighted code with the info string's title,
  // marked lines and line numbers. `chrome` adds the title bar; a tab of a
  // code group shows its title as the tab's label instead.
  std::string codeBlock(const CodeInfo& info, std::string_view code, Language language, SourceLine where, bool chrome) {
    if (code_renderer_) {
      if (const auto rendered = code_renderer_(info.language, code)) return *rendered;
    }
    if (info.language == "math") return math(code, true, where) + "\n";
    const auto lines = markCodeLines(code, info);
    const bool numbered = info.line_numbers.value_or(context_.line_numbers);
    bool highlighted = false, diff = false, focus = false, flagged = false;
    std::string kept;
    for (const auto& line : lines) {
      highlighted = highlighted || line.highlighted;
      diff = diff || line.added || line.removed;
      focus = focus || line.focused;
      flagged = flagged || line.warning || line.error;
      append(kept, line.text);
      append(kept, "\n");
    }
    std::string html = highlightHtml(kept, language);
    if (html.size() > kMaximumMarkdownOutputBytes / 2) html = escaped(kept);
    std::string pre = "<pre";
    const bool per_line = numbered || highlighted || diff || focus || flagged;
    if (per_line) {
      pre += " class=\"code-lines";
      if (numbered) pre += " line-numbers";
      if (highlighted) pre += " has-highlighted";
      if (diff) pre += " has-diff";
      if (focus) pre += " has-focused";
      pre += "\"";
    }
    pre += "><code";
    if (!info.language.empty()) pre += " class=\"language-" + escaped(info.language) + "\"";
    pre += ">";
    if (!per_line) {
      append(pre, html);
    } else {
      std::size_t number = 0;
      for (std::size_t start = 0; start < html.size() && number < lines.size(); ++number) {
        const auto newline = html.find('\n', start);
        const auto content = std::string_view(html).substr(start, newline == std::string::npos ? std::string::npos : newline - start);
        start = newline == std::string::npos ? html.size() : newline + 1;
        const auto& line = lines[number];
        std::string classes = "line";
        if (line.highlighted) classes += " highlighted";
        if (line.added) classes += " diff add";
        if (line.removed) classes += " diff remove";
        if (line.focused) classes += " focused";
        if (line.warning) classes += " warning";
        if (line.error) classes += " error";
        append(pre, "<span class=\"" + classes + "\">");
        if (numbered) {
          append(pre, "<span class=\"line-number\" aria-hidden=\"true\">" + std::to_string(info.first_number + number) + "</span>");
        }
        append(pre, content);
        append(pre, "</span>\n");
      }
    }
    pre += "</code></pre>\n";
    if (!chrome || info.title.empty()) return pre;
    return "<div class=\"code-block\"><div class=\"code-title\">" + escaped(info.title) + "</div>" + pre + "</div>\n";
  }

  // ---- containers ----

  // The line that closes the container opened at `index`, counting the
  // containers nested in it and skipping fenced code; the end of the lines
  // when it is never closed (it then runs to the end of its parent).
  std::size_t containerEnd(const Lines& lines, std::size_t index) {
    std::size_t open = 0;
    std::optional<Fence> fence;
    for (std::size_t at = index + 1; at < lines.size(); ++at) {
      spend(lines[at].size() + 1);
      if (fence) {
        if (fenceCloses(*fence, lines[at])) fence.reset();
        continue;
      }
      if ((fence = fenceStart(lines[at]))) continue;
      const auto line = containerLine(lines[at]);
      if (!line) continue;
      if (!line->kind.empty()) ++open;
      else if (open == 0) return at;
      else --open;
    }
    return lines.size();
  }

  // Splits a container's lines where `starts` says a part begins, at its own
  // level only (not inside fenced code or a nested container). Lines before
  // the first part are returned as part 0 with an empty label.
  struct Part {
    std::string label;
    Lines lines;
    LineNumbers numbers;
  };
  std::vector<Part> splitParts(const Lines& lines, const LineNumbers& numbers,
                               const std::function<std::optional<std::string>(std::size_t)>& starts) {
    std::vector<Part> parts(1);
    std::size_t nested = 0;
    std::optional<Fence> fence;
    for (std::size_t at = 0; at < lines.size(); ++at) {
      spend(lines[at].size() + 1);
      if (fence) {
        if (fenceCloses(*fence, lines[at])) fence.reset();
      } else if (const auto line = containerLine(lines[at])) {
        if (!line->kind.empty()) ++nested;
        else if (nested > 0) --nested;
      } else {
        if (nested == 0) {
          if (const auto label = starts(at)) {
            parts.push_back(Part{*label, {}, {}});
            if (tabLabel(lines[at])) continue;  // `== Label` itself is not content
          }
        }
        fence = fenceStart(lines[at]);
      }
      parts.back().lines.push_back(lines[at]);
      parts.back().numbers.push_back(numbers[at]);
    }
    return parts;
  }

  // Radio inputs and labels, then the panels: CSS shows the panel of the
  // checked input, so the tabs work without a script (and on the dashboard,
  // whose policy allows none).
  std::string tabSet(std::size_t number, std::string_view kind, const std::vector<std::string>& labels,
                     const std::vector<std::string>& panels) {
    const std::string id = "tabs:" + std::to_string(number);
    std::string out = "<div class=\"tabbed " + std::string(kind) + "\">";
    for (std::size_t index = 0; index < labels.size(); ++index) {
      const std::string input = id + ":" + std::to_string(index + 1);
      out += "<input type=\"radio\" class=\"tab-input\" name=\"" + id + "\" id=\"" + input + "\"" + (index == 0 ? " checked" : "") +
             "><label class=\"tab-label\" for=\"" + input + "\">" + labels[index] + "</label>";
    }
    out += "<div class=\"tab-panels\">";
    for (const auto& panel : panels) out += "<div class=\"tab-panel\">" + panel + "</div>";
    out += "</div></div>\n";
    return out;
  }

  std::string renderContainer(const ContainerLine& container, const Lines& lines, const LineNumbers& numbers,
                              std::size_t depth, SourceLine where) {
    if (depth + 1 >= kMaximumMarkdownDepth) return blocks(lines, numbers, depth + 1);
    const auto title = [&](std::string_view fallback) {
      if (container.title.empty()) return escaped(fallback);
      enterBlock(LineNumbers{where}, 0, 1);
      return inlineText(withoutComments(container.title), 0);
    };
    if (container.kind == "details") {
      return std::string(container.open ? "<details class=\"details-container\" open>" : "<details class=\"details-container\">") +
             "<summary>" + title(label("details")) + "</summary>\n" + blocks(lines, numbers, depth + 1) + "</details>\n";
    }
    // A tab set is numbered before its panels render, so ids follow the
    // document's order even when sets nest.
    const std::size_t number = container.kind == "code-group" || container.kind == "tabs" ? ++tab_sets_ : 0;
    if (container.kind == "code-group") {
      // Every fenced block or `<<<` snippet starts a tab; anything else
      // joins the tab before it.
      auto parts = splitParts(lines, numbers, [&](std::size_t at) -> std::optional<std::string> {
        if (fenceStart(lines[at]) || (context_.include_reader && snippetLine(lines[at]))) return std::string();
        return std::nullopt;
      });
      std::vector<std::string> labels, panels;
      std::string before = blocks(parts.front().lines, parts.front().numbers, depth + 1);
      for (std::size_t index = 1; index < parts.size(); ++index) {
        auto& part = parts[index];
        std::string panel;
        std::string tab;
        if (const auto fence = fenceStart(part.lines.front())) {
          std::size_t at = 1;
          const auto indent = indentation(part.lines.front());
          std::string code;
          while (at < part.lines.size() && !fenceCloses(*fence, part.lines[at])) {
            append(code, removeIndent(part.lines[at++], indent));
            append(code, "\n");
          }
          if (at < part.lines.size()) ++at;
          const CodeInfo info = parseCodeInfo(fence->info);
          tab = !info.title.empty() ? info.title : !info.language.empty() ? info.language : std::to_string(index);
          panel = codeBlock(info, code, languageForName(info.language), part.numbers.front(), false);
          panel += blocks(Lines(part.lines.begin() + static_cast<std::ptrdiff_t>(at), part.lines.end()),
                          LineNumbers(part.numbers.begin() + static_cast<std::ptrdiff_t>(at), part.numbers.end()), depth + 1);
        } else {
          CodeInfo info;
          panel = snippet(trim(part.lines.front()).substr(4), part.numbers.front(), false, &info);
          tab = !info.title.empty() ? info.title : std::to_string(index);
          panel += blocks(Lines(part.lines.begin() + 1, part.lines.end()), LineNumbers(part.numbers.begin() + 1, part.numbers.end()),
                          depth + 1);
        }
        labels.push_back(escaped(tab));
        panels.push_back(std::move(panel));
      }
      return before + tabsOrPanels(number, "code-group", labels, panels, where);
    }
    if (container.kind == "tabs") {
      auto parts = splitParts(lines, numbers, [&](std::size_t at) -> std::optional<std::string> {
        if (const auto tab = tabLabel(lines[at])) return std::string(*tab);
        return std::nullopt;
      });
      std::vector<std::string> labels, panels;
      std::string before = blocks(parts.front().lines, parts.front().numbers, depth + 1);
      for (std::size_t index = 1; index < parts.size(); ++index) {
        enterBlock(LineNumbers{parts[index].numbers.empty() ? where : parts[index].numbers.front()}, 0, 1);
        labels.push_back(inlineText(withoutComments(parts[index].label), 0));
        panels.push_back(blocks(parts[index].lines, parts[index].numbers, depth + 1));
      }
      return before + tabsOrPanels(number, "tab-set", labels, panels, where);
    }
    return "<div class=\"alert alert-" + container.kind + "\"><p class=\"alert-title\">" + title(label(container.kind)) +
           "</p>\n" + blocks(lines, numbers, depth + 1) + "</div>\n";
  }

  // A tab set, or -- with more tabs than the stylesheet can switch -- the
  // panels one after the other under their labels, reported.
  std::string tabsOrPanels(std::size_t number, std::string_view kind, const std::vector<std::string>& labels,
                           const std::vector<std::string>& panels, SourceLine where) {
    if (labels.empty()) return {};
    if (labels.size() <= kMaximumTabs) return tabSet(number, kind, labels, panels);
    report("a tab set has more than " + std::to_string(kMaximumTabs) + " tabs; they are shown one after another", where);
    std::string out;
    for (std::size_t index = 0; index < labels.size(); ++index) {
      out += "<p class=\"tab-label\">" + labels[index] + "</p>\n" + panels[index];
    }
    return out;
  }

  // ---- footnotes ----

  void collectFootnoteLabels(const Lines& lines) {
    std::optional<Fence> fence;
    for (const auto& line : lines) {
      spend(line.size() + 1);
      if (fence) {
        if (fenceCloses(*fence, line)) fence.reset();
        continue;
      }
      if ((fence = fenceStart(line))) continue;
      if (const auto label = footnoteDefinition(line)) footnote_labels_.insert(footnoteKey(*label));
    }
  }

  // Takes the definition starting at `index`: its first line's text, lazy
  // continuation lines, and blocks indented by four columns after blank
  // lines. The first definition of a label wins. Returns the next index.
  std::size_t collectFootnote(const Lines& lines, const LineNumbers& numbers, std::size_t index) {
    std::size_t content = 0;
    const auto key = footnoteKey(*footnoteDefinition(lines[index], &content));
    Footnote& note = footnotes_[key];
    const bool first = !note.collected;
    note.collected = true;
    Lines body{std::string(trimLeft(std::string_view(lines[index]).substr(content)))};
    LineNumbers body_numbers{numbers[index]};
    ++index;
    while (index < lines.size()) {
      if (trim(lines[index]).empty()) {
        std::size_t next = index;
        while (next < lines.size() && trim(lines[next]).empty()) ++next;
        if (next >= lines.size() || indentation(lines[next]) < 4) break;
        for (; index < next; ++index) {
          body.emplace_back();
          body_numbers.push_back(numbers[index]);
        }
        continue;
      }
      if (indentation(lines[index]) >= 4) {
        body.push_back(removeIndent(lines[index], 4));
      } else if (!trim(body.back()).empty() && !beginsBlock(lines[index]) && !footnoteDefinition(lines[index])) {
        body.push_back(lines[index]);
      } else {
        break;
      }
      body_numbers.push_back(numbers[index]);
      ++index;
    }
    if (first) {
      note.lines = std::move(body);
      note.numbers = std::move(body_numbers);
    }
    return index;
  }

  // `[^label]` at value[index], when the label is defined: the reference's
  // markup, and the number of bytes it spans.
  std::optional<std::pair<std::string, std::size_t>> footnoteReference(std::string_view value, std::size_t index) {
    // A label is at most 64 bytes, so the search never looks further.
    const auto close = value.substr(0, std::min(value.size(), index + 2 + 65)).find(']', index + 2);
    if (close == std::string_view::npos || close == index + 2 || close - index - 2 > 64) return std::nullopt;
    const auto written = value.substr(index + 2, close - index - 2);
    if (written.find_first_of(" \t\n[") != std::string_view::npos) return std::nullopt;
    const auto key = footnoteKey(written);
    if (footnote_labels_.count(key) == 0) return std::nullopt;
    Footnote& note = footnotes_[key];
    if (note.number == 0) {
      footnote_order_.push_back(key);
      note.number = footnote_order_.size();
    }
    ++note.references;
    const std::string number = std::to_string(note.number);
    const std::string id = "fnref:" + number + (note.references > 1 ? ":" + std::to_string(note.references) : "");
    return std::pair<std::string, std::size_t>{
        "<sup class=\"footnote-ref\"><a href=\"#fn:" + number + "\" id=\"" + id + "\">" + number + "</a></sup>", close + 1 - index};
  }

  // The referenced footnotes in order of first reference, each with links
  // back to every reference. A footnote may reference another, so the list
  // can grow while it is rendered.
  std::string footnoteSection() {
    if (footnote_order_.empty()) return {};
    std::vector<std::string> bodies;
    for (std::size_t index = 0; index < footnote_order_.size(); ++index) {
      const Footnote& note = footnotes_[footnote_order_[index]];
      bodies.push_back(blocks(note.lines, note.numbers, 1));
    }
    std::string out = "<section class=\"footnotes\" aria-label=\"" + escaped(context_.labels.footnotes) + "\">\n<ol>\n";
    for (std::size_t index = 0; index < footnote_order_.size(); ++index) {
      const Footnote& note = footnotes_[footnote_order_[index]];
      const std::string number = std::to_string(note.number);
      std::string backrefs;
      for (std::size_t reference = 1; reference <= note.references; ++reference) {
        const std::string suffix = reference > 1 ? ":" + std::to_string(reference) : "";
        backrefs += " <a href=\"#fnref:" + number + suffix + "\" class=\"footnote-backref\" aria-label=\"" +
                    escaped(context_.labels.back_to_reference + " " + number + (reference > 1 ? "-" + std::to_string(reference) : "")) +
                    "\">\xe2\x86\xa9</a>";
      }
      std::string& body = bodies[index];
      constexpr std::string_view kParagraphEnd = "</p>\n";
      if (body.ends_with(kParagraphEnd)) body.insert(body.size() - kParagraphEnd.size(), backrefs);
      else body += "<p>" + backrefs.substr(1) + "</p>\n";
      append(out, "<li id=\"fn:" + number + "\">\n" + body + "</li>\n");
    }
    append(out, "</ol>\n</section>\n");
    return out;
  }

  // ---- definition lists ----

  // Term lines, then `: definition` items (each may continue on lines
  // indented by two or more, and across blank lines that are followed by
  // such lines), repeated for each term group. Returns the next index.
  std::size_t definitionList(std::string& output, const Lines& lines, const LineNumbers& numbers, std::size_t at, std::size_t depth) {
    const auto blank = [&](std::size_t index) { return trim(lines[index]).empty(); };
    append(output, "<dl>\n");
    while (true) {
      while (at < lines.size() && !blank(at) && !definitionMarker(lines[at])) {
        enterBlock(numbers, at, at + 1);
        append(output, "<dt>" + inlineText(withoutComments(trim(lines[at])), 0) + "</dt>\n");
        ++at;
      }
      bool loose = false;
      if (at < lines.size() && blank(at)) {
        loose = true;
        ++at;
      }
      while (at < lines.size()) {
        const auto marker = definitionMarker(lines[at]);
        if (!marker) break;
        Lines definition{lines[at].substr(*marker)};
        LineNumbers definition_numbers{numbers[at]};
        ++at;
        while (at < lines.size()) {
          spend(lines[at].size() + 1);
          if (blank(at)) {
            if (at + 1 < lines.size() && indentation(lines[at + 1]) >= 2 && !blank(at + 1)) {
              loose = true;
              definition.emplace_back();
              definition_numbers.push_back(numbers[at]);
              ++at;
              continue;
            }
            break;
          }
          if (definitionMarker(lines[at])) break;
          if (indentation(lines[at]) >= 2) {
            definition.push_back(removeIndent(lines[at], std::min<std::size_t>(indentation(lines[at]), 4)));
          } else if (!trim(definition.back()).empty() && !beginsBlock(lines[at])) {
            definition.push_back(lines[at]);
          } else {
            break;
          }
          definition_numbers.push_back(numbers[at]);
          ++at;
        }
        append(output, "<dd>" + blocks(definition, definition_numbers, depth + 1, !loose) + "</dd>\n");
        if (at + 1 < lines.size() && blank(at) && definitionMarker(lines[at + 1])) {
          loose = true;
          ++at;
        }
      }
      // Another group: (blank lines,) term lines, (a blank line,) a definition.
      std::size_t next = at;
      while (next < lines.size() && blank(next)) ++next;
      std::size_t terms_end = next;
      while (terms_end < lines.size() && !blank(terms_end) && !definitionMarker(lines[terms_end]) && !beginsBlock(lines[terms_end])) {
        ++terms_end;
      }
      std::size_t probe = terms_end;
      if (probe < lines.size() && blank(probe)) ++probe;
      if (terms_end > next && probe < lines.size() && definitionMarker(lines[probe]) &&
          (terms_end == lines.size() || blank(terms_end) || definitionMarker(lines[terms_end]))) {
        at = next;
        continue;
      }
      break;
    }
    append(output, "</dl>\n");
    return at;
  }


  void enterBlock(const LineNumbers& numbers, std::size_t first, std::size_t end, std::string_view text = {}) {
    block_numbers_.assign(numbers.begin() + static_cast<std::ptrdiff_t>(first),
                          numbers.begin() + static_cast<std::ptrdiff_t>(end));
    block_text_ = text;
    counted_offset_ = 0;
    counted_newlines_ = 0;
  }

  // The source line of a link whose destination is `written`, a view into the
  // block's text; the block's first line when it is not (a table cell, a
  // heading -- single lines anyway). A comment spanning lines inside a
  // paragraph can shift the count; the result stays within the block.
  SourceLine lineOf(std::string_view written) {
    if (block_numbers_.empty()) return {};
    const char* begin = block_text_.data();
    const std::less<const char*> before;  // a total order, even for unrelated buffers
    if (begin == nullptr || before(written.data(), begin) || before(begin + block_text_.size(), written.data())) {
      return block_numbers_.front();
    }
    const auto offset = static_cast<std::size_t>(written.data() - begin);
    if (offset < counted_offset_) {
      counted_offset_ = 0;
      counted_newlines_ = 0;
    }
    counted_newlines_ += static_cast<std::size_t>(
        std::count(block_text_.begin() + static_cast<std::ptrdiff_t>(counted_offset_),
                   block_text_.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
    counted_offset_ = offset;
    return block_numbers_[std::min(counted_newlines_, block_numbers_.size() - 1)];
  }
  // A link's href or an image's src, resolved from where its text came from.
  std::optional<std::string> target(std::string_view written, bool image) {
    const SourceLine where = lineOf(written);
    const std::string_view directory =
        where.origin < include_directories_.size() ? std::string_view(include_directories_[where.origin]) : std::string_view();
    return linkTarget(written, image, context_, where.line, directory);
  }


  void spend(std::size_t count) {
    if (count > work_) throw std::length_error("Markdown parsing work limit exceeded");
    work_ -= count;
  }

  std::size_t find(std::string_view value, std::string_view needle, std::size_t start) {
    for (std::size_t index = start; index + needle.size() <= value.size(); ++index) {
      spend(needle.size());
      if (value.substr(index, needle.size()) == needle && (index == 0 || value[index - 1] != '\\')) return index;
    }
    return std::string_view::npos;
  }

  std::string withoutComments(std::string_view value) {
    std::string output;
    for (std::size_t index = 0; index < value.size();) {
      spend(1);
      if (comment_open_) {
        const auto end = value.find("-->", index);
        spend(end == std::string_view::npos ? value.size() - index : end + 3 - index);
        if (end == std::string_view::npos) break;
        comment_open_ = false;
        index = end + 3;
        continue;
      }
      if (value[index] == '\\' && index + 1 < value.size() && punctuation(value[index + 1])) {
        append(output, value.substr(index, 2));
        index += 2;
        continue;
      }
      if (value[index] == '`') {
        std::size_t count = 1;
        while (index + count < value.size() && value[index + count] == '`') ++count;
        const auto delimiter = value.substr(index, count);
        auto end = find(value, delimiter, index + count);
        while (end != std::string_view::npos && end + count < value.size() && value[end + count] == '`') {
          std::size_t skip = count;
          while (end + skip < value.size() && value[end + skip] == '`') ++skip;
          end = find(value, delimiter, end + skip);
        }
        const auto length = end == std::string_view::npos ? count : end + count - index;
        append(output, value.substr(index, length));
        index += length;
        continue;
      }
      if (value.substr(index).starts_with("<!--")) {
        comment_open_ = true;
        index += 4;
        continue;
      }
      append(output, value.substr(index++, 1));
    }
    return output;
  }

  bool beginsBlock(std::string_view line) {
    if (headingLevel(line) != 0 || thematic(line) || fenceStart(line).has_value() || listMarker(line).has_value() ||
        (indentation(line) <= 3 && trim(line).starts_with('>')) || detailsOpener(line).has_value() || detailsCloser(line) ||
        containerLine(line).has_value() || (context_.include_reader && snippetLine(line)) ||
        (indentation(line) <= 3 && trim(line).starts_with("$$"))) {
      return true;
    }
    const auto label = footnoteDefinition(line);
    return label && footnote_labels_.count(footnoteKey(*label)) != 0;
  }

  // The index of the `</details>` line that closes a <details> opener at
  // `index`, skipping fenced code and counting nested ones; nothing when the
  // line opens no block or the block is never closed (it then stays text).
  std::optional<std::size_t> detailsEnd(const Lines& lines, std::size_t index) {
    if (indentation(lines[index]) > 3 || !detailsOpener(lines[index]).has_value()) return std::nullopt;
    std::size_t open = 1;
    std::optional<Fence> fence;
    for (std::size_t at = index + 1; at < lines.size(); ++at) {
      spend(lines[at].size() + 1);
      if (fence.has_value()) {
        const auto closing = trim(lines[at]);
        std::size_t count = 0;
        while (count < closing.size() && closing[count] == fence->marker) ++count;
        if (count >= fence->length && trim(closing.substr(count)).empty()) fence.reset();
        continue;
      }
      if ((fence = fenceStart(lines[at]))) continue;
      if (detailsOpener(lines[at]).has_value()) ++open;
      if (detailsCloser(lines[at]) && --open == 0) return at;
    }
    return std::nullopt;
  }

  // The heading's unique id and its plain text (for an outline).
  std::pair<std::string, std::string> headingId(std::string_view rendered) {
    // Slugs follow the heading's displayed text, so links contribute their
    // label and images their alternative text rather than their destinations.
    // Only markup generated by inlineText is stripped; escaped raw HTML stays
    // visible, just as it does in the heading itself.
    std::string text;
    for (std::size_t index = 0; index < rendered.size();) {
      spend(1);
      if (rendered[index] == '<') {
        const auto end = rendered.find('>', index + 1);
        if (end == std::string_view::npos) break;
        const auto tag = rendered.substr(index, end + 1 - index);
        // A formula contributes what it shows, not its TeX annotation; a
        // footnote reference contributes nothing.
        const auto skipTo = [&](std::string_view closing) {
          const auto stop = rendered.find(closing, end);
          return stop == std::string_view::npos ? rendered.size() : stop + closing.size();
        };
        if (tag.starts_with("<annotation")) {
          spend(end - index);
          index = skipTo("</annotation>");
          continue;
        }
        if (tag.starts_with("<sup class=\"footnote-ref\">")) {
          spend(end - index);
          index = skipTo("</sup>");
          continue;
        }
        if (tag.starts_with("<img ")) {
          const auto alt = tag.find(" alt=\"");
          if (alt != std::string_view::npos) {
            const auto finish = tag.find('"', alt + 6);
            if (finish != std::string_view::npos) append(text, tag.substr(alt + 6, finish - alt - 6));
          }
        } else if (tag == "<br>") append(text, " ");
        spend(end - index);
        index = end + 1;
      } else append(text, rendered.substr(index++, 1));
    }
    std::string plain;
    for (std::size_t index = 0; index < text.size();) {
      spend(1);
      const auto rest = std::string_view(text).substr(index);
      bool decoded = false;
      for (const auto& [entity, character] :
           {std::pair<std::string_view, char>{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
            {"&quot;", '"'}, {"&#39;", '\''}}) {
        if (rest.starts_with(entity)) {
          plain += character;
          index += entity.size();
          decoded = true;
          break;
        }
      }
      if (!decoded) plain += text[index++];
    }
    std::string result;
    bool space = false;
    for (std::size_t at = 0; at < plain.size(); ++at) {
      const auto byte = static_cast<unsigned char>(plain[at]);
      if (byte >= 0x80) {
        // Letters of any script stay; emoji and other pictographs, arrows
        // and dingbats do not, as on GitHub.
        const std::size_t length = byte >= 0xf0 ? 4 : byte >= 0xe0 ? 3 : byte >= 0xc0 ? 2 : 1;
        const auto sequence = std::string_view(plain).substr(at, length);
        if (!symbolSequence(sequence)) {
          if (space) result += '-';
          result += sequence;
          space = false;
        }
        at += sequence.size() - 1;
        continue;
      }
      if (byte == ' ' || byte == '\t' || byte == '\n') space = !result.empty();
      else if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_') {
        if (space) result += '-';
        result += static_cast<char>(byte);
        space = false;
      } else if (byte >= 'A' && byte <= 'Z') {
        if (space) result += '-';
        result += static_cast<char>(byte - 'A' + 'a');
        space = false;
      }
    }
    if (result.empty()) result = "section";
    auto& count = heading_counts_[result];
    std::string candidate;
    do {
      candidate = result + (count == 0 ? "" : "-" + std::to_string(count));
      ++count;
    } while (!heading_ids_.insert(candidate).second);
    return {candidate, std::string(trim(plain))};
  }

  void tableRow(std::string& output, const std::vector<std::string>& cells,
                const std::vector<std::string>& alignment, bool header) {
    const std::string tag = header ? "th" : "td";
    for (std::size_t index = 0; index < alignment.size(); ++index) {
      append(output, "<" + tag + " class=\"align-" + alignment[index] + "\">");
      if (index < cells.size()) append(output, inlineText(withoutComments(cells[index]), 0));
      append(output, "</" + tag + ">");
    }
  }

  std::string inlineText(std::string_view value, std::size_t depth, bool allow_links = true) {
    if (depth >= kMaximumMarkdownDepth) return escaped(value);
    std::string output;
    // Allowed inline tags opened in this run of text; any still open at its
    // end are closed there, so one never leaks into the rest of the page.
    std::vector<std::string_view> open_tags;
    for (std::size_t index = 0; index < value.size();) {
      spend(1);
      if (value[index] == '\\' && index + 1 < value.size() && (punctuation(value[index + 1]) || value[index + 1] == '\n')) {
        if (value[index + 1] == '\n') append(output, "<br>\n");
        else escape(output, value.substr(index + 1, 1));
        index += 2;
        continue;
      }
      if (value[index] == '\n') {
        if (index >= 2 && value[index - 1] == ' ' && value[index - 2] == ' ') {
          while (!output.empty() && output.back() == ' ') output.pop_back();
          append(output, "<br>\n");
        } else append(output, "\n");
        ++index;
        continue;
      }
      if (value[index] == '`') {
        std::size_t count = 1;
        while (index + count < value.size() && value[index + count] == '`') ++count;
        const auto delimiter = value.substr(index, count);
        auto end = find(value, delimiter, index + count);
        while (end != std::string_view::npos && end + count < value.size() && value[end + count] == '`') {
          std::size_t skip = count;
          while (end + skip < value.size() && value[end + skip] == '`') ++skip;
          end = find(value, delimiter, end + skip);
        }
        if (end != std::string_view::npos) {
          std::string code(value.substr(index + count, end - index - count));
          std::replace(code.begin(), code.end(), '\n', ' ');
          if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && code.find_first_not_of(' ') != std::string::npos) code = code.substr(1, code.size() - 2);
          append(output, "<code>"); escape(output, code); append(output, "</code>");
          index = end + count;
          continue;
        }
        escape(output, delimiter);
        index += count;
        continue;
      }
      if (value[index] == '$') {
        if (const auto formula = inlineMath(value, index)) {
          append(output, formula->first);
          index += formula->second;
          continue;
        }
        escape(output, value.substr(index, 1));
        ++index;
        continue;
      }
      if (allow_links && value[index] == '[' && index + 1 < value.size() && value[index + 1] == '^') {
        if (const auto reference = footnoteReference(value, index)) {
          append(output, reference->first);
          index += reference->second;
          continue;
        }
      }
      if (value[index] == ':' && (index == 0 || std::isalnum(static_cast<unsigned char>(value[index - 1])) == 0)) {
        // An emoji shortcode: `:name:` with nothing alphanumeric right before it.
        std::size_t end = index + 1;
        while (end < value.size() && end - index <= kMaximumEmojiShortcodeBytes &&
               (std::isalnum(static_cast<unsigned char>(value[end])) != 0 || value[end] == '_' || value[end] == '+' ||
                value[end] == '-')) {
          ++end;
        }
        if (end < value.size() && value[end] == ':' && end > index + 1) {
          if (const auto emoji = emojiForShortcode(value.substr(index + 1, end - index - 1))) {
            append(output, *emoji);
            index = end + 1;
            continue;
          }
        }
      }
      // A link's label is rendered without links (`allow_links` is false
      // there), but an image may still be the label: `[![alt](src)](href)`.
      const bool image = value[index] == '!' && index + 1 < value.size() && value[index + 1] == '[';
      if (image || (allow_links && value[index] == '[')) {
        const auto label_start = index + (image ? 2 : 1);
        std::size_t label_end = label_start;
        std::size_t nested = 1;
        for (; label_end < value.size(); ++label_end) {
          spend(1);
          if (value[label_end] == '\\' && label_end + 1 < value.size()) { ++label_end; continue; }
          if (value[label_end] == '[') ++nested;
          if (value[label_end] == ']' && --nested == 0) break;
        }
        if (label_end < value.size() && label_end + 1 < value.size() && value[label_end + 1] == '(') {
          const auto destination_start = label_end + 2;
          std::size_t end = destination_start;
          nested = 1;
          bool angle = end < value.size() && value[end] == '<';
          for (; end < value.size(); ++end) {
            spend(1);
            if (value[end] == '\\' && end + 1 < value.size()) { ++end; continue; }
            if (angle) { if (value[end] == '>') angle = false; continue; }
            if (value[end] == '(') ++nested;
            if (value[end] == ')' && --nested == 0) break;
          }
          if (end < value.size()) {
            auto destination = trim(value.substr(destination_start, end - destination_start));
            std::string_view title;
            if (destination.starts_with('<')) {
              const auto close = destination.find('>');
              if (close != std::string_view::npos) { title = trim(destination.substr(close + 1)); destination = destination.substr(1, close - 1); }
            } else {
              const auto space = destination.find_first_of(" \t\n");
              if (space != std::string_view::npos) { title = trim(destination.substr(space)); destination = destination.substr(0, space); }
            }
            const bool valid_title = title.empty() || (title.size() >= 2 &&
              ((title.front() == '"' && title.back() == '"') || (title.front() == '\'' && title.back() == '\'')));
            const auto url = valid_title ? target(destination, image) : std::nullopt;
            if (url) {
              append(output, image ? "<img src=\"" : "<a href=\"");
              escape(output, *url); append(output, "\"");
              if (!title.empty()) { append(output, " title=\""); escape(output, unescape(title.substr(1, title.size() - 2))); append(output, "\""); }
              if (image) {
                append(output, " alt=\""); escape(output, unescape(value.substr(label_start, label_end - label_start))); append(output, "\" loading=\"lazy\">");
              } else {
                append(output, ">"); append(output, inlineText(value.substr(label_start, label_end - label_start), depth + 1, false)); append(output, "</a>");
              }
            } else escape(output, value.substr(index, end + 1 - index));
            index = end + 1;
            continue;
          }
        }
      }
      if (value[index] == '<') {
        if (const auto tag = inlineHtmlTag(value.substr(index))) {
          spend(tag->length);
          if (tag->empty) {
            append(output, "<" + std::string(tag->name) + ">");
          } else if (!tag->closing) {
            append(output, "<" + std::string(tag->name) + ">");
            open_tags.push_back(tag->name);
          } else if (!open_tags.empty() && open_tags.back() == tag->name) {
            append(output, "</" + std::string(tag->name) + ">");
            open_tags.pop_back();
          } else {
            escape(output, value.substr(index, tag->length));  // closes nothing open here
          }
          index += tag->length;
          continue;
        }
      }
      if (value[index] == '<' && allow_links) {
        const auto end = find(value, ">", index + 1);
        if (end != std::string_view::npos) {
          const auto candidate = value.substr(index + 1, end - index - 1);
          const auto colon = candidate.find(':');
          const bool email = colon == std::string_view::npos && candidate.find('@') != std::string_view::npos && candidate.find_first_of(" \t\n<>\"") == std::string_view::npos;
          const auto url = (colon != std::string_view::npos || email) ? linkTarget(email ? "mailto:" + std::string(candidate) : std::string(candidate), false, context_, lineOf(candidate).line) : std::nullopt;
          if (url) {
            append(output, "<a href=\""); escape(output, *url); append(output, "\">"); escape(output, candidate); append(output, "</a>");
            index = end + 1;
            continue;
          }
        }
      }
      if (value[index] == '~') {
        // GitHub strikethrough: exactly two tildes, no whitespace just inside
        // either pair. A single tilde and a run of three or more are text.
        std::size_t count = 1;
        while (index + count < value.size() && value[index + count] == '~') ++count;
        if (count == 2 && index + 2 < value.size() && std::isspace(static_cast<unsigned char>(value[index + 2])) == 0) {
          std::size_t end = index + 2;
          bool closed = false;
          while (end + 1 < value.size()) {
            spend(1);
            if (value[end] == '\\' && end + 1 < value.size()) { end += 2; continue; }
            if (value[end] != '~') { ++end; continue; }
            std::size_t run = 1;
            while (end + run < value.size() && value[end + run] == '~') ++run;
            if (run == 2 && end > index + 2 && std::isspace(static_cast<unsigned char>(value[end - 1])) == 0) { closed = true; break; }
            end += run;
          }
          if (closed) {
            append(output, "<del>");
            append(output, inlineText(value.substr(index + 2, end - index - 2), depth + 1, allow_links));
            append(output, "</del>");
            index = end + 2;
            continue;
          }
        }
        escape(output, value.substr(index, count));
        index += count;
        continue;
      }
      if (value[index] == '*' || value[index] == '_') {
        const auto marker = value[index];
        std::size_t count = 1;
        while (index + count < value.size() && value[index + count] == marker && count < 3) ++count;
        const bool intraword = marker == '_' && index > 0 && std::isalnum(static_cast<unsigned char>(value[index - 1])) != 0;
        if (!intraword && index + count < value.size() && std::isspace(static_cast<unsigned char>(value[index + count])) == 0) {
          std::size_t end = index + count;
          while (end < value.size()) {
            spend(1);
            if (value[end] == '\\' && end + 1 < value.size()) { end += 2; continue; }
            if (value[end] != marker) { ++end; continue; }
            std::size_t run = 1;
            while (end + run < value.size() && value[end + run] == marker) ++run;
            const bool can_close = end > index + count &&
                std::isspace(static_cast<unsigned char>(value[end - 1])) == 0 &&
                !(marker == '_' && end + run < value.size() &&
                  std::isalnum(static_cast<unsigned char>(value[end + run])) != 0);
            if (run >= count && can_close) { end += run - count; break; }
            end += run;
          }
          if (end >= value.size()) end = std::string_view::npos;
          if (end != std::string_view::npos && end > index + count && std::isspace(static_cast<unsigned char>(value[end - 1])) == 0) {
            append(output, count == 1 ? "<em>" : count == 2 ? "<strong>" : "<em><strong>");
            append(output, inlineText(value.substr(index + count, end - index - count), depth + 1, allow_links));
            append(output, count == 1 ? "</em>" : count == 2 ? "</strong>" : "</strong></em>");
            index = end + count;
            continue;
          }
        }
      }
      escape(output, value.substr(index++, 1));
    }
    while (!open_tags.empty()) {
      append(output, "</" + std::string(open_tags.back()) + ">");
      open_tags.pop_back();
    }
    return output;
  }
};

// Front matter is read with the shared YAML subset. The block handed to the
// parser starts with a blank line standing in for the opening fence, so the
// line numbers in its messages are the file's own.
constexpr YamlBounds kFrontMatterYamlBounds{
    kMaximumFrontMatterBytes + 2,
    4096,
    kMaximumFrontMatterLines + 2,
    kMaximumFrontMatterKeyBytes,
    kMaximumFrontMatterValueBytes,
    4 * kMaximumFrontMatterValueBytes,
    64,
    kMaximumFrontMatterLines,
    kMaximumFrontMatterLines,
    8,
};
constexpr YamlDialect kFrontMatterYamlDialect{kFrontMatterYamlBounds, "front matter", "the front matter"};

bool fenceLine(std::string_view line, std::string_view marker) {
  return line.starts_with(marker) && trim(line.substr(marker.size())).empty();
}

bool hasOnlyFrontMatterKeyBytes(std::string_view key) {
  return std::all_of(key.begin(), key.end(), [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '.' ||
           byte == '_' || byte == '-';
  });
}

// The `key` of a flat `key: value` line, or empty when the line has no such
// shape. `rest` receives what follows the colon.
std::string_view frontMatterKey(std::string_view line, std::string_view& rest) {
  const auto colon = line.find(':');
  if (colon == std::string_view::npos || colon == 0) return {};
  if (colon + 1 < line.size() && line[colon + 1] != ' ') return {};
  const auto key = line.substr(0, colon);
  if (!hasOnlyFrontMatterKeyBytes(key)) return {};
  rest = trim(line.substr(colon + 1));
  return key;
}

}  // namespace

std::string renderMarkdown(std::string_view source, const LinkContext& context,
                           std::vector<MarkdownHeading>* outline, const MarkdownCodeRenderer& code_renderer) {
  if (source.size() > kMaximumMarkdownInputBytes) throw std::length_error("Markdown input exceeds 512 KiB");
  if (outline != nullptr) outline->clear();
  Lines lines;
  LineNumbers numbers;
  for (std::size_t start = 0; start < source.size();) {
    const auto newline = source.find('\n', start);
    auto line = source.substr(start, newline == std::string_view::npos ? source.size() - start : newline - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.emplace_back(line);
    numbers.push_back({context.first_line + numbers.size(), 0});
    if (newline == std::string_view::npos) break;
    start = newline + 1;
  }
  return Renderer(context, source.size(), outline, code_renderer).render(std::move(lines), std::move(numbers));
}

bool isValidFrontMatterKey(std::string_view key) {
  return !key.empty() && key.size() <= kMaximumFrontMatterKeyBytes && hasOnlyFrontMatterKeyBytes(key);
}

std::optional<FrontMatter> splitFrontMatter(std::string_view source) {
  // Physical lines up to the closing fence, without newlines or a trailing CR;
  // lines[0] is the opening fence.
  std::vector<std::string_view> lines;
  std::size_t body_offset = std::string_view::npos;
  for (std::size_t start = 0; start <= source.size();) {
    const auto newline = source.find('\n', start);
    const auto end = newline == std::string_view::npos ? source.size() : newline;
    auto line = source.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (lines.empty()) {
      if (!fenceLine(line, "---")) return {};
    } else {
      if (end > kMaximumFrontMatterBytes) return {};
      if (fenceLine(line, "---") || fenceLine(line, "...")) {
        body_offset = newline == std::string_view::npos ? source.size() : newline + 1;
        break;
      }
      if (lines.size() > kMaximumFrontMatterLines) return {};
    }
    lines.push_back(line);
    if (newline == std::string_view::npos) break;
    start = newline + 1;
  }
  if (body_offset == std::string_view::npos) return {};

  // Every line must look like YAML (a flat entry, an indented continuation, a
  // list item, a comment, or blank), or this is a document that merely starts
  // with a thematic break.
  FrontMatter result;
  result.body_offset = body_offset;
  std::string block = "\n";
  std::string_view pending_key;  // a `key:` with nothing after it, awaiting its block
  std::size_t pending_line = 0;
  bool any = false;
  for (std::size_t number = 1; number < lines.size(); ++number) {
    const auto line = lines[number];
    block.append(line);
    block.push_back('\n');
    const auto content = trim(line);
    if (content.empty() || content.front() == '#') continue;
    any = true;
    const bool continuation = line.front() == ' ' || line.front() == '\t' || line.starts_with("- ") || line == "-";
    if (!pending_key.empty() && !continuation && result.error.empty()) {
      result.error = "front matter: '" + std::string(pending_key) + "' has no value (line " +
                     std::to_string(pending_line + 1) + ")";
    }
    pending_key = {};
    if (continuation) continue;
    std::string_view rest;
    const auto key = frontMatterKey(line, rest);
    if (key.empty()) return {};
    if (rest.empty()) {
      pending_key = key;
      pending_line = number;
    }
  }
  if (!pending_key.empty() && result.error.empty()) {
    result.error = "front matter: '" + std::string(pending_key) + "' has no value (line " +
                   std::to_string(pending_line + 1) + ")";
  }
  if (!result.error.empty() || !any) return result;
  try {
    result.structured = parseYamlSubset(block, kFrontMatterYamlDialect);
    for (const auto& [key, value] : result.structured.entries) {
      if (value.kind == YamlNode::Kind::Scalar) result.entries.emplace_back(key, value.scalar);
    }
  } catch (const std::exception& error) {
    result.error = error.what();
  }
  return result;
}

std::string_view markdownBody(std::string_view source) {
  const auto front = splitFrontMatter(source);
  return front ? source.substr(front->body_offset) : source;
}

}  // namespace ckgit
