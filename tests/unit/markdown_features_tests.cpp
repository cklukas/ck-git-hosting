// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ckgit/markdown.hpp"

namespace {

using ckgit::LinkContext;
using ckgit::MarkdownHeading;
using ckgit::MarkdownInclude;
using ckgit::renderMarkdown;

void check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error("Markdown features: " + std::string(message));
}

bool contains(std::string_view value, std::string_view part) { return value.find(part) != std::string_view::npos; }

std::size_t occurrences(std::string_view value, std::string_view part) {
  std::size_t count = 0;
  for (auto at = value.find(part); at != std::string_view::npos; at = value.find(part, at + part.size())) ++count;
  return count;
}

// A documentation context: links keep their target, problems are collected,
// and included files come from `files` (a path as the reader gets it, to its
// content), each part's directory derived from its path.
struct Fixture {
  std::map<std::string, std::string> files;
  std::vector<std::pair<std::size_t, std::string>> reports;
  std::vector<std::string> reads;
  LinkContext context;

  Fixture() {
    context.resolver = [](std::string_view target, bool, std::size_t) -> std::optional<std::string> { return std::string(target); };
    context.reporter = [this](const std::string& message, std::size_t line) { reports.emplace_back(line, message); };
    context.include_reader = [this](std::string_view target, bool markdown, std::size_t) -> std::optional<MarkdownInclude> {
      reads.push_back(std::string(markdown ? "md:" : "code:") + std::string(target));
      // A leading `/` is the repository root, which `files` is keyed from.
      const auto found = files.find(std::string(target.starts_with('/') ? target.substr(1) : target));
      if (found == files.end()) return std::nullopt;
      const auto slash = found->first.rfind('/');
      return MarkdownInclude{found->second, slash == std::string::npos ? std::string() : found->first.substr(0, slash)};
    };
  }

  std::string render(const std::string& source, std::vector<MarkdownHeading>* outline = nullptr) {
    reports.clear();
    reads.clear();
    return renderMarkdown(source, context, outline);
  }

  bool reported(std::string_view part) const {
    for (const auto& [line, message] : reports) {
      if (contains(message, part)) return true;
    }
    return false;
  }
};

