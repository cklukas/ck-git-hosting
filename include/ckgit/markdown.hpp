// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ckgit/yaml_subset.hpp"

namespace ckgit {

inline constexpr std::size_t kMaximumMarkdownInputBytes = 512 * 1024;
inline constexpr std::size_t kMaximumMarkdownOutputBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaximumMarkdownDepth = 8;

// Where a document's relative links point. Without a `resolver`, they become
// dashboard routes inside `project` at `commit_id` (documents keep `ref`,
// images pin the snapshot), relative to `directory`. A resolver also learns
// the 1-based source line the link is written on (0 when unknown), so a
// caller can point at a link it cannot resolve.
using MarkdownLinkResolver =
    std::function<std::optional<std::string>(std::string_view target, bool image, std::size_t line)>;

// An application-owned renderer for fenced code. Its result is trusted HTML;
// nullopt keeps the ordinary escaped code block. Never supplied by Markdown.
using MarkdownCodeRenderer = std::function<std::optional<std::string>(std::string_view language, std::string_view source)>;

// A file a document includes: `<<< path` (a code snippet) or
// `<!--@include: path-->` (a part written in Markdown). `content` is the
// whole file; `directory` is where it lives relative to the including
// document's directory ("" when beside it, "parts", "../shared"), so relative
// links and images written inside an included part still point where its
// author meant.
struct MarkdownInclude {
  std::string content;
  std::string directory;
};

// An application-owned reader for included files. `target` is the path as
// written, relative to the including document (a leading `/` or `@/` means
// the repository root), already decoded and free of control bytes; `markdown`
// says which form named it; `line` is the 1-based source line of the
// directive. nullopt means the file cannot be included -- the reader reports
// why itself -- and the directive then stays visible as text. Never supplied
// by Markdown; without a reader both forms are ordinary text (and the include
// comment is omitted like any other comment).
using MarkdownIncludeReader =
    std::function<std::optional<MarkdownInclude>(std::string_view target, bool markdown, std::size_t line)>;

// Receives a problem the renderer found and rendered around instead of
// stopping: a formula that does not parse, an unknown emoji-like container,
// an include region that does not exist. `line` is 1-based (0 when unknown).
using MarkdownReporter = std::function<void(const std::string& message, std::size_t line)>;

// Words the renderer writes itself, so a page in another language can carry
// its own: the default titles of alerts and `:::` containers, the footnote
// section's label and the footnote back-reference's accessible name.
struct MarkdownLabels {
  std::string note = "Note";
  std::string tip = "Tip";
  std::string important = "Important";
  std::string warning = "Warning";
  std::string caution = "Caution";
  std::string info = "Info";
  std::string danger = "Danger";
  std::string details = "Details";
  std::string footnotes = "Footnotes";
  std::string back_to_reference = "Back to reference";
};

struct LinkContext {
  std::string project;
  std::string commit_id;
  std::string directory;
  // Ordinary document links retain the selected branch/tag; image URLs use
  // commit_id so all images belong to the snapshot read for this page.
  std::string ref{};
  // When set, every relative target reaches it instead of the dashboard
  // rules: not an absolute http(s)/mailto URL, not a pure `#fragment`, only
  // after the usual sanitisation (no control bytes, backslashes, `//` or
  // `?`), percent-decoded, with its fragment removed. The answer is the final
  // href/src as it should appear (the renderer appends the fragment and
  // HTML-escapes the attribute), or nullopt to leave the link as text — the
  // same outcome an invalid target has. `image` says whether the target is
  // an image source. `project` and `commit_id` may then be empty.
  MarkdownLinkResolver resolver{};
  // The source line the rendered text starts on, for the resolver's `line`:
  // a document rendered without its front matter starts further down.
  std::size_t first_line = 1;
  // See MarkdownIncludeReader; empty leaves `<<<` and `@include` as text.
  MarkdownIncludeReader include_reader{};
  // See MarkdownReporter; problems go unreported when empty.
  MarkdownReporter reporter{};
  // Whether fenced code shows line numbers unless its info string says
  // `:line-numbers` / `showLineNumbers` or `:no-line-numbers` itself.
  bool line_numbers = false;
  MarkdownLabels labels{};
};

// One heading of a rendered document, as a table of contents needs it: the
// level (1–6), the `id` the <hN> carries, and the heading's plain text with
// inline markup removed (a link contributes its label, an image its
// alternative text, code its content).
struct MarkdownHeading {
  int level = 1;
  std::string id;
  std::string text;
};

// A bounded CommonMark subset with the extensions documentation relies on:
//
// - GitHub's: pipe tables, `~~strikethrough~~`, task-list items (`- [ ]`,
//   `- [x]`), footnotes (`[^label]` and `[^label]: text`), alerts (a
//   blockquote whose first line is exactly `[!NOTE]`, `[!TIP]`,
//   `[!IMPORTANT]`, `[!WARNING]` or `[!CAUTION]`) and math (`$…$`, `$$…$$`,
//   and ```` ```math ```` fences, rendered to MathML).
// - Containers: `::: tip [title]` … `:::` (also `:::tip[title]`) for info,
//   note, tip, important, warning, caution, danger and details, and the
//   tabbed `::: code-group` (one tab per fenced block) and `::: tabs`
//   (`== Label` starts a tab). More colons on the opener nest containers.
// - Definition lists (a term line, then `: definition`), emoji shortcodes
//   (`:tada:`), and fenced code with a title (`title="…"` or `[…]`), line
//   highlighting (`{1,3-5}`, `[!code highlight]`), line numbers
//   (`:line-numbers`, `showLineNumbers`), and diff and focus markers
//   (`[!code ++]`, `[!code --]`, `[!code focus]`, `[!code warning]`,
//   `[!code error]`, `highlight-next-line`, `highlight-start`/`-end`).
// - Includes, when the context has a reader: `<<< path` places a file (or
//   its `#region`, or `#L10-L20`) as a code block, and
//   `<!--@include: path-->` splices in another Markdown file (optionally a
//   `#region` or `{from,to}` line range), up to 4 levels deep.
//
// Raw HTML is escaped apart from a few attribute-free tags; relative links
// resolve inside the repository and images use the immutable raw route.
// Exceeding a size or work bound throws std::length_error; no partial page is
// returned. Deeper nesting becomes escaped text rather than further recursion.
// When `outline` is given it is replaced by the document's top-level headings
// (those inside quotes, alerts, containers or list items are left out), in
// order, with the same ids the HTML carries.
std::string renderMarkdown(std::string_view source, const LinkContext& context,
                           std::vector<MarkdownHeading>* outline = nullptr,
                           const MarkdownCodeRenderer& code_renderer = {});

// YAML front matter: bounds on the block at the start of a file.
inline constexpr std::size_t kMaximumFrontMatterLines = 512;        // lines between the fences
inline constexpr std::size_t kMaximumFrontMatterBytes = 32 * 1024;  // the block, fences included
inline constexpr std::size_t kMaximumFrontMatterKeyBytes = 64;
inline constexpr std::size_t kMaximumFrontMatterValueBytes = 4096;

// A front matter key is ASCII letters, digits, `.`, `_` and `-` (a line whose
// key has any other byte is not front matter at all), non-empty, and within
// kMaximumFrontMatterKeyBytes: the only kind of key a block can carry, and so
// the only kind a configuration may name.
bool isValidFrontMatterKey(std::string_view key);

// The front matter of a Markdown file: an opening `---` on the very first
// line, a YAML mapping in the same strict subset as `.ckgit/ci.yml` (block
// and flow mappings and sequences, plain, single- or double-quoted scalars
// with the escapes `\\ \" \n \t`, `|` and `>` block scalars, `#` comments,
// spaces-only indentation), and a closing `---` or `...` line, all within
// kMaximumFrontMatterLines and kMaximumFrontMatterBytes.
//
// `structured` is the whole mapping, every key with its value of whatever
// kind, in file order; `entries` repeats just the scalar-valued keys as
// strings, for callers that only read those. An unknown key is the caller's
// business. A block that has the shape of front matter but is not valid YAML
// (a duplicate key, an unterminated quote, a key without a value, a value
// over its bound) still splits, with `structured` and `entries` empty and `error` saying
// why in the same "front matter: ... (line N)" wording as workflow errors, so
// a page never shows its metadata as text and a checker can report the
// mistake.
struct FrontMatter {
  YamlNode structured;  // bounded parsed metadata, including mappings and lists
  std::vector<std::pair<std::string, std::string>> entries;
  std::size_t body_offset = 0;  // where the Markdown body begins in the source
  std::string error;            // empty when `structured` and `entries` are trustworthy
};

// Returns nullopt when the file does not begin with a front-matter block: no
// opening fence on line 1, no closing fence within the bounds, or a line in
// between that is neither `key: value`, an indented continuation, a `- item`,
// a comment nor blank (so a file that merely starts with a thematic break is
// all body). Never throws.
std::optional<FrontMatter> splitFrontMatter(std::string_view source);

// The Markdown body of a file: everything after its front matter, or the whole
// file when it has none.
std::string_view markdownBody(std::string_view source);

}  // namespace ckgit
