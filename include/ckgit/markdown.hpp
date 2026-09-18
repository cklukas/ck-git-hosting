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

namespace ckgit {

inline constexpr std::size_t kMaximumMarkdownInputBytes = 512 * 1024;
inline constexpr std::size_t kMaximumMarkdownOutputBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaximumMarkdownDepth = 8;

// Where a document's relative links point. Without a `resolver`, they become
// dashboard routes inside `project` at `commit_id` (documents keep `ref`,
// images pin the snapshot), relative to `directory`.
using MarkdownLinkResolver = std::function<std::optional<std::string>(std::string_view target, bool image)>;

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

// A bounded CommonMark subset with the GitHub extensions documentation relies
// on: pipe tables, `~~strikethrough~~`, task-list items (`- [ ]`, `- [x]`),
// and alerts (a blockquote whose first line is exactly `[!NOTE]`, `[!TIP]`,
// `[!IMPORTANT]`, `[!WARNING]` or `[!CAUTION]`). Raw HTML is always escaped;
// relative links resolve inside the repository and images use the immutable
// raw route. Exceeding a size or work bound throws std::length_error; no
// partial page is returned. Deeper nesting becomes escaped text rather than
// further recursion. When `outline` is given it is replaced by the document's
// top-level headings (those inside quotes, alerts or list items are left out),
// in order, with the same ids the HTML carries.
std::string renderMarkdown(std::string_view source, const LinkContext& context,
                           std::vector<MarkdownHeading>* outline = nullptr);

// YAML front matter: bounds on the block at the start of a file.
inline constexpr std::size_t kMaximumFrontMatterLines = 64;       // lines between the fences
inline constexpr std::size_t kMaximumFrontMatterBytes = 8 * 1024;  // the block, fences included
inline constexpr std::size_t kMaximumFrontMatterKeyBytes = 64;
inline constexpr std::size_t kMaximumFrontMatterValueBytes = 4096;

// The front matter of a Markdown file: an opening `---` on the very first
// line, `key: value` lines in the same strict YAML subset as `.ckgit/ci.yml`
// (plain, single- or double-quoted scalars with the escapes `\\ \" \n \t`,
// `#` comments, spaces-only indentation), and a closing `---` or `...` line,
// all within kMaximumFrontMatterLines and kMaximumFrontMatterBytes.
//
// `entries` holds the scalar-valued keys in file order; a key whose value is
// a list or a mapping is left out, and an unknown key is the caller's
// business. A block that has the shape of front matter but is not valid YAML
// (a duplicate key, an unterminated quote, a key without a value, a value
// over its bound) still splits, with `entries` empty and `error` saying why
// in the same "front matter: ... (line N)" wording as workflow errors, so a
// page never shows its metadata as text and a checker can report the mistake.
struct FrontMatter {
  std::vector<std::pair<std::string, std::string>> entries;
  std::size_t body_offset = 0;  // where the Markdown body begins in the source
  std::string error;            // empty when `entries` is trustworthy
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
