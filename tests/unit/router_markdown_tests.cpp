// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/http_request.hpp"
#include "ckgit/http_router.hpp"
#include "ckgit/markdown.hpp"
#include "ckgit/cli_help.hpp"
#include "ckgit/web_renderer.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error("Router/Markdown: " + std::string(message));
}

bool contains(std::string_view value, std::string_view part) {
  return value.find(part) != std::string_view::npos;
}

}  // namespace

void testRouterMarkdown() {
  using namespace ckgit;
  const std::string sha(40, 'a');
  const std::string sha256(64, 'b');
  const std::string project = "/project/example/";
  const LinkContext root_context{"example", sha, ""};
  const LinkContext directory_context{"example", sha, "docs/tutorials"};

  const auto about_page = pageLayout("Projects", "<p>Page content</p>");
  check(contains(about_page, "<a class=\"brand\" href=\"/\" aria-label=\"ck-git-hosting\">") &&
        contains(about_page, "<span class=\"brand-monogram\">ck</span>") &&
        contains(about_page, "<span class=\"brand-wordmark\">git<span class=\"brand-caption\">hosting</span></span>"),
        "text logo has a complete accessible product name and requires no image asset");
  check(contains(about_page, "class=\"about-trigger\" popovertarget=\"about-dialog\" popovertargetaction=\"show\" aria-haspopup=\"dialog\">About</button>") &&
        contains(about_page, "id=\"about-dialog\" class=\"about-dialog\" popover=\"auto\" role=\"dialog\" aria-labelledby=\"about-title\""),
        "About uses a labelled native auto-popover dialog and a native opening control");
  check(contains(about_page, "class=\"about-close\" popovertarget=\"about-dialog\" popovertargetaction=\"hide\" autofocus>Close</button>") &&
        !contains(about_page, "aria-modal=\"true\"") && !contains(about_page, "<script") && !contains(about_page, "onclick="),
        "About supports native close, Escape and light dismissal without modal claims or JavaScript");
  check(contains(about_page, "<h2 id=\"about-title\">About ck-git-hosting</h2>") &&
        contains(about_page, "<p class=\"about-version\">Version <code>" + htmlEscape(buildVersion()) + "</code></p>") &&
        contains(about_page, "© 2026 C. Klukas") && contains(about_page, "Licensed under the MIT License."),
        "About identifies the running build and the repository's actual copyright and license");
  const auto about_id = about_page.find("id=\"about-dialog\"");
  check(about_id != std::string::npos && about_page.find("id=\"about-dialog\"", about_id + 1) == std::string::npos &&
        contains(about_page, "<main id=\"main-content\"><p>Page content</p></main>"),
        "shared layout emits one About target while preserving normal page content");

  // A project with a published Pages site gets a header Docs button linking to
  // it on the separate Pages origin; a project without one shows no such button.
  ProjectSummary with_pages{};
  with_pages.name = "demo";
  with_pages.pages_site_url = "http://host.example:8421/demo/";
  const auto pages_page = pageLayout("demo", "<p>x</p>", &with_pages);
  check(contains(pages_page, "class=\"docs-trigger\" href=\"http://host.example:8421/demo/\" target=\"_blank\" rel=\"noopener\">Docs</a>"),
        "a project with a published site gets a header Docs link to the Pages origin");
  ProjectSummary no_pages{};
  no_pages.name = "demo";
  // The stylesheet always defines .docs-trigger, so assert on the button markup.
  check(!contains(pageLayout("demo", "<p>x</p>", &no_pages), ">Docs</a>"),
        "a project without a published site shows no Docs link");

  // The no-JS refresh fallback is wrapped in <noscript>, not emitted bare: a
  // bare <meta refresh> arms its timer the moment the browser parses it, well
  // before any later body script could remove it, so a script-enabled reader
  // must never see it live at all rather than racing to cancel it.
  const auto refreshing = pageLayout("demo", "<p>x</p>", nullptr, nullptr, 3u);
  check(contains(refreshing, "<noscript><meta http-equiv=\"refresh\" content=\"3\"></noscript>"),
        "an active-run page's refresh fallback is confined to <noscript>");
  check(!contains(pageLayout("demo", "<p>x</p>"), "http-equiv=\"refresh\""),
        "a page with no refresh interval emits no refresh meta at all");

  check(isObjectId(sha) && isObjectId(sha256), "both full Git object formats are accepted");
  check(!isObjectId("abcdef") && !isObjectId(std::string(40, 'A')) && !isObjectId(std::string(40, 'g')),
        "abbreviated, uppercase, and nonhex object IDs rejected");
  check(isValidUtf8("Gr\xc3\xbc\xc3\x9f\x65/\xf0\x9f\x98\x80"), "valid UTF-8 decoded");
  for (const std::string& invalid : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
      std::string("\xf4\x90\x80\x80"), std::string("\x80"), std::string("\xe2\x82")}) {
    check(!isValidUtf8(invalid), "invalid, overlong, surrogate, truncated UTF-8 rejected");
  }
  const std::string filename = "docs/Gr\xc3\xbc\xc3\x9f\x65 image:#?.svg";
  const auto encoded = encodePathSegment(filename);
  check(encoded == "docs/Gr%C3%BC%C3%9Fe%20image%3A%23%3F.svg", "canonical byte encoding keeps slash");
  check(decodePathSegment(encoded) == filename, "canonical encoding round-trips");
  for (const std::string_view invalid : {"%2F", "%2f", "%00", "%0A", "%7F", "%5C", "%41", "%7e",
      "%C0%AF", "%ED%A0%80", "%F4%90%80%80", "%C2%80", "a%", "%G0", "a b", "a:b"}) {
    check(!decodePathSegment(invalid), "noncanonical escapes and unsafe characters rejected");
  }

  check(parseHttpRoute("/").kind == RouteKind::kTable, "project table route");
  check(parseHttpRoute("/by-name").sort_by_name, "project table name sort route");
  check(parseHttpRoute("/project/example").kind == RouteKind::kOverview, "project overview route");
  check(parseHttpRoute(project + "commits/heads/feature/nested").ref == "heads/feature/nested", "slash-bearing explicit branch route");
  check(parseHttpRoute(project + "commits/tags/v1/before/" + sha).cursor == sha, "commit cursor route");
  check(parseHttpRoute(project + "graph/main/before/" + sha).kind == RouteKind::kGraph, "paged graph route");
  check(parseHttpRoute(project + "commit/" + sha256).ref == sha256, "full object commit route");
  check(parseHttpRoute(project + "tree/main:").kind == RouteKind::kTree, "empty root tree path accepted");
  const auto blob = parseHttpRoute(project + "blob/heads/feature/nested:" + encoded);
  check(blob.kind == RouteKind::kBlob && blob.path == filename && blob.ref == "heads/feature/nested",
        "decoded blob route accepts encoded colon inside path");
  check(parseHttpRoute(project + "raw/" + sha + ":diagram.svg").kind == RouteKind::kRaw, "immutable raw route");
  check(parseHttpRoute(project + "calendar/main").month == 0, "current month route");
  const auto month = parseHttpRoute(project + "calendar/heads/feature/nested/2026/09");
  check(month.kind == RouteKind::kCalendar && month.ref == "heads/feature/nested" && month.year == 2026 && month.month == 9,
        "explicit calendar month route");
  const auto day = parseHttpRoute(project + "day/tags/v1/2024-02-29");
  check(day.kind == RouteKind::kDay && day.day == 29 && day.month == 2, "leap date route");
  for (const std::string& ref : {std::string("heads/release/2026/09"), std::string("tags/release/2026/13"),
                                  std::string("heads/release/20ab/11")}) {
    const auto url = sourceUrl("example", "calendar", ref);
    const auto current = parseHttpRoute(url);
    check(url.ends_with(':') && current.kind == RouteKind::kCalendar && current.ref == ref && current.year == 0,
          "calendar preserves a complete date-shaped ref with an explicit boundary");
    const auto selected_month = parseHttpRoute(url + "/2024/02");
    check(selected_month.kind == RouteKind::kCalendar && selected_month.ref == ref &&
          selected_month.year == 2024 && selected_month.month == 2,
          "calendar month suffix follows the ref boundary without changing identity");
  }
  for (const auto* kind : {"commits", "graph"}) {
    for (const std::string& ref : {"heads/topic/before/" + sha, std::string("tags/release/before/not-a-cursor")}) {
      const auto url = sourceUrl("example", kind, ref);
      const auto newest = parseHttpRoute(url);
      check(url.ends_with(':') && newest.ref == ref && newest.cursor.empty(),
            "history preserves cursor-shaped ref names");
      const auto older = parseHttpRoute(url + "/before/" + sha256);
      check(older.ref == ref && older.cursor == sha256,
            "paging suffix follows the ref boundary without changing identity");
    }
  }
  check(sourceUrl("example", "calendar", "heads/main") == project + "calendar/heads/main" &&
        sourceUrl("example", "commits", "tags/v1") == project + "commits/tags/v1",
        "common unambiguous route spellings stay compatible");
  for (const auto& suffix : {std::string("calendar/heads/main:/2024/13"),
                            std::string("calendar/heads/main:/2024/02/extra"),
                            std::string("commits/heads/main:/before/not-an-id"),
                            std::string("commits/heads/main::/before/") + sha}) {
    check(parseHttpRoute(project + suffix).kind == RouteKind::kNotFound,
          "explicit ref boundaries do not admit malformed route suffixes");
  }
  for (const auto& suffix : std::vector<std::string>{"tree/main:a/../b", "tree/main:./b", "tree/main:a//b", "tree/main:a/",
      "tree/-main:a", "tree/heads/-main:a", "tree/.hidden:a", "tree/main:a:b", "tree/main:a%2Fb", "tree/main:a%00b",
      "blob/main:", "raw/main:a", "commit/abc123", "commit/" + std::string(40, 'A'), "commits/main/before/abc123",
      "day/main/2023-02-29", "day/main/2100-02-29", "day/main/2026-13-01", "calendar/main/2026/13",
      "tree/main:" + std::string(300, 'x'), "tree/" + std::string(300, 'x') + ":a", "tree/main:a?b", "tree/main:a#b"}) {
    check(parseHttpRoute(project + suffix).kind == RouteKind::kNotFound, "invalid route rejected: " + suffix);
  }
  for (const std::string_view invalid : {"/project/%65xample", "/project/../tree/main:", "/project/example/", "/unknown", "//"}) {
    check(parseHttpRoute(invalid).kind == RouteKind::kNotFound, "invalid project route rejected");
  }
  std::string deep_path;
  for (std::size_t index = 0; index < kMaximumRoutePathDepth; ++index) deep_path += (index == 0 ? "a" : "/a");
  check(parseHttpRoute(project + "tree/main:" + deep_path).kind == RouteKind::kTree, "maximum tree depth accepted");
  check(parseHttpRoute(project + "tree/main:" + deep_path + "/a").kind == RouteKind::kNotFound, "excess tree depth rejected");
  check(parseReadOnlyHttpRequest("GET " + project + "blob/main:" + encoded + " HTTP/1.1\r\nHost: localhost\r\n\r\n").has_value(),
        "HTTP accepts valid escaped browsing route");
  check(!parseReadOnlyHttpRequest("GET " + project + "blob/main:a%2Fb HTTP/1.1\r\nHost: localhost\r\n\r\n"),
        "HTTP rejects noncanonical escaped route");

  const auto formatting = renderMarkdown("# Hello, world!\n\nA *small* **bold** ***both*** and `a < b` paragraph.  \nNext\\\nline.\n\n---\n\n# Hello, world!\n", root_context);
  check(contains(formatting, "<h1 id=\"hello-world\">Hello, world!</h1>") && contains(formatting, "id=\"hello-world-1\""),
        "headings have stable distinct anchors");
  const auto colliding_headings = renderMarkdown("# Intro\n# Intro\n# Intro-1\n# Intro\n# Intro-2\n", root_context);
  for (const auto* anchor : {"intro", "intro-1", "intro-1-1", "intro-2", "intro-2-1"}) {
    const std::string attribute = std::string("id=\"") + anchor + "\"";
    const auto first = colliding_headings.find(attribute);
    check(first != std::string::npos && colliding_headings.find(attribute, first + attribute.size()) == std::string::npos,
          "generated suffixes cannot collide with later literal heading names");
  }
  check(contains(renderMarkdown("# Intro-1\n# Intro\n# Intro\n", root_context), "id=\"intro-2\""),
        "heading suffixes skip IDs already occupied by earlier literal names");
  const auto linked_headings = renderMarkdown(
      "## [Install](setup.md)\n## **Read** `the code`\n## ![Build status](badge.svg)\n## A & B\n", root_context);
  check(contains(linked_headings, "id=\"install\"") && !contains(linked_headings, "id=\"installsetupmd\"") &&
        contains(linked_headings, "id=\"read-the-code\"") && contains(linked_headings, "id=\"build-status\"") &&
        contains(linked_headings, "id=\"a-b\""),
        "heading anchors use visible link labels, code, image alternative text, and decoded entities");
  check(contains(formatting, "<em>small</em>") && contains(formatting, "<strong>bold</strong>") &&
        contains(formatting, "<em><strong>both</strong></em>") && contains(formatting, "<code>a &lt; b</code>"),
        "inline emphasis and code supported");
  check(contains(formatting, "paragraph.<br>\nNext<br>\nline.") && contains(formatting, "<hr>"), "both hard break forms and thematic break");
  check(contains(renderMarkdown("**bold *inside*** and *italic **inside***", root_context),
        "<strong>bold <em>inside</em></strong> and <em>italic <strong>inside</strong></em>"), "nested emphasis delimiters");
  check(contains(renderMarkdown("foo_bar_baz", root_context), "foo_bar_baz"), "intraword underscores stay literal");
  check(contains(renderMarkdown("`` a ` b ``", root_context), "<code>a ` b</code>"), "multi-backtick inline code");

  const auto code = renderMarkdown("```cpp\n<script>\n  two\n```\n\n~~~evil\"attr\n&hello\n~~~\n\n    a\n    b\n", root_context);
  check(contains(code, "<pre><code class=\"language-cpp\">&lt;script&gt;\n  two\n</code></pre>"), "fenced code keeps newlines and indentation");
  check(renderMarkdown("```cpp\nint x = 1; // c\n```\n", root_context) ==
            "<pre><code class=\"language-cpp\"><span class=\"hl-k\">int</span> x = <span class=\"hl-n\">1</span>; <span class=\"hl-c\">// c</span>\n</code></pre>\n",
        "a fence whose info string names a language is highlighted");
  check(renderMarkdown("```rust\nfn x() { let y = 1; }\n```\n", root_context) ==
            "<pre><code class=\"language-rust\">fn x() { let y = 1; }\n</code></pre>\n",
        "a fence naming an unknown language stays plain");
  check(renderMarkdown("```yaml\n# c\nkey: \"v\"\n```\n", root_context) ==
            "<pre><code class=\"language-yaml\"><span class=\"hl-c\"># c</span>\n<span class=\"hl-a\">key</span>: <span class=\"hl-s\">&quot;v&quot;</span>\n</code></pre>\n",
        "YAML fences are highlighted with escaped quotes");
  check(!contains(code, "class=\"language-evil") && contains(code, "&amp;hello"), "unsafe fence info ignored");
  check(contains(code, "<pre><code>a\nb\n</code></pre>"), "indented code block");
  const auto quote = renderMarkdown("> quoted **word**  \n> next\n>\n> - a\n> - b\n", root_context);
  check(contains(quote, "<blockquote>") && contains(quote, "<strong>word</strong><br>") && contains(quote, "<ul>"), "nested quoted blocks and hard breaks");
  const auto lists = renderMarkdown("3. three\n4. four\n   - nested\n     - deep\n", root_context);
  check(contains(lists, "<ol start=\"3\">") && std::count(lists.begin(), lists.end(), '<') >= 12 && contains(lists, "deep"), "ordered and nested unordered lists");

  const auto safe = renderMarkdown("<script>alert('x')</script>\n\n<img src=x onerror=evil>\n\n[bad](javascript:alert(1)) ![bad](data:image/svg+xml,x)\n[bad](JaVaScRiPt:alert)\n[good](https://example.test/?a=1&b=2 \"title\")\n<https://example.test/> <person@example.test>\n", root_context);
  check(!contains(safe, "<script>") && !contains(safe, "<img src=x") && contains(safe, "&lt;script&gt;"), "raw HTML is text");
  check(!contains(safe, "href=\"javascript:") && !contains(safe, "href=\"JaVa") && !contains(safe, "src=\"data:"), "active URL schemes blocked");
  check(contains(safe, "href=\"https://example.test/?a=1&amp;b=2\" title=\"title\"") &&
        contains(safe, "href=\"mailto:person@example.test\""), "safe links, titles, and autolinks");
  check(renderMarkdown("<!-- header comment -->\n\nbefore <!-- hidden -->after", root_context) ==
            "<p>before after</p>\n", "HTML comments disappear without creating empty paragraphs");
  const auto comment_blocks = renderMarkdown(
      "visible <!-- hidden\n\n# hidden heading\n\n```\nhidden fence\n\n--> recovered\n\n# End\n",
      root_context);
  check(contains(comment_blocks, "visible") && contains(comment_blocks, "recovered") &&
        contains(comment_blocks, "id=\"end\"") && !contains(comment_blocks, "hidden") &&
        !contains(comment_blocks, "<pre>"), "multiline comments suppress Markdown-looking blocks until closing");
  check(renderMarkdown("before <!-- not closed\n\n# hidden", root_context) == "<p>before </p>\n",
        "unterminated comments hide their remaining content safely");
  const auto comment_code = renderMarkdown(
      "`<!-- inline -->` and ``a ` <!-- long --> b``\n\n"
      "```html\n<!-- fenced -->\n```\n\n"
      "    <!-- indented -->\n\n"
      "> ```\n> <!-- nested fence -->\n> ```\n\n"
      "\\<!-- escaped -->\n\n<script>still escaped</script>", root_context);
  for (const auto* marker : {"inline", "long", "fenced", "indented", "nested fence", "escaped"})
    check(contains(comment_code, std::string("&lt;!-- ") + marker + " --&gt;"),
          "comments inside code and escaped comment syntax remain visible");
  check(contains(comment_code, "&lt;script&gt;still escaped&lt;/script&gt;") &&
        !contains(comment_code, "<script>"), "comment hiding does not enable raw HTML");
  check(contains(renderMarkdown("# Install <!-- hidden -->\n\n[Install](#install)", root_context),
                 "id=\"install\""), "hidden heading comments do not alter heading anchors");
  check(renderMarkdown("<!--" + std::string(kMaximumMarkdownInputBytes - 4, 'x'), root_context).empty(),
        "maximum-size unterminated comment stays within the existing work and output bounds");
  const auto relative = renderMarkdown("[guide](../guide.md#install) ![Diagram](../img/flow.svg) [root](/README.md) [folder](../) [anchor](#a%20b) [escape](../../../secret) [quoted](<space name.md>)", directory_context);
  check(contains(relative, "/blob/" + sha + ":docs/guide.md#install"), "relative link uses resolved commit and README directory");
  check(contains(relative, "src=\"/project/example/raw/" + sha + ":docs/img/flow.svg\""), "SVG stays an img using the raw route");
  check(contains(relative, "/blob/" + sha + ":README.md") && contains(relative, "/tree/" + sha + ":docs"), "root-relative and directory links rewritten");
  check(contains(relative, "href=\"#a%20b\"") && contains(relative, "space%20name.md"), "fragment and filename escaping");
  check(!contains(relative, "href=\"/project/example/blob/" + sha + ":secret"), "relative links cannot traverse above repository root");
  const LinkContext branch_context{"example", sha, "docs/tutorials", "heads/feature/guide"};
  const auto branch_links = renderMarkdown("[guide](../guide.md#install) [folder](../) ![Diagram](../img/flow.svg)", branch_context);
  check(contains(branch_links, "href=\"/project/example/blob/heads/feature/guide:docs/guide.md#install\"") &&
        contains(branch_links, "href=\"/project/example/tree/heads/feature/guide:docs\""),
        "relative document and folder links retain the selected branch identity");
  check(contains(branch_links, "src=\"/project/example/raw/" + sha + ":docs/img/flow.svg\""),
        "images stay at the immutable content snapshot when document links retain their branch");
  const LinkContext tag_context{"example", sha, "", "tags/release/v1"};
  check(contains(renderMarkdown("[guide](docs/guide.md#install)", tag_context),
                 "href=\"/project/example/blob/tags/release/v1:docs/guide.md#install\""),
        "relative documentation links preserve an explicit tag identity and heading fragment");
  check(!contains(renderMarkdown("[bad](//evil.test/a) [bad](a?b=c)", root_context), "<a "), "unsupported relative URL forms remain text");
  const auto table = renderMarkdown("| Name | Value |\n| :--- | ---: |\n| **one** | `x|y` |\n| a\\|b | 2 |\n", root_context);
  check(contains(table, "<table><thead>") && contains(table, "class=\"align-right\"") &&
        contains(table, "<code>x|y</code>") && contains(table, "a|b"), "pipe table cells, alignment, code and escaped pipes");

  std::string nested;
  for (std::size_t index = 0; index < kMaximumMarkdownDepth + 4; ++index) nested += "> ";
  nested += "<script>deep</script>";
  const auto bounded = renderMarkdown(nested, root_context);
  check(!contains(bounded, "<script>") && contains(bounded, "&lt;script&gt;deep"), "excess nesting becomes escaped text");
  bool input_limited = false;
  try { static_cast<void>(renderMarkdown(std::string(kMaximumMarkdownInputBytes + 1, 'a'), root_context)); }
  catch (const std::length_error&) { input_limited = true; }
  check(input_limited, "input size cap is enforced");
  bool output_limited = false;
  std::string large_output;
  while (large_output.size() + 2 <= kMaximumMarkdownInputBytes) large_output += "#\n";
  try { static_cast<void>(renderMarkdown(large_output, root_context)); }
  catch (const std::length_error&) { output_limited = true; }
  check(output_limited, "output size cap is enforced");
  bool work_limited = false;
  try { static_cast<void>(renderMarkdown(std::string(10000, '['), root_context)); }
  catch (const std::length_error&) { work_limited = true; }
  check(work_limited, "pathological delimiter scans have a work cap");
  check(renderMarkdown(std::string(kMaximumMarkdownInputBytes, 'a'), root_context).size() <= kMaximumMarkdownOutputBytes,
        "maximum-size ordinary Markdown remains renderable");
}

