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
Ctrl+C to stop. Rerun the command after editing pages; this version does not
automatically rebuild them.

Use `--out public` to keep the generated files. An existing ckdocs output is
replaced by `serve`; with `build`, add `--clean` to replace it explicitly.

## Continue with a guide

| Task | Guide |
|---|---|
| Arrange pages and navigation | [Pages and navigation](../ckdocs/pages.md) |
| Set page titles, authors, and dates | [Page metadata](../ckdocs/front-matter.md) |
| Configure branding, navigation, and search | [Site configuration](../ckdocs/configuration.md) |
| Format content and link files | [Markdown, links, and assets](../ckdocs/markdown.md) |
| Add a diagram | [Mermaid and 23 examples](../ckdocs/mermaid.md) |
| Publish the site | [Publishing](../ckdocs/publishing.md) |
