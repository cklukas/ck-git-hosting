// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Internal: one builder per diagram family. Each parses its own source
// (the type has already been detected) and lays it out into a
// cplot::Scene. render() dispatches to these on the detected type.
#pragma once

#include <string_view>

#include <cplot/scene.hpp>

#include "cdiagram/render.hpp"

namespace cdiagram::detail {

cplot::Scene build_pie(std::string_view source, const RenderOptions& options);
cplot::Scene build_sequence(std::string_view source, const RenderOptions& options);
cplot::Scene build_flowchart(std::string_view source, const RenderOptions& options);
cplot::Scene build_state(std::string_view source, const RenderOptions& options);
cplot::Scene build_er(std::string_view source, const RenderOptions& options);
cplot::Scene build_class(std::string_view source, const RenderOptions& options);
cplot::Scene build_quadrant(std::string_view source, const RenderOptions& options);
cplot::Scene build_mindmap(std::string_view source, const RenderOptions& options);
cplot::Scene build_timeline(std::string_view source, const RenderOptions& options);
cplot::Scene build_journey(std::string_view source, const RenderOptions& options);
cplot::Scene build_gantt(std::string_view source, const RenderOptions& options);
cplot::Scene build_git(std::string_view source, const RenderOptions& options);
cplot::Scene build_xychart(std::string_view source, const RenderOptions& options);
cplot::Scene build_kanban(std::string_view source, const RenderOptions& options);
cplot::Scene build_packet(std::string_view source, const RenderOptions& options);
cplot::Scene build_requirement(std::string_view source, const RenderOptions& options);
cplot::Scene build_radar(std::string_view source, const RenderOptions& options);
cplot::Scene build_treemap(std::string_view source, const RenderOptions& options);
cplot::Scene build_block(std::string_view source, const RenderOptions& options);
cplot::Scene build_c4(std::string_view source, const RenderOptions& options);
cplot::Scene build_sankey(std::string_view source, const RenderOptions& options);
cplot::Scene build_architecture(std::string_view source, const RenderOptions& options);
cplot::Scene build_zenuml(std::string_view source, const RenderOptions& options);
} // namespace cdiagram::detail
