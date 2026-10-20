---
description: The bounds ckdocs enforces on sites, pages, configuration, front matter, Markdown, code, math, diagrams, search and the preview server, and what happens when one is reached.
---
# Limits

ckdocs is bounded everywhere, so a mistaken or hostile input can never make a
build run away. Most bounds are far beyond what documentation needs. When one
is reached, ckdocs either stops with a message naming it or, where a part can
be left out safely, leaves that part out and warns. Which files a build may
read at all is covered under [Security](security.md).

## Sites and pages

| Bound | Value | When reached |
|---|---|---|
| Pages in a site | 4,096 | The build stops. |
| One Markdown page | 512 KiB | The page is left out with a warning (the home page stops the build). |
| Rendered page | 4 MiB of HTML | The build stops, naming the page. |
| Navigation depth | 4 levels (tab, two levels of group, pages) | A configuration error; derived navigation folds deeper directories into their parent. |
| Navigation entries | 4,096 | A configuration error. |
| Sites in a portal | 8 | A configuration error. |
| Translations | 16 | A configuration error. |
| Header links | 8 | A configuration error. |
| `exclude` patterns | 64 | A configuration error. |
| Whole output | 8 GiB, 100,000 files, 1 GiB per file | The build stops (the bounds of ck-git Pages). |

## Configuration and front matter

| Bound | Value |
|---|---|
| `ckdocs.yml` | 64 KiB |
| A title (`site.title`, a nav title, a page title, an author) | 128 bytes |
| A description, footer or interface text | 1,024 bytes |
| A URL | 1,024 bytes |
| A site id | 32 bytes |
| Front matter block | 512 lines and 32 KiB |
| Front matter key / value | 64 bytes / 4 KiB (a block value 16 KiB) |
| Front matter nesting | 8 levels |
| Tags or keywords on a page | 32, each 1–64 bytes |
| Declared foreign front matter keys | 32 |
| Landing page | 8 hero buttons, 16 feature cards |

A value over its bound in front matter is ignored with a warning; in
`ckdocs.yml` it is an error.

## Markdown

| Bound | Value | When reached |
|---|---|---|
| Nesting of quotes, lists and containers | 8 levels | Deeper content is shown as preformatted text. |
| Includes | 4 levels deep, 256 per page, 512 KiB of included text | Further includes stay visible as text, with a warning; too much included text stops the build. |
| Tabs in one set | 16 | The panels are shown one after another, with a warning. |
| Highlighted line ranges on one block | 64 | Further ranges are ignored. |
| Emoji shortcode | 48 bytes | Longer names are text. |
| Formula | 16 KiB, 64 levels of nesting | The formula is marked as an error and reported. |
| Errors reported per formula | 16 | "further errors omitted". |
| Syntax highlighting | 128 KiB of code per block | The block is shown without highlighting. |

## Diagrams

| Bound | Value |
|---|---|
| Diagrams on one page | 128 |
| Distinct diagrams in a site | 1,024 |
| Diagrams the preview server keeps between rebuilds | 4,096 |

A diagram over a bound stays as its Mermaid source, with a warning.

## Search

| Bound | Value | When reached |
|---|---|---|
| `search-index.json` | 2 MiB | The last pages are left out of the index, with a warning. |
| Excerpt per section | 200 bytes | Cut at a character boundary. |

## The preview server

| Bound | Value |
|---|---|
| Request headers | 16 KiB |
| Open pages receiving live updates | 64 |
| Files listed as changed after a rebuild | 512 (beyond that, every open page refreshes its content) |
