---
title: ckdocs
layout: home
description: ckdocs turns a repository's Markdown into a fast, static documentation site — one binary, no dependencies.
hero:
  name: ckdocs
  text: Documentation sites from Markdown
  tagline: >
    One small binary turns a repository's Markdown into a complete, static
    documentation site: navigation, search, diagrams, math and highlighted
    code — no Node, no plugins, no external services.
  image:
    src: images/ckdocs.svg
    alt: The ckdocs logo
  actions:
    - text: Get started
      link: getting-started.md
      theme: brand
    - text: Configuration
      link: configuration.md
    - text: Command line
      link: cli.md
features:
  - icon: ":zap:"
    title: Fast and dependency-free
    details: A native binary builds a site in milliseconds; the preview server updates the open page as you type.
    link: cli.md
  - icon: ":memo:"
    title: Markdown that goes further
    details: Containers, tabs, footnotes, definition lists, emoji, TeX math and includes, on top of GitHub's Markdown.
    link: markdown.md
  - icon: ":art:"
    title: Diagrams and code
    details: 23 Mermaid diagram types drawn at build time, and code highlighted in 73 languages with titles, line marks and copy buttons.
    link: code.md
  - icon: ":globe_with_meridians:"
    title: Many languages
    details: Translations with a language menu and fallbacks, and the site's own words in 21 built-in languages.
    link: translations.md
  - icon: ":mag:"
    title: Search and AI ready
    details: Built-in search, a site index, sitemap.xml, llms.txt and a clean Markdown version of every page.
    link: search.md
  - icon: ":white_check_mark:"
    title: Checked before it ships
    details: Every link, heading anchor and include is checked; ckdocs check fails a CI job on the first broken one.
    link: getting-started.md
---
# Why ckdocs

ckdocs reads the Markdown already in a repository — a `README.md` and a
`docs/` tree — and writes a self-contained website: every page with tabs, a
sidebar, an outline, breadcrumbs and previous/next links, plus a site index,
search, and files for search engines and AI tools. The result is plain HTML and
CSS with a few small inline scripts; it works from `file://`, under any URL
prefix, and on any static host.

- **Nothing to install around it.** No package manager, no runtime, no theme
  to download: the binary is the whole tool, so it runs the same on a laptop,
  in a CI sandbox, or on a Raspberry Pi.
- **Safe by construction.** Pages are only what Git tracks, files ship only
  when a page links to them, and raw HTML is limited to a few attribute-free
  tags, so a site can never carry a script you did not mean to publish.
- **Familiar syntax.** Pages written for GitHub, VitePress or Docusaurus —
  their alerts, containers, code-block options, front matter and home pages —
  carry over largely unchanged.

Start with [Get started](getting-started.md), or browse the guides in the
sidebar.
