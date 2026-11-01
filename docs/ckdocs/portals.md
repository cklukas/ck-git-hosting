---
description: Build several documentation sites in one portal — with a welcome page, a site selector, links between the sites, sites from other Git branches, and redirects from old addresses.
---
# Several sites in one build

## A portal

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
logo, favicon, description, footer, links, and stylesheet), and `home` names an
optional Markdown page whose content appears on the welcome page above the
list of sites. The keys that describe one page tree — `source`, `nav`,
`search`, `exclude`, `front_matter`, `toc`, `markdown` and `i18n` — are build
errors next to `sites`; they belong in each site's own configuration, where
each site may also have translations of its own. `site.lang` and `ui` set the
welcome page's language and words.

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
| the portal's logo, favicon, stylesheet, and files the intro references | At their path relative to the work tree root. |
| redirect pages | At the old page paths of the `redirect_from_root` site. |
| `.ckdocs` | One marker for the whole output. |

### The header of a site's pages

Every page of a site shows a brand linking to the welcome page, followed by a
site selector: a small dropdown, built on a native disclosure element so it
needs no script, that lists every site's home page and marks the current one.
The site's own `site.title` stays in the page title and the sidebar, and its
tabs, search, and site index cover only that site.

A site may carry its own mark. Its `site.logo`, `site.brand` and
`site.favicon`, when its configuration names them, replace the portal's on
its pages, so readers see at a glance which manual they are in; a site that
names none shows the portal's. Its own `links`, `footer` and `stylesheet`
work the same way: the site's when it has them, else the portal's. The
welcome page always shows the portal's. All links stay relative, so the
result works from `file://` and under any URL prefix. The `--out` and
`--clean` rules and the Pages size and entry limits apply to the whole build.

### Links between the sites

Sites read from the same tree link to each other the way their files do:
a link from a page of one site to a Markdown file that is a page of another
(`[the ckdocs manual](../ckdocs/getting-started.md)`), or to a directory
holding another site's `README.md` or `index.md`, leads to that page in the
other site's directory. The link is checked like any other: a target that is
a page of no site is reported. Heading fragments are checked only within a
site; a fragment into another site is kept as written. A site from another
Git ref (below) is read from a tree of its own, so its pages are not found
this way; link to them with their published `https://` address.

### Findings

Each site's links are checked within that site (and the portal's other
sites, as above), and its broken links, broken heading fragments, and
warnings are reported with its id as a prefix, for example `python:
docs/guide.md:3: link target 'nope.md' is not a page of this site`. The intro page's links may point to a
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
   the build. The bundled archive library performs extraction; no external
   `tar` is required. Only regular files and directories are materialized;
   links are omitted. Uncommitted changes on that branch are not included.
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

## This documentation

The manual you are reading is a portal of two sites read from one repository:
`hosting` (ck-git-hosting itself, from `docs/` with its own
`docs/ck-git-hosting.yml`) and `ckdocs` (this manual, from `docs/ckdocs/`
with `docs/ckdocs/ckdocs.yml`). The repository's `ckdocs.yml` lists them,
gives the portal its brand, logo and favicon, and sets
`redirect_from_root: true` on `hosting`, which used to be the whole build.
The ckdocs site names a logo and favicon of its own.
