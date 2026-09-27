# Page titles and metadata

## Front matter

A leading block delimited by `---` lines, in the same small scalar syntax
`.ckgit/ci.yml` uses (plain, single-, or double-quoted values, and exactly
the four escapes `\\ \" \n \t`), at most 64 lines and 8 KiB between the
fences:

```yaml
---
title: Getting started
description: The three commands to build and check a site.
author: C. Klukas
date: 2026-08-09
nav_order: 1
---
```

| Key | Type | Effect |
|---|---|---|
| `title` | string, ≤ 128 bytes | This page's title, overriding the [automatically derived title](pages.md). |
| `description` | string, ≤ 1024 bytes | A one-line summary, used as the page's `<meta name="description">`. |
| `author` | string, ≤ 128 bytes | The page's `<meta name="author">`, and the first part of its byline. |
| `date` | `YYYY-MM-DD` | A calendar date that exists (`2024-02-29` does, `2026-02-29` does not): the page's `<meta name="date">`, and the second part of its byline. |
| `nav_order` | integer | This page's (or, on a directory's index page, that directory's) order among its siblings. |
| `nav_exclude` | `true`/`false` | Keep this page out of the sidebar and tabs (still built, still linked from the site index). |

A page with an `author` or a `date` shows a small byline, such as
"C. Klukas · 2026-08-09" — only the parts present — right under its
leading `#` heading, or at the very top of the page when it does not open
with one. It is plain markup styled by the built-in stylesheet (the date is
a `<time datetime="…">` element), so it needs no script; it is not part of
the [search](configuration.md#search) index. A value ckdocs cannot use — an empty or
over-long `title` or `author`, a `description` over its bound, a value with
a control character, a `nav_order` that is not an integer, a `nav_exclude`
that is neither `true` nor `false`, a `date` in any other form or naming a
day that does not exist — is ignored with a warning naming the page.

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
warning exactly as before, so the typo protection stays. Only keys with a
single (scalar) value are looked at: one whose value is a list or a mapping
is skipped without a warning — ckdocs's own, declared, or neither.

A block that has the *shape* of front matter but is not valid in
this syntax (a duplicate key, an unterminated quote, a key with nothing
after it) is still recognized as front matter — its content never leaks
onto the rendered page as literal text — and is reported the same way,
naming the exact problem and line. A file that merely starts with a
thematic break (`---`, then prose, then another `---`) has no front matter
at all; it is correctly left as ordinary body content. The dashboard's
rendered preview of a Markdown file hides its front matter the same way;
the file's own Source view still shows it.
