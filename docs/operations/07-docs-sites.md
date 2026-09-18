# Documentation sites from Markdown

`ckdocs` turns a repository's `README.md` and a `docs/` tree into a
self-contained static documentation site — a logo and site title, top-level
tabs, a sidebar with the current tab's pages and the current page's own
sections, breadcrumbs, previous/next, and a footer. It is a separate,
dependency-free binary (see [Packages, continuous integration, and
releases](02-packages-and-releases.md)): no JavaScript, no network access,
and no assumptions about what else is installed. This repository publishes
its own documentation with it; every example below is this repository.

## Your first site in three commands

```text
ckdocs build --root . --out public
open public/index.html               # macOS; xdg-open on Linux
ckdocs check --root .
```

`build` writes a complete site to `--out` (default `<root>/public`); opening
its `index.html` directly from the filesystem works, because every link
between pages is relative. `check` does the same build into a private
temporary directory that is always removed afterward, and treats every
warning as a failure — run it before you publish, and let a CI step run it
on every push (see [Publishing](#publishing-on-ck-git-pages-and-github-pages)
below).

## Preview locally

```text
ckdocs serve --root .
```

Builds the same way `build` does — into `--out` when given, replacing a
previous `ckdocs` site there the same way `--clean` would, or otherwise into
a private directory removed when the command exits — and serves it on
`127.0.0.1:8422` (`--port` to choose another; `127.0.0.1` only, deliberately
never reachable from another machine, unlike `ck-pagesd`) until you press
Ctrl+C, using the same directory-to-`index.html` folding and
extension-to-content-type mapping a published Pages site uses. Since every
page link is already relative, opening `index.html` straight from the
filesystem looks the same in the ordinary case; `serve` is worth it for the
cases that would not — testing what a real 404 or a file's actual served
content type looks like — and for previewing from a browser's dev tools the
way you would the published site. There is no rebuild on change in this
version — edit a page, then rerun the command.

## How pages are found, named, and ordered

- **Which files.** Every `.md`/`.markdown` file under the *source tree* —
  `source:` in `ckdocs.yml`, else `docs/` when it exists, else the
  repository root. Inside a Git work tree, only files Git actually tracks
  are considered, exactly like `git ls-files`: a gitignored directory (this
  repository's own `docs/planning/`, for tracked instance) is never read,
  even though it exists on disk, so nothing you deliberately excluded from
  the repository leaks into a published site by accident. Outside a work
  tree (a release tarball, a CI sandbox's checkout), a plain directory walk
  is used instead, skipping dotfiles, dot-directories, and symlinks.
  `exclude:` patterns (see below) remove further candidates before anything
  else happens.
- **The home page.** `home:` in the config; else the source tree's own
  `README.md`/`index.md`; else the repository root's `README.md`; else the
  first page in path order. The home page always becomes the site's root
  `index.html`; two pages that would land on the same output path (most
  often two candidate home pages) is a build error naming both.
- **Output paths.** Every other page keeps its path relative to the source
  tree with `.md`/`.markdown` replaced by `.html`; a directory's own
  `README.md`/`index.md` becomes that directory's `index.html`. So
  `docs/operations/01-installation.md` is served as
  `operations/01-installation.html`.
- **Titles.** Front matter `title:`; else the page's first `#` heading,
  rendered to plain text (a link keeps its label, an image its alternative
  text, code its content — never text found inside a fenced code block);
  else the filename with a leading run of digits and the `-`/`_` right
  after it removed, remaining `-`/`_` turned into spaces, and only the
  first letter capitalized (`01-getting-started.md` → "Getting started";
  `ci-yml-reference.md` → "Ci yml reference" — write `title:` when a
  filename does not capitalize the way you want). A directory that has no
  explicit `nav:` title takes its own `README.md`/`index.md`'s title the
  same way, else its own directory name.
- **Order.** Front matter `nav_order` (an integer, ascending; a directory's
  own is its `README.md`/`index.md`'s), then filename — so this
  repository's `01-`, `02-`, … prefixes order themselves with no
  configuration. `nav_exclude: true` keeps a page out of the sidebar and
  tabs while still building it and listing it on the site index (a
  changelog you link to directly is the usual case).
- **Navigation**, when `ckdocs.yml` has no `nav:`: a **Home** tab, then one
  tab per first-level directory of the source (titled as above) or
  top-level page (a one-page tab), each in the order just described;
  subdirectories become nested groups the same way, up to 4 levels deep
  overall (a tab, then up to two levels of group, then pages) — a
  directory that would sit deeper is folded flat into its parent instead of
  being silently dropped. An explicit `nav:` (below) names tabs and groups
  directly; every page it does not mention is still built and reachable
  from the generated site index, so nothing tracked ever silently
  disappears.

## Front matter

A leading block delimited by `---` lines, in the same small scalar syntax
`.ckgit/ci.yml` uses (plain, single-, or double-quoted values, and exactly
the four escapes `\\ \" \n \t`), at most 64 lines and 8 KiB between the
fences:

```yaml
---
title: Getting started
description: The three commands to build and check a site.
nav_order: 1
---
```

| Key | Type | Effect |
|---|---|---|
| `title` | string, ≤ 128 bytes | This page's title, overriding the filename/heading rule above. |
| `description` | string, ≤ 1024 bytes | A one-line summary, used as the page's `<meta name="description">`. |
| `nav_order` | integer | This page's (or, on a directory's index page, that directory's) order among its siblings. |
| `nav_exclude` | `true`/`false` | Keep this page out of the sidebar and tabs (still built, still linked from the site index). |

An unrecognized key is ignored — front matter written for Jekyll or Hugo is
common to paste in — but `ckdocs check`/`--strict` reports it as a warning,
so a typo such as `nave_order` is still caught rather than silently doing
nothing. A block that has the *shape* of front matter but is not valid in
this syntax (a duplicate key, an unterminated quote, a key with nothing
after it) is still recognized as front matter — its content never leaks
onto the rendered page as literal text — and is reported the same way,
naming the exact problem and line. A file that merely starts with a
thematic break (`---`, then prose, then another `---`) has no front matter
at all; it is correctly left as ordinary body content. The dashboard's
rendered preview of a Markdown file hides its front matter the same way;
the file's own Source view still shows it.

## `ckdocs.yml` reference

An optional file at the repository root. Its absence means every default
above; once it exists, `site.title` is required and every other key stays
optional. Unlike a page's front matter, an unknown top-level or `site.`
key is a build error — this file is the project's own, not pasted from
elsewhere.

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
| `home` | path to a Markdown page | see [above](#how-pages-are-found-named-and-ordered) | Overrides which page becomes the site's `index.html`. |
| `exclude` | list of patterns | none | Each entry is a path (a whole subtree) or a prefix ending in one `*`; matched against both the repository-relative and the source-relative form of every candidate, so `docs/drafts` and, with `source: docs`, plain `drafts` both work. Up to 64 entries. |
| `nav` | list of entries (below) | derived, see above | Names the tabs and groups explicitly. Nesting is bounded to 4 levels; every path named must be a page the site would otherwise build. |
| `search` | `true`/`false` | `false` | Reserved for opt-in search (a later addition); accepted and validated today but has no effect yet. |

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
      - title: Continuous delivery   # a nested group (this repository's own ckdocs.yml
        pages:                       # groups its CI/release pages exactly this way)
          - docs/operations/04-ci-cd.md
```

Every path is repository-root-relative and must name an existing page.
Nesting depth counts a tab as level 1: a group may nest to level 3, so its
pages sit at level 4 — the same bound the zero-config derivation above
observes.

## Markdown

The renderer is the same bounded CommonMark subset (plus the GitHub
extensions below) the read-only dashboard uses for READMEs and Markdown
files, so a page looks the same wherever it is read.

| Construct | Syntax | Notes |
|---|---|---|
| Headings, paragraphs, emphasis, inline code, links, images, autolinks | as in CommonMark | Every heading gets a GitHub-style slug `id`, deduplicated (`install`, `install-1`, …). |
| Blockquotes, lists | as in CommonMark | Ordered, unordered, nested, loose or tight. |
| Fenced and indented code | ` ``` ` / four-space indent | A fence's info string (` ```cpp `) also selects syntax highlighting for C/C++, Python, shell, YAML, and JSON; an unrecognized name renders as plain code, never an error. |
| Tables | GFM pipe tables | With `:---`/`---:`/`:---:` alignment. |
| Alerts | a blockquote whose first line is exactly `[!NOTE]`, `[!TIP]`, `[!IMPORTANT]`, `[!WARNING]`, or `[!CAUTION]` | Renders as a titled, coloured callout; a marker with trailing text, in lowercase, or with nothing below it stays an ordinary blockquote. |
| Task lists | `- [ ] text` / `- [x] text` | A disabled checkbox; the list gets a distinct class for styling. |
| Strikethrough | `~~text~~` | Exactly two tildes; one or three are literal. |

**Deliberately unsupported:** raw HTML (always shown as literal, escaped
text — an inline comment is simply removed, but a `<div>` never becomes a
live element), footnotes, definition lists, emoji shortcodes, and math.
Supporting raw HTML in a tool meant to be pointed at arbitrary Markdown
would mean either sandboxing a site's own markup — defeating the point of a
generator that ships no script of its own — or trusting it outright, which
this product does not do anywhere else either. A small, entirely
predictable renderer is the design point, not an oversight.

## Links and assets

Inside a page, an ordinary link's `[label]` and `(destination)`, or an
image's `![alt text]` and `(destination)`:

- `http://`, `https://`, `mailto:`, and a pure `#fragment` pass through
  unchanged.
- Anything else is resolved relative to the *linking page's own source
  path* (or the repository root, with a leading `/`):
  - a target naming another page — `other.md`, or a directory whose
    `README.md`/`index.md` is a page (`sub/`, or `sub` with a trailing
    slash implied) — becomes a link to that page's own `.html`, itself
    relative to the linking page's output location, so the whole site
    keeps working from `file://` and under any URL prefix;
  - a target naming any other existing, non-symlink file up to 1 GiB is
    copied into the site at its own repository-relative path (relative to
    the source tree, or root-relative outside it) and linked there — the
    **pull model**: a file ships only because a page actually references
    it, so a source tree can hold drafts and private notes without
    publishing them by accident;
  - anything else (a missing target, a directory with no index page, a
    `.md` file outside the site) is reported and rendered as plain,
    visible text rather than a link to nowhere.
- A `#fragment` on a page link is checked, after the whole site renders,
  against the target page's actual heading ids; a fragment that names
  nothing is reported the same way a missing target is.

`ckdocs build` still writes the site when something is reported — the
broken reference stays visible as text, which is usually more useful than
an aborted build — and exits `3`. `--strict` (and `check`, which always
implies it) turns the same reports into a build failure, exit `1`, printing
each one naming the page and the target, so an editor's "next error"
shortcut goes straight to it.

## Publishing on ck-git Pages and GitHub Pages

The same output directory is published, unchanged, on both hosts:

| | ck-git Pages (the LAN) | GitHub Pages |
|---|---|---|
| Where the build runs | this project's own sandboxed CI, on push | a GitHub Actions workflow, on push to `master` |
| How `ckdocs` gets there | compiled by that CI run's own `make … all` | compiled by that workflow's own `make … client` — no dependency on a released package |
| What publishes it | a top-level `pages: { path: public }` in `.ckgit/ci.yml`, read by [`ck-pagesd`](04-ci-cd.md#pages) | `actions/upload-pages-artifact` + `actions/deploy-pages` in `.github/workflows/pages.yml` |
| Reached at | `http://<server>:<pages_http_port>/ck-git-hosting/`, and the dashboard's **Docs** button | `https://cklukas.github.io/ck-git-hosting/` |

Both publish only a successful build of the repository's own default
branch — a tag build never touches either (see [Pages](04-ci-cd.md#pages)
for the LAN side's exact rule). This repository's own `.ckgit/ci.yml` and
`.github/workflows/pages.yml` are the worked example: a `docs` step running
`ckdocs build --root "$PWD" --out "$PWD/public" --strict` right after
`check`, so a broken link fails the build *before* either host ever
publishes it — the [`docs_site.sh`](../../tests/integration/docs_site.sh)
integration test enforces exactly that on every push, against this same
`ckdocs.yml`.
