// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "links.hpp"

#include <cctype>

namespace cdiagram::detail {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t'; }

/// Trim ASCII whitespace from both ends of `s`.
std::string_view trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

void skip_spaces(std::string_view s, std::size_t& i) {
    while (i < s.size() && is_space(s[i])) ++i;
}

/// Read a whitespace-delimited word starting at `i`. Advances `i` past it.
std::string read_word(std::string_view s, std::size_t& i) {
    const std::size_t start = i;
    while (i < s.size() && !is_space(s[i])) ++i;
    return std::string(s.substr(start, i - start));
}

/// If `s[i]` opens a double-quoted string, read its contents into `out` (no
/// escape processing — Mermaid quotes are literal), advance `i` past the
/// closing quote, and return true. Otherwise leave `i` and return false.
bool read_quoted(std::string_view s, std::size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    const std::size_t start = ++i;
    while (i < s.size() && s[i] != '"') ++i;
    out.assign(s.substr(start, i - start));
    if (i < s.size()) ++i; // consume closing quote
    return true;
}

bool is_target_token(const std::string& tok) {
    return tok.size() > 1 && tok.front() == '_';
}

/// After a URL has been read at `i`, consume an optional trailing tooltip
/// (a quoted string) and/or link target (a `_…` token) in either order.
void read_tooltip_and_target(std::string_view s, std::size_t& i, LinkDirective& out) {
    for (int pass = 0; pass < 2; ++pass) {
        skip_spaces(s, i);
        if (i >= s.size()) return;
        if (s[i] == '"') {
            std::string tt;
            if (read_quoted(s, i, tt) && out.title.empty()) out.title = std::move(tt);
        } else {
            const std::size_t save = i;
            const std::string tok = read_word(s, i);
            if (is_target_token(tok)) {
                if (out.target.empty()) out.target = tok;
            } else {
                i = save; // not a target — stop
                return;
            }
        }
    }
}

/// Extract the first `"key": "value"` pair from a JSON-object string (the
/// sequence `links` menu form). Only the first pair is used — the suite renders
/// one link per element, not a menu.
bool read_first_json_pair(std::string_view s, std::string& key, std::string& value) {
    std::size_t i = 0;
    while (i < s.size() && s[i] != '"') ++i;
    if (!read_quoted(s, i, key)) return false;
    while (i < s.size() && s[i] != ':') ++i;
    if (i >= s.size()) return false;
    ++i;
    while (i < s.size() && s[i] != '"') ++i;
    if (!read_quoted(s, i, value)) return false;
    return !value.empty();
}

/// Parse the `<url> ["tooltip"] [_target]` tail that follows `click <ref>`,
/// `click <ref> href`, or class `link <ref>`. `i` is positioned at the URL.
bool read_url_tail(std::string_view s, std::size_t& i, LinkDirective& out) {
    skip_spaces(s, i);
    std::string url;
    if (read_quoted(s, i, url)) {
        out.href = std::move(url);
    } else {
        // Bare (unquoted) URL up to the next space — tolerated for robustness.
        out.href = read_word(s, i);
        if (out.href.empty()) return false;
    }
    read_tooltip_and_target(s, i, out);
    return true;
}

} // namespace

bool is_click_statement(std::string_view line) {
    line = trim(line);
    return line == "click" || (line.size() > 5 && line.rfind("click", 0) == 0 &&
                               is_space(line[5]));
}

bool parse_click_statement(std::string_view line, std::string& ref, LinkDirective& out) {
    line = trim(line);
    if (!is_click_statement(line)) return false;
    std::size_t i = 5; // past "click"
    skip_spaces(line, i);
    // The element ref is a bare token, or a quoted string so labels with spaces
    // (kanban cards, timeline events, journey tasks) stay addressable.
    if (i < line.size() && line[i] == '"') {
        if (!read_quoted(line, i, ref)) return false;
    } else {
        ref = read_word(line, i);
    }
    if (ref.empty()) return false;
    skip_spaces(line, i);
    out = LinkDirective{};
    if (i >= line.size()) return false; // `click <ref>` with no action
    if (line[i] == '"') return read_url_tail(line, i, out); // bare-url form
    const std::size_t save = i;
    const std::string word = read_word(line, i);
    if (word == "href") return read_url_tail(line, i, out); // explicit href form
    if (word == "call") {                                   // `call cb()` callback
        out.callback = true;
        return true;
    }
    // A bare word is a callback function name (`click X cb`); no static URL.
    (void)save;
    out.callback = true;
    return true;
}

bool parse_class_link_statement(std::string_view line, std::string& ref, LinkDirective& out) {
    line = trim(line);
    std::size_t i = 0;
    const std::string kw = read_word(line, i);
    const bool is_link = kw == "link";
    const bool is_callback = kw == "callback";
    if (!is_link && !is_callback) return false;
    skip_spaces(line, i);
    ref = read_word(line, i);
    if (ref.empty()) return false;
    // `link Actor: … @ …` is sequence syntax, not a class link.
    if (!ref.empty() && ref.back() == ':') return false;
    skip_spaces(line, i);
    out = LinkDirective{};
    if (is_callback) {
        out.callback = true;
        return true;
    }
    if (i >= line.size()) return false;
    return read_url_tail(line, i, out);
}

bool parse_sequence_link_statement(std::string_view line, std::string& ref, LinkDirective& out) {
    line = trim(line);
    std::size_t i = 0;
    const std::string kw = read_word(line, i);
    const bool plural = kw == "links";
    if (kw != "link" && !plural) return false;
    // Actor name runs up to the ':'.
    skip_spaces(line, i);
    const std::size_t colon = line.find(':', i);
    if (colon == std::string_view::npos) return false;
    ref = std::string(trim(line.substr(i, colon - i)));
    if (ref.empty()) return false;
    std::string_view rest = trim(line.substr(colon + 1));
    out = LinkDirective{};
    if (plural) {
        // links Actor: {"Label": "url", ...} — take the first pair only.
        std::string label, url;
        if (!read_first_json_pair(rest, label, url)) return false;
        out.title = std::move(label);
        out.href = std::move(url);
        return true;
    }
    // link Actor: Label @ url
    const std::size_t at = rest.rfind('@');
    if (at == std::string_view::npos) return false;
    out.title = std::string(trim(rest.substr(0, at)));
    out.href = std::string(trim(rest.substr(at + 1)));
    return !out.href.empty();
}

} // namespace cdiagram::detail
