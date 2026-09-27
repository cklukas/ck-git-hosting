// libcworks — YAML-subset parser (shared across CK Office)
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cworks/yaml.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>

#include "cworks/app_error.hpp"
#include "cworks/error.hpp"

namespace cworks {

namespace {

void poll(const std::function<void()>& checkpoint) {
    if (checkpoint) checkpoint();
}


struct Line {
    int indent = 0;
    std::string content; // trimmed, comments removed
    int number = 0;      // 1-based source line
    std::size_t source_begin = 0; // first byte of content in the original document
    std::size_t source_end = 0;   // one past content in the original document
};

/// A document that will not parse as YAML. This parser knows only the
/// position; the caller knows the document kind, so the code claims exactly
/// the parser's knowledge: the text is not the format it was handed as
/// (InvalidFormat — the suite's code for every won't-parse reader, matching
/// ctable's CSV/JSON/Parquet). Content that parses but says something wrong
/// is the caller's schema check, and validation_failed, one layer up.
[[noreturn]] void fail(const std::string& file, int line, const std::string& message) {
    AppError e = invalid_format(file);
    e.summary = "yaml error: " + message + "\n  " + file + ":" + std::to_string(line);
    throw Error(std::move(e));
}

/// Tracks whether a character lies inside a quoted scalar, so the structural
/// characters of a line (a comment `#`, a flow `,`, a key's `:`) are only
/// recognized where they are structure and not where they are text.
///
/// It knows about escapes, which matters because the writer emits them: text
/// carrying both quote characters is written double-quoted with `\"` inside
/// (see emit_scalar). A scanner that treated that as a closing quote would
/// leave the string early and then read the rest of the value as structure —
/// trimming it at a `#`, or splitting a key at a `:` that is part of a
/// sentence. Backslash escapes apply only inside double quotes; YAML's
/// single-quoted style escapes by doubling the quote, which the toggle
/// handles on its own.
struct QuoteScan {
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;

    void consume(char c) {
        if (escaped) { escaped = false; return; }
        if (in_double && c == '\\') { escaped = true; return; }
        if (c == '\'' && !in_double) { in_single = !in_single; return; }
        if (c == '"' && !in_single) { in_double = !in_double; }
    }

