---
description: Install ckdocs, build and check a first site, preview it with live updates, and find your way through the rest of the manual.
---
# Get started with ckdocs

## Install

ckdocs ships with ck-git-hosting and as a package of its own:

| Where | How |
|---|---|
| Debian, Ubuntu, Raspberry Pi OS | `sudo apt install ./ckdocs_<version>_<arch>.deb` from a [release](https://github.com/cklukas/ck-git-hosting/releases) |
| macOS (Homebrew) | `brew tap cklukas/ck-git-hosting https://github.com/cklukas/ck-git-hosting`, then `brew install ckgit`: it installs `ckdocs` with the client |
| From source | `make BUILD_DIR=… all` builds `ckdocs` with everything else (a C++20 compiler and `make`; nothing else) |

`ckdocs --version` prints the version it was built from.

## Build your first site

Put a `README.md` at your repository root and additional pages in `docs/`,
then run:

```sh
ckdocs build --root . --out public
ckdocs check --root .
```

Open `public/index.html` in a browser. `build` writes the site; `check` builds
into a private directory, reports broken links and other warnings, and leaves
nothing behind — the command for CI. Within a Git repository, pages must be
tracked by Git to be included (`git add` a new page first).

Without a configuration file ckdocs derives everything: the site title from
the directory's name, one tab per directory of `docs/`, page titles from their
first heading, and the order from file names. A `ckdocs.yml` names the site
and arranges its navigation:

```yaml
version: 1
site:
  title: My project
  description: What the project does, in one sentence.
search: true
```

## Preview while you write

```sh
ckdocs serve --root .
```

Open `http://127.0.0.1:8422`. While `serve` runs, it watches the
configuration, the pages, the files they include, and the source tree, and
rebuilds the site as soon as a change has settled — usually in a few
milliseconds, with diagrams it has drawn before reused. Open pages update
themselves: the page you are reading swaps in its new content and keeps its
scroll position, a page whose output did not change stays as it is, and
changed images and stylesheets reload. A rebuild that fails, for example on a
configuration error, prints the error, shows it over the open pages, and keeps
serving the last good site.

Use `--port` to choose another port, `--no-watch` to serve the first build
only, `--drafts` to include pages marked `draft: true`, and `--out DIR` to
keep the generated files. Press Ctrl+C to stop. See
[Command line](cli.md) for every option.

## Build several sites at once

A configuration with a `sites:` list builds a portal: a welcome page at the
output root and each site under its own directory, with a site selector next
to the brand. Sites link to each other's pages like ordinary files, and a site
can come from another Git branch:

```sh
ckdocs build --root . --out public --clean
ckdocs check --root . --ref-tree main=../project-main
```

See [Several sites in one build](portals.md).

## Where to go next

| Task | Guide |
|---|---|
| Arrange pages and navigation | [Pages and navigation](pages.md) |
| Set titles, descriptions, tags, layouts and addresses | [Page metadata](front-matter.md) |
| Format content and link files | [Markdown, links, and assets](markdown.md) |
| Show and include code | [Code blocks](code.md) |
| Add a diagram | [Mermaid diagrams](mermaid.md) |
| Change the look: logo, favicon, colours, stylesheet | [Look and feel](appearance.md) |
| Publish in several languages | [Translations](translations.md) |
| Combine several sites or branches | [Several sites in one build](portals.md) |
| Search, sitemap, llms.txt and Markdown versions | [Search and AI tools](search.md) |
| Publish the site | [Publishing](publishing.md) |
| Look up a setting, an option or a limit | [Configuration](configuration.md), [Command line](cli.md), [Limits](limits.md) |