void testCodeBlocks() {
  Fixture fixture;
  check(fixture.render("```\nx\n```\n") == "<pre><code>x\n</code></pre>\n", "a plain fence is unchanged");
  check(fixture.render("```c++\nint x;\n```\n") ==
            "<pre><code class=\"language-c++\"><span class=\"hl-k\">int</span> x;\n</code></pre>\n",
        "a language name may contain + and #");
  const auto titled = fixture.render("```cpp title=\"my file.cpp\" {2} :line-numbers\nint a;\nint b;\n```\n");
  check(contains(titled, "<div class=\"code-block\"><div class=\"code-title\">my file.cpp</div>"
                         "<pre class=\"code-lines line-numbers has-highlighted\"><code class=\"language-cpp\">"),
        "a quoted title, highlighted lines and line numbers");
  check(contains(titled, "<span class=\"line\"><span class=\"line-number\" aria-hidden=\"true\">1</span>") &&
            contains(titled, "<span class=\"line highlighted\"><span class=\"line-number\" aria-hidden=\"true\">2</span>"),
        "each line is a span; the second is highlighted");
  check(contains(fixture.render("```sh [install.sh]\nls\n```\n"), "<div class=\"code-title\">install.sh</div>"),
        "a bracketed label is a title");
  check(contains(fixture.render("```ts{1}\nlet a;\n```\n"), "<code class=\"language-ts\"><span class=\"line highlighted\">"),
        "line ranges may follow the language directly");
  check(contains(fixture.render("```js showLineNumbers=10\na\nb\n```\n"), "aria-hidden=\"true\">11</span>b</span>"),
        "Docusaurus's showLineNumbers=N numbers from N");
  check(contains(fixture.render("```js:line-numbers=5\na\n```\n"), "aria-hidden=\"true\">5</span>a</span>"),
        "VitePress's :line-numbers=N numbers from N");
  fixture.context.line_numbers = true;
  check(contains(fixture.render("```\na\n```\n"), "line-numbers") && !contains(fixture.render("```js :no-line-numbers\na\n```\n"), "line-number"),
        "the context may number every block, and a block may opt out");
  fixture.context.line_numbers = false;

  const auto marked = fixture.render(
      "```text\nkeep(); // [!code ++]\ndrop(); // [!code --]\nlook(); // [!code focus]\nwarn(); // [!code warning]\nfail(); // [!code error]\n"
      "x = 1; // [!code highlight:2]\ny = 2;\n```\n");
  check(contains(marked, "<pre class=\"code-lines has-highlighted has-diff has-focused\">"), "the block's classes say what it holds");
  check(contains(marked, "<span class=\"line diff add\">keep();</span>") && contains(marked, "<span class=\"line diff remove\">drop();</span>") &&
            contains(marked, "<span class=\"line focused\">look();</span>") && contains(marked, "<span class=\"line warning\">warn();</span>") &&
            contains(marked, "<span class=\"line error\">fail();</span>") && contains(marked, "<span class=\"line highlighted\">x = 1;</span>") &&
            contains(marked, "<span class=\"line highlighted\">y = 2;</span>"),
        "[!code …] markers mark their line (highlight:2 two lines) and lose their comment");
  check(contains(fixture.render("```text\nx = 1  # [!code ++]\n```\n"), "<span class=\"line diff add\">x = 1</span>") &&
            contains(fixture.render("```text\n<p></p> <!-- [!code --] -->\n```\n"), "<span class=\"line diff remove\">&lt;p&gt;&lt;/p&gt;</span>"),
        "markers work in #- and <!-- -->-comments too");
  check(contains(fixture.render("```text\nx(); // [!code nope]\n```\n"), "[!code nope]"), "an unknown marker stays code");
  const auto magic = fixture.render("```text\n// highlight-next-line\na();\nb();\n// highlight-start\nc();\nd();\n// highlight-end\ne();\n```\n");
  check(!contains(magic, "highlight-") && contains(magic, "<span class=\"line highlighted\">a();</span>") &&
            contains(magic, "<span class=\"line\">b();</span>") && contains(magic, "<span class=\"line highlighted\">c();</span>") &&
            contains(magic, "<span class=\"line highlighted\">d();</span>") && contains(magic, "<span class=\"line\">e();</span>"),
        "Docusaurus's highlight comments mark lines and disappear");
  check(!contains(fixture.render("```js {3-1}\na\n```\n"), "highlighted") && !contains(fixture.render("```js {x}\na\n```\n"), "highlighted"),
        "invalid ranges are ignored");
  check(fixture.render("~~~evil\"attr\n&x\n~~~\n") == "<pre><code>&amp;x\n</code></pre>\n", "an unsafe language word names no language");
}