    bool inside() const { return in_single || in_double; }
};

/// YAML's double-quoted style, escaping what that style escapes. The reader
/// undoes it (parse_scalar_text) and the line scanners know about it
/// (QuoteScan), so such a scalar round-trips verbatim.
std::string escaped_double_quoted(const std::string& scalar) {
    std::string out = "\"";
    out.reserve(scalar.size() + 8);
    for (const char c : scalar) {
        switch (c) {
        // A newline inside a scalar is the dangerous one: written literally
        // it ends the line, and every following line of the value is read as
        // a fresh key — the file parses as garbage or not at all.
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

std::string strip_comment(const std::string& s, const std::function<void()>& checkpoint) {
    QuoteScan scan;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i % 1024 == 0) poll(checkpoint);
        const char c = s[i];
        scan.consume(c);
        if (c == '#' && !scan.inside() &&
            (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
            return s.substr(0, i);
    }
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

YamlNode parse_scalar_text(std::string text, const std::string& file, int line,
                           std::size_t source_begin,
                           const std::function<void()>& checkpoint = {});
std::size_t find_key_colon(const std::string& s, const std::function<void()>& checkpoint);

struct FlowPart {
    std::string text;
    std::size_t begin = 0;
};

/// Split "a, b, c" on top-level commas (respecting quotes).
std::vector<FlowPart> split_flow(const std::string& s, const std::string& file, int line,
                                 const std::function<void()>& checkpoint) {
    std::vector<FlowPart> parts;
    std::string cur;
    std::size_t part_begin = 0;
    QuoteScan scan;
    int depth = 0;
    for (std::size_t at = 0; at < s.size(); ++at) {
        if (at % 1024 == 0) poll(checkpoint);
        const char c = s[at];
        scan.consume(c);
        if (!scan.inside()) {
            if (c == '[' || c == '{') ++depth;
            if (c == ']' || c == '}') --depth;
            if (c == ',' && depth == 0) {
                const std::string trimmed = trim(cur);
                if (!trimmed.empty()) {
                    const std::size_t leading = cur.find_first_not_of(" \t\r");
                    parts.push_back({trimmed, part_begin + leading});
                }
                cur.clear();
                part_begin = at + 1;
                continue;
            }
        }
        cur += c;
    }
    if (scan.inside()) fail(file, line, "unterminated quote");
    const std::string last = trim(cur);
    if (!last.empty()) {
        const std::size_t leading = cur.find_first_not_of(" \t\r");
        parts.push_back({last, part_begin + leading});
    }
    return parts;
}

YamlNode parse_scalar_text(std::string text, const std::string& file, int line,
                           std::size_t source_begin,
                           const std::function<void()>& checkpoint) {
    poll(checkpoint);
    YamlNode n;
    n.line = line;
    const std::size_t leading = text.find_first_not_of(" \t\r");
    if (leading != std::string::npos) source_begin += leading;
    text = trim(text);
    n.source_begin = source_begin;
    n.source_end = source_begin + text.size();
    if (text.empty() || text == "~" || text == "null") {
        n.type = YamlNode::Type::Null;
        return n;
    }
    if (text.front() == '[') {
        if (text.back() != ']') fail(file, line, "unterminated flow list");
        n.type = YamlNode::Type::List;
        for (const FlowPart& part : split_flow(text.substr(1, text.size() - 2), file, line,
                                               checkpoint)) {
            n.list.push_back(parse_scalar_text(
                part.text, file, line, source_begin + 1 + part.begin, checkpoint));
        }
        return n;
    }
    if (text.front() == '{') {
        if (text.back() != '}') fail(file, line, "unterminated flow mapping");
        n.type = YamlNode::Type::Map;
        for (const FlowPart& part : split_flow(text.substr(1, text.size() - 2), file, line,
                                               checkpoint)) {
            const std::size_t colon = find_key_colon(part.text, checkpoint);
            if (colon == std::string::npos)
                fail(file, line, "expected 'key: value' inside {...}, got '" + part.text + "'");
            std::string key = trim(part.text.substr(0, colon));
            if (key.size() >= 2 && ((key.front() == '"' && key.back() == '"') ||
                                    (key.front() == '\'' && key.back() == '\'')))
                key = key.substr(1, key.size() - 2);
            for (const auto& [k, v] : n.map) {
                poll(checkpoint);
                (void)v;
                if (k == key) fail(file, line, "duplicate key '" + key + "' in {...}");
            }
            n.map.emplace_back(std::move(key),
                               parse_scalar_text(part.text.substr(colon + 1), file, line,
                                                 source_begin + 1 + part.begin + colon + 1,
                                                 checkpoint));
        }
        return n;
    }
    n.type = YamlNode::Type::Scalar;
    if (text.size() >= 2 &&
        ((text.front() == '"' && text.back() == '"') ||
         (text.front() == '\'' && text.back() == '\''))) {
        n.scalar = text.substr(1, text.size() - 2);
        n.quoted = true;
        // Undo the writer's escapes for the double-quoted style.
        if (text.front() == '"' && n.scalar.find('\\') != std::string::npos) {
            std::string out;
            out.reserve(n.scalar.size());
            for (std::size_t i = 0; i < n.scalar.size(); ++i) {
                if (i % 1024 == 0) poll(checkpoint);
                if (n.scalar[i] != '\\' || i + 1 >= n.scalar.size()) {
                    out.push_back(n.scalar[i]);
                    continue;
                }
                switch (n.scalar[++i]) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                // Not an escape this writer emits: keep both characters, so
                // text that merely contains a backslash survives.
                default: out.push_back('\\'); out.push_back(n.scalar[i]); break;
                }
            }
            n.scalar = out;
        }
    } else {
        n.scalar = text;
    }
    return n;
}

/// Find "key: value" split point outside quotes; returns npos if none.
std::size_t find_key_colon(const std::string& s, const std::function<void()>& checkpoint) {
    QuoteScan scan;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i % 1024 == 0) poll(checkpoint);
        const char c = s[i];
        scan.consume(c);
        if (c == ':' && !scan.inside()) {
            if (i + 1 >= s.size() || s[i + 1] == ' ' || s[i + 1] == '\t') return i;
        }
    }
    return std::string::npos;
}

class Parser {
public:
    Parser(std::vector<Line> lines, std::string file, const std::function<void()>& checkpoint)
        : lines_(std::move(lines)), file_(std::move(file)), checkpoint_(checkpoint) {}

    YamlNode parse_block(std::size_t& i, int min_indent) {
        poll(checkpoint_);
        if (i >= lines_.size() || lines_[i].indent < min_indent) return YamlNode{};
        const int indent = lines_[i].indent;
        if (lines_[i].content.rfind("- ", 0) == 0 || lines_[i].content == "-") {
            return parse_list(i, indent);
        }
        return parse_map(i, indent);
    }

private:
    YamlNode parse_map(std::size_t& i, int indent) {
        YamlNode node;
        node.type = YamlNode::Type::Map;
        node.line = lines_[i].number;
        node.source_begin = lines_[i].source_begin;
        while (i < lines_.size() && lines_[i].indent == indent) {
            poll(checkpoint_);
            const Line& ln = lines_[i];
            if (ln.content.rfind("- ", 0) == 0)
                fail(file_, ln.number, "unexpected list item inside mapping");
            const std::size_t colon = find_key_colon(ln.content, checkpoint_);
            if (colon == std::string::npos)
                fail(file_, ln.number, "expected 'key: value', got '" + ln.content + "'");
            std::string key = trim(ln.content.substr(0, colon));
            if (key.size() >= 2 && ((key.front() == '"' && key.back() == '"') ||
                                    (key.front() == '\'' && key.back() == '\'')))
                key = key.substr(1, key.size() - 2);
            const std::string raw_rest = ln.content.substr(colon + 1);
            const std::string rest = trim(raw_rest);
            ++i;
            YamlNode value;
            if (!rest.empty()) {
                value = parse_scalar_text(raw_rest, file_, ln.number,
                                          ln.source_begin + colon + 1, checkpoint_);
            } else if (i < lines_.size() && lines_[i].indent > indent) {
                value = parse_block(i, indent + 1);
            } else if (i < lines_.size() && lines_[i].indent == indent &&
                       (lines_[i].content.rfind("- ", 0) == 0 ||
                        lines_[i].content == "-")) {
                // Standard YAML: a block list may sit at the same
                // indentation as its mapping key —
                //   steps:
                //   - filter: amount > 0
                value = parse_list(i, indent);
            } // else null
            value.line = ln.number;
            for (const auto& [k, v] : node.map) {
                poll(checkpoint_);
                (void)v;
                if (k == key) fail(file_, ln.number, "duplicate key '" + key + "'");
            }
            node.map.emplace_back(std::move(key), std::move(value));
            node.source_end = node.map.back().second.has_source_range()
                ? node.map.back().second.source_end
                : ln.source_end;
        }
        if (i < lines_.size() && lines_[i].indent > indent)
            fail(file_, lines_[i].number, "unexpected indentation");
        return node;
    }

    YamlNode parse_list(std::size_t& i, int indent) {
        YamlNode node;
        node.type = YamlNode::Type::List;
        node.line = lines_[i].number;
        node.source_begin = lines_[i].source_begin;
        while (i < lines_.size() && lines_[i].indent == indent &&
               (lines_[i].content.rfind("- ", 0) == 0 || lines_[i].content == "-")) {
            poll(checkpoint_);
            const Line& ln = lines_[i];
            const std::string raw_rest =
                ln.content == "-" ? std::string{} : ln.content.substr(2);
            const std::string rest = trim(raw_rest);
            const std::size_t rest_leading = raw_rest.find_first_not_of(" \t\r");
            const std::size_t rest_begin = ln.source_begin + 2 +
                (rest_leading == std::string::npos ? 0 : rest_leading);
            if (rest.empty()) {
                ++i;
                if (i < lines_.size() && lines_[i].indent > indent) {
                    node.list.push_back(parse_block(i, indent + 1));
                } else {
                    node.list.push_back(YamlNode{});
                }
                node.source_end = node.list.back().has_source_range()
                    ? node.list.back().source_end
                    : ln.source_end;
                continue;
            }
            if (rest.front() == '{' || rest.front() == '[') {
                // Flow item: "- {k: v}" / "- [a, b]" — the colon inside
                // belongs to the flow syntax, not to a block mapping.
                node.list.push_back(parse_scalar_text(raw_rest, file_, ln.number,
                                                      ln.source_begin + 2, checkpoint_));
                ++i;
                node.source_end = node.list.back().source_end;
                continue;
            }
            const std::size_t colon = find_key_colon(rest, checkpoint_);
            if (colon != std::string::npos) {
                // Map item: "- key: value" plus continuation lines indented
                // beyond the dash column.
                const int item_indent = indent + 2;
                // Rewrite this line as the first line of the item map.
                lines_[i].indent = item_indent;
                lines_[i].content = rest;
                lines_[i].source_begin = rest_begin;
                YamlNode item = parse_map(i, item_indent);
                node.list.push_back(std::move(item));
            } else {
                node.list.push_back(parse_scalar_text(raw_rest, file_, ln.number,
                                                      ln.source_begin + 2, checkpoint_));
                ++i;
            }
            node.source_end = node.list.back().has_source_range()
                ? node.list.back().source_end
                : ln.source_end;
        }
        return node;
    }

    std::vector<Line> lines_;
    std::string file_;
    const std::function<void()>& checkpoint_;
};

} // namespace

double YamlNode::as_double(bool* ok) const {
    if (ok) *ok = false;
    if (type != Type::Scalar) return 0.0;
    char* end = nullptr;
    const double v = std::strtod(scalar.c_str(), &end);
    if (end == scalar.c_str() || *end != '\0') return 0.0;
    if (ok) *ok = true;
    return v;
}

bool YamlNode::as_bool(bool* ok) const {
    if (ok) *ok = true;
    if (scalar == "true" || scalar == "yes" || scalar == "on" || scalar == "1") return true;
    if (scalar == "false" || scalar == "no" || scalar == "off" || scalar == "0") return false;
    if (ok) *ok = false;
    return false;
}

YamlNode parse_yaml(const std::string& text, const std::string& filename,
                    const std::function<void()>& checkpoint) {
    poll(checkpoint);
    std::vector<Line> lines;
    std::istringstream in(text);
    std::string raw;
    int number = 0;
    std::size_t document_offset = 0;
    while (std::getline(in, raw)) {
        poll(checkpoint);
        ++number;
        if (raw.find('\t') != std::string::npos) {
            const std::string before_tab = raw.substr(0, raw.find('\t'));
            if (trim(before_tab).empty())
                fail(filename, number, "tabs are not allowed for indentation");
        }
        const std::string no_comment = strip_comment(raw, checkpoint);
        int indent = 0;
        while (indent < static_cast<int>(no_comment.size()) &&
               no_comment[static_cast<std::size_t>(indent)] == ' ')
            ++indent;
        const std::string content = trim(no_comment);
        if (!content.empty() && content != "---") {
            const std::size_t leading = no_comment.find_first_not_of(" \t\r");
            lines.push_back({indent, content, number, document_offset + leading,
                             document_offset + leading + content.size()});
        }
        document_offset += raw.size() + 1;
    }
    if (lines.empty()) return YamlNode{};
    // A single-line flow document ({k: v} or [a, b]) is valid YAML —
    // the form dump_yaml_flow produces and edit fields hand back.
    if (lines.size() == 1 &&
        (lines[0].content.front() == '{' || lines[0].content.front() == '['))
        return parse_scalar_text(lines[0].content, filename, lines[0].number,
                                 lines[0].source_begin, checkpoint);
    std::size_t i = 0;
    Parser parser(std::move(lines), filename, checkpoint);
    return parser.parse_block(i, 0);
}

namespace {

std::string flow_scalar(const YamlNode& node) {
    bool needs_quote = node.quoted || node.scalar.empty() || node.scalar == "null" ||
                       node.scalar == "~";
    // A leading or trailing space is stripped on reparse, so it must be
    // quoted to round-trip.
    if (!node.scalar.empty() &&
        (node.scalar.front() == ' ' || node.scalar.back() == ' '))
        needs_quote = true;
    bool has_double = false;
    bool has_single = false;
    bool has_control = false;
    for (const char c : node.scalar) {
        if (c == ':' || c == '#' || c == '[' || c == ']' || c == '{' || c == '}' || c == ',')
            needs_quote = true;
        if (c == '"') has_double = true;
        if (c == '\'') has_single = true;
        if (c == '\n' || c == '\r' || c == '\t') has_control = true;
    }
    // Text carrying both quote characters cannot be written in either plain
    // quoting style, so it takes YAML's double-quoted style WITH escapes —
    // the style that exists for exactly this. Refusing it instead would make
    // ordinary content (a page of recognized text quoting someone) an error.
    if ((has_double && has_single) || has_control)
        return escaped_double_quoted(node.scalar);
    if (has_single) needs_quote = true;
    if (!needs_quote) return node.scalar;
    if (has_double) return "'" + node.scalar + "'";
    return "\"" + node.scalar + "\"";
}

std::string map_key(const std::string& key) {
    bool needs_quote = key.empty() ||
                       key.front() == ' ' || key.back() == ' ' ||
                       key == "-" || key.rfind("- ", 0) == 0;
    bool has_double = false;
    bool has_single = false;
    for (const char c : key) {
        if (c == ':' || c == '#' || c == '[' || c == ']' || c == '{' ||
            c == '}' || c == ',' || c == '\t' || c == '\r' || c == '\n') {
            needs_quote = true;
        }
        if (c == '"') has_double = true;
        if (c == '\'') has_single = true;
    }
    if (key.find('\n') != std::string::npos || key.find('\r') != std::string::npos) {
        throw Error(unsupported("cannot serialize a map key containing a line break: " + key));
    }
    if (has_double && has_single) {
        throw Error(unsupported("cannot serialize a map key containing both \" and ': " + key));
    }
    if (has_double) return "'" + key + "'";
    if (has_single) return "\"" + key + "\"";
    return needs_quote ? "\"" + key + "\"" : key;
}

} // namespace

std::string dump_yaml_flow(const YamlNode& node) {
    switch (node.type) {
    case YamlNode::Type::Null: return "null";
    case YamlNode::Type::Scalar: return flow_scalar(node);
    case YamlNode::Type::List: {
        std::string out = "[";
        for (std::size_t i = 0; i < node.list.size(); ++i) {
            if (i) out += ", ";
            out += dump_yaml_flow(node.list[i]);
        }
        return out + "]";
    }
    case YamlNode::Type::Map: {
        std::string out = "{";
        bool first = true;
        for (const auto& [key, value] : node.map) {
            if (!first) out += ", ";
            first = false;
            out += map_key(key) + ": " + dump_yaml_flow(value);
        }
        return out + "}";
    }
    }
    return "null";
}

std::string dump_yaml(const YamlNode& node, int indent) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    std::string out;
    switch (node.type) {
    case YamlNode::Type::Null:
        out = "null";
        break;
    case YamlNode::Type::Scalar: {
        // node.quoted forces quotes: a TEXT scalar like "null" or
        // "123" must not reparse as null or a number.
        bool needs_quote = node.quoted || node.scalar.empty() ||
                           node.scalar == "null" || node.scalar == "~";
        // A leading/trailing space is trimmed on reparse, so quote it
        // to round-trip (mirrors flow_scalar).
        if (!node.scalar.empty() &&
            (node.scalar.front() == ' ' || node.scalar.back() == ' '))
            needs_quote = true;
        bool has_double = false;
        bool has_single = false;
        bool has_control = false;
        for (const char c : node.scalar) {
            if (c == ':' || c == '#' || c == '[' || c == ']') needs_quote = true;
            if (c == '"') has_double = true;
            if (c == '\'') has_single = true;
            if (c == '\n' || c == '\r' || c == '\t') has_control = true;
        }
        // Text carrying both quote characters takes the double-quoted style
        // WITH escapes — the style that exists for it. Refusing it would make
        // ordinary content (recognized text that quotes someone) an error.
        if ((has_double && has_single) || has_control) {
            out = escaped_double_quoted(node.scalar);
            break;
        }
        // Otherwise quoted scalars reparse verbatim, so a scalar containing
        // double quotes wraps in single quotes.
        if (has_double) out = "'" + node.scalar + "'";
        else out = needs_quote ? "\"" + node.scalar + "\"" : node.scalar;
        break;
    }
    case YamlNode::Type::Map: {
        if (node.map.empty()) {
            out = pad + "{}\n";
            break;
        }
        for (const auto& [k, v] : node.map) {
            out += pad + map_key(k) + ":";
            if (v.is_map() && v.map.empty()) {
                out += " {}\n";
            } else if (v.is_list() && v.list.empty()) {
                out += " []\n";
            } else if (v.is_map() ||
                       (v.is_list() && !v.list[0].is_scalar())) {
                out += "\n" + dump_yaml(v, indent + 2);
            } else if (v.is_list()) {
                out += " [";
                for (std::size_t i = 0; i < v.list.size(); ++i) {
                    if (i) out += ", ";
                    out += flow_scalar(v.list[i]); // quote elements that need it
                }
                out += "]\n";
            } else {
                out += " " + dump_yaml(v, 0) + "\n";
            }
        }
        break;
    }
    case YamlNode::Type::List: {
        if (node.list.empty()) {
            out = pad + "[]\n";
            break;
        }
        for (const auto& item : node.list) {
            if (item.is_map() && item.map.empty()) {
                out += pad + "- {}\n";
            } else if (item.is_list() && item.list.empty()) {
                out += pad + "- []\n";
            } else if (item.is_map()) {
                std::string body = dump_yaml(item, indent + 2);
                // Replace the first two spaces of the first line with "- ".
                if (body.size() > static_cast<std::size_t>(indent) + 2) {
                    out += pad + "- " + body.substr(static_cast<std::size_t>(indent) + 2);
                }
            } else {
                out += pad + "- " + dump_yaml(item, 0) + "\n";
            }
        }
        break;
    }
    }
    return out;
}

void set_path(YamlNode& root, const std::string& path, const std::string& value) {
    YamlNode* node = &root;
    std::size_t pos = 0;
    while (pos < path.size()) {
        std::size_t dot = path.find('.', pos);
        if (dot == std::string::npos) dot = path.size();
        const std::string seg = path.substr(pos, dot - pos);
        if (seg.empty())
            throw Error(validation_failed("set_path: empty path segment in '" + path + "'"));
        const bool is_index = std::isdigit(static_cast<unsigned char>(seg[0])) != 0;
        if (is_index) {
            const std::size_t idx = static_cast<std::size_t>(std::atol(seg.c_str()));
            if (!node->is_list()) {
                node->type = YamlNode::Type::List;
            }
            while (node->list.size() <= idx) node->list.emplace_back();
            node = &node->list[idx];
        } else {
            if (!node->is_map()) node->type = YamlNode::Type::Map;
            node = &node->insert(seg);
        }
        pos = dot + 1;
    }
    *node = parse_scalar_text(value, "--set", 0, 0);
}

} // namespace cworks
