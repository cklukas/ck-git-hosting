// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/markdown.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ckgit/http_router.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {

using Lines = std::vector<std::string>;

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

std::optional<std::string> linkTarget(std::string_view original, bool image, const LinkContext& context) {
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
  if (!isValidProjectName(context.project) || !isObjectId(context.commit_id)) return {};
  const auto decoded = decodeUrl(std::string_view(value).substr(0, hash));
  if (!decoded || decoded->empty()) return {};
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
  const bool directory = path.empty() || decoded->back() == '/' || *decoded == "." || *decoded == ".." ||
                         decoded->ends_with("/.") || decoded->ends_with("/..");
  const auto route = image ? "/raw/" : directory ? "/tree/" : "/blob/";
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

struct Fence { char marker; std::size_t length; std::string language; };

std::optional<Fence> fenceStart(std::string_view line) {
  if (indentation(line) > 3) return {};
  line = trim(line);
  if (line.empty() || (line.front() != '`' && line.front() != '~')) return {};
  std::size_t count = 0;
  while (count < line.size() && line[count] == line.front()) ++count;
  if (count < 3) return {};
  auto info = trim(line.substr(count));
  if (line.front() == '`' && info.find('`') != std::string_view::npos) return {};
  info = info.substr(0, info.find_first_of(" \t"));
  std::string language;
  if (info.size() <= 32 && std::all_of(info.begin(), info.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
               (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
      })) language = info;
  return Fence{line.front(), count, language};
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

class Renderer {
 public:
  Renderer(const LinkContext& context, std::size_t input_size)
      : context_(context), work_(input_size * 64 + 4096) {}

  std::string blocks(Lines lines, std::size_t depth = 0, bool tight = false) {
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
        const auto indent = indentation(lines[index++]);
        append(output, "<pre><code");
        if (!fence->language.empty()) append(output, " class=\"language-" + fence->language + "\"");
        append(output, ">");
        while (index < lines.size()) {
          const auto closing = trim(lines[index]);
          std::size_t count = 0;
          while (count < closing.size() && closing[count] == fence->marker) ++count;
          if (indentation(lines[index]) <= 3 && count >= fence->length && trim(closing.substr(count)).empty()) { ++index; break; }
          escape(output, removeIndent(lines[index++], indent));
          append(output, "\n");
        }
        append(output, "</code></pre>\n");
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
        auto text = trim(lines[index++]);
        text = trim(text.substr(level));
        std::size_t closing = text.size();
        while (closing > 0 && text[closing - 1] == '#') --closing;
        if (closing < text.size() && (closing == 0 || text[closing - 1] == ' ' || text[closing - 1] == '\t')) text = trim(text.substr(0, closing));
        const auto visible = withoutComments(text);
        const auto rendered = inlineText(visible, 0);
        append(output, "<h" + std::to_string(level) + " id=\"" + escaped(headingId(rendered)) + "\">");
        append(output, rendered);
        append(output, "</h" + std::to_string(level) + ">\n");
      } else if (thematic(lines[index])) {
        append(output, "<hr>\n");
        ++index;
      } else if (trim(lines[index]).starts_with('>')) {
        Lines quoted;
        while (index < lines.size() && indentation(lines[index]) <= 3 && trim(lines[index]).starts_with('>')) {
          auto line = trimLeft(lines[index++]);
          line.remove_prefix(1);
          if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
          quoted.emplace_back(line);
        }
        append(output, "<blockquote>\n");
        append(output, blocks(quoted, depth + 1));
        append(output, "</blockquote>\n");
      } else if (const auto marker = listMarker(lines[index])) {
        const bool ordered = marker->ordered;
        append(output, ordered ? "<ol" : "<ul");
        if (ordered && marker->start != "1") append(output, " start=\"" + marker->start + "\"");
        append(output, ">\n");
        while (index < lines.size()) {
          const auto current = listMarker(lines[index]);
          if (!current || current->ordered != ordered || current->indent != marker->indent) break;
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
              ++index;
              continue;
            }
            const auto next = listMarker(lines[index]);
            if (next && next->indent == marker->indent) break;
            if (indentation(lines[index]) <= marker->indent && beginsBlock(lines[index])) break;
            item.push_back(removeIndent(lines[index++], current->content_indent));
          }
          append(output, "<li>");
          append(output, blocks(item, depth + 1, !loose));
          append(output, "</li>\n");
        }
        append(output, ordered ? "</ol>\n" : "</ul>\n");
      } else if (index + 1 < lines.size() && lines[index].find('|') != std::string::npos && tableAlignment(lines[index + 1])) {
        const auto alignments = *tableAlignment(lines[index + 1]);
        const auto headers = tableCells(lines[index]);
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
          tableRow(output, tableCells(lines[index++]), alignments, false);
          append(output, "</tr>\n");
        }
        append(output, "</tbody></table>\n");
      } else {
        std::string paragraph = lines[index++];
        while (index < lines.size() && !trim(lines[index]).empty() && !beginsBlock(lines[index])) {
          if (index + 1 < lines.size() && lines[index].find('|') != std::string::npos && tableAlignment(lines[index + 1])) break;
          append(paragraph, "\n");
          append(paragraph, lines[index++]);
        }
        const auto visible = withoutComments(paragraph);
        if (!trim(visible).empty()) {
          if (!tight) append(output, "<p>");
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
  std::map<std::string, std::size_t> heading_counts_;
  std::set<std::string> heading_ids_;
  bool comment_open_{false};

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
    return headingLevel(line) != 0 || thematic(line) || fenceStart(line).has_value() ||
           listMarker(line).has_value() || (indentation(line) <= 3 && trim(line).starts_with('>'));
  }

  std::string headingId(std::string_view rendered) {
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
    for (const unsigned char byte : plain) {
      if (byte == ' ' || byte == '\t' || byte == '\n') space = !result.empty();
      else if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte >= 0x80 || byte == '-' || byte == '_') {
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
    return candidate;
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
      const bool image = value[index] == '!' && index + 1 < value.size() && value[index + 1] == '[';
      if (allow_links && (value[index] == '[' || image)) {
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
            const auto url = valid_title ? linkTarget(destination, image, context_) : std::nullopt;
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
      if (value[index] == '<' && allow_links) {
        const auto end = find(value, ">", index + 1);
        if (end != std::string_view::npos) {
          const auto candidate = value.substr(index + 1, end - index - 1);
          const auto colon = candidate.find(':');
          const bool email = colon == std::string_view::npos && candidate.find('@') != std::string_view::npos && candidate.find_first_of(" \t\n<>\"") == std::string_view::npos;
          const auto url = (colon != std::string_view::npos || email) ? linkTarget(email ? "mailto:" + std::string(candidate) : std::string(candidate), false, context_) : std::nullopt;
          if (url) {
            append(output, "<a href=\""); escape(output, *url); append(output, "\">"); escape(output, candidate); append(output, "</a>");
            index = end + 1;
            continue;
          }
        }
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
    return output;
  }
};

}  // namespace

std::string renderMarkdown(std::string_view source, const LinkContext& context) {
  if (source.size() > kMaximumMarkdownInputBytes) throw std::length_error("Markdown input exceeds 512 KiB");
  Lines lines;
  for (std::size_t start = 0; start < source.size();) {
    const auto newline = source.find('\n', start);
    auto line = source.substr(start, newline == std::string_view::npos ? source.size() - start : newline - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.emplace_back(line);
    if (newline == std::string_view::npos) break;
    start = newline + 1;
  }
  return Renderer(context, source.size()).blocks(lines);
}

}  // namespace ckgit