void testContainers() {
  Fixture fixture;
  struct Kind { const char* name; const char* title; };
  for (const Kind& kind : {Kind{"info", "Info"}, Kind{"note", "Note"}, Kind{"tip", "Tip"}, Kind{"important", "Important"},
                           Kind{"warning", "Warning"}, Kind{"caution", "Caution"}, Kind{"danger", "Danger"}}) {
    check(fixture.render(std::string("::: ") + kind.name + "\nBody *text*\n:::\n") ==
              std::string("<div class=\"alert alert-") + kind.name + "\"><p class=\"alert-title\">" + kind.title +
                  "</p>\n<p>Body <em>text</em></p>\n</div>\n",
          std::string("container ") + kind.name);
  }
  check(contains(fixture.render("::: warning Mind **this**\nx\n:::\n"), "<p class=\"alert-title\">Mind <strong>this</strong></p>"),
        "a title after the kind, with inline markup");
  check(contains(fixture.render(":::tip[Pro tip]\nx\n:::\n"), "<div class=\"alert alert-tip\"><p class=\"alert-title\">Pro tip</p>"),
        "Docusaurus's bracketed title");
  check(fixture.render("::: details Show me {open}\nHidden\n:::\n") ==
            "<details class=\"details-container\" open><summary>Show me</summary>\n<p>Hidden</p>\n</details>\n",
        "details with a title, open");
  check(contains(fixture.render("::: details\nx\n:::\n"), "<summary>Details</summary>"), "details without a title");
  const auto nested = fixture.render(":::: warning\nOuter\n::: tip\nInner\n:::\nAfter\n::::\nOutside\n");
  check(nested == "<div class=\"alert alert-warning\"><p class=\"alert-title\">Warning</p>\n<p>Outer</p>\n"
                  "<div class=\"alert alert-tip\"><p class=\"alert-title\">Tip</p>\n<p>Inner</p>\n</div>\n<p>After</p>\n</div>\n"
                  "<p>Outside</p>\n",
        "containers nest");
  check(contains(fixture.render("::: tip\nA\n```md\n:::\n```\nB\n:::\n"), "<pre><code class=\"language-md\">:::\n</code></pre>\n<p>B</p>"),
        "a ::: line inside fenced code closes nothing");
  check(fixture.render("::: unknown\nx\n:::\n") == "<p>::: unknown\nx</p>\n<p>:::</p>\n", "an unknown kind is text");
  check(fixture.render("::: tip\nNever closed\n") == "<div class=\"alert alert-tip\"><p class=\"alert-title\">Tip</p>\n<p>Never closed</p>\n</div>\n",
        "an unclosed container runs to the end");
  check(fixture.render("Text\n::: tip\nx\n:::\n") == "<p>Text</p>\n<div class=\"alert alert-tip\"><p class=\"alert-title\">Tip</p>\n<p>x</p>\n</div>\n",
        "a container interrupts a paragraph");
  fixture.context.labels.tip = "Tipp";
  fixture.context.labels.note = "Hinweis";
  check(contains(fixture.render("::: tip\nx\n:::\n"), ">Tipp</p>") && contains(fixture.render("> [!NOTE]\n> x\n"), ">Hinweis</p>"),
        "default titles come from the context's labels, for alerts too");
}

void testTabs() {
  Fixture fixture;
  const auto group = fixture.render("::: code-group\n```sh [npm]\nnpm i\n```\n\n```sh\nyarn\n```\n```\nplain\n```\n:::\n");
  check(group == "<div class=\"tabbed code-group\">"
                 "<input type=\"radio\" class=\"tab-input\" name=\"tabs:1\" id=\"tabs:1:1\" checked><label class=\"tab-label\" for=\"tabs:1:1\">npm</label>"
                 "<input type=\"radio\" class=\"tab-input\" name=\"tabs:1\" id=\"tabs:1:2\"><label class=\"tab-label\" for=\"tabs:1:2\">sh</label>"
                 "<input type=\"radio\" class=\"tab-input\" name=\"tabs:1\" id=\"tabs:1:3\"><label class=\"tab-label\" for=\"tabs:1:3\">3</label>"
                 "<div class=\"tab-panels\"><div class=\"tab-panel\"><pre><code class=\"language-sh\">npm i\n</code></pre>\n</div>"
                 "<div class=\"tab-panel\"><pre><code class=\"language-sh\">yarn\n</code></pre>\n</div>"
                 "<div class=\"tab-panel\"><pre><code>plain\n</code></pre>\n</div></div></div>\n",
        "a code group: a radio tab per fence, labelled by its label, else its language, else its number");
  const auto tabs = fixture.render("::: tabs\n== One\nFirst *body*\n== Two\n::: tabs\n== Inner\nx\n:::\n:::\n");
  check(occurrences(tabs, "class=\"tabbed tab-set\"") == 2 && contains(tabs, "for=\"tabs:1:1\">One</label>") &&
            contains(tabs, "for=\"tabs:1:2\">Two</label>") && contains(tabs, "for=\"tabs:2:1\">Inner</label>") &&
            !contains(tabs, "tabs:1:3") && contains(tabs, "<p>First <em>body</em></p>"),
        "tab sets nest, and an inner set's == lines stay its own");
  std::string many = "::: tabs\n";
  for (int index = 0; index < 17; ++index) many += "== T" + std::to_string(index) + "\nx\n";
  const auto sequential = fixture.render(many + ":::\n");
  check(!contains(sequential, "tab-input") && occurrences(sequential, "<p class=\"tab-label\">") == 17 && fixture.reported("more than 16 tabs"),
        "more tabs than the stylesheet switches are shown one after another, reported");
}

