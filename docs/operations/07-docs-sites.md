---
description: Build a documentation site from Markdown with ckdocs, preview it with live rebuilds, and check it for broken links.
---
# Get started with ckdocs

`ckdocs` turns Markdown into a static documentation site with navigation,
search, and Mermaid diagrams. It is a standalone binary with no external
rendering tools. Generated sites work offline and under a URL prefix.

## Build your first site

Put a `README.md` at your repository root and additional pages in `docs/`,
then run:

```sh
ckdocs build --root . --out public
ckdocs check --root .
```

Open `public/index.html` in a browser. `check` reports broken links and other
warnings without leaving an output directory. Within a Git repository, pages
must be tracked by Git to be included.

## Preview locally

```sh
ckdocs serve --root .
```

Open `http://127.0.0.1:8422`. Use `--port` to choose another port and press
Ctrl+C to stop. While it runs, `serve` watches the configuration, the pages,
and the source tree and rebuilds the site about a second after you save a
change; reload the browser page to see it. A rebuild that fails, for example
on a configuration error, prints the error and keeps serving the last good
site. Inside a Git repository a new page appears once Git tracks it
(`git add`). Use `--no-watch` to serve the first build only.

Use `--out public` to keep the generated files. An existing ckdocs output is
replaced by `serve`; with `build`, add `--clean` to replace it explicitly.

## Build several sites at once

A configuration with a `sites:` list builds a portal: a welcome page at the
output root and each site under its own directory, every page with a site
selector next to the brand. A site can come from another Git branch; when
that branch is not available in the current repository, pass its tree with
`--ref-tree`:

```sh
ckdocs build --root . --out public --clean
ckdocs check --root . --ref-tree main=../project-main
```

`--ref-tree REF=DIR` may be repeated and works with `build`, `check`, and
`serve`. `--source` does not apply to a portal; each site sets its source in
its own configuration. See
[Several sites in one build](../ckdocs/configuration.md#several-sites-in-one-build).

## Continue with a guide

| Task | Guide |
|---|---|
| Arrange pages and navigation | [Pages and navigation](../ckdocs/pages.md) |
| Set page titles, authors, and dates | [Page metadata](../ckdocs/front-matter.md) |
| Configure branding, navigation, and search | [Site configuration](../ckdocs/configuration.md) |
| Combine several sites or branches | [Several sites in one build](../ckdocs/configuration.md#several-sites-in-one-build) |
| Format content and link files | [Markdown, links, and assets](../ckdocs/markdown.md) |
| Add a diagram | [Mermaid and 23 examples](../ckdocs/mermaid.md) |
| Publish the site | [Publishing](../ckdocs/publishing.md) |
