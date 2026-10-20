---
description: How ckdocs finds pages, names and orders them, and builds the tabs, sidebar, and site index.
---
# Pages and navigation

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
  [`exclude:` patterns](configuration.md) remove further candidates before anything
  else happens.
- **Partials and drafts.** A file or directory whose name starts with `_`
  (`_parts/intro.md`) holds pieces other pages
  [include](markdown.md#including-files); it is never a page of its own. A
  page whose front matter says `draft: true` is left out of the site unless
  the build is given `--drafts`, and a link to it is reported.
- **Files that cannot be pages.** A Markdown file over the 512 KiB page
  limit, one that cannot be read, or one that is not valid UTF-8 is left out
  of the site with a warning, and the build goes on; a `nav:` entry naming
  it is dropped, and a link to it is reported with the reason. Split such a
  file or list it under `exclude:`. Only the home page must be usable: a
  home page that is not stops the build.
- **The home page.** `home:` in the config; else the source tree's own
  `README.md`/`index.md`; else the repository root's `README.md`; else the
  first page in path order. The home page always becomes the site's root
  `index.html`; two pages that would land on the same output path (most
  often two candidate home pages) is a build error naming both.
- **Output paths.** Every other page keeps its path relative to the source
  tree with `.md`/`.markdown` replaced by `.html`; a directory's own
  `README.md`/`index.md` becomes that directory's `index.html`. So
  `docs/operations/01-installation.md` is served as
  `operations/01-installation.html`. A page's front matter
  [`slug`](front-matter.md#front-matter) gives it another address; the
  generated tag pages take `tags/` once any page has a tag, and each
  [translation](translations.md) its language's directory.
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
  being silently dropped. An explicit [`nav:`](configuration.md) names tabs and groups
  directly; every page it does not mention is still built and reachable
  from the generated site index, so nothing tracked ever silently
  disappears.
- **Markdown versions.** By default, every HTML page also has a clean `.html.md` version. Use the **Markdown** footer link to read its unrendered text. The [llms publishing setting](search.md#llmstxt-and-markdown-versions) controls these versions and discovery links together.
- **The site index.** `site-index.html` lists every tab, page, and second-
  and third-level heading as plain nested lists, with each page's
  [`description`](front-matter.md#page-descriptions) as a short line under
  its link.