void testFootnotes() {
  Fixture fixture;
  const auto notes = fixture.render("A[^b] and B[^A] and again[^b].\n\n[^a]: First *note*.\n[^b]: Second\n    continued.\n\n    More.\n[^unused]: Never shown.\n");
  check(contains(notes, "<p>A<sup class=\"footnote-ref\"><a href=\"#fn:1\" id=\"fnref:1\">1</a></sup> and B"
                        "<sup class=\"footnote-ref\"><a href=\"#fn:2\" id=\"fnref:2\">2</a></sup> and again"
                        "<sup class=\"footnote-ref\"><a href=\"#fn:1\" id=\"fnref:1:2\">1</a></sup>.</p>"),
        "references number by first use; labels ignore case; a repeat gets its own id");
  check(contains(notes, "<section class=\"footnotes\" aria-label=\"Footnotes\">\n<ol>\n<li id=\"fn:1\">\n<p>Second\ncontinued.</p>\n"
                        "<p>More. <a href=\"#fnref:1\" class=\"footnote-backref\" aria-label=\"Back to reference 1\">\xe2\x86\xa9</a> "
                        "<a href=\"#fnref:1:2\" class=\"footnote-backref\" aria-label=\"Back to reference 1-2\">\xe2\x86\xa9</a></p>\n</li>\n"
                        "<li id=\"fn:2\">\n<p>First <em>note</em>. <a href=\"#fnref:2\""),
        "the footnotes section, in reference order, with indented paragraphs and links back");
  check(!contains(notes, "Never shown") && !contains(notes, "[^a]:"), "an unreferenced definition is not shown; definitions are not text");
  check(fixture.render("Undefined[^x].\n") == "<p>Undefined[^x].</p>\n", "a reference to no definition is text");
  check(contains(fixture.render("A[^1].\n\n[^1]: See[^2].\n[^2]: Inner.\n"), "<li id=\"fn:2\">"), "a footnote may reference another");
  check(!contains(fixture.render("`[^1]`\n\n[^1]: x\n"), "footnote-ref"), "code spans keep [^1] literal");
}

void testDefinitionLists() {
  Fixture fixture;
  check(fixture.render("Term\n: One\n: Two\n") == "<dl>\n<dt>Term</dt>\n<dd>One\n</dd>\n<dd>Two\n</dd>\n</dl>\n",
        "a term with two tight definitions");
  check(fixture.render("A\nB\n\n: Shared, *loose*\n  continued\n") ==
            "<dl>\n<dt>A</dt>\n<dt>B</dt>\n<dd><p>Shared, <em>loose</em>\ncontinued</p>\n</dd>\n</dl>\n",
        "several terms, a blank line before the definition makes it loose; indented lines continue it");
  check(fixture.render("One\n: x\n\nTwo\n: y\n") == "<dl>\n<dt>One</dt>\n<dd>x\n</dd>\n<dt>Two</dt>\n<dd>y\n</dd>\n</dl>\n",
        "consecutive groups form one list");
  check(contains(fixture.render("Term\n: - a\n  - b\n"), "<dd><ul>\n<li>a\n</li>\n<li>b\n</li>\n</ul>\n</dd>"), "a definition holds blocks");
  check(fixture.render(": no term\n") == "<p>: no term</p>\n", "a definition marker without a term is text");
}

