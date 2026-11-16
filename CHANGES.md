# Changes

## 0.11.0 — 2026-11-16

### Windows HCS VM runner and isolation-checked claims

- Added the opt-in offline Hyper-V/HCS VM backend (`ci_windows_backend=hyperv-hcs`),
  native Windows guest service (`ckgit-windows-vm-guest`), host preflight and
  first-boot verification tool (`ckgit-windows-vm-host-check`), and two-phase
  operator provisioning script (`scripts/setup-windows-hcs.ps1`), packaged in
  the Windows runner installer's `hcs` directory.
- Added reserved Windows isolation labels (`lpac` and `hyperv-hcs`) to the
  runner claim protocol and enforced them on the server before issuing a lease,
  so jobs requesting `runs_on: [windows, x64, lpac]` or
  `runs_on: [windows, x64, hyperv-hcs]` only run on a runner configured with
  that backend.
- Deferred VM output uploads until after guest acknowledgement, restricted
  per-VM disk ACLs to disposable job disks, and qualified HCS job cancellation,
  timeout, and owner-death teardown.

### Server, Pages, and transport performance and reliability

- `ck-pagesd` now serves requests across an 8-worker bounded thread pool with
  64 KiB chunked file streaming, zero-copy `HEAD` inspection, strong `ETag` and
  `If-None-Match` (`304 Not Modified`) revalidation, and immutable cache
  headers for content-hashed diagram SVGs.
- `ck-git-hostingd` now streams CI artifact and release asset downloads directly
  from disk, emits strong `ETag` headers with `If-None-Match` (`304 Not Modified`)
  short-circuiting on immutable object-ID routes (`/raw/<oid>:…`,
  `/blob/<oid>:…`, `/source/<oid>:…`, and `/commit/<oid>`), and reuses the
  resolved commit metadata when rendering commit diffs.
- `ck-git-shell` now forwards validated `GIT_PROTOCOL` values (such as
  `version=2`) to standard SSH `git-upload-pack` and `git-receive-pack`
  invocations in addition to remote runner gateways.
- Fixed HTTP request parsing for canonical percent-encoded static site paths
  (such as non-ASCII tag URLs) and socket chunk over-reads past `\r\n\r\n`,
  ignored `SIGPIPE` in `ck-pagesd` and `ck-git-hostingd`, returned
  `405 Method Not Allowed` for `POST` requests to `ck-pagesd` and
  `ckdocs serve`, preserved concurrent `refreshMetadata()` updates across
  `ProjectIndex::rebuild()`, and kept multi-job workflow parents in `Pending`
  while waiting for queued child jobs.

### ckdocs and web dashboard UX

- `ckdocs` pages now use a 3-column layout on wide desktop viewports
  (`1180px` and wider) that docks the "On this page" outline into a sticky
  right column with active-section scrollspy, reveal `#` permalink anchors on
  headings, include `−`, `100%`, and `+` (plus Ctrl/Cmd-wheel) zoom controls in
  the expanded Mermaid diagram dialog, include the theme picker on portal
  welcome pages, and render standalone `site.selector.links` beside the brand
  on single-site builds.
- `ckdocs` search now filters by whitespace-separated query tokens, highlights
  matched terms with `<mark>`, and supports `ArrowDown`, `ArrowUp`, and `Enter`
  keyboard navigation.
- Dashboard commit diffs now include old and new line-number columns, per-file
  headers, server-side syntax highlighting, and a zero-JavaScript
  **Side-by-side diff** toggle on wide viewports; file previews include a
  **Commit history** link and the dashboard tour screenshots have been
  refreshed.

## 0.10.1 — 2026-11-02

This patch release makes the accumulated CI and integration fixes portable
across the supported build paths while retaining the complete 0.10.0 feature
set, including the ckdocs portal selector links and the experimental native
macOS sandbox.

### CI portability and integration

- Make-based release builds now include all dependency-review, VM, native
  macOS, and Darwin VM-host sources and tests.
