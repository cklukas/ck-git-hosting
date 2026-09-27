// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "source.hpp"

#include <cworks/format.hpp>
#include <cworks/text.hpp>

namespace cdiagram::detail {

namespace {

/// Split on LF, dropping a trailing CR — the suite normalises to LF
/// before any parsing (determinism: CRLF and LF sources are identical).
std::vector<std::string> raw_lines(std::string_view source) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : source) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r') current.pop_back();
            lines.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    lines.push_back(std::move(current));
    return lines;
}

std::size_t leading_ws(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i;
}

bool is_comment(const std::string& trimmed) {
    return trimmed.size() >= 2 && trimmed[0] == '%' && trimmed[1] == '%';
}

} // namespace

std::vector<SourceLine> significant_lines(std::string_view source) {
    const std::vector<std::string> raw = raw_lines(source);

    // A leading YAML front-matter block: the first significant line is
    // exactly "---", closed by the next "---". Everything between is
    // configuration cdiagram does not yet consume (skipped, not an
    // error, so front matter never breaks a diagram).
    std::size_t start = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const std::string t = cworks::trim(raw[i]);
        if (t.empty()) continue;
        if (t == "---") {
            for (std::size_t j = i + 1; j < raw.size(); ++j) {
                if (cworks::trim(raw[j]) == "---") {
                    start = j + 1;
                    break;
                }
            }
        }
        break;
    }

    std::vector<SourceLine> lines;
    for (std::size_t i = start; i < raw.size(); ++i) {
        const std::string trimmed = cworks::trim(raw[i]);
        if (trimmed.empty() || is_comment(trimmed)) continue;
        lines.push_back({trimmed, i + 1, leading_ws(raw[i])});
    }
    return lines;
}

std::string frontmatter(std::string_view source) {
    const std::vector<std::string> raw = raw_lines(source);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const std::string t = cworks::trim(raw[i]);
        if (t.empty()) continue;
        if (t != "---") return {};
        std::string block;
        for (std::size_t j = i + 1; j < raw.size(); ++j) {
            if (cworks::trim(raw[j]) == "---") return block;
            block += raw[j];
            block += '\n';
        }
        return {};  // unterminated block: treated as absent, like significant_lines
    }
    return {};
}

std::string unquote(std::string_view text) {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        return std::string(text.substr(1, text.size() - 2));
    return std::string(text);
}

bool parse_number(std::string_view text, double& out) {
    return cworks::parse_double(text, out);
}

std::vector<std::string> label_lines(std::string_view text) {
    static constexpr std::string_view breaks[] = {"<br/>", "<br />", "<br>", "\\n"};
    std::vector<std::string> lines;
    std::string current;
    std::size_t i = 0;
    while (i < text.size()) {
        bool matched = false;
        for (const std::string_view brk : breaks) {
            if (text.compare(i, brk.size(), brk) == 0) {
                lines.push_back(std::move(current));
                current.clear();
                i += brk.size();
                matched = true;
                break;
            }
        }
        if (!matched) current.push_back(text[i++]);
    }
    lines.push_back(std::move(current));
    for (std::string& line : lines) line = cworks::trim(line);
    return lines;
}

} // namespace cdiagram::detail
