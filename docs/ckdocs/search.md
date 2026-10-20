---
description: How readers and machines find their way through a ckdocs site — search, the site index, sitemap.xml, llms.txt, and the Markdown version of every page.
---
# Search and AI tools

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
sections linking straight to their heading. A page whose title,
[description](front-matter.md#page-descriptions) or
[tags](front-matter.md#tags) match is listed itself first, with its
description (or tags) as the summary. A page whose front matter says
`search: false` (or `unlisted: true`) is left out of the index, and each
[translation](translations.md) has an index of its own. It runs only on the page it
ships with, the Pages origin, never the dashboard.

The box starts hidden and only that script reveals it, so a visitor with
scripting disabled never sees a non-functional input — they get the
`<noscript>` fallback instead, a plain link back to the site index, exactly
the no-script answer that already exists. The index itself is capped at
2 MiB; a site large enough to exceed that keeps as many complete pages as
fit and drops the rest from the end, reported as a build warning rather
than failing the build — in practice, thousands of pages would be needed
to get there.


## The site index

Every build writes `site-index.html`, linked from every page's footer: every
tab, page and second- and third-level heading as plain nested lists, with
each page's [description](front-matter.md#page-descriptions) as a short line
under its link, and the pages outside the navigation under their own heading.
It needs no script, so it is the way to find something with scripting
switched off.

## sitemap.xml

With [`site.url`](configuration.md#ckdocsyml-reference) set, the build also
writes `sitemap.xml`: every page's absolute URL in navigation order, with a
page's front matter `date` as its `<lastmod>`, and every
[translated](translations.md) page (fallbacks are not listed). A sitemap needs
absolute URLs, so without `site.url` there is none; `--site-url` sets it for
one build. A portal writes one covering every site.

## llms.txt and Markdown versions

By default, every build writes `llms.txt` at the output root: a plain-text index of the
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

- [Set up the client](https://cklukas.github.io/ck-git-hosting/hosting/guides/client-setup.html.md): Install ckgit on a device and pair it with your server.
```

With the default `llms: true`, each HTML content page has a clean Markdown
version at the same URL with `.md` appended: `guide.html.md`, or
`index.html.md` for a directory home. The generated site index, portal welcome
page, and legacy redirect pages have Markdown versions too. Markdown contains
the article (including landing-page content and bylines), with working links
to published Markdown pages and copied assets. Code blocks and Mermaid source
remain fenced code. Front matter and the navigation, search, theme controls,
and other site chrome are omitted.

Every HTML page includes relative discovery links, following the
[llms.txt v2 proposal](https://llmstxt.org/changes.html):

```html
<link rel="describedby" href="../llms.txt" type="text/plain">
<link rel="alternate" type="text/markdown" href="guide.html.md">
```

A subtle **Markdown** footer link opens the current page's unrendered text.
The generated **Site index** page also has an **llms.txt** link beside it,
pointing to the covering documentation index (the root index in a portal or
a translation).
The built-in server and ck-git Pages serve `.md` as UTF-8 plain text, so the
browser can display it directly. Other hosts choose their own response MIME
type. The `alternate` link identifies the content as Markdown for agents.
No server-specific HTTP `Link` headers are required: discovery works from
the static HTML files, under any URL prefix. A portal uses one covering
`llms.txt` at its root; child pages link back to that file.

To disable the whole feature:

```yaml
version: 1
site:
  title: My docs
llms: false
```

This removes generated `llms.txt` and `*.html.md` files, the discovery links,
and both footer links. Rebuild with `--clean` to replace an earlier output.
The setting is independent of search and the sitemap. For portals, only the
portal's `llms` setting controls output; child settings do not override it.

When the same source is published to more than one host, override `site.url`
for that build instead of changing its configuration:

```sh
ckdocs build --site-url http://rpi4:8421/my-project/ --strict
```

The override applies to single sites and portals, and is also available for
`check` and `serve`. It uses the same URL validation as `site.url`; relative
HTML discovery and footer links remain portable between hosts.

Without `site.url`, `llms.txt` links stay relative to the site root. They
target the Markdown page versions, while the sitemap targets the HTML pages.
A portal writes one `llms.txt` at its root, covering every site. A linked file
that would be copied to one of the generated names (`llms.txt`,
`sitemap.xml`, `search-index.json`, `site-index.html`, or a page's `.md`
version) is reported as a broken link instead.
