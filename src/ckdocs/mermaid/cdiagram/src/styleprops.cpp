// ckdiagram — shared Mermaid style-directive vocabulary
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "styleprops.hpp"

#include <cctype>
#include <vector>

#include <cworks/text.hpp>

#include "diagnostics.hpp"
#include "source.hpp"

namespace cdiagram::detail {

using cplot::Color;

std::vector<std::string> split_style_entries(std::string_view text) {
    // rgb()/rgba() values and quoted strings contain commas of their own:
    // an entry ends at a comma outside parentheses and quotes.
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t depth = 0;
        bool quoted = false;
        std::size_t end = start;
        while (end < text.size() && (text[end] != ',' || depth > 0 || quoted)) {
            if (text[end] == '"') quoted = !quoted;
            else if (text[end] == '(') ++depth;
            else if (text[end] == ')' && depth > 0) --depth;
            ++end;
        }
        const std::string entry = cworks::trim(std::string(text.substr(start, end - start)));
        if (!entry.empty()) out.push_back(entry);
        if (end >= text.size()) break;
        start = end + 1;
    }
    return out;
}

StyleProps parse_style_props(std::string_view text, const RenderOptions& options,
                             std::string_view what, std::size_t line) {
    StyleProps p;
    for (const std::string& entry : split_style_entries(text)) {
        const std::size_t colon = entry.find(':');
        if (colon == std::string::npos) {
            diagnose(options, cworks::Diagnostic::Severity::Warning,
                     at_line(what, line,
                             "style entry '" + entry + "' is not 'key:value' (ignored)"));
            continue;
        }
        const std::string key = cworks::trim(entry.substr(0, colon));
        const std::string val = cworks::trim(entry.substr(colon + 1));
        if (key == "fill" || key == "stroke" || key == "color") {
            const std::optional<Color> c =
                val == "none" || val == "transparent"
                    ? std::optional<Color>(cplot::colors::transparent)
                    : cplot::parse_css_color(val);
            if (!c) {
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line(what, line, "unrecognized colour '" + val + "' (ignored)"));
                continue;
            }
            if (key == "fill") p.fill = c;
            else if (key == "stroke") p.stroke = c;
            else p.text = c;
        } else if (key == "stroke-width") {
            std::string num = val;
            if (num.size() >= 2 && num.substr(num.size() - 2) == "px") num.resize(num.size() - 2);
            double w = 0.0;
            if (parse_number(cworks::trim(num), w)) p.stroke_width = w;
            else
                diagnose(options, cworks::Diagnostic::Severity::Warning,
                         at_line(what, line, "invalid stroke-width '" + val + "' (ignored)"));
        } else if (key == "stroke-dasharray") {
            // SVG accepts commas, whitespace, or both as separators;
            // DashPattern::parse owns that grammar.
            p.dash = cplot::DashPattern::parse(val);
        } else {
            diagnose(options, cworks::Diagnostic::Severity::Warning,
                     at_line(what, line, "unsupported style property '" + key + "' (ignored)"));
        }
    }
    return p;
}

/// Split a comma-separated identifier list ("A,B,C"), trimming each.
std::vector<std::string> split_commas(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t len = comma == std::string_view::npos ? text.size() - start : comma - start;
        const std::string one = cworks::trim(std::string(text.substr(start, len)));
        if (!one.empty()) out.push_back(one);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

void StyleSheet::define(const std::string& name, const StyleProps& props) {
    defs_[name].merge(props);
}

void StyleSheet::assign(const std::string& element, std::string cls) {
    classes_[element].push_back(std::move(cls));
}

void StyleSheet::style(const std::string& element, const StyleProps& props) {
    direct_[element].merge(props);
}

StyleProps StyleSheet::resolve(const std::string& element) const {
    StyleProps resolved;
    if (const auto def = defs_.find("default"); def != defs_.end())
        resolved.merge(def->second);
    if (const auto assigned = classes_.find(element); assigned != classes_.end())
        for (const std::string& cls : assigned->second)
            if (const auto it = defs_.find(cls); it != defs_.end())
                resolved.merge(it->second);
    if (const auto direct = direct_.find(element); direct != direct_.end())
        resolved.merge(direct->second);
    return resolved;
}

bool parse_style_statement(const std::string& line, StyleSheet& sheet,
                           const RenderOptions& options, std::string_view what,
                           std::size_t number, const char* assign_keyword) {
    const std::string keyword = line.substr(0, line.find_first_of(" \t"));
    // `line.substr(keyword.size())` is always safe: a bare keyword line has
    // pos == size(). It must still be CONSUMED (diagnosed), never fall
    // through to a builder's node grammar.
    if (keyword == "classDef") {
        const std::string rest = cworks::trim(line.substr(keyword.size()));
        const std::size_t sp = rest.find_first_of(" \t");
        if (sp == std::string::npos) {
            diagnose(options, cworks::Diagnostic::Severity::Warning,
                     at_line(what, number, "classDef needs a name and properties (ignored)"));
            return true;
        }
        const StyleProps props =
            parse_style_props(cworks::trim(rest.substr(sp + 1)), options, what, number);
        for (const std::string& name : split_commas(rest.substr(0, sp)))
            sheet.define(name, props);
        return true;
    }
    if (keyword == assign_keyword) {
        const std::string rest = cworks::trim(line.substr(keyword.size()));
        const std::size_t sp = rest.find_last_of(" \t");
        if (sp == std::string::npos) {
            diagnose(options, cworks::Diagnostic::Severity::Warning,
                     at_line(what, number,
                             std::string(assign_keyword) +
                                 " needs element ids and a class name (ignored)"));
            return true;
        }
        const std::string cls = cworks::trim(rest.substr(sp + 1));
        for (const std::string& id :
             split_commas(unquote(cworks::trim(rest.substr(0, sp)))))
            sheet.assign(id, cls);
        return true;
    }
    if (keyword == "style") {
        const std::string rest = cworks::trim(line.substr(keyword.size()));
        const std::size_t sp = rest.find_first_of(" \t");
        if (sp == std::string::npos) {
            diagnose(options, cworks::Diagnostic::Severity::Warning,
                     at_line(what, number, "style needs an element id and properties (ignored)"));
            return true;
        }
        sheet.style(rest.substr(0, sp),
                    parse_style_props(cworks::trim(rest.substr(sp + 1)), options, what, number));
        return true;
    }
    return false;
}

std::string take_class_annotations(std::string token, std::vector<std::string>& classes) {
    std::size_t mark = token.find(":::");
    while (mark != std::string::npos) {
        std::size_t end = mark + 3;
        while (end < token.size() &&
               (std::isalnum(static_cast<unsigned char>(token[end])) || token[end] == '_' ||
                token[end] == '-'))
            ++end;
        if (end > mark + 3) classes.push_back(token.substr(mark + 3, end - mark - 3));
        token = cworks::trim(token.substr(0, mark)) + token.substr(end);
        mark = token.find(":::");
    }
    return cworks::trim(token);
}

std::string take_class_annotations(std::string token, StyleSheet& sheet) {
    std::size_t mark = token.find(":::");
    while (mark != std::string::npos) {
        std::size_t end = mark + 3;
        while (end < token.size() &&
               (std::isalnum(static_cast<unsigned char>(token[end])) || token[end] == '_' ||
                token[end] == '-'))
            ++end;
        const std::string element = cworks::trim(token.substr(0, mark));
        if (end > mark + 3) sheet.assign(element, token.substr(mark + 3, end - mark - 3));
        token = element + token.substr(end);
        mark = token.find(":::");
    }
    return cworks::trim(token);
}

} // namespace cdiagram::detail
