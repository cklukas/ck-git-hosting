// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "mermaid.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <cdiagram/render.hpp>
#include <cplot/svg.hpp>

namespace ckgit {
namespace {

bool safeDiagramLink(std::string_view href) {
  if (std::any_of(href.begin(), href.end(), [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; })) return false;
  const auto colon = href.find(':');
  const auto slash = href.find_first_of("/?#");
  if (colon == std::string_view::npos || (slash != std::string_view::npos && slash < colon)) return true;
  std::string scheme(href.substr(0, colon));
  for (char& c : scheme) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
  return scheme == "http" || scheme == "https" || scheme == "mailto";
}

}  // namespace

DocsDiagram renderMermaid(std::string_view source) {
  // Bound parsing and layout before invoking the native engine. The Markdown
  // input/output bounds remain independent of these per-diagram limits.
  if (source.size() > 64 * 1024) throw std::length_error("source exceeds 64 KiB");
  if (std::count(source.begin(), source.end(), '\n') > 1024) throw std::length_error("source exceeds 1024 lines");
  if (source.find('\0') != std::string_view::npos) throw std::invalid_argument("source contains a NUL byte");
  DocsDiagram result;
  cworks::Diagnostics diagnostics;
  cdiagram::RenderOptions options;
  options.diagnostics = &diagnostics;
  const auto render = [&](cdiagram::ThemeMode mode) {
    options.mode = mode;
    const auto scene = cdiagram::render(source, options);
    // SVGs may also be opened directly, outside an <img>'s restricted context.
    for (const auto& link : cplot::collect_links(scene)) {
      if (!safeDiagramLink(link.href)) throw std::invalid_argument("diagram link uses an unsafe URL");
    }
    if (!std::isfinite(scene.width) || !std::isfinite(scene.height) || scene.width <= 0 || scene.height <= 0 ||
        scene.width > 100000 || scene.height > 100000) throw std::length_error("diagram dimensions exceed 100000 pixels");
    if (result.alternative_text.empty()) result.alternative_text = scene.meta_title.empty()
        ? cdiagram::type_title(cdiagram::detect_type(source)) : scene.meta_title;
    auto svg = cplot::SvgRenderer().render(scene);
    if (svg.size() > 4 * 1024 * 1024) throw std::length_error("SVG exceeds 4 MiB");
    return svg;
  };
  result.light_svg = render(cdiagram::ThemeMode::Light);
  options.diagnostics = nullptr;  // identical parser diagnostics in both themes
  result.dark_svg = render(cdiagram::ThemeMode::Dark);
  for (const auto& diagnostic : diagnostics) result.warnings.push_back(diagnostic.message);
  return result;
}

}  // namespace ckgit
