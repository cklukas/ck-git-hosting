// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Reading, writing and validating the `placement` front-matter extension.
//
// `write_into` is text surgery rather than a YAML round trip on purpose. Front
// matter belongs to the author: their key order, their comments, their
// spacing. Reformatting all of it because one machine-written block changed
// would put noise in every diff, so this locates exactly the `placement:` block
// and replaces exactly those lines.

#include "cdiagram/placement.hpp"

#include <algorithm>
#include <string>

#include <cworks/format.hpp>
#include <cworks/yaml.hpp>

#include "source.hpp"

namespace cdiagram {

namespace {

void report(cworks::Diagnostics* diagnostics, cworks::Diagnostic::Severity severity,
            std::string message) {
    if (diagnostics == nullptr) return;
    diagnostics->push_back({severity, "placement: " + std::move(message)});
}

/// The indentation of `line`, in characters.
std::size_t indent_of(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i;
}

bool blank_or_comment(std::string_view line) {
    const std::size_t start = indent_of(line);
    return start >= line.size() || line[start] == '#';
}

std::string format_coordinate(double value) {
    // Two decimals is the resolution a person can act on and the resolution the
    // renderers round to anyway; more would make a diff for a mouse tremor.
    return cworks::format_double_fixed(value, 2);
}

std::optional<double> number_of(const cworks::YamlNode* node) {
    if (node == nullptr || !node->is_scalar()) return std::nullopt;
    bool ok = false;
    const double value = node->as_double(&ok);
    if (!ok) return std::nullopt;
    return value;
}

} // namespace

const ObjectPlacement* Placement::find(std::string_view key) const {
    for (const Entry& entry : entries_)
        if (entry.first == key) return &entry.second;
    return nullptr;
}

void Placement::set(std::string_view key, const ObjectPlacement& value) {
    if (value.empty()) {
        erase(key);
        return;
    }
    for (Entry& entry : entries_) {
        if (entry.first != key) continue;
        entry.second = value;
        return;
    }
    entries_.emplace_back(std::string(key), value);
}

void Placement::erase(std::string_view key) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& entry) { return entry.first == key; }),
                   entries_.end());
}

void Placement::rename(std::string_view from, std::string_view to) {
    for (Entry& entry : entries_) {
        if (entry.first == from) entry.first = std::string(to);
        // An alignment that pointed at the old name must follow it, or the
        // rename would silently drop the constraint.
        if (entry.second.align && entry.second.align->to == from)
            entry.second.align->to = std::string(to);
    }
}

Placement Placement::parse(std::string_view source, cworks::Diagnostics* diagnostics) {
    Placement placement;
    const std::string front = detail::frontmatter(source);
    if (front.empty()) return placement;

    cworks::YamlNode root;
    try {
        root = cworks::parse_yaml(front, "<diagram>");
    } catch (const std::exception& e) {
        report(diagnostics, cworks::Diagnostic::Severity::Warning,
               std::string("front matter is not valid YAML (") + e.what() + "), ignored");
        return placement;
    }

    const cworks::YamlNode* block = root.find("placement");
    if (block == nullptr) return placement;
    if (!block->is_map()) {
        report(diagnostics, cworks::Diagnostic::Severity::Warning,
               "expected a block of settings, ignored");
        return placement;
    }

    if (const auto version = number_of(block->find("version"))) {
        placement.version_ = static_cast<int>(*version);
        if (placement.version_ > kPlacementVersion)
            report(diagnostics, cworks::Diagnostic::Severity::Warning,
                   "was written by a newer version of the format; entries this build does not "
                   "understand are kept but not applied");
    }

    const cworks::YamlNode* objects = block->find("objects");
    if (objects == nullptr) return placement;
    if (!objects->is_map()) {
        report(diagnostics, cworks::Diagnostic::Severity::Warning,
               "'objects' must be a block of object keys, ignored");
        return placement;
    }

    for (const auto& [key, entry] : objects->map) {
        if (!entry.is_map()) {
            report(diagnostics, cworks::Diagnostic::Severity::Warning,
                   "'" + key + "' is not a block of settings, ignored");
            continue;
        }
        ObjectPlacement value;

        if (const cworks::YamlNode* pin = entry.find("pin")) {
            const auto x = number_of(pin->find("x"));
            const auto y = number_of(pin->find("y"));
            if (x && y) value.pin = PlacementPin{*x, *y};
            else
                report(diagnostics, cworks::Diagnostic::Severity::Warning,
                       "'" + key + "' has a pin without numeric x and y, ignored");
        }

        if (const cworks::YamlNode* align = entry.find("align")) {
            const cworks::YamlNode* to = align->find("to");
            const cworks::YamlNode* axis = align->find("axis");
            if (to == nullptr || !to->is_scalar() || to->scalar.empty()) {
                report(diagnostics, cworks::Diagnostic::Severity::Warning,
                       "'" + key + "' has an alignment without a target, ignored");
            } else if (axis == nullptr || (axis->scalar != "x" && axis->scalar != "y")) {
                report(diagnostics, cworks::Diagnostic::Severity::Warning,
                       "'" + key + "' has an alignment whose axis is not 'x' or 'y', ignored");
            } else if (to->scalar == key) {
                report(diagnostics, cworks::Diagnostic::Severity::Warning,
                       "'" + key + "' is aligned to itself, ignored");
            } else {
                value.align = PlacementAlign{to->scalar,
                                             axis->scalar == "y" ? PlacementAxis::Y
                                                                 : PlacementAxis::X};
            }
        }

        if (!value.empty()) placement.entries_.emplace_back(key, std::move(value));
    }
    return placement;
}

