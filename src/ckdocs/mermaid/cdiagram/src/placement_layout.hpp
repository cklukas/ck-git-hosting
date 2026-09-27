// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Internal: applying the `placement` extension to a finished layout.
//
// This runs *after* `layout_dag`, never instead of it. The layout engine keeps
// full ownership of everything the author did not place by hand, and deleting
// the front-matter block restores its drawing exactly — which is the property
// that makes an authored position safe to offer in an editor at all.
#pragma once

#include <string>
#include <vector>

#include <cworks/limits.hpp>

#include "cdiagram/placement.hpp"
#include "dag_layout.hpp"

namespace cdiagram::detail {

/// Move the placed nodes, follow their edges and cluster frames, and keep the
/// drawing inside positive coordinates. `keys` is parallel to `graph.nodes`.
///
/// Unresolvable entries (a pin on an unknown key, an alignment to something
/// that is not there, an alignment cycle) are reported and skipped: a
/// placement block never breaks a diagram.
void apply_placement(const Placement& placement, const std::vector<std::string>& keys,
                     const DagGraph& graph, DagResult& result,
                     cworks::Diagnostics* diagnostics);

} // namespace cdiagram::detail
