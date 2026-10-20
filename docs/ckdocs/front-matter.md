---
description: Set a page's title, description, URL, tags, layout, sidebar, outline, pager, and draft state with front matter.
---
# Page titles and metadata

## Front matter

A leading block delimited by `---` lines, in the YAML subset `.ckgit/ci.yml`
uses: mappings and lists in block or flow style (`tags: [a, b]`), plain,
single- or double-quoted values (with exactly the four escapes
`\\ \" \n \t`), `|` and `>` block text, and `#` comments; at most 512 lines
and 32 KiB between the fences. A one-line value (a title, a description, a
hero's tagline) written as a `|` or `>` block is joined into one line.

```yaml
---
title: Getting started
description: The three commands to build and check a site.
author: C. Klukas
date: 2026-08-09
nav_order: 1
tags: [setup, basics]
---
```

The keys ckdocs reads, grouped by what they change. Several have a second
name, the one Docusaurus or VitePress uses, so pages written for those carry
over unchanged.

| Key | Type | Effect |
|---|---|---|
| `title` | string, ≤ 128 bytes | This page's title, overriding the [automatically derived title](pages.md). |
| `description` | string, ≤ 1024 bytes | A one-line summary of the page: its `<meta name="description">`, its line under the link in the site index, its search result, and its entry in `llms.txt` (see [Page descriptions](#page-descriptions)). |
| `author` | string, ≤ 128 bytes | The page's `<meta name="author">`, and the first part of its byline. |
| `date` | `YYYY-MM-DD` | A calendar date that exists (`2024-02-29` does, `2026-02-29` does not): the page's `<meta name="date">`, the second part of its byline, and its `<lastmod>` in the sitemap. |
| `keywords` | list of strings | The page's `<meta name="keywords">`. |
| `lang` | language tag | This page's language when it differs from the site's (`<html lang>`). |
| **Address and listing** | | |
| `slug` | path | The page's address instead of its file's: `/install` (from the site root) or `setup/first` (from the page's own directory) is written as `install.html` or `setup/first.html`; a trailing `/` writes `index.html` inside that directory. Links to the page's file still reach it. |
| `tags` | list of strings, ≤ 32 | Tag links under the page, and a page per tag; see [Tags](#tags). |
| `draft` | `true`/`false` | Leaves the page out of the site; links to it are reported. `--drafts` builds drafts too, for previewing. |
| `search` | `true`/`false` | `false` keeps the page out of the search index. |
| `unlisted` | `true`/`false` | Builds the page but keeps it out of the navigation and search: reachable only by its address and the site index. |
| **Navigation** | | |
| `nav_order` (`sidebar_position`) | integer | This page's (or, on a directory's index page, that directory's) order among its siblings. |
| `nav_exclude` | `true`/`false` | Keep this page out of the sidebar and tabs (still built, still linked from the site index). |
| `sidebar_label` | string | The page's title in the tabs, sidebar and breadcrumbs, when it should be shorter than its title. |
| `prev`, `next` (`pagination_prev`, `pagination_next`) | `false`, a page, or `{text, link}` | Hides the pager's link, or points it at another page (written like a link, relative to this page), optionally with its own label. |
| `pager` | `true`/`false` | `false` hides the previous/next links. |
| **Layout** | | |
| `layout` | `doc`, `page`, `home` | `doc` (the default): sidebar, outline, breadcrumbs, pager. `page`: the content alone, full width. `home` (the home page only): a landing page with a [hero and features](#home-layout). |
| `sidebar` | `true`/`false` | `false` hides the sidebar's page list. |
| `toc` | `true`/`false` | `false` hides "On this page". `hide_table_of_contents: true` and `outline: false` do the same. |
| `toc_min_heading_level`, `toc_max_heading_level` | 1–6 | The heading levels "On this page" lists, instead of the site's ([`toc`](configuration.md#ckdocsyml-reference) in ckdocs.yml, else 2 and 3). `outline: [2, 4]`, `outline: 3` (that level only) and `outline: deep` (2 to 6) say the same in VitePress's spelling. |
| `hide_title` | `true`/`false` | Drops the page's leading `#` heading; the title still names the page in navigation, the browser's tab and search. |
| `page_class` | class names | Extra classes on the page's `<body>`, for a site stylesheet to target. |
| `hero`, `features` | mapping, list | The home page's banner and cards; see [Home layout](#home-layout). |

A value ckdocs cannot use — an empty or over-long `title`, a `nav_order` that
is not an integer, a `layout` it does not know, a `date` in any other form, a
`slug` with characters a path cannot have — is ignored with a warning naming
the page and the key.

### Page descriptions

A `description` is the page's answer to "what is this page for?", in one
sentence. ckdocs shows it in four places:

- **Site index:** as a muted line under the page's link, so the index reads
  as an annotated table of contents.
- **Search results:** with [search](configuration.md#search) on, a query that
  matches a page's title, description or tags lists the page itself, with the
  description as its summary, ahead of its matching sections.
- **`llms.txt`:** as the text after the page's link (see
  [Sitemap and llms.txt](configuration.md#sitemap-and-llmstxt)).
- **The page's `<meta name="description">`:** what search engines and link
  previews show.

It is not shown on the page itself, which usually opens with its own
introduction. The home page falls back to `site.description` when it has
none. Keep it to one plain sentence that says what a reader finds or can do
there, not a repeat of the title:

```yaml
---
description: Pair a device with the server, check the connection, and fix the usual setup problems.
---
```

A page with an `author` or a `date` shows a small byline, such as
"C. Klukas · 2026-08-09" — only the parts present — right under its
leading `#` heading, or at the very top of the page when it does not open
with one. It is plain markup styled by the built-in stylesheet (the date is
a `<time datetime="…">` element), so it needs no script; it is not part of
the [search](configuration.md#search) index.

### Tags

```yaml
---
tags: [setup, networking]
---
```

Each tag of a page links, under the page, to that tag's own page, which lists
every page carrying the tag (title order, with descriptions); `tags/index.html`
lists all tags with their page counts. The tag pages are written under
`tags/`, which is therefore reserved once any page has a tag. Search matches
tags too.

### Other tools' keys

An unrecognized key is ignored — front matter written for Jekyll or Hugo is
common to paste in — but `ckdocs check`/`--strict` reports it as a warning,
so a typo such as `nave_order` is still caught rather than silently doing
nothing. A key that belongs to another tool reading the same pages (a PDF
exporter's `format`, say) is declared once in `ckdocs.yml` instead:

```yaml
front_matter:
  foreign_keys: [format, pdf.theme]
```

A declared key is accepted silently on every page; ckdocs never interprets
it, but carries it into the page's head as
`<meta name="front-matter:format" content="…">`, name and value escaped,
the value exactly as parsed. The `front-matter:` prefix is deliberate: a key
meant for another tool can never act as a standard meta name a browser or
crawler obeys (`robots`, `referrer`, `viewport`, …) or shadow one ckdocs
writes itself. A declared key whose value is a list or a mapping, longer
than 1024 bytes, or containing a control character (a newline included) is
still accepted silently, just not carried. Every key *not* declared keeps
warning exactly as before, so the typo protection stays. The keys ckdocs reads
itself cannot be declared foreign.

A block that has the *shape* of front matter but is not valid in
this syntax (a duplicate key, an unterminated quote, a key with nothing
after it) is still recognized as front matter — its content never leaks
onto the rendered page as literal text — and is reported the same way,
naming the exact problem and line. A file that merely starts with a
thematic break (`---`, then prose, then another `---`) has no front matter
at all; it is correctly left as ordinary body content. The dashboard's
rendered preview of a Markdown file hides its front matter the same way;
the file's own Source view still shows it.

## Home layout

A site's home page may use `layout: home` with a structured `hero` and
`features`. This data-only format also accepts existing VitePress home pages:

```yaml
---
layout: home
hero:
  name: Example project
  text: Build something useful.
  tagline: A short explanation of who the project helps.
  image:
    src: /images/overview.svg
    alt: Architecture overview
  actions:
    - text: Get started
      link: /guide
      theme: brand
features:
  - title: Predictable interfaces
    details: Inspect the API before calling it.
---
```

The hero and feature cards precede the Markdown body, in a full-width home
layout. All text is escaped, and image/action destinations use the same
checked resolver as ordinary content. `hero.image` accepts either `src` and
`alt`, or `light` and optional `dark` mappings, each with `src` and `alt`.
`hero.image` may also be just a path. There may be at most eight actions
(`theme: brand` or `alt`, the default) and sixteen features. A feature may add
an `icon` (a short text or an emoji shortcode such as `":rocket:"`) shown above
its title, and a `link`: the whole card then leads there. A long `tagline` or
`details` may be written as a `>` block; its lines are joined. Without a
`title` of its own the page takes the hero's `name`, and without a
`description` the `tagline`. Unsupported fields or invalid shapes are warnings
and fail strict builds. The ordinary front matter byte/line bounds still apply; structured
metadata may nest eight levels. Home layouts on other pages are reported.

Navigation groups also provide native dropdown menus in the header. A theme
picker shows one compact icon for the current preference. Opening it reveals
system, sun, and moon choices, with the selected preference highlighted. The
choice is stored locally when available; the home image and diagrams follow it. Mermaid
figures offer compact icon controls for an expanded native dialog (Escape
closes it) and opening the SVG. Icons have tooltips and accessible labels;
the SVG link also works without scripting. Search accepts the usual Ctrl/Cmd+K
shortcut. These controls ship inline with the site and load no external code.
