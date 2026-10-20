---
description: Every ckdocs command and option — build, check and serve — with their defaults, the live preview server, and the exit codes scripts and CI rely on.
---
# Command line

## Commands

```text
ckdocs build [--root DIR] [--source DIR] [--site-url URL] [--config FILE] [--ref-tree REF=DIR]...
             [--out DIR] [--clean] [--drafts] [--strict] [--quiet]
ckdocs check [--root DIR] [--source DIR] [--site-url URL] [--config FILE] [--ref-tree REF=DIR]...
             [--drafts] [--quiet]
ckdocs serve [--root DIR] [--source DIR] [--site-url URL] [--config FILE] [--ref-tree REF=DIR]...
             [--out DIR] [--port PORT] [--no-watch] [--drafts] [--quiet]
ckdocs --version
ckdocs --help
```

| Command | What it does |
|---|---|
| `build` | Renders the site into `--out` (default `<root>/public`). Broken links and other findings are printed as warnings and the site is still written (exit 3), unless `--strict` makes them a failure. |
| `check` | Builds into a private temporary directory that is always removed, with `--strict` implied, and prints the same report: the command for CI and for "is it ready to publish?". |
| `serve` | Builds, then serves the site on `127.0.0.1` (loopback only) and keeps it up to date while you edit; see [The preview server](#the-preview-server). |

## Options

| Option | Commands | Meaning |
|---|---|---|
| `--root DIR` | all | The repository to read. Default: the current directory. |
| `--source DIR` | all | The page tree, relative to `--root`, instead of the configuration's `source` (or `docs/`). Not for a portal: each site sets its own. |
| `--site-url URL` | all | `site.url` for this run — the address the site is published at — so one source can be published to several hosts with correct sitemaps and llms.txt links. Validated like `site.url`. Also for portals. |
| `--config FILE` | all | Read this configuration instead of `<root>/ckdocs.yml`. |
| `--ref-tree REF=DIR` | all | Use `DIR` (relative to the current directory) as the tree of Git ref `REF` for the portal sites that name it, instead of extracting it from the repository. Repeatable; portals only. See [Sites from other branches](portals.md#sites-from-other-branches). |
| `--out DIR` | build, serve | Where to write the site. `serve` without it uses a private directory removed on exit. |
| `--clean` | build | Replace a site `--out` already holds. `--out` must be absent, empty, or a site ckdocs wrote (it carries a `.ckdocs` marker); anything else is refused untouched. |
| `--port PORT` | serve | The loopback port; default 8422, `0` picks a free one. |
| `--no-watch` | serve | Serve the first build only. |
| `--drafts` | all | Build the pages marked `draft: true`, which are otherwise left out. |
| `--strict` | build | Fail on any warning (see [Exit codes](#exit-codes)). `check` always implies it. |
| `--quiet` | all | Print only errors: no summary line and no warning list. |
| `--version` | — | Print the version ckdocs was built from. |
| `-h`, `--help` | — | Show the built-in help. |

## What counts as a warning

A build finishes with warnings, and `--strict` or `check` fails, on any of:

- a link, image, heading fragment, include or code snippet that resolves to
  nothing, reported as `page:line` with the target and the reason;
- a formula or Mermaid diagram that does not parse;
- a front matter key that is neither ckdocs's own nor declared in
  `front_matter.foreign_keys`, a value ckdocs cannot use, or a landing page
  that does not parse;
- a page an explicit `nav` does not mention;
- a Markdown file left out because it is over the page limit, unreadable or
  not UTF-8;
- a search index that had to drop pages to stay within its bound.

The site is written all the same (a broken link stays visible as text), so a
warning never costs you the rest of the site; a configuration error, a
collision of two pages on one output path, or an unreadable home page stops
the build instead.

## The preview server

`serve` builds once, prints `ckdocs: serve ready on <port>`, and serves the
site the way ck-git Pages would — only on `127.0.0.1`, never on the network.

While it runs it watches the configuration, every page, every file a page
includes, and the source tree. It looks for changes as often as the scan
allows — ten times a second for a small tree, less often for a very large
one — and rebuilds as soon as a change has held still for one scan, which
usually takes a few milliseconds: diagrams drawn by an earlier build are
reused until their source changes. Inside a Git work tree a new page appears
once Git tracks it.

Pages it sends carry a small script that keeps them current: after a
rebuild, the server names the output files whose bytes changed, and an open
page whose own file changed swaps in its new content in place, keeping its
scroll position; changed images and stylesheets reload; anything else stays
untouched. A rebuild that fails prints its error, shows it over the open
pages until the next good build, and keeps serving the last good site. If the
server restarts, open pages reload. None of this reaches a built site: the
script is added to pages as they are served.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Built or checked with nothing to report; `serve` after Ctrl+C. |
| 1 | The build failed, or a warning became a failure under `--strict` (always for `check`). |
| 2 | Invalid arguments; the usage is printed. |
| 3 | Built with warnings (`build` without `--strict`); the warnings are printed. |

## Examples

```sh
ckdocs build
ckdocs build --root /srv/checkout --out /srv/checkout/public --strict
ckdocs check --root .
ckdocs serve --drafts --port 9000
ckdocs build --site-url https://docs.example.com/ --clean
ckdocs build --ref-tree main=../project-main --clean
```