- macOS VM protocol fixtures use portable thread primitives and the required
  private filesystem permissions.
- macOS remote runners honor explicit `ci_allow_degraded=true`; VM storage
  preflight is applied only when VM execution is selected.
- Bulk-publish integration failures now emit fixture diagnostics in CI.

## 0.10.0 — 2026-11-02

This release combines the accumulated Windows, CI, runner, macOS, and ckdocs
work since 0.9.0.

### ckdocs: portal site selector links and icons

- Portal site selectors can now include configured external destinations with
  a title, URL, and optional local icon under `site.selector.links`.
- Generated portal sites can define their own selector icon with
  `sites[].icon`; all selector icons use the configurable `site.selector.icon_size`
  (12–48 CSS pixels), are centered and contain-fitted, and keep a consistent
  label gap.
- Selector icons are copied into the generated portal, so the dropdown remains
  self-contained under URL prefixes and when opened from `file://`.

### CI and runners

- Windows CI now has explicit runner routing, native toolchain staging,
  restricted LPAC execution, service supervision and recovery checks, artifact
  upload handling, installer packaging, and documented qualification evidence.
- Distributed CI gains stronger claim and cancellation handling, job selection,
  execution reports, remote-runner lifecycle handling, safer artifact and Pages
  path validation, and reduced build-tool telemetry.
- The native macOS Seatbelt runner is included as an experimental, non-VM
  sandbox with an exclusive build identity, supervisor, admin setup flow,
  cleanup checks, policy tests, and qualification documentation. Privileged
  lifecycle and crash-recovery qualification is still open; it is not enabled
  for hosted jobs by default.
- macOS Virtualization runner scaffolding, offline guest transport, storage
  checks, and lifecycle tests are included as experimental infrastructure. It
  is not advertised as a qualified CI execution path.

### Dependency review foundations

- CI dependency-lock parsing, request/review records, provisioned-inventory
  records, administrative review commands, and protocol design/tests are now
  present. Jobs carrying dependency requirements remain held because trusted
  inventory admission and one-use start permits are intentionally not enabled
  yet.

## 0.9.0 — 2026-10-27

### ckdocs: XY charts

- Every `bar` and `line` statement in an `xychart-beta` is now a series of its
  own, as in Mermaid: bar series stand side by side within each category and
  lines are drawn over them. Before, only the last bar and the last line
  statement were drawn. A series whose value count differs from the
  categories is reported with its line number.
- `xychart-beta horizontal` draws horizontal bars with the axes swapped.
- New `error` line, written directly under a `bar` or `line` series: one list
  gives symmetric amounts (`error [1.4, 0.3, 0.8]`), two lists give the lower,
  then the upper amount. Amounts are offsets from the value; an empty entry
  draws no whisker. This is an extension of the CWorks renderer that ckdocs
  embeds; standard Mermaid renderers reject a diagram that uses it. See
  `docs/ckdocs/xy-charts.md`.
- Clickable chart regions keep their source order with several series.

## 0.8.2 — 2026-10-25

### CI scheduling

- New branch pushes automatically cancel older pending and running runs for
  the same project and branch. Pending jobs leave the queue immediately;
  active jobs stop through the existing local/remote cancellation protocol.
- Set top-level `auto_cancel: false` in the existing `.ckgit/ci.yml` to build
  every commit in order. The default is `true`; tag/release jobs and other
  branches and projects are preserved.
- Cancelled jobs are not resurrected when a runner disconnects, and a late
  success report cannot publish a cancelled Pages build. Existing queued
  jobs are superseded by the next branch push after upgrading the server.

## 0.8.1 — 2026-10-20

### Security

- ckdocs never reads a file outside the repository it builds. Before, a
  symbolic link to a directory committed to the repository
  (`docs/system -> /etc`) let an include, a `<<<` snippet, an image or a
  linked file copy a file from outside the repository into the built site,
  for example `<!--@include: system/passwd-->`. A link that was the file
  itself was already refused.
