# Markdown, links, and assets

## Markdown

The renderer is the same bounded CommonMark subset (plus the GitHub
extensions below) the read-only dashboard uses for READMEs and Markdown
files, so a page looks the same wherever it is read.

| Construct | Syntax | Notes |
|---|---|---|
| Headings, paragraphs, emphasis, inline code, links, images, autolinks | as in CommonMark | Every heading gets a GitHub-style slug `id`, deduplicated (`install`, `install-1`, …). |
| Blockquotes, lists | as in CommonMark | Ordered, unordered, nested, loose or tight. |
| Fenced and indented code | ` ``` ` / four-space indent | A fence's info string (` ```cpp `) also selects syntax highlighting for C/C++, Python, shell, YAML, and JSON; an unrecognized name renders as plain code, never an error. |
| Mermaid diagrams | ` ```mermaid ` | Native SVG rendering; see the [complete gallery](mermaid.md#mermaid-diagrams). |
| Tables | GFM pipe tables | With `:---`/`---:`/`:---:` alignment. |
| Alerts | a blockquote whose first line is exactly `[!NOTE]`, `[!TIP]`, `[!IMPORTANT]`, `[!WARNING]`, or `[!CAUTION]` | Renders as a titled, coloured callout; a marker with trailing text, in lowercase, or with nothing below it stays an ordinary blockquote. |
| Task lists | `- [ ] text` / `- [x] text` | A disabled checkbox; the list gets a distinct class for styling. |
| Strikethrough | `~~text~~` | Exactly two tildes; one or three are literal. |

**Unsupported:** raw HTML, footnotes, definition lists, emoji shortcodes,
and math. HTML is displayed as escaped text; HTML comments outside code
blocks are omitted.

## Links and assets

Inside a page, an ordinary link's `[label]` and `(destination)`, or an
image's `![alt text]` and `(destination)`:

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
    `.md` file outside the site) is reported and rendered as plain,
    visible text rather than a link to nowhere.
- A `#fragment` on a page link is checked, after the whole site renders,
  against the target page's actual heading ids; a fragment that names
  nothing is reported the same way a missing target is.

`ckdocs build` still writes the site when something is reported — the
broken reference stays visible as text, which is usually more useful than
an aborted build — and exits `3`. `--strict` (and `check`, which always
implies it) turns the same reports into a build failure, exit `1`, printing
each one naming the page and the target, so an editor's "next error"
shortcut goes straight to it.
