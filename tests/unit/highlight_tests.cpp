// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/highlight.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using ckgit::Language;
using ckgit::TokenKind;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("highlight: " + message);
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

void testDetection() {
  require(ckgit::languageForName("cpp") == Language::Cpp && ckgit::languageForName("C++") == Language::Cpp &&
              ckgit::languageForName("h") == Language::Cpp && ckgit::languageForName("py") == Language::Python &&
              ckgit::languageForName("bash") == Language::Shell && ckgit::languageForName("console") == Language::Shell &&
              ckgit::languageForName("yml") == Language::Yaml && ckgit::languageForName("json") == Language::Json,
          "fence info strings name languages, case-insensitively");
  require(ckgit::languageForName("rust") == Language::None && ckgit::languageForName("") == Language::None &&
              ckgit::languageForName(std::string(40, 'c')) == Language::None,
          "unknown or oversize info strings are no language");
  require(ckgit::languageForPath("src/a.cpp") == Language::Cpp && ckgit::languageForPath("x.H") == Language::Cpp &&
              ckgit::languageForPath("setup_doctor.inc") == Language::Cpp && ckgit::languageForPath("t.py") == Language::Python &&
              ckgit::languageForPath("run.sh") == Language::Shell && ckgit::languageForPath(".ckgit/ci.yml") == Language::Yaml &&
              ckgit::languageForPath("p.json") == Language::Json,
          "extensions name languages");
  require(ckgit::languageForPath("README.md") == Language::None && ckgit::languageForPath("Makefile") == Language::None &&
              ckgit::languageForPath(".bashrc") == Language::None && ckgit::languageForPath("dir.py/file") == Language::None,
          "other names, dotfiles and directories are no language");
  require(ckgit::detectLanguage("deploy", "#!/bin/sh\necho") == Language::Shell &&
              ckgit::detectLanguage("tool", "#!/usr/bin/env python3\n") == Language::Python &&
              ckgit::detectLanguage("tool", "#!/usr/bin/env -S bash -e\n") == Language::None &&
              ckgit::detectLanguage("tool", "plain") == Language::None &&
              ckgit::detectLanguage("a.py", "#!/bin/sh\n") == Language::Python,
          "a shebang names the language when the extension does not");
}

void testCpp() {
  const auto html = ckgit::highlightHtml(
      "#include <x>\nint main() { // hi\n  return 0x1F; /* multi\nline */ const char* s = \"a\\\"b\"; }\n", Language::Cpp);
  require(html ==
              "<span class=\"hl-p\">#include &lt;x&gt;</span>\n"
              "<span class=\"hl-k\">int</span> main() { <span class=\"hl-c\">// hi</span>\n"
              "  <span class=\"hl-k\">return</span> <span class=\"hl-n\">0x1F</span>; <span class=\"hl-c\">/* multi</span>\n"
              "<span class=\"hl-c\">line */</span> <span class=\"hl-k\">const</span> <span class=\"hl-k\">char</span>* s = "
              "<span class=\"hl-s\">&quot;a\\&quot;b&quot;</span>; }\n",
          "C++ directives, keywords, numbers, line and block comments, strings; spans split at newlines");
  require(ckgit::highlightHtml("R\"x(a\"b)x\" 'c' 1'000 u8\"s\" L'x' a'b", Language::Cpp) ==
              "<span class=\"hl-s\">R&quot;x(a&quot;b)x&quot;</span> <span class=\"hl-s\">&#39;c&#39;</span> "
              "<span class=\"hl-n\">1&#39;000</span> <span class=\"hl-s\">u8&quot;s&quot;</span> "
              "<span class=\"hl-s\">L&#39;x&#39;</span> a&#39;b",
          "raw strings, character literals, digit separators, prefixed strings, and a stray quote");
  require(ckgit::highlightHtml("#define X 1 // c\n#define Y \\\n  2\nx", Language::Cpp) ==
              "<span class=\"hl-p\">#define X 1 </span><span class=\"hl-c\">// c</span>\n"
              "<span class=\"hl-p\">#define Y \\</span>\n<span class=\"hl-p\">  2</span>\nx",
          "a directive ends at a comment and continues over a trailing backslash");
  require(ckgit::highlightHtml("a.if_x #no", Language::Cpp) == "a.if_x #no", "identifiers containing keywords and a mid-line # are plain");
  const auto tokens = ckgit::highlightTokens("int x; // c", Language::Cpp);
  require(tokens.size() == 2 && tokens[0].begin == 0 && tokens[0].end == 3 && tokens[0].kind == TokenKind::Keyword &&
              tokens[1].begin == 7 && tokens[1].end == 11 && tokens[1].kind == TokenKind::Comment,
          "tokens carry byte ranges and kinds in order");
}

