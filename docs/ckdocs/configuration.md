---
description: "Every ckdocs.yml setting: site details, branding and language, page tree and navigation, search, llms.txt, outline levels, code, interface text, translations and portals."
---
# Configuration

## `ckdocs.yml` reference

`ckdocs.yml` is optional. Without it, ckdocs uses the defaults below and
[derives navigation from your files](pages.md). When the file is present,
`version` and `site.title` are required. Unknown top-level, `site.`, and
`front_matter.` keys are build errors. A `sites:` list builds several sites
at once instead — see [Several sites in one build](portals.md). The command
line can override two settings for one run: `--source` (the page tree) and
`--site-url` (`site.url`); see [Command line](cli.md).

| Key | Type | Default | Notes |
|---|---|---|---|
| `version` | integer | — (required) | Must be `1`. |
| `site.title` | string, ≤ 128 bytes | — (required once the file exists) | The site's name: header brand text, `<title>` suffix, and the default for `site.brand`. |
| `site.brand` | string, ≤ 128 bytes | `site.title` | Header wordmark, when it should differ from the title. |
| `site.logo` | path | none | An image shown before the brand; copied into the site. |
| `site.favicon` | path | none | A browser tab icon (PNG, SVG, or ICO), copied into the site and linked from every page, including the generated site index. It can reuse `site.logo`; shared assets are copied once. Paths are repository-root-relative. |
| `site.description` | string, ≤ 1024 bytes | none | The home page's `<meta name="description">`. |
| `site.footer` | string, ≤ 1024 bytes | none | Footer text, alongside "Built with ckdocs `<version>`". |
| `site.links` | list of `{title, url, icon?}` | none | Up to 8 header links; `url` must be `http://`, `https://`, or `mailto:`. Optional `icon: github` displays a circular GitHub icon link; `title` supplies its accessible name and tooltip. Omit `icon` for a text link. |
| `site.stylesheet` | path | none | An extra stylesheet, copied as-is and linked after the built-in one. |
| `site.lang` | language tag | `en` | The pages' language: `<html lang>`, the [interface text](#interface-text), and right-to-left layout for Arabic, Hebrew, Persian and Urdu. |
| `site.url` | absolute `http://` or `https://` URL | none | Where the site is published, such as `https://example.github.io/project/`; a missing trailing `/` is added. With it, the build also writes `sitemap.xml`, and `llms.txt` links are absolute. See [sitemap.xml](search.md#sitemapxml). |
| `source` | path | `docs/` if it exists, else the repository root | The page tree, relative to the repository root. |
| `home` | path to a Markdown page | see [page discovery](pages.md#how-pages-are-found-named-and-ordered) | Overrides which page becomes the site's `index.html`. |
| `exclude` | list of patterns | none | Each entry is a path (a whole subtree) or a prefix ending in one `*`; matched against both the repository-relative and the source-relative form of every candidate, so `docs/drafts` and, with `source: docs`, plain `drafts` both work. Up to 64 entries. |
| `nav` | list of entries (below) | derived from pages | Names the tabs and groups explicitly. Nesting is bounded to 4 levels; every path named must be a page the site would otherwise build. |
| `search` | `true`/`false` | `false` | Adds client-side search — see [Search](search.md#search). |
| `llms` | `true`/`false` | `true` | Generates llms.txt and Markdown page versions, adds v2 discovery links and a footer Markdown link. Set `false` to disable all four together. In a portal this setting belongs to the portal and overrides each child site. See [llms.txt and Markdown versions](search.md#llmstxt-and-markdown-versions). |
| `toc.min_level`, `toc.max_level` | 1–6 | 2, 3 | The heading levels "On this page" lists; a page's [front matter](front-matter.md#front-matter) may choose others. |
| `markdown.line_numbers` | `true`/`false` | `false` | Numbers the lines of every code block; a block's `:no-line-numbers` opts out. See [Code blocks](code.md). |
| `ui` | mapping | none | Interface text overrides for `site.lang`; see [Interface text](#interface-text). |
| `i18n` | mapping | none | Translations of the site, each built under its own language directory; see [Translations](translations.md). |
| `front_matter.foreign_keys` | list of keys | none | Front matter keys another tool reads, accepted silently and carried as `front-matter:<key>` meta tags — see [Front matter](front-matter.md#front-matter). Up to 32 entries, each 1–64 ASCII letters, digits, `.`, `_`, or `-`, listed once, and none of the keys ckdocs reads itself (see [Page metadata](front-matter.md#front-matter)); `front_matter:` takes no other key. |
| `sites` | list | none | Makes the file a portal of several sites; see [Several sites in one build](portals.md). |

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

## Interface text

The words ckdocs writes around the content — "On this page", "Previous",
"Next", the search box, "Site index", the footer, the titles of alerts and
containers, the copy button — follow `site.lang`. They are built in for
English, Arabic, Chinese (simplified and traditional), Czech, Danish, Dutch,
Finnish, French, German, Hebrew, Italian, Japanese, Korean, Norwegian, Polish,
Portuguese, Russian, Spanish, Swedish, Turkish and Ukrainian; a regional tag
such as `de-CH` uses its language's text, and any other language starts from
English. `ui:` replaces single texts:

```yaml
site:
  title: Handbuch
  lang: de
ui:
  site_index: Inhaltsverzeichnis
  built_with: Gebaut mit {generator}
```

| Keys | Where |
|---|---|
| `skip_to_content`, `menu`, `home`, `sections`, `pages`, `breadcrumb`, `links` | The skip link, the small-screen menu button, the derived navigation's first tab, and the accessible names of the navigation regions. |
| `on_this_page`, `previous`, `next`, `previous_next` | The outline's heading and the pager. |
| `search`, `search_label`, `site_index`, `other_pages` | The search box, the site index and its section of pages outside the navigation. |
| `built_with` | The footer; `{generator}` is "ckdocs" and its version. |
| `tags`, `tagged`, `page_count`, `page_count_one` | Tag lists and pages; `{tag}` and `{count}` are filled in. |
| `copy_code`, `copied` | The copy button and its confirmation. |
| `note`, `tip`, `important`, `warning`, `caution`, `info`, `danger`, `details`, `footnotes`, `back_to_reference` | Default titles of alerts and containers, the footnotes' accessible name and their back links. |
| `languages`, `not_translated`, `sites`, `documentation_sites`, `moved` | The language menu, a translation's fallback notice (`{language}`, `{default}`), a portal's site selector, and its redirect pages. |
| `overview`, `features` | The label of a category's own page in its menu, and the accessible name of a landing page's feature cards. |
| `search_open`, `search_close` | The search button's accessible names. |
| `theme`, `theme_system`, `theme_light`, `theme_dark` | The colour theme picker and its three choices. |
| `diagram_open`, `diagram_expand`, `diagram_close` | A diagram's buttons: open its SVG, show it full size, close that view. |
| `markdown_version`, `llms_index` | The titles of the footer's links to the page's Markdown version and, on the site index, to `llms.txt`. |

A key ckdocs does not know, or an empty text, is a configuration error.
