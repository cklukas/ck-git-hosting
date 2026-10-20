---
description: Publish a project's documentation as a website with ckdocs and ck-git Pages, straight from its Markdown.
---
# Documentation sites

Every project can publish its documentation as a website. `ckdocs`, which is
installed with ck-git-hosting (and packaged on its own as `ckdocs`), turns the
project's `README.md` and `docs/` Markdown into a static site with navigation,
search, diagrams, math and highlighted code; ck-git Pages then serves it on
your network.

## From Markdown to a published site

1. **Write.** Keep the documentation as Markdown in the repository, as usual.
   Optionally add a `ckdocs.yml` with the site's title and navigation.
2. **Preview.** `ckdocs serve --root .` shows the site on
   `http://127.0.0.1:8422` and updates the open page as you edit.
3. **Build in CI.** A job runs `ckdocs build --out public --strict`, which
   fails on any broken link.
4. **Publish.** A top-level `pages: { path: public }` in the project's
   [`.ckgit/ci.yml`](06-ci-yml-reference.md) hands the built site to
   [Pages](pages-hosting.md#pages); the dashboard's **Docs** button opens it.

```yaml
version: 1
pages:
  path: public
jobs:
  - name: docs
    steps:
      - run: ckdocs build --out public --strict
```

## The ckdocs manual

ckdocs has a manual of its own, published next to this one — pick it from the
site selector in the header:

| To | Read |
|---|---|
| Build, check and preview a first site | [Get started with ckdocs](../ckdocs/getting-started.md) |
| Arrange pages and navigation | [Pages and navigation](../ckdocs/pages.md) |
| Format content: containers, tabs, footnotes, math, includes | [Markdown, links, and assets](../ckdocs/markdown.md) |
| Show code from the repository | [Code blocks](../ckdocs/code.md) |
| Draw diagrams | [Mermaid diagrams](../ckdocs/mermaid.md) |
| Publish in several languages | [Translations](../ckdocs/translations.md) |
| Publish on ck-git Pages and GitHub Pages | [Publishing](../ckdocs/publishing.md) |
| Look up a setting or an option | [Configuration](../ckdocs/configuration.md), [Command line](../ckdocs/cli.md) |

This documentation is itself built with ckdocs, as a
[portal](../ckdocs/portals.md) of two sites: ck-git-hosting and ckdocs.