void testPython() {
  require(ckgit::highlightHtml("@dataclass\ndef f(x):  # c\n    return f\"{x}\" + '''a\nb''' + 1.5e3\n", Language::Python) ==
              "<span class=\"hl-a\">@dataclass</span>\n"
              "<span class=\"hl-k\">def</span> f(x):  <span class=\"hl-c\"># c</span>\n"
              "    <span class=\"hl-k\">return</span> <span class=\"hl-s\">f&quot;{x}&quot;</span> + "
              "<span class=\"hl-s\">&#39;&#39;&#39;a</span>\n<span class=\"hl-s\">b&#39;&#39;&#39;</span> + "
              "<span class=\"hl-n\">1.5e3</span>\n",
          "Python decorators, keywords, comments, prefixed and triple-quoted strings, numbers");
  require(ckgit::highlightHtml("rb'x' Fr\"y\" xr'z' 'open\nnext", Language::Python) ==
              "<span class=\"hl-s\">rb&#39;x&#39;</span> <span class=\"hl-s\">Fr&quot;y&quot;</span> xr<span class=\"hl-s\">&#39;z&#39;</span> "
              "<span class=\"hl-s\">&#39;open</span>\nnext",
          "two-letter prefixes in any case; other identifiers before a quote are not prefixes; an open string ends at the line");
}

void testShell() {
  require(ckgit::highlightHtml(
              "#!/bin/sh\nif [ \"$1\" = x ]; then echo ${HOME} $? # done\nfi\ncat <<EOF\n$body: y\nEOF\necho finished\n",
              Language::Shell) ==
              "<span class=\"hl-c\">#!/bin/sh</span>\n"
              "<span class=\"hl-k\">if</span> [ <span class=\"hl-s\">&quot;$1&quot;</span> = x ]; <span class=\"hl-k\">then</span> echo "
              "<span class=\"hl-v\">${HOME}</span> <span class=\"hl-v\">$?</span> <span class=\"hl-c\"># done</span>\n"
              "<span class=\"hl-k\">fi</span>\ncat &lt;&lt;EOF\n<span class=\"hl-s\">$body: y</span>\nEOF\necho finished\n",
          "shell comments, keywords at word starts, strings, variables, and a here-document body");
  require(ckgit::highlightHtml("cat <<-'X'\n\tline\n\tX\n$# 'a\nb' --set in/if exit=1 in", Language::Shell) ==
              "cat &lt;&lt;-&#39;X&#39;\n<span class=\"hl-s\">\tline</span>\n\tX\n"
              "<span class=\"hl-v\">$#</span> <span class=\"hl-s\">&#39;a</span>\n<span class=\"hl-s\">b&#39;</span> --set in/if exit=1 "
              "<span class=\"hl-k\">in</span>",
          "a tab-stripping quoted delimiter, $# is not a comment, single quotes span lines, keywords are whole words");
}

