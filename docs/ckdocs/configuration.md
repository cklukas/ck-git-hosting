---
description: "Every ckdocs.yml setting: site details, navigation, search, sitemap and llms.txt, and portals with several sites."
---
# Site configuration

## `ckdocs.yml` reference

`ckdocs.yml` is optional. Without it, ckdocs uses the defaults below and
[derives navigation from your files](pages.md). When the file is present,
`version` and `site.title` are required. Unknown top-level, `site.`, and
`front_matter.` keys are build errors. A `sites:` list builds several sites
at once instead — see [Several sites in one build](#several-sites-in-one-build).

| Key | Type | Default | Notes |
|---|---|---|---|
| `version` | integer | — (required) | Must be `1`. |
| `site.title` | string, ≤ 128 bytes | — (required once the file exists) | The site's name: header brand text, `<title>` suffix, and the default for `site.brand`. |
| `site.brand` | string, ≤ 128 bytes | `site.title` | Header wordmark, when it should differ from the title. |
| `site.logo` | path | none | An image shown before the brand; copied into the site. |
| `site.description` | string, ≤ 1024 bytes | none | The home page's `<meta name="description">`. |
| `site.footer` | string, ≤ 1024 bytes | none | Footer text, alongside "Built with ckdocs `<version>`". |
| `site.links` | list of `{title, url, icon?}` | none | Up to 8 header links; `url` must be `http://`, `https://`, or `mailto:`. Optional `icon: github` displays a circular GitHub icon link; `title` supplies its accessible name and tooltip. Omit `icon` for a text link. |
| `site.stylesheet` | path | none | An extra stylesheet, copied as-is and linked after the built-in one. |
| `site.url` | absolute `http://` or `https://` URL | none | Where the site is published, such as `https://example.github.io/project/`; a missing trailing `/` is added. With it, the build also writes `sitemap.xml`, and `llms.txt` links are absolute. See [Sitemap and llms.txt](#sitemap-and-llmstxt). |
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

Category tabs open a dropdown when you click anywhere on their label or
chevron; individual page tabs remain links. The category's overview page is
inside the dropdown. Escape closes the menu and returns focus to its label;
clicking outside also closes it. Native disclosure controls work without JavaScript.

Sidebar categories use borderless disclosure rows. The category containing
the current page starts expanded; other categories start collapsed. If an
index page would repeat its category's name, its navigation label becomes
**Overview**. Its page title and breadcrumbs remain unchanged.

## Search

Without `search: true`, the [generated site index](pages.md#how-pages-are-found-named-and-ordered)
is the answer to "how do I find something" — every page and heading, one
plain list, no script needed. `search: true` adds a round search button to
the header. Clicking it or pressing Cmd/Ctrl+K expands and focuses the input;
Escape collapses it and returns focus to the button. Clicking outside also
collapses it. Search is backed by a generated `search-index.json` (each page's title,
URL, and one entry per top-level heading with a plain-text excerpt of that
section, up to 200 bytes) and a small inline script — under 2 KiB, part of
the site itself, never fetched from anywhere else — that fetches the index
on first use, filters it client-side as you type, and lists matching
sections linking straight to their heading. A page whose title or
[description](front-matter.md#page-descriptions) matches is listed itself
first, with its description as the summary. It runs only on the page it
ships with, the Pages origin, never the dashboard.

The box starts hidden and only that script reveals it, so a visitor with
scripting disabled never sees a non-functional input — they get the
`<noscript>` fallback instead, a plain link back to the site index, exactly
the no-script answer that already exists. The index itself is capped at
2 MiB; a site large enough to exceed that keeps as many complete pages as
fit and drops the rest from the end, reported as a build warning rather
than failing the build — in practice, thousands of pages would be needed
to get there.

## Sitemap and llms.txt

Every build writes `llms.txt` at the output root: a plain-text index of the
site for AI tools, in the [llms.txt](https://llmstxt.org/) format. It holds
the site title, `site.description` as a quote, one section per tab with each
page as a Markdown link and its front matter
[`description`](front-matter.md#page-descriptions), and the pages outside
the navigation under an `Optional` section. A page without a description
gets the first sentence of its text instead (at most 200 bytes), so no
entry is a bare link. Explicit descriptions are better, because they also
appear in the site index and in search results.

```text
# ck-git-hosting

> Private Git hosting, continuous integration, and documentation for your server.

## Guides

- [Set up the client](https://cklukas.github.io/ck-git-hosting/guides/client-setup.html)
```

With `site.url` set, the build also writes `sitemap.xml`, listing every page's
absolute URL in navigation order, with a page's front matter `date` as its
`<lastmod>`. A sitemap needs absolute URLs, so without `site.url` there is
none, and `llms.txt` links stay relative to the site root. A portal writes
one of each at its root, covering every site, with its own `site.url`. A
linked file that would be copied to one of these generated names is
reported as a broken link instead.

## Several sites in one build

One build can hold several documentation sites — two editions of a project,
say, each maintained on its own Git branch — behind a generated welcome page.
A top-level `sites:` list turns `ckdocs.yml` into such a *portal*
configuration:

```yaml
version: 1
site:
  title: paglets
  description: Mobile agents for Python and C++.
home: welcome.md
sites:
  - id: cpp
    title: C++ edition
    description: The native port, built from this branch.
    config: docs/ckdocs.yml
  - id: python
    title: Python edition
    description: The original implementation.
    ref: main
    redirect_from_root: true
```

In a portal configuration `site` describes the portal (its title, brand,
logo, description, footer, links, and stylesheet), and `home` names an
optional Markdown page whose content appears on the welcome page above the
list of sites. The keys that describe one page tree — `source`, `nav`,
`search`, `exclude`, and `front_matter` — are build errors next to `sites`;
they belong in each site's own configuration.

| Key | Type | Default | Notes |
|---|---|---|---|
| `sites` | list, 1 to 8 entries | none | Makes the file a portal configuration. |
| `sites[].id` | string | — (required) | 1 to 32 lowercase letters, digits, or `-`, not starting with `-`, unique. The site's output directory and the prefix of its findings. |
| `sites[].title` | string, ≤ 128 bytes | — (required) | Shown in the site selector and on the welcome page; also the site's title when its own configuration has none. |
| `sites[].description` | string, ≤ 1024 bytes | none | Shown under the title on the welcome page. |
| `sites[].config` | path | `ckdocs.yml` | The site's own configuration, relative to the site's root. It is an ordinary single-site configuration and must not list `sites` itself. A missing default file means built-in defaults; a missing explicit one is an error. |
| `sites[].ref` | branch, tag, or commit | none | The site's root is the committed tree of this Git ref instead of the work tree root — see [Sites from other branches](#sites-from-other-branches). |
| `sites[].redirect_from_root` | `true`/`false` | `false` | At most one site. Writes a redirect page at each of the site's old root-level page paths — see [Redirects](#redirects). |

A site without `ref` and without `config` would read the portal's own
`ckdocs.yml`, which lists `sites`, so such a site always needs a `config` of
its own, for example `docs/ckdocs.yml` with `source: docs`.

### Output layout

| Path in `--out` | Content |
|---|---|
| `index.html` | The welcome page: the portal title, description, the `home` intro, then one card per site linking to its home page. |
| `<id>/` | The site, laid out exactly as a single-site build would lay it out: its own `index.html`, `site-index.html`, `search-index.json` when its configuration turns search on, and its assets. |
| the portal's logo, stylesheet, and files the intro references | At their path relative to the work tree root. |
| redirect pages | At the old page paths of the `redirect_from_root` site. |
| `.ckdocs` | One marker for the whole output. |

Every page of a site shows the portal's brand and logo, linking to the
welcome page, followed by a site selector: a small dropdown, built on a
native disclosure element so it needs no script, that lists every site's
home page and marks the current one. The site's own `site.title` stays in
the page title and the sidebar, and its tabs, search, and site index cover
only that site. A site's own `links` and `footer` are shown on its pages;
when it has none, the portal's are shown instead. The same applies to
`stylesheet`. A site's `logo` is not shown, because the portal's brand
replaces it. All links stay relative, so the result works from `file://`
and under any URL prefix. The `--out` and `--clean` rules and the Pages size
and entry limits apply to the whole build.

Each site's links are checked within that site, as in a single-site build,
and its broken links, broken heading fragments, and warnings are reported
with its id as a prefix, for example `python: docs/guide.md:3: link target
'nope.md' is not a page of this site`. The intro page's links may point to a
file under the work tree root, which is copied, or to a page of a site as
`<id>/<page>.html` — written from the root, for example
`/python/guide.html`. A fragment-only link in the intro must name one of its
own headings. `--strict` and `ckdocs check` treat these findings as they do
in a single-site build.

### Sites from other branches

A site with `ref` is rooted at the committed tree of that Git ref, so a
branch can build another branch's documentation alongside its own. ckdocs
finds the tree in this order:

1. The directory given with `--ref-tree REF=DIR` for exactly that ref. `DIR`
   is relative to the current directory and is read like any root: tracked
   files if it is a Git work tree, a directory walk otherwise.
2. Inside a Git work tree, `<ref>` or else `origin/<ref>`, extracted with
   `git archive` into a private temporary directory that is removed after
   the build. Uncommitted changes on that branch are not included.
3. Otherwise the build fails with, for example:
   `site 'python': ref 'main' is not available (no Git repository here, or a clone without that branch); fetch it, or pass --ref-tree main=DIR`.

Several sites may name the same ref; it is read once. The site's `config`
and all paths inside it are relative to that tree.

```sh
ckdocs build --clean
ckdocs build --ref-tree main=../project-main --out public --clean
```

[Publishing](publishing.md#several-sites-from-several-branches) shows how to
make the other branch available on GitHub Actions and in ck-git CI.

### Redirects

When a project that used to publish one site becomes a portal, its old URLs
would point at nothing. `redirect_from_root: true` on the site that used to
be the whole build writes, for each of that site's pages except its home
page, a small page at the old path that refreshes immediately to the page's
new place under `<id>/`, carries a canonical link to it, and shows a visible
link for anything that does not follow the refresh. The old `index.html` is
the welcome page now, so it is not redirected. A fragment in an old URL is
not carried over, because the redirect uses no script.

A redirect path that falls inside another site's directory, or that another
output of the portal already uses, is a build error naming the page; rename
the page or drop `redirect_from_root`.
