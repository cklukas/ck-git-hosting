// ckdiagram — Mermaid front-matter configuration
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Mermaid's official front-matter surface is exactly three top-level
// keys: `title`, `displayMode` (a gantt-only legacy alias), and `config`
// — a per-diagram override of the render configuration. This helper
// parses the YAML block once and offers typed lookups into the
// diagram's `config.<section>` block. Malformed YAML and wrong-typed
// values are diagnosed and ignored: front matter never breaks a
// diagram, exactly as in Mermaid.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <cworks/yaml.hpp>

#include "cdiagram/render.hpp"

namespace cdiagram::detail {

class FrontMatter {
public:
    /// Parse the source's front matter; `config_section` names the
    /// diagram's block under `config:` (e.g. "sankey", "xyChart").
    FrontMatter(std::string_view source, const RenderOptions& options,
                std::string_view config_section);

    /// The top-level `title:` (empty when absent).
    const std::string& title() const { return title_; }
    /// The top-level `displayMode:` (empty when absent).
    const std::string& display_mode() const { return display_mode_; }
    /// The top-level `link:` — a whole-diagram hyperlink (a cworks extension
    /// over Mermaid's front matter). Empty when absent.
    const std::string& link() const { return link_; }
    /// The optional `linkTarget:` for `link` (e.g. `_blank`). Empty when absent.
    const std::string& link_target() const { return link_target_; }

    /// config.<section>.<key> as text / number / boolean. Absent keys
    /// return nullopt silently (Mermaid ignores unknown keys the same
    /// way); present keys of the wrong shape are diagnosed and skipped.
    std::optional<std::string> text(std::string_view key) const;
    std::optional<double> number(std::string_view key) const;
    std::optional<bool> boolean(std::string_view key) const;

    /// config.<section>.<key> as a canvas dimension in pixels: `fallback`
    /// when the key is absent, non-numeric, or too small to lay out.
    double dimension(std::string_view key, double fallback) const;

private:
    const cworks::YamlNode* entry(std::string_view key) const;

    cworks::YamlNode root_;
    std::string title_;
    std::string display_mode_;
    std::string link_;
    std::string link_target_;
    std::string section_;
    const RenderOptions* options_ = nullptr;
};

} // namespace cdiagram::detail