void testEmoji() {
  Fixture fixture;
  check(fixture.render(":tada: :+1: :flag_de: :nope: a:smile: `:smile:`\n") ==
            "<p>\xf0\x9f\x8e\x89 \xf0\x9f\x91\x8d \xf0\x9f\x87\xa9\xf0\x9f\x87\xaa :nope: a:smile: <code>:smile:</code></p>\n",
        "shortcodes become emoji; unknown ones, ones glued to a word, and code stay text");
  check(fixture.render("10:30:45\n") == "<p>10:30:45</p>\n", "times are not shortcodes");
}

void testMath() {
  Fixture fixture;
  const auto inline_math = fixture.render("So $x^2$ costs $5 and $10, \\$y\\$ too.\n");
  check(contains(inline_math, "So <math><semantics><mrow><msup><mi>x</mi><mn>2</mn></msup></mrow>"
                              "<annotation encoding=\"application/x-tex\">x^2</annotation></semantics></math> costs $5 and $10, $y$ too."),
        "inline math; dollar amounts and escaped dollars stay text");
  check(contains(fixture.render("$$\na + b\n$$\n"), "<math display=\"block\">") && contains(fixture.render("$$ x $$\n"), "<math display=\"block\">") &&
            contains(fixture.render("```math\ny\n```\n"), "<math display=\"block\">") && contains(fixture.render("In $$z$$ text\n"), "<math display=\"block\">"),
        "display math: a $$ block, a one-line $$ … $$, a math fence, and $$ inline");
  check(fixture.render("$$\nnever closed\n\nnext\n") == "<p>$$\nnever closed</p>\n<p>next</p>\n", "an unclosed $$ is text");
  const auto mixed = fixture.render("Pay $5 and $y$ more.\n");
  check(contains(mixed, "<p>Pay $5 and <math><semantics><mrow><mi>y</mi></mrow>") && contains(mixed, "</math> more.</p>"),
        "a formula holds no unescaped $: an amount before a formula stays text");
  const auto two = fixture.render("$$a$$ and $$b$$\n");
  check(two.starts_with("<p><math display=\"block\">") && contains(two, "</math> and <math display=\"block\">") && fixture.reports.empty(),
        "a line with two $$ formulas is a paragraph holding both");
  std::string dollars;
  for (int index = 0; index < 20000; ++index) dollars += "$a ";
  check(contains(fixture.render(dollars + "\n"), "$a $a"), "thousands of unmatched dollar signs stay text, within the work bound");
  const auto failed = fixture.render("Line\n\n$\\nosuch$\n");
  check(contains(failed, "<merror>") && fixture.reports.size() == 1 && fixture.reports[0].first == 3 &&
            contains(fixture.reports[0].second, "math: unknown command \\nosuch"),
        "a formula that does not parse is marked and reported at its line");
}

void testHeadingIds() {
  Fixture fixture;
  std::vector<MarkdownHeading> outline;
  fixture.render("# Launch :rocket: now\n## The $\\alpha$ part\n## Notes[^n]\n\n[^n]: x\n", &outline);
  check(outline.size() == 3 && outline[0].id == "launch-now" && outline[1].id == "the-α-part" && outline[1].text == "The α part" &&
            outline[2].id == "notes" && outline[2].text == "Notes",
        "an id leaves out emoji, a formula's source, and footnote numbers");
}

