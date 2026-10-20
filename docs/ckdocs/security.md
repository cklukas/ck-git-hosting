---
description: What ckdocs guarantees when it builds content others control — it never reads a file outside the repository, never follows a symbolic link, and is bounded everywhere.
---
# Security

ckdocs is built to be run on content you do not fully trust: a cloned
repository, a pull request, a CI job anyone with push access can change.
Building a site never reads anything outside the repository, and never runs
anything from it.

## Only the repository's own files

Everything a site takes from disk is a regular file inside the repository:
its pages, includes and `<<<` snippets, the configs, logos, favicons,
stylesheets, and every file a page links to or shows. ckdocs enforces this
for every file it reads, not just for some features:

- **`..` stops at the repository root.** `<!--@include: ../../../etc/passwd-->`
  is refused as leaving the repository.
- **A leading `/` means the repository root**, not the file system's:
  `<!--@include: /etc/passwd-->` names `<repository>/etc/passwd`, which does
  not exist.
- **Symbolic links are never followed**, whether the link is the file itself
  or a directory on the way to it. A link committed as `docs/system -> /etc`
  does not make `<!--@include: system/passwd-->`, `<<< system/hosts` or
  `![](system/passwd)` work. Every directory on the path and the file itself
  are opened without following links, so nothing can swap a part of the path
  between the check and the read.
- **Hidden files and directories** (a name starting with `.`) are never
  pages, includes or snippets.
- **Page discovery** walks the tree without entering linked directories, and
  in a Git work tree takes only files Git tracks.

A path that cannot be read this way is reported as a broken reference, with
the page and line, and `ckdocs check` fails on it. The directive or link
stays visible as text; nothing from behind it reaches the site.

The one path that may go through a link is the root itself: the directory
`--root` names, or the current directory. You chose that one.

## Nothing runs

A build executes nothing from the repository: there are no plugins, no
build hooks and no scripts. Mermaid diagrams and TeX math are rendered by
ckdocs itself, at build time. Markdown may carry only a few attribute-free
inline HTML tags ([the list](markdown.md)); any other HTML is shown as text,
so a page cannot bring scripts, styles or event handlers of its own.

## Bounded everywhere

Every input has a bound — page and include size, nesting, the number of
pages, diagrams and includes, formula size, the output's total size — so a
hostile page cannot make a build run away. [Limits](limits.md) lists them.
