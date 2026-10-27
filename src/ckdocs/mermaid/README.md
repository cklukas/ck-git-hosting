# Native Mermaid rendering in ckdocs

This is an internal copy of the CWorks diagram renderer, adapted for ckdocs
under MIT by its copyright owner, Dr. Christian Klukas. It requires only a
C++20 compiler and the standard library. The original projects are unchanged.
`ORIGIN.json` records the source revision, original file hashes, adaptations,
and origins of the retained tests and SVG fixtures.

## Boundary

- `cdiagram`: all 23 diagram parsers and their static layout implementations.
- `cplot`: scenes, deterministic text measurement, SVG output, and the chart
  geometry used by pie, XY, radar, quadrant, Sankey, and treemap diagrams.
- `libcworks`: text, YAML, diagnostics, bounds, formatting, base64, and
  deterministic mathematics used by that code.

Only ckdocs and `ckdocs-mermaid-tests` link these objects. Other project
binaries use the ordinary Markdown renderer without a diagram callback.
`../mermaid.cpp` is the ckdocs adapter; the common site builder receives its
SVGs through `DocsBuildOptions::mermaid_renderer`.

The copied namespaces are private implementation details. This directory is
not an independently installed library or a complete CWorks SDK. Keep its
include directories private to the two build targets.

## Adaptations

The copy omits the editors, object model, animation, table adapters and
configuration import, CLI, custom registry, central user configuration,
PDF/raster/SIXEL export, font discovery, font embedding, and image codecs.
Bitmap scene items are explicitly refused by the SVG backend. Standard SVG
text uses the reader's fonts; layout retains the original deterministic
Helvetica metrics. No fonts or external packages are bundled.

The chart implementation retains common geometry shared by several chart
forms, so source files still contain some chart methods Mermaid does not use.
Pruning these further should preserve the original SVG fixtures.

The adapter bounds source size, line count, SVG size, and canvas dimensions.
The copied parsers additionally bound packet bit ranges, block columns, Gantt
timespans, and graph size/routing expansion before expensive layout. These
fail with source diagnostics through the same site warning path.

## Verification and updates

Run `make BUILD_DIR=<approved-build-directory> test-mermaid` from the project
root. The suite checks all catalogue examples and minimal forms in light and
dark modes, geometry/diagnostics, and byte parity with the original SVG
fixtures. `tests/integration/docs_site.sh` checks the CLI, the actual 23-type
examples in `docs/ckdocs/diagrams/`, relative SVG references, and
HTTP image responses. Common unit tests cover caching, fallback, escaping,
atomic output, and asset collisions.

When copying a later fix, use the origin hashes to review the relevant source
diff, preserve the local adaptations, update provenance, and run both suites.
Do not automatically replace the copied tree with a newer upstream tree.

The XY-chart backports recorded in `ORIGIN.json` follow CWorks in order: the
explicit source ordinals for chart regions, then several series and the
horizontal form, then the `error` line. They port only what the XY chart
renders: the CWorks editor model, scene map and C APIs of the same commits
are not part of this copy, and the `error` line maps onto the custom error
amounts this copy's cplot already has, so the later CWorks error-bar model is
not needed. The goldens `xychart`, `xychart-horizontal` and `xychart-error`
are byte-identical copies of the CWorks ones; a change to either side must
keep them byte-identical. `error` is a CWorks extension that standard Mermaid
renderers reject, and `docs/ckdocs/xy-charts.md` says so.

The label-layout backport recorded in `ORIGIN.json` keeps the original import
revision and hashes intact. Its `backports` entry records the source commit and
both source and embedded hashes for the changed files. Quadrant point labels
and headings share bounded placement with priority for visible markers; XY
axes reserve measured label overhang. No font embedding dependency was added.