void testYaml() {
  require(ckgit::highlightHtml(
              "---\n# c\nname: \"x\" # t\nlist:\n  - a: 1\n  - true\nrun: |\n  echo a: b\n\nnext: ~\n", Language::Yaml) ==
              "<span class=\"hl-k\">---</span>\n<span class=\"hl-c\"># c</span>\n"
              "<span class=\"hl-a\">name</span>: <span class=\"hl-s\">&quot;x&quot;</span> <span class=\"hl-c\"># t</span>\n"
              "<span class=\"hl-a\">list</span>:\n  - <span class=\"hl-a\">a</span>: <span class=\"hl-n\">1</span>\n"
              "  - <span class=\"hl-k\">true</span>\n<span class=\"hl-a\">run</span>: <span class=\"hl-k\">|</span>\n"
              "  echo a: b\n\n<span class=\"hl-a\">next</span>: <span class=\"hl-k\">~</span>\n",
          "YAML document marker, comments, keys, strings, numbers, booleans, and an unhighlighted block scalar");
  require(ckgit::highlightHtml("on: { branches: [main, 'x y'], tags: [\"v*\"] }\nurl: http://h/p:1\nkey with space: -2.5e3\nref: &a x\nother: *a\n",
                               Language::Yaml) ==
              "<span class=\"hl-a\">on</span>: { <span class=\"hl-a\">branches</span>: [main, <span class=\"hl-s\">&#39;x y&#39;</span>], "
              "<span class=\"hl-a\">tags</span>: [<span class=\"hl-s\">&quot;v*&quot;</span>] }\n"
              "<span class=\"hl-a\">url</span>: http://h/p:1\n<span class=\"hl-a\">key with space</span>: <span class=\"hl-n\">-2.5e3</span>\n"
              "<span class=\"hl-a\">ref</span>: <span class=\"hl-v\">&amp;a</span> x\n<span class=\"hl-a\">other</span>: <span class=\"hl-v\">*a</span>\n",
          "flow collections, a colon inside a URL value, keys with spaces, negative exponents, anchors and aliases");
  require(ckgit::highlightHtml("steps:\n  - run: |\n      make: all\n    name: x\n", Language::Yaml) ==
              "<span class=\"hl-a\">steps</span>:\n  - <span class=\"hl-a\">run</span>: <span class=\"hl-k\">|</span>\n"
              "      make: all\n    <span class=\"hl-a\">name</span>: x\n",
          "a block scalar under a sequence item ends at the next sibling key");
}

void testJson() {
  require(ckgit::highlightHtml("{\"a\": [1, true, null, \"s\\\\n\"], \"b\":\n -2.5}", Language::Json) ==
              "{<span class=\"hl-a\">&quot;a&quot;</span>: [<span class=\"hl-n\">1</span>, <span class=\"hl-k\">true</span>, "
              "<span class=\"hl-k\">null</span>, <span class=\"hl-s\">&quot;s\\\\n&quot;</span>], <span class=\"hl-a\">&quot;b&quot;</span>:\n "
              "<span class=\"hl-n\">-2.5</span>}",
          "JSON keys, strings with escapes, numbers, literals");
}

void testBounds() {
  require(ckgit::highlightHtml("<a>&'\"\x01", Language::None) == "&lt;a&gt;&amp;&#39;&quot;\xef\xbf\xbd",
          "no language means plain escaped text with control bytes replaced");
  require(ckgit::highlightTokens("int", Language::None).empty(), "no language yields no tokens");
  const std::string large(ckgit::kMaximumHighlightBytes + 1, 'x');
  require(!contains(ckgit::highlightHtml(large, Language::Cpp), "<span") && ckgit::highlightTokens(large, Language::Cpp).empty(),
          "text over the size bound renders plain");
  std::string dense;
  for (int index = 0; index < 4000; ++index) dense += "1;";
  require(ckgit::highlightHtml(dense, Language::Cpp) == dense, "markup that would grow the text eightfold is dropped");
  require(ckgit::highlightHtml("\"open\n# c", Language::Cpp) == "<span class=\"hl-s\">&quot;open</span>\n<span class=\"hl-p\"># c</span>",
          "an open string ends at its line and does not swallow the rest");
  for (const auto language : {Language::Cpp, Language::Python, Language::Shell, Language::Yaml, Language::Json}) {
    const auto tokens = ckgit::highlightTokens("\"a\n'b\n<<\n$\n#\n/*\nR\"(\n", language);
    std::size_t last = 0;
    for (const auto& token : tokens) {
      require(token.begin >= last && token.end > token.begin && token.end <= 22, "tokens are ordered, non-empty, in range");
      last = token.end;
    }
  }
}

}  // namespace

void testHighlight() {
  testDetection();
  testCpp();
  testPython();
  testShell();
  testYaml();
  testJson();
  testBounds();
}