- Every file a site reads (pages, includes, snippets, configs, logos,
  stylesheets and assets) is now opened one path component at a time
  without following links. A path that goes through a link, or leaves the
  repository with `..`, is refused and reported as a broken reference, so
  `ckdocs check` fails on it. A regression test covers every kind of file.

## 0.8.0 — 2026-10-20

A major ckdocs release: translations, much richer Markdown and code blocks,
full YAML front matter, a live-updating preview, and agent-readable Markdown
for every page. This repository's documentation is now a portal of two
manuals, ck-git-hosting and ckdocs.

### ckdocs

**Translations**

- A site can be published in several languages. Each translation mirrors the
  default page tree and is built into its own directory. Every page gets a
  language menu, `hreflang` alternates and translated sitemap entries.
- Pages that are not translated yet fall back to the default language, with
  a note on the page. Navigation, links and search stay in the reader's
  language.
- The site's own interface text is built in for 21 languages and English.
  Single texts can be overridden with `ui:`. Right-to-left languages get a
  mirrored layout.

**Markdown**

- Footnotes, definition lists, emoji shortcodes (`:zap:`), and custom
  containers (`::: tip`, `::: warning`, `::: details`, …).
- Tabs and code groups.
- TeX math (`$…$`, `$$…$$`, `math` fences), rendered to MathML at build
  time, with no scripts or fonts to load.
- The same constructs render on the dashboard as well.

**Code blocks**

- Syntax highlighting for 73 languages, up from 5.
- Titles, line numbers, highlighted lines, and diff, focus, warning and error
  markers (`// [!code ++]`, `highlight-next-line`, …).
- Snippets taken from repository files with `<<< path`, including regions
  and line ranges. A missing file or region is reported as a broken
  reference.
- A copy button on every code block.

**Front matter and pages**

- Front matter is now full YAML.
- New keys: `layout` (`doc`, `home`, `page`), `sidebar`, `toc` depth,
  `hide_table_of_contents`, `slug`, `tags` (with generated tag pages),
  `draft`, `sidebar_label` and `sidebar_position`.
- Landing pages (`layout: home`) with a hero, actions and feature cards.
- Partials (`_name.md`) and drafts. `--drafts` builds drafts too.

**Live preview**

- `ckdocs serve` updates the open page in the browser as soon as a file is
  saved, with no manual reload. It watches every file that pages include,
  and caches diagrams between rebuilds.

**Agents and search engines**

- `llms.txt` (v2) is on by default.
- Every page has a clean Markdown version at its URL plus `.md`, with
  discovery links in the page head and a Markdown link in the footer. The
  site index's footer also links to `llms.txt`.
- `--site-url` sets the publishing host for `sitemap.xml` and `llms.txt`
  without editing the configuration.

**Portals**

- A portal site's own logo, favicon and brand now appear in the header of
  that site's pages.
- Links between the sites of a portal resolve to the other site's built
  pages.
- Portals and sites can share a favicon.

**Interface**

- Compact icon controls in the header: GitHub, an expandable search box
  (⌘K / Ctrl+K), and a one-icon colour theme picker (system, light, dark).
- Navigation uses full-row disclosures. The current section starts
  expanded.
- Landing pages and navigation are kept when a site is published on Pages.
- Fixed dotted flowchart edge labels.

### Documentation

- The docs are now two manuals in one portal:
  - **ck-git-hosting** under `hosting/`;
  - **ckdocs** under `ckdocs/`, with a landing page.

  The site menu in the header switches between them, and former page
  addresses redirect to their new places.
- New logos and favicons for both manuals, and an illustrated hero image for
  ckdocs.
- The ckdocs manual covers every feature. New pages:
  - getting started;
  - code blocks;
  - look and feel;
  - translations;
  - several sites in one build;
  - search and AI tools;
  - command line;
  - limits.