std::string Placement::to_yaml() const {
    if (entries_.empty()) return {};
    std::vector<Entry> sorted = entries_;
    std::sort(sorted.begin(), sorted.end(),
              [](const Entry& a, const Entry& b) { return a.first < b.first; });

    std::string out = "placement:\n";
    out += "  version: " + std::to_string(version_) + "\n";
    out += "  objects:\n";
    for (const Entry& entry : sorted) {
        out += "    " + entry.first + ":\n";
        if (entry.second.pin) {
            out += "      pin: {x: " + format_coordinate(entry.second.pin->x) +
                   ", y: " + format_coordinate(entry.second.pin->y) + "}\n";
        }
        if (entry.second.align) {
            out += "      align: {to: " + entry.second.align->to + ", axis: " +
                   (entry.second.align->axis == PlacementAxis::Y ? "y" : "x") + "}\n";
        }
    }
    return out;
}

std::string Placement::write_into(std::string_view source) const {
    const std::string block = to_yaml();

    // Split the source into lines, keeping their newlines, so reassembly is
    // exact regardless of whether the file ends with one.
    std::vector<std::string> lines;
    std::size_t position = 0;
    while (position < source.size()) {
        std::size_t end = source.find('\n', position);
        if (end == std::string_view::npos) end = source.size() - 1;
        lines.emplace_back(source.substr(position, end - position + 1));
        position = end + 1;
    }

    // Locate the front-matter fences.
    std::size_t open = lines.size();
    std::size_t close = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string_view line = lines[i];
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        if (line.empty()) continue;
        if (open == lines.size()) {
            if (line != "---") break; // no front matter at all
            open = i;
            continue;
        }
        if (line == "---") {
            close = i;
            break;
        }
    }

    const bool has_front = open < lines.size() && close < lines.size();

    if (!has_front) {
        if (block.empty()) return std::string(source);
        // No front matter yet: give the file one, above everything else.
        return "---\n" + block + "---\n" + std::string(source);
    }

    // Find an existing top-level `placement:` block inside the fences: the key
    // line plus every following line indented deeper than it.
    std::size_t block_start = lines.size();
    std::size_t block_end = lines.size();
    for (std::size_t i = open + 1; i < close; ++i) {
        std::string_view line = lines[i];
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
        if (indent_of(line) != 0) continue;
        if (line.rfind("placement:", 0) != 0) continue;
        block_start = i;
        block_end = i + 1;
        while (block_end < close &&
               (blank_or_comment(lines[block_end]) || indent_of(lines[block_end]) > 0))
            ++block_end;
        // Trailing blank lines belong to whatever follows, not to the block.
        while (block_end > block_start + 1 && blank_or_comment(lines[block_end - 1])) --block_end;
        break;
    }

    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (block_start < lines.size() && i == block_start) {
            out += block; // may be empty: that is the removal case
            i = block_end - 1;
            continue;
        }
        if (block_start == lines.size() && i == close && !block.empty()) out += block;
        out += lines[i];
    }
    return out;
}

} // namespace cdiagram