void testIncludes() {
  Fixture fixture;
  fixture.files["parts/a.md"] = "---\ntitle: x\n---\n## Part heading\n\nSee [img](pic.png) and [root](/r.md).\n\n"
                                "<!-- #region usage -->\nRegion **only**.\n<!-- #endregion usage -->\n";
  fixture.files["parts/loop.md"] = "Loop\n<!--@include: loop.md-->\n";
  fixture.files["parts/nest.md"] = "<!--@include: a.md#usage-->\n";
  fixture.files["src/app.cpp"] = "int main() {\n  // #region body\n  int a = 1;\n  return a;\n  // #endregion body\n}\n";
  std::vector<MarkdownHeading> outline;
  const auto whole = fixture.render("# Doc\n\n<!--@include: parts/a.md-->\n", &outline);
  check(contains(whole, "<h2 id=\"part-heading\">Part heading</h2>") && contains(whole, "<a href=\"parts/pic.png\">img</a>") &&
            contains(whole, "<a href=\"/r.md\">root</a>") && !contains(whole, "title: x") && outline.size() == 2,
        "a part is spliced in without its front matter; its relative links resolve from its own directory");
  check(fixture.render("<!--@include: parts/a.md#usage-->\n") == "<p>Region <strong>only</strong>.</p>\n", "a #region of a part");
  check(fixture.render("<!--@include: parts/a.md{1,1}-->\n") == "<h2 id=\"part-heading\">Part heading</h2>\n",
        "a {from,to} line range, counted in the part's body after its front matter");
  check(fixture.render("<!--@include: parts/nest.md-->\n") == "<p>Region <strong>only</strong>.</p>\n" &&
            fixture.reads == std::vector<std::string>{"md:parts/nest.md", "md:parts/a.md"},
        "a part's own include is relative to it");
  const auto loop = fixture.render("<!--@include: parts/loop.md-->\n");
  check(occurrences(loop, "Loop") == 1 && contains(loop, "&lt;!--@include: loop.md--&gt;") && fixture.reported("would include itself"),
        "an include cycle stops, stays visible and is reported");
  check(fixture.render("<!--@include: none.md-->\n") == "<p>&lt;!--@include: none.md--&gt;</p>\n", "a part that cannot be read stays visible");
  check(contains(fixture.render("<!--@include: parts/a.md#missing-->\n"), "&lt;!--@include") && fixture.reported("has no region 'missing'"),
        "a missing region is reported");
  check(contains(fixture.render("```md\n<!--@include: parts/a.md-->\n```\n"), "&lt;!--@include: parts/a.md--&gt;"),
        "an include inside fenced code is code");

  const auto snippet = fixture.render("<<< src/app.cpp#body{2 cpp:line-numbers} [Body]\n");
  check(contains(snippet, "<div class=\"code-title\">Body</div><pre class=\"code-lines line-numbers has-highlighted\"><code class=\"language-cpp\">") &&
            contains(snippet, "aria-hidden=\"true\">1</span><span class=\"hl-k\">int</span> a = <span class=\"hl-n\">1</span>;</span>") &&
            fixture.reads == std::vector<std::string>{"code:src/app.cpp"} &&
            contains(snippet, "<span class=\"line highlighted\"><span class=\"line-number\" aria-hidden=\"true\">2</span>") &&
            !contains(snippet, "#region"),
        "a snippet's region, dedented, with highlighted line, language, numbers and title");
  const auto lines = fixture.render("<<< @/src/app.cpp#L1-L1\n");
  check(lines.starts_with("<pre><code class=\"language-cpp\">") && contains(lines, "main") && occurrences(lines, "\n") == 2 &&
            fixture.reads == std::vector<std::string>{"code:/src/app.cpp"},
        "@/ is the root, and #L1-L1 a line range");
  check(fixture.render("<<< src/app.cpp#nope\n") == "<p>&lt;&lt;&lt; src/app.cpp#nope</p>\n" && fixture.reported("has no region 'nope'"),
        "a snippet that cannot be made stays visible and is reported");
  const auto group = fixture.render("::: code-group\n<<< src/app.cpp#body [Region]\n```sh [Shell]\nx\n```\n:::\n");
  check(contains(group, ">Region</label>") && contains(group, ">Shell</label>") && !contains(group, "code-title"),
        "a snippet is a code group's tab, labelled by its title");

  LinkContext plain;
  plain.resolver = fixture.context.resolver;
  check(renderMarkdown("<<< src/app.cpp\n\n<!--@include: parts/a.md-->\n", plain) == "<p>&lt;&lt;&lt; src/app.cpp</p>\n",
        "without a reader a snippet is text and an include an ordinary comment");
}

}  // namespace

void testMarkdownFeatures() {
  testCodeBlocks();
  testContainers();
  testTabs();
  testFootnotes();
  testDefinitionLists();
  testEmoji();
  testMath();
  testHeadingIds();
  testIncludes();
}
