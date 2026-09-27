# Site configuration

## `ckdocs.yml` reference

`ckdocs.yml` is optional. Without it, ckdocs uses the defaults below and
[derives navigation from your files](pages.md). When the file is present,
`version` and `site.title` are required. Unknown top-level, `site.`, and
`front_matter.` keys are build errors.

| Key | Type | Default | Notes |
|---|---|---|---|
| `version` | integer | — (required) | Must be `1`. |
| `site.title` | string, ≤ 128 bytes | — (required once the file exists) | The site's name: header brand text, `<title>` suffix, and the default for `site.brand`. |
| `site.brand` | string, ≤ 128 bytes | `site.title` | Header wordmark, when it should differ from the title. |
| `site.logo` | path | none | An image shown before the brand; copied into the site. |
| `site.description` | string, ≤ 1024 bytes | none | The home page's `<meta name="description">`. |
| `site.footer` | string, ≤ 1024 bytes | none | Footer text, alongside "Built with ckdocs `<version>`". |
| `site.links` | list of `{title, url}` | none | Up to 8 header links; `url` must be `http://`, `https://`, or `mailto:`. |
| `site.stylesheet` | path | none | An extra stylesheet, copied as-is and linked after the built-in one. |
| `source` | path | `docs/` if it exists, else the repository root | The page tree, relative to the repository root. |
| `home` | path to a Markdown page | see [page discovery](pages.md#how-pages-are-found-named-and-ordered) | Overrides which page becomes the site's `index.html`. |
| `exclude` | list of patterns | none | Each entry is a path (a whole subtree) or a prefix ending in one `*`; matched against both the repository-relative and the source-relative form of every candidate, so `docs/drafts` and, with `source: docs`, plain `drafts` both work. Up to 64 entries. |
| `nav` | list of entries (below) | derived from pages | Names the tabs and groups explicitly. Nesting is bounded to 4 levels; every path named must be a page the site would otherwise build. |
| `search` | `true`/`false` | `false` | Adds client-side search — see [Search](#search). |
| `front_matter.foreign_keys` | list of keys | none | Front matter keys another tool reads, accepted silently and carried as `front-matter:<key>` meta tags — see [Front matter](front-matter.md#front-matter). Up to 32 entries, each 1–64 ASCII letters, digits, `.`, `_`, or `-`, listed once, and none of the keys ckdocs reads itself (`title`, `description`, `author`, `date`, `nav_order`, `nav_exclude`); `front_matter:` takes no other key. |

A `nav:` entry is one of:

```yaml
nav:
  - docs/quick-start.md              # a bare page path: a one-page tab, titled by the page
  - page: README.md                  # the same, spelled out
    title: Home                      # ... with an explicit tab title
  - title: Operations                # a group: a tab or, nested, a sub-group
    pages:
      - docs/operations/01-a.md
      - docs/operations/02-b.md
      - title: Continuous delivery   # a nested group
        pages:
          - docs/operations/04-ci-cd.md
```

Every path is repository-root-relative and must name an existing page.
Nesting depth counts a tab as level 1: a group may nest to level 3, so its
pages can sit at level 4. The same limit applies to automatic navigation.

## Search

Without `search: true`, the [generated site index](pages.md#how-pages-are-found-named-and-ordered)
is the answer to "how do I find something" — every page and heading, one
plain list, no script needed. `search: true` adds a proper search box to
the header, backed by a generated `search-index.json` (each page's title,
URL, and one entry per top-level heading with a plain-text excerpt of that
section, up to 200 bytes) and a small inline script — under 2 KiB, part of
the site itself, never fetched from anywhere else — that fetches the index
on first use, filters it client-side as you type, and lists matching
sections linking straight to their heading. It runs only on the page it
ships with, the Pages origin, never the dashboard.

The box starts hidden and only that script reveals it, so a visitor with
scripting disabled never sees a non-functional input — they get the
`<noscript>` fallback instead, a plain link back to the site index, exactly
the no-script answer that already exists. The index itself is capped at
2 MiB; a site large enough to exceed that keeps as many complete pages as
fit and drops the rest from the end, reported as a build warning rather
than failing the build — in practice, thousands of pages would be needed
to get there.
