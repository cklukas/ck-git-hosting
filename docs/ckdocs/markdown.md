---
description: The Markdown ckdocs renders, the inline HTML it allows, and how links, images, and files are resolved and checked.
---
# Markdown, links, and assets

## Markdown

The renderer is the same bounded CommonMark subset (plus the GitHub
extensions below) the read-only dashboard uses for READMEs and Markdown
files, so a page looks the same wherever it is read.

| Construct | Syntax | Notes |
|---|---|---|
| Headings, paragraphs, emphasis, inline code, links, images, autolinks | as in CommonMark | Every heading gets a GitHub-style slug `id`, deduplicated (`install`, `install-1`, …). |
| Linked images | `[![alt text](image.svg "title")](page.md)` | An image as a link's label, in a paragraph or a table cell, with or without a title: a clickable `<img>`. Both targets resolve as described below. |
| Blockquotes, lists | as in CommonMark | Ordered, unordered, nested, loose or tight. |
| Fenced and indented code | ` ``` ` / four-space indent | A fence's info string (` ```cpp `) also selects syntax highlighting for C/C++, Python, shell, YAML, and JSON; an unrecognized name renders as plain code, never an error. |
| Mermaid diagrams | ` ```mermaid ` | Native SVG rendering; see the [complete gallery](mermaid.md#mermaid-diagrams). |
| Tables | GFM pipe tables | With `:---`/`---:`/`:---:` alignment. |
| Alerts | a blockquote whose first line is exactly `[!NOTE]`, `[!TIP]`, `[!IMPORTANT]`, `[!WARNING]`, or `[!CAUTION]` | Renders as a titled, coloured callout; a marker with trailing text, in lowercase, or with nothing below it stays an ordinary blockquote. |
| Task lists | `- [ ] text` / `- [x] text` | A disabled checkbox; the list gets a distinct class for styling. |
| Strikethrough | `~~text~~` | Exactly two tildes; one or three are literal. |
| Inline HTML | `<kbd>`, `<sub>`, `<sup>`, `<mark>`, `<ins>`, `<small>`, `<br>`, `<wbr>` | Lowercase and without attributes only. A closing tag with no matching open tag stays text, and a tag still open at the end of its paragraph is closed there. |
| Disclosures | `<details>` (or `<details open>`) alone on a line, an optional `<summary>…</summary>` line, Markdown blocks, then `</details>` alone on a line | Nests; a fenced `</details>` does not close it. Without its closing line the opener stays text. |

**Unsupported:** any other raw HTML, footnotes, definition lists, emoji
shortcodes, and math. Other HTML, including an allowed tag with attributes,
is displayed as escaped text; HTML comments outside code blocks are omitted.
Because no allowed tag takes attributes, none can carry a link, a style, or a
script, so the same Markdown is safe on the dashboard and on a docs site.

```markdown
Press <kbd>Ctrl</kbd>+<kbd>C</kbd> to stop. H<sub>2</sub>O, x<sup>2</sup>.

<details>
<summary>Full log</summary>

Collapsed **Markdown** content.

</details>
```

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
