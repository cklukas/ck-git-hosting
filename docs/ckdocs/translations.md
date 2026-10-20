---
description: Publish a site in several languages, with a language menu, translated interface text, and pages that fall back to the original until they are translated.
---
# Translations

## A site in several languages

The default language's pages live where they always do; each translation is
a page tree of its own, mirroring the default one, and is built into a
directory named after its language:

```text
docs/
  README.md            → index.html
  guide/start.md       → guide/start.html
  de/
    README.md          → de/index.html
    guide/start.md     → de/guide/start.html
```

```yaml
version: 1
site:
  title: Handbook
  lang: en
i18n:
  locales:
    - lang: de
      label: Deutsch
```

That is all: `docs/de` (the default source plus the language tag) is the
German tree, kept out of the English pages automatically, and every page gets a
language menu in its header that leads to the same page in each language.

| `i18n` key | Default | Meaning |
|---|---|---|
| `label` | the language's own name | The default language's name in the language menu. |
| `locales[].lang` | — (required) | A language tag (`de`, `pt-BR`, `zh-Hant`): the translation's `<html lang>`, its [interface text](configuration.md#interface-text), and its output directory. Not the site's own `site.lang`, and each once. |
| `locales[].label` | the language's own name ("Deutsch", "日本語") | Its name in the language menu. |
| `locales[].source` | `<source>/<lang>` | Its page tree, relative to the repository root. |
| `locales[].title`, `description` | the site's | Its site title and description. |
| `locales[].nav` | the default nav, mapped | An explicit navigation in its own paths, like the top-level [`nav`](configuration.md#ckdocsyml-reference). |
| `locales[].nav_titles` | none | Translations of the default nav's tab and group titles, `{English title: translation}`. |
| `locales[].fallback` | `true` | Whether untranslated pages fall back to the default language (below). |
| `locales[].ui` | none | Interface text overrides for this language, as top-level [`ui`](configuration.md#interface-text). |

Up to 16 translations. The [front matter](front-matter.md) of a translated page
is its own: its title, description, tags and slug may all differ.

## Pages that are not translated yet

A translation rarely keeps pace with the original. While `fallback` is on,
every page the translation lacks is built into it from the default
language: at the same address, in the translation's navigation, with the
translation's interface around it, and with a note at its top saying that it
has not been translated yet. The note and the page's `lang` tell readers and
screen readers which language the text is in. Translate a page by adding its
file at the same path in the translation's tree; the fallback disappears.

With `fallback: false` a translation has only its own pages (it then needs its
own `README.md` or `index.md`), and the language menu leads from a page it
lacks to its home page.

## Links, navigation and search

- **Links** in a translated page resolve within its own language: write
  `[Setup](setup.md)` and the reader stays in German, on the translation or on
  its fallback. A link written in a default-language page that stands in as a
  fallback likewise leads to the translation where one exists.
- **Navigation** without a `nav:` of the translation's own follows the
  default language's: an explicit `nav` is mapped page by page (with
  `nav_titles` for its titles), and a derived one is derived from the
  translation's tree, fallbacks included.
- **Search** has an index per language; each language's search finds its
  own pages.
- **Search engines** get `<link rel="alternate" hreflang>` on every page that
  exists in several languages (fallbacks are not alternates), with
  `x-default` pointing at the default language; with `site.url` the links are
  absolute, and `sitemap.xml` lists every translated page. `llms.txt` keeps to
  the default language.

## Right-to-left languages

For Arabic, Hebrew, Persian, Urdu and the other right-to-left languages, pages
carry `dir="rtl"`, and the layout mirrors: the sidebar, the menus, the pager
and quotations swap sides. A fallback page's text keeps its own direction
inside the mirrored layout.

## With several sites

In a [portal](configuration.md#several-sites-in-one-build), translations
belong to each site's own configuration; a site's languages are built under
`<id>/<lang>/`, and the language menu and site selector sit side by side.
