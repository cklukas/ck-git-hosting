---
description: The Markdown ckdocs renders — containers, tabs, footnotes, definition lists, emoji, math, includes — the inline HTML it allows, and how links, images, and files are resolved and checked.
---
# Markdown, links, and assets

## Markdown

The renderer is a bounded CommonMark subset with the GitHub extensions and
the documentation extensions below. The read-only dashboard uses the same
renderer for READMEs and Markdown files, so a page looks the same wherever it
is read. The dashboard has no include reader, so `<<<` and
`<!--@include:-->` are the only constructs that stay text there.

| Construct | Syntax | Notes |
|---|---|---|
| Headings, paragraphs, emphasis, inline code, links, images, autolinks | as in CommonMark | Every heading gets a GitHub-style slug `id`, deduplicated (`install`, `install-1`, …); emoji, other pictographs and formula sources are left out of it. |
| Linked images | `[![alt text](image.svg "title")](page.md)` | An image as a link's label, in a paragraph or a table cell, with or without a title: a clickable `<img>`. Both targets resolve as described below. |
| Blockquotes, lists | as in CommonMark | Ordered, unordered, nested, loose or tight. |
| Fenced and indented code | ` ``` ` / four-space indent | Syntax highlighting for 73 languages, titles, line numbers, highlighted lines, and diff and focus markers; see [Code blocks](code.md). |
| Mermaid diagrams | ` ```mermaid ` | Native SVG rendering; see the [complete gallery](mermaid.md#mermaid-diagrams). |
| Tables | GFM pipe tables | With `:---`/`---:`/`:---:` alignment. |
| Alerts | a blockquote whose first line is exactly `[!NOTE]`, `[!TIP]`, `[!IMPORTANT]`, `[!WARNING]`, or `[!CAUTION]` | Renders as a titled, coloured callout; a marker with trailing text, in lowercase, or with nothing below it stays an ordinary blockquote. |
| Containers | `::: tip [title]` … `:::` | Callouts with an optional title, and collapsible details; see [Containers](#containers). |
| Tabs | `::: tabs` with `== Label` lines; `::: code-group` | Switch between panels without a script; see [Tabs and code groups](#tabs-and-code-groups). |
| Footnotes | `text[^1]` and `[^1]: note` | Numbered by first reference, listed at the end with links back; see [Footnotes](#footnotes). |
| Definition lists | a term line, then `: definition` | See [Definition lists](#definition-lists). |
| Math | `$…$`, `$$…$$`, ` ```math ` | TeX math rendered to MathML at build time; see [Math](#math). |
| Emoji | `:tada:` | About 1,700 shortcodes; see [Emoji](#emoji). |
| Includes | `<!--@include: part.md-->`, `<<< file` | Another Markdown file, or a file as code; see [Including files](#including-files) and [Code snippets](code.md#code-snippets-from-files). |
| Task lists | `- [ ] text` / `- [x] text` | A disabled checkbox; the list gets a distinct class for styling. |
| Strikethrough | `~~text~~` | Exactly two tildes; one or three are literal. |
| Inline HTML | `<kbd>`, `<sub>`, `<sup>`, `<mark>`, `<ins>`, `<small>`, `<br>`, `<wbr>` | Lowercase and without attributes only. A closing tag with no matching open tag stays text, and a tag still open at the end of its paragraph is closed there. |
| Disclosures | `<details>` (or `<details open>`) alone on a line, an optional `<summary>…</summary>` line, Markdown blocks, then `</details>` alone on a line | Nests; a fenced `</details>` does not close it. Without its closing line the opener stays text. |

**Unsupported:** any other raw HTML, and components (MDX or Vue). Other HTML,
including an allowed tag with attributes, is displayed as escaped text; HTML
comments outside code blocks are omitted. Because no allowed tag takes
attributes, none can carry a link, a style, or a script, so the same Markdown
is safe on the dashboard and on a docs site.

```markdown
Press <kbd>Ctrl</kbd>+<kbd>C</kbd> to stop. H<sub>2</sub>O, x<sup>2</sup>.

<details>
<summary>Full log</summary>

Collapsed **Markdown** content.

</details>
```

## Containers

A container starts with three or more colons and a kind, and ends with a line
of colons. The kinds are `info`, `note`, `tip`, `important`, `warning`,
`caution`, `danger` and `details`. A title may follow the kind, either after a
space (VitePress) or in brackets right after it (Docusaurus); without one, the
kind's own word is the title, in the page's language.

```markdown
::: tip
Run `ckdocs check` before you publish.
:::

::: warning Mind the cache
Old pages stay cached for an hour.
:::

:::danger[Data loss]
This deletes the output directory.
:::

::: details Show the full log {open}
…
:::
```

::: tip
This is a `tip` container; the other callout kinds differ only in colour.
:::

Containers nest. Give the outer one more colons (`::::`) to keep the source
readable; a line of colons always closes the innermost open container, and a
`:::` line inside fenced code is code. An unknown kind is ordinary text, and a
container that is never closed runs to the end of the block it is in.

## Tabs and code groups

`::: tabs` switches between panels; each `== Label` line starts one. A
`::: code-group` makes a tab of each fenced code block (or
[snippet](code.md#code-snippets-from-files)) in it, labelled by the block's
`[label]` or title, else its language:

````markdown
::: code-group
```sh [macOS]
brew install ckgit
```
```sh [Debian]
sudo apt install ./ckgit.deb
```
:::

::: tabs
== Linux
Install the package.
== macOS
Use Homebrew.
:::
````

::: code-group
```sh [macOS]
brew install ckgit
```
```sh [Debian]
sudo apt install ./ckgit.deb
```
:::

Tabs are radio buttons and labels styled by the stylesheet, so they work
without a script, on the dashboard too, and with the keyboard (arrow keys move
between the tabs of a set). A set holds at most 16 tabs; a longer one is shown
as its panels one after another under their labels, with a warning.

## Footnotes

```markdown
The cache is cleared hourly.[^cache]

[^cache]: Unless `pages_cache` is set in the server configuration.
    An indented paragraph continues the note.
```

A reference is a superscript number, numbered in order of first use, that
links to its note; the notes are listed at the end of the page, each with a
link back to every reference to it. Labels ignore case. A reference to a label
that has no definition stays text, and a definition nothing refers to is not
shown. Notes may hold several paragraphs (indented four spaces) and may refer
to other notes.

## Definition lists

```markdown
Term
: The definition.
: A second definition.

Another term
Its synonym

: A loose definition, set as paragraphs,
  continued on indented lines.
```

One or more term lines followed by `: ` definitions make a definition list;
consecutive groups form one list. A blank line before a definition makes the
definitions paragraphs. A definition continues on lines indented by two or
more spaces and may contain lists, code, or any other blocks.

## Math

Write TeX math between `$` signs inline, and between `$$` lines (or in a
` ```math ` fence) as a display:

```markdown
The identity $e^{i\pi} + 1 = 0$ holds.

$$
\sum_{k=1}^{n} k = \frac{n(n+1)}{2}
$$
```

The identity $e^{i\pi} + 1 = 0$ holds, and

$$
\sum_{k=1}^{n} k = \frac{n(n+1)}{2}
$$

Formulas are converted to MathML when the site is built, so they need no
script, stay selectable, and scale with the text; the TeX source travels along
as an annotation. The converter covers what documentation uses: scripts,
fractions and binomials, roots, Greek letters and about 400 symbols, big
operators with limits, functions such as `\sin` and `\lim`, `\left`/`\right`
and sized delimiters, accents and braces, font styles (`\mathbb`, `\mathcal`,
`\mathbf`, …), `\text`, spacing, colour, and the matrix, `cases`, `aligned`,
`gather` and `array` environments. `\boxed` and `\fbox` draw no frame, and
`\tag` is placed after its row rather than at the margin.

A dollar sign starts a formula only when a non-space follows it, and ends one
only after a non-space and before anything that is not a digit, so prices such
as $5 and $10 stay text; write `\$` for a literal dollar sign. A formula the
converter does not understand is shown with the problem marked and reported
as a warning naming the page and line, so `ckdocs check` catches it.

## Emoji

`:name:` becomes the emoji the name stands for: faces (`:smile:`), gestures
(`:+1:`, `:clap:`), people and jobs, animals and plants, food, travel and
weather, activities, objects (`:rocket:`, `:bulb:`, `:wrench:`), symbols
(`:white_check_mark:`, `:warning:`, `:x:`), and flags as `:flag_de:` (any
ISO 3166 code) or the short `:de:`, `:us:`, `:uk:` and a few others. Names are
lowercase; a name that is not in the table, one glued to a word or a number
(`a:smile:`, `10:30:45`), and one in code stay text.

## Including files

A line holding only `<!--@include: path-->` is replaced by the Markdown file
it names, before anything else is read, so its headings join the page's
outline:

```markdown
<!--@include: _parts/requirements.md-->
<!--@include: _parts/requirements.md#debian-->
<!--@include: _parts/changelog.md{1,20}-->
```

- **Paths** are relative to the including page, or to the repository root
  with a leading `/` or `@/`. Hidden files and directories cannot be included.
- **Part of a file:** `#name` takes the lines between `<!-- #region name -->`
  and `<!-- #endregion name -->`; `{from,to}` takes a range of lines (either
  end may be left out), counted from the first line after the file's front
  matter, which is never included.
- **Links inside a part** resolve from the part's own directory, as its author
  wrote them; a broken one is reported at the page's include line.
- **Parts include parts**, up to four levels; a file that would include itself
  is reported, and the directive stays visible as text, as it does for a file
  that does not exist.
- **Partials:** a file or directory whose name starts with `_` is never a
  page of its own, so `_parts/` holds pieces that are only included.

`ckdocs serve` watches the included files and rebuilds when one changes.

## Links and assets

Inside a page, an ordinary link's `[label]` and `(destination)`, or an
image's `![alt text]` and `(destination)` — both of them in a linked image,
`[![alt text](figure.svg)](page.md)`:

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
    `.md` file outside the site or left out of it, a file whose copy would
    overwrite a page) is reported and rendered as plain, visible text
    rather than a link to nowhere.
- A `#fragment` on a page link is checked, after the whole site renders,
  against the target page's actual heading ids; a fragment that names
  nothing is reported the same way a missing target is.

`ckdocs build` still writes the site when something is reported — the
broken reference stays visible as text, which is usually more useful than
an aborted build — and exits `3`. `--strict` (and `check`, which always
implies it) turns the same reports into a build failure, exit `1`, printing
each one naming the page, the line, and the target, so an editor's "next
error" shortcut goes straight to it:

```text
warning: docs/guide.md:12: link target 'setup.md' is not a page of this site (3 links; also lines 40, 88)
```

A page that links to the same missing target several times is reported
once, with every line it does so on; a bad `#fragment` repeated on a page
is reported once with its count.

### Clean documentation URLs

After checking a repository-relative destination, ckdocs also recognizes
extensionless page links (`/guide`), `.html` page links (`/guide.html`), and
indexed directories (`/section/`) relative to the configured source tree.
A leading `/` may reference a file in the source's `public/` directory;
only referenced files are copied, with `public/` omitted from their output
path. Existing repository-relative targets take precedence. This lets a
VitePress documentation tree retain its page and image links while moving
to ckdocs. Missing targets and traversal outside the repository still fail
strict validation.
