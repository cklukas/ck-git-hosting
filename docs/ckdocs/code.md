---
description: Syntax highlighting for 73 languages, code titles, line numbers, highlighted lines, diff and focus markers, copy buttons, and code snippets taken from the repository's files.
---
# Code blocks

## Highlighting and titles

A fence's info string names its language, and the code is highlighted when
the site is built: keywords, types, function names, strings, numbers,
comments, properties, preprocessor lines and annotations, variables, and
inserted and deleted diff lines each get their colour, in light and dark. An
unrecognized name renders as plain code, never an error; `text`, `txt` and
`plain` ask for none.

After the language, the info string may carry a title and options:

````markdown
```cpp title="src/main.cpp" {2,4-5} :line-numbers
int main() {
  int answer = 42;
  // the next two lines are highlighted
  report(answer);
  return 0;
}
```
````

```cpp title="src/main.cpp" {2,4-5} :line-numbers
int main() {
  int answer = 42;
  // the next two lines are highlighted
  report(answer);
  return 0;
}
```

| Option | Effect |
|---|---|
| `title="…"` or `[…]` | A title bar above the block (in a [code group](markdown.md#tabs-and-code-groups), the tab's label). |
| `{1,3-5}` | Highlights those lines. It may follow the language directly: ` ```ts{1} `. |
| `:line-numbers` or `showLineNumbers` | Numbers the lines; `=N` (`:line-numbers=10`, `showLineNumbers=10`) starts at N. |
| `:no-line-numbers` | Turns numbering off for this block when [`markdown.line_numbers`](configuration.md#ckdocsyml-reference) turns it on for the site. |

Every block on a ckdocs page gets a **copy button** when the reader points at
it. It copies the code as written: without line numbers, and without the lines
a diff marks as removed. Where the browser refuses to copy (some embedded
views do), the code is selected instead, ready for the reader's own copy
shortcut. The button is made by a small inline script, so a reader without
scripting sees no button that does nothing; pages without code blocks do not
carry it.

## Marking lines in the code

A comment at the end of a line can mark it; the comment itself is removed from
the output. Any comment syntax works (`//`, `#`, `--`, `;`, `%`, `/* */`,
`<!-- -->`):

| Marker | Effect |
|---|---|
| `// [!code highlight]` | Highlights the line; `[!code highlight:3]` this line and the next two. |
| `// [!code ++]` / `// [!code --]` | Shows the line as inserted or removed, with a `+` or `−` in the margin. |
| `// [!code focus]` | Blurs every other line until the reader points at the block; `[!code focus:2]` focuses two lines. |
| `// [!code warning]` / `// [!code error]` | Tints the line yellow or red. |
| `// highlight-next-line` | (A comment on a line of its own, as in Docusaurus.) Highlights the next line. |
| `// highlight-start` … `// highlight-end` | Highlights every line between them. |

```js
function greet(name) {
  console.log("Hello " + name); // [!code --]
  console.log(`Hello ${name}`); // [!code ++]
}
```

## Code snippets from files

A line `<<< path` puts a file of the repository on the page as a code block,
highlighted by its extension, so documentation shows the code the project
actually builds:

```markdown
<<< ../src/server/main.cpp
<<< @/scripts/install.sh{3-8 sh}
<<< ../src/client/sync.cpp#retry{2 cpp:line-numbers} [sync.cpp]
<<< /tests/integration/docs_site.sh#L10-L25
```

| Part | Meaning |
|---|---|
| `path` | Relative to the page, or to the repository root with a leading `/` or `@/`. Hidden files and directories cannot be included. |
| `#name` | Only the lines between `#region name` and `#endregion name` in whatever comment the language uses (`// #region name`, `# #region name`, `<!-- #region name -->`), without the markers, and with their shared indentation removed. |
| `#L10-L25` (or `#L10`) | Only those lines, as on GitHub. |
| `{…}` | Highlighted lines and, optionally, a language (to override the extension) with `:line-numbers`. |
| `[title]` | A title bar; in a code group, the tab's label (default: the file's name). |

A snippet whose file or region does not exist stays visible as text and is
reported as a broken reference naming the page and line, so `ckdocs check`
fails on it. `ckdocs serve` watches every file a page includes. On the
dashboard, which reads no other files, the line stays text.

## Languages

The names below are what a fence may say; matching ignores case. Files on the
dashboard are highlighted by their extension or name (`Makefile`,
`Dockerfile`, `CMakeLists.txt`, …) and, for scripts, their `#!` line.

| Family | Languages and fence names |
|---|---|
| C family | C++ (`cpp`, `c++`, `cxx`, `cc`, `hpp`, `h`, `cuda`), C (`c`), Objective-C (`objc`, `objective-c`), C# (`csharp`, `c#`, `cs`), D (`d`) |
| JVM and mobile | Java, Kotlin (`kotlin`, `kt`), Scala, Groovy (`groovy`, `gradle`), Dart, Swift |
| Systems | Go (`go`, `golang`), Rust (`rust`, `rs`), Zig |
| Web and scripting | JavaScript (`js`, `jsx`, `mjs`), TypeScript (`ts`, `tsx`), PHP, Perl (`perl`, `pl`), Ruby (`ruby`, `rb`), Lua |
| Scientific | Python (`python`, `py`), R, Julia, MATLAB/Octave, Nim, Crystal |
| Functional | Elixir, Erlang, Haskell (`haskell`, `hs`), Elm, OCaml, F# (`fsharp`, `f#`), Common Lisp (`lisp`, `elisp`), Scheme/Racket, Clojure (`clojure`, `clj`) |
| SQL | `sql`, `mysql`, `postgresql`, `sqlite`, `plsql`, `tsql` |
| Windows and legacy | Visual Basic (`vb`, `vbnet`), PowerShell (`powershell`, `ps1`, `pwsh`), Batch (`bat`, `cmd`), Fortran, Pascal/Delphi, Ada |
| Hardware | Verilog/SystemVerilog, VHDL, assembly (`asm`, `nasm`, `gas`) |
| Data, schemas and configuration | JSON (`json`, `jsonc`, `json5`), YAML, TOML, INI (`ini`, `cfg`, `conf`, `properties`), GraphQL, Protocol Buffers (`proto`), Thrift, Solidity, HCL/Terraform (`hcl`, `tf`), Nix |
| Tools and build | Shell (`bash`, `sh`, `zsh`, `console`), Awk, Tcl, CMake, Make (`makefile`, `make`), Dockerfile (`dockerfile`, `docker`), Nginx |
| Markup and text | HTML (`html`, `xhtml`, `svg`, `vue`, `svelte`; scripts and styles inside are highlighted too), XML, CSS, SCSS/Sass, Less, Markdown (`md`), LaTeX (`tex`), Diff (`diff`, `patch`), HTTP |