// GitHub-flavoured extensions: alerts, task lists, strikethrough, the heading
// outline, and front matter.
void testMarkdownExtensions() {
  using namespace ckgit;
  const LinkContext ctx{"example", std::string(40, 'a'), ""};

  struct Alert { const char* marker; const char* kind; const char* title; };
  for (const Alert& alert : {Alert{"NOTE", "note", "Note"}, Alert{"TIP", "tip", "Tip"},
                             Alert{"IMPORTANT", "important", "Important"}, Alert{"WARNING", "warning", "Warning"},
                             Alert{"CAUTION", "caution", "Caution"}}) {
    const auto html = renderMarkdown(std::string("> [!") + alert.marker + "]\n> Body **text**\n", ctx);
    check(html == std::string("<div class=\"alert alert-") + alert.kind + "\"><p class=\"alert-title\">" + alert.title +
                      "</p>\n<p>Body <strong>text</strong></p>\n</div>\n",
          std::string("alert kind ") + alert.marker);
  }
  check(renderMarkdown("> [!NOTE] extra\n> more\n", ctx) == "<blockquote>\n<p>[!NOTE] extra\nmore</p>\n</blockquote>\n",
        "a marker with text after it on the same line is an ordinary blockquote");
  check(renderMarkdown("> [!note]\n> body\n", ctx) == "<blockquote>\n<p>[!note]\nbody</p>\n</blockquote>\n",
        "a lowercase marker is an ordinary blockquote");
  check(renderMarkdown("> [!NOTE]\n", ctx) == "<blockquote>\n<p>[!NOTE]</p>\n</blockquote>\n",
        "a marker with nothing below it is an ordinary blockquote");
  check(renderMarkdown("> [!TIP]\n> > inner\n", ctx) ==
            "<div class=\"alert alert-tip\"><p class=\"alert-title\">Tip</p>\n<blockquote>\n<p>inner</p>\n</blockquote>\n</div>\n",
        "a quote nests inside an alert as a plain blockquote");
  check(renderMarkdown("> [!WARNING]  \n>\n> - a\n> - b\n", ctx) ==
            "<div class=\"alert alert-warning\"><p class=\"alert-title\">Warning</p>\n<ul>\n<li>a\n</li>\n<li>b\n</li>\n</ul>\n</div>\n",
        "trailing spaces after the marker and a blank quoted line are fine");

  check(renderMarkdown("- [ ] open\n- [x] done\n- [X] also\n- plain\n", ctx) ==
            "<ul class=\"contains-task-list\">\n"
            "<li class=\"task-list-item\"><input type=\"checkbox\" disabled> open\n</li>\n"
            "<li class=\"task-list-item\"><input type=\"checkbox\" disabled checked> done\n</li>\n"
            "<li class=\"task-list-item\"><input type=\"checkbox\" disabled checked> also\n</li>\n"
            "<li>plain\n</li>\n</ul>\n",
        "task items unchecked, checked either way, mixed with a plain item");
  check(renderMarkdown("- [ ] a\n\n  second\n", ctx) ==
            "<ul class=\"contains-task-list\">\n<li class=\"task-list-item\"><p><input type=\"checkbox\" disabled> a</p>\n<p>second</p>\n</li>\n</ul>\n",
        "a loose task item keeps its checkbox inside the first paragraph");
  check(renderMarkdown("- [ ] first\n  second\n", ctx) ==
            "<ul class=\"contains-task-list\">\n<li class=\"task-list-item\"><input type=\"checkbox\" disabled> first\nsecond\n</li>\n</ul>\n",
        "a task item's paragraph may continue on the next line");
  check(renderMarkdown("- [ ] parent\n  - [x] child\n", ctx) ==
            "<ul class=\"contains-task-list\">\n<li class=\"task-list-item\"><input type=\"checkbox\" disabled> parent\n"
            "<ul class=\"contains-task-list\">\n<li class=\"task-list-item\"><input type=\"checkbox\" disabled checked> child\n</li>\n</ul>\n"
            "</li>\n</ul>\n",
        "task lists nest");
  check(contains(renderMarkdown("1. [ ] one\n2. [x] two\n", ctx), "<ol class=\"contains-task-list\">\n<li class=\"task-list-item\">"),
        "ordered lists carry task items too");
  const auto literal = renderMarkdown("- [ ]\n- [ ]x\n- text [ ] mid\n- [ ] # not a heading\n- [y] no\n", ctx);
  check(!contains(literal, "checkbox") && !contains(literal, "contains-task-list") && contains(literal, "<li>[ ]\n</li>") &&
            contains(literal, "<li>[ ]x\n</li>") && contains(literal, "text [ ] mid") &&
            contains(literal, "<li>[ ] # not a heading\n</li>") && contains(literal, "<li>[y] no\n</li>"),
        "brackets that are not a task marker followed by text stay literal");

  check(renderMarkdown("~~gone~~ and ~~ a~~ and ~a~ and ~~x~~~", ctx) ==
            "<p><del>gone</del> and ~~ a~~ and ~a~ and ~~x~~~</p>\n",
        "double tildes strike through; whitespace inside, single and triple tildes stay text");
  check(renderMarkdown("~~[link](https://example.test/)~~ **~~x~~** `~~code~~` \\~~no~~", ctx) ==
            "<p><del><a href=\"https://example.test/\">link</a></del> <strong><del>x</del></strong> <code>~~code~~</code> ~~no~~</p>\n",
        "strikethrough wraps links, nests in emphasis, and yields to code spans and escapes");

  std::vector<MarkdownHeading> outline{{9, "stale", "stale"}};
  const auto outlined = renderMarkdown(
      "# Top\n\n## **Read** `the code`\n\n> ### Quoted\n\n- ### Listed\n\n> [!NOTE]\n> #### Alerted\n\n## Top\n\n## A & B <!-- c -->\n",
      ctx, &outline);
  check(outline.size() == 4, "the outline is replaced and lists top-level headings only");
  check(outline[0].level == 1 && outline[0].id == "top" && outline[0].text == "Top", "outline entry 1");
  check(outline[1].level == 2 && outline[1].id == "read-the-code" && outline[1].text == "Read the code",
        "outline text is the heading's plain text");
  check(outline[2].level == 2 && outline[2].id == "top-1" && outline[2].text == "Top", "outline ids are the deduplicated HTML ids");
  check(outline[3].level == 2 && outline[3].id == "a-b" && outline[3].text == "A & B", "outline text has entities decoded and comments removed");
  check(contains(outlined, "id=\"quoted\"") && contains(outlined, "id=\"listed\"") && contains(outlined, "id=\"alerted\""),
        "nested headings still get ids in the HTML");

  const std::string doc =
      "---\ntitle: \"A \\\"quoted\\\" title\"\ndescription: 'it''s'  # comment\nnav_order: 3\n\n# a comment line\n"
      "tags:\n  - a\nnav_exclude: true\n---\n# Body\n";
  const auto front = splitFrontMatter(doc);
  check(front.has_value() && front->error.empty(), "a valid block splits without an error");
  check(front->entries.size() == 4 && front->entries[0].first == "title" && front->entries[0].second == "A \"quoted\" title" &&
            front->entries[1].first == "description" && front->entries[1].second == "it's" &&
            front->entries[2].first == "nav_order" && front->entries[2].second == "3" &&
            front->entries[3].first == "nav_exclude" && front->entries[3].second == "true",
        "scalar entries in file order, quoting and escapes decoded, a list-valued key left out");
  check(front->body_offset == doc.find("# Body") && markdownBody(doc) == "# Body\n" &&
            renderMarkdown(markdownBody(doc), ctx) == "<h1 id=\"body\">Body</h1>\n",
        "the body starts after the closing fence");
  check(markdownBody("---\nk: v\n...\nbody") == "body", "a `...` line closes front matter");
  check(markdownBody("---\r\nk: v\r\n---\r\nbody") == "body" && splitFrontMatter("---\r\nk: v\r\n---\r\n")->entries[0].second == "v",
        "CRLF line endings are accepted");
  const auto empty = splitFrontMatter("---\n---\nbody");
  check(empty && empty->entries.empty() && empty->error.empty() && empty->body_offset == 8, "an empty block is front matter");
  check(!splitFrontMatter("---\nk: v\n") && !splitFrontMatter("---\nk: v") && !splitFrontMatter("---") &&
            !splitFrontMatter("\n---\nk: v\n---\n") && !splitFrontMatter(" ---\nk: v\n---\n") && !splitFrontMatter("plain"),
        "no closing fence, or no opening fence on line 1, means no front matter");
  check(!splitFrontMatter("---\n\nSome text\n\n---\n") && contains(renderMarkdown(markdownBody("---\n\nSome text\n\n---\n"), ctx), "<hr>"),
        "a document that merely starts with a thematic break is all body");
  std::string many = "---\n";
  for (int index = 0; index < 64; ++index) many += "k" + std::to_string(index) + ": v\n";
  const auto at_limit = splitFrontMatter(many + "---\n");
  check(at_limit && at_limit->error.empty() && at_limit->entries.size() == 64, "64 lines are within the bound");
  check(!splitFrontMatter(many + "k64: v\n---\n"), "65 lines are not front matter");
  std::string big = "---\n";
  for (int index = 0; index < 60; ++index) big += "k" + std::to_string(index) + ": " + std::string(200, 'x') + "\n";
  check(!splitFrontMatter(big + "---\n"), "a block over 8 KiB is not front matter");
  const auto duplicate = splitFrontMatter("---\na: 1\na: 2\n---\nbody");
  check(duplicate && duplicate->entries.empty() && duplicate->error == "front matter: duplicate key 'a' (line 3)" &&
            markdownBody("---\na: 1\na: 2\n---\nbody") == "body",
        "a duplicate key is reported with the file's line number and the block still splits");
  const auto missing = splitFrontMatter("---\ntitle:\n---\n");
  check(missing && missing->error == "front matter: 'title' has no value (line 2)", "a key without a value is reported");
  check(splitFrontMatter("---\ntitle:\nx: 1\n---\n")->error == "front matter: 'title' has no value (line 2)",
        "a key without a value followed by another key is reported");
  check(splitFrontMatter("---\nk: 'open\n---\n")->error == "front matter: unterminated single-quoted scalar (line 2)",
        "YAML errors carry the front matter prefix and the file line");
  const auto long_value = splitFrontMatter("---\nk: " + std::string(kMaximumFrontMatterValueBytes + 1, 'v') + "\n---\n");
  check(long_value && long_value->entries.empty() && !long_value->error.empty(), "a value over its bound is an error");
  const auto long_key = splitFrontMatter("---\n" + std::string(kMaximumFrontMatterKeyBytes + 1, 'k') + ": v\n---\n");
  check(long_key && long_key->entries.empty() && contains(long_key->error, "a key is empty or too long (line 2)"),
        "a key over its bound is an error");
  check(splitFrontMatter("---\n- a\n- b\n---\n")->error == "front matter: a block mapping has no entries (line 2)",
        "a top-level list is an error, not body text");
  check(markdownBody("plain") == "plain", "a file without front matter is all body");

  // A pluggable resolver takes over relative targets (a static site maps
  // them itself); everything else is untouched.
  std::vector<std::pair<std::string, bool>> seen;
  LinkContext resolving{"example", std::string(40, 'a'), "docs"};
  resolving.resolver = [&seen](std::string_view target, bool image) -> std::optional<std::string> {
    seen.emplace_back(std::string(target), image);
    if (target == "missing.md") return std::nullopt;
    std::string upper(target);
    for (auto& byte : upper) byte = static_cast<char>(std::toupper(static_cast<unsigned char>(byte)));
    return upper;
  };
  const auto resolved = renderMarkdown(
      "[x](a/b.md#sec) ![i](img/p%20q.png \"t\") [m](missing.md) [h](https://example.test/x) [f](#only) "
      "[bad](a\\b) [q](a?b) [s](//x) ![g](#frag) <https://auto.test/> [e](mailto:a@example.test) [j](javascript:x)",
      resolving);
  check(contains(resolved, "href=\"A/B.MD#sec\""),
        "the resolver receives the decoded path without its fragment, which is appended to its answer");
  check(contains(resolved, "src=\"IMG/P Q.PNG\" title=\"t\" alt=\"i\""), "images reach the resolver decoded, flagged as images");
  check(contains(resolved, "[m](missing.md)") && !contains(resolved, "MISSING"), "nullopt from the resolver leaves the link as text");
  check(contains(resolved, "href=\"https://example.test/x\"") && contains(resolved, "href=\"#only\"") &&
            contains(resolved, "href=\"https://auto.test/\"") && contains(resolved, "href=\"mailto:a@example.test\""),
        "absolute URLs, pure fragments, autolinks and mail links bypass the resolver");
  check(contains(resolved, "[bad](a\\b)") && contains(resolved, "[q](a?b)") && contains(resolved, "[s](//x)") &&
            contains(resolved, "![g](#frag)") && contains(resolved, "[j](javascript:x)"),
        "unsafe targets are rejected before the resolver sees them");
  check(seen.size() == 3 && seen[0] == std::pair<std::string, bool>{"a/b.md", false} &&
            seen[1] == std::pair<std::string, bool>{"img/p q.png", true} && seen[2] == std::pair<std::string, bool>{"missing.md", false},
        "exactly the sanitised relative targets reach the resolver, in order");
  LinkContext standalone{};
  standalone.resolver = [](std::string_view target, bool) -> std::optional<std::string> { return std::string(target) + ".html"; };
  check(renderMarkdown("[g](guide.md#top)", standalone) == "<p><a href=\"guide.md.html#top\">g</a></p>\n",
        "a resolver needs no project or commit identity");
  check(contains(renderMarkdown("[g](guide.md)", ctx), "href=\"/project/example/blob/" + std::string(40, 'a') + ":guide.md\""),
        "without a resolver relative links are dashboard routes as before");
}
