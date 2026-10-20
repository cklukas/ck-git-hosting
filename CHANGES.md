# Changes

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
