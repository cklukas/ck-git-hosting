// ckdiagram — Mermaid front-matter configuration
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT

#include "frontmatter.hpp"

#include <exception>

#include "diagnostics.hpp"
#include "source.hpp"

namespace cdiagram::detail {

FrontMatter::FrontMatter(std::string_view source, const RenderOptions& options,
                         std::string_view config_section)
    : section_(config_section), options_(&options) {
    const std::string block = frontmatter(source);
    if (block.empty()) return;
    try {
        root_ = cworks::parse_yaml(block);
    } catch (const std::exception&) {
        diagnose_unsupported(options, section_, 1, "front matter",
                             "the front-matter block is not valid YAML");
        root_ = cworks::YamlNode{};
        return;
    }
    if (const cworks::YamlNode* title = root_.find("title");
        title && title->type == cworks::YamlNode::Type::Scalar)
        title_ = title->scalar;
    if (const cworks::YamlNode* mode = root_.find("displayMode");
        mode && mode->type == cworks::YamlNode::Type::Scalar)
        display_mode_ = mode->scalar;
    if (const cworks::YamlNode* link = root_.find("link");
        link && link->type == cworks::YamlNode::Type::Scalar)
        link_ = link->scalar;
    if (const cworks::YamlNode* target = root_.find("linkTarget");
        target && target->type == cworks::YamlNode::Type::Scalar)
        link_target_ = target->scalar;
}

const cworks::YamlNode* FrontMatter::entry(std::string_view key) const {
    const cworks::YamlNode* config = root_.find("config");
    const cworks::YamlNode* section = config ? config->find(section_) : nullptr;
    return section ? section->find(std::string(key)) : nullptr;
}

std::optional<std::string> FrontMatter::text(std::string_view key) const {
    const cworks::YamlNode* node = entry(key);
    if (!node) return std::nullopt;
    if (node->type != cworks::YamlNode::Type::Scalar) {
        diagnose_unsupported(*options_, section_, 1, key, "expects a single value");
        return std::nullopt;
    }
    return node->scalar;
}

std::optional<double> FrontMatter::number(std::string_view key) const {
    const std::optional<std::string> value = text(key);
    if (!value) return std::nullopt;
    double parsed = 0.0;
    if (!parse_number(*value, parsed)) {
        diagnose_unsupported(*options_, section_, 1, key,
                             "expects a number, got '" + *value + "'");
        return std::nullopt;
    }
    return parsed;
}

double FrontMatter::dimension(std::string_view key, double fallback) const {
    const std::optional<double> value = number(key);
    if (!value) return fallback;
    if (*value < 60.0) {
        diagnose_unsupported(*options_, section_, 1, key,
                             "is too small for a canvas and was ignored");
        return fallback;
    }
    return *value;
}

std::optional<bool> FrontMatter::boolean(std::string_view key) const {
    const std::optional<std::string> value = text(key);
    if (!value) return std::nullopt;
    if (*value == "true") return true;
    if (*value == "false") return false;
    diagnose_unsupported(*options_, section_, 1, key,
                         "expects true or false, got '" + *value + "'");
    return std::nullopt;
}

} // namespace cdiagram::detail
