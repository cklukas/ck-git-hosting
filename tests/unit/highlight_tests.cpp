// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/highlight.hpp"

#include <chrono>
#include <set>
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

// Exact markup for a snippet; the message names the language and shows both
// sides when they differ.
void expectHtml(Language language, std::string_view source, std::string_view html, const std::string& what) {
  const auto actual = ckgit::highlightHtml(source, language);
  require(actual == html, std::string(ckgit::languageName(language)) + ": " + what + "\nexpected: " + std::string(html) +
                              "\nactual:   " + actual);
}

void testDetection() {
  require(ckgit::languageForName("cpp") == Language::Cpp && ckgit::languageForName("C++") == Language::Cpp &&
              ckgit::languageForName("h") == Language::Cpp && ckgit::languageForName("py") == Language::Python &&
              ckgit::languageForName("bash") == Language::Shell && ckgit::languageForName("console") == Language::Shell &&
              ckgit::languageForName("yml") == Language::Yaml && ckgit::languageForName("json") == Language::Json,
          "fence info strings name languages, case-insensitively");
  require(ckgit::languageForName("cobol") == Language::None && ckgit::languageForName("") == Language::None &&
              ckgit::languageForName(std::string(40, 'c')) == Language::None,
          "unknown or oversize info strings are no language");
  require(ckgit::languageForPath("src/a.cpp") == Language::Cpp && ckgit::languageForPath("x.H") == Language::Cpp &&
              ckgit::languageForPath("setup_doctor.inc") == Language::Cpp && ckgit::languageForPath("t.py") == Language::Python &&
              ckgit::languageForPath("run.sh") == Language::Shell && ckgit::languageForPath(".ckgit/ci.yml") == Language::Yaml &&
              ckgit::languageForPath("p.json") == Language::Json,
          "extensions name languages");
  require(ckgit::languageForPath("notes.txt") == Language::None && ckgit::languageForPath(".bashrc") == Language::None &&
              ckgit::languageForPath(".gitignore") == Language::None && ckgit::languageForPath("dir.py/file") == Language::None &&
              ckgit::languageForPath("LICENSE") == Language::None,
          "plain text, dotfiles, unknown names and directories are no language");
  require(ckgit::detectLanguage("deploy", "#!/bin/sh\necho") == Language::Shell &&
              ckgit::detectLanguage("tool", "#!/usr/bin/env python3\n") == Language::Python &&
              ckgit::detectLanguage("tool", "#!/usr/bin/env -S bash -e\n") == Language::Shell &&
              ckgit::detectLanguage("tool", "plain") == Language::None &&
              ckgit::detectLanguage("a.py", "#!/bin/sh\n") == Language::Python,
          "a shebang names the language when the extension does not, also behind env -S");
}

void testNames() {
  const std::pair<std::string_view, Language> names[] = {
      {"c", Language::C}, {"c#", Language::CSharp}, {"CS", Language::CSharp}, {"csharp", Language::CSharp},
      {"f#", Language::FSharp}, {"fs", Language::FSharp}, {"objc", Language::ObjectiveC},
      {"objectivec", Language::ObjectiveC}, {"js", Language::JavaScript}, {"jsx", Language::JavaScript},
      {"ts", Language::TypeScript}, {"tsx", Language::TypeScript}, {"golang", Language::Go}, {"rs", Language::Rust},
      {"kt", Language::Kotlin}, {"rb", Language::Ruby}, {"pl", Language::Perl}, {"ps1", Language::PowerShell},
      {"pwsh", Language::PowerShell}, {"bat", Language::Batch}, {"cmd", Language::Batch}, {"md", Language::Markdown},
      {"htm", Language::Html}, {"xhtml", Language::Html}, {"svg", Language::Html}, {"vue", Language::Html},
      {"svelte", Language::Html}, {"docker", Language::Dockerfile}, {"make", Language::Makefile},
      {"mk", Language::Makefile}, {"tex", Language::Latex}, {"proto", Language::Protobuf}, {"tf", Language::Hcl},
      {"hcl", Language::Hcl}, {"gql", Language::GraphQL}, {"patch", Language::Diff}, {"toml", Language::Toml},
      {"ini", Language::Ini}, {"cfg", Language::Ini}, {"properties", Language::Ini}, {"conf", Language::Ini},
      {"postgres", Language::Sql}, {"psql", Language::Sql}, {"postgresql", Language::Sql}, {"mysql", Language::Sql},
      {"sqlite", Language::Sql}, {"plsql", Language::Sql}, {"tsql", Language::Sql}, {"vbnet", Language::VisualBasic},
      {"delphi", Language::Pascal}, {"octave", Language::Matlab}, {"systemverilog", Language::Verilog},
      {"racket", Language::Scheme}, {"gradle", Language::Groovy}, {"nasm", Language::Assembly},
      {"shell-session", Language::Shell}, {"terminal", Language::Shell}, {"elisp", Language::CommonLisp},
  };
  for (const auto& [name, language] : names) {
    require(ckgit::languageForName(name) == language, "the fence name " + std::string(name) + " is " +
                                                          std::string(ckgit::languageName(language)));
  }
  for (const auto* plain : {"text", "txt", "plain", "plaintext", "TEXT"}) {
    require(ckgit::languageForName(plain) == Language::None, std::string(plain) + " is plain text");
  }
}

void testPaths() {
  const std::pair<std::string_view, Language> paths[] = {
      {"Makefile", Language::Makefile}, {"sub/GNUmakefile", Language::Makefile}, {"Makefile.am", Language::Makefile},
      {"rules.mk", Language::Makefile}, {"Dockerfile", Language::Dockerfile}, {"Dockerfile.dev", Language::Dockerfile},
      {"Containerfile", Language::Dockerfile}, {"app.dockerfile", Language::Dockerfile},
      {"CMakeLists.txt", Language::CMake}, {"x.cmake", Language::CMake}, {"Gemfile", Language::Ruby},
      {"Rakefile", Language::Ruby}, {"Jenkinsfile", Language::Groovy}, {"README.md", Language::Markdown},
      {"a.rs", Language::Rust}, {"a.go", Language::Go}, {"types.d.ts", Language::TypeScript},
      {"a.c", Language::C}, {"a.h", Language::Cpp}, {"a.m", Language::ObjectiveC}, {"a.mm", Language::ObjectiveC},
      {"nginx.conf", Language::Nginx}, {"site.conf", Language::Ini}, {"Cargo.lock", Language::Toml},
      {"Cargo.toml", Language::Toml}, {"setup.cfg", Language::Ini}, {"page.HTML", Language::Html},
      {"pom.xml", Language::Xml}, {"x.patch", Language::Diff}, {"main.tf", Language::Hcl}, {"a.ps1", Language::PowerShell},
      {"run.bat", Language::Batch}, {"doc.tex", Language::Latex}, {"s.scss", Language::Scss}, {"q.sql", Language::Sql},
      {"api.http", Language::Http}, {"x.ex", Language::Elixir}, {"x.hs", Language::Haskell}, {"x.kt", Language::Kotlin},
      {"x.swift", Language::Swift}, {"x.zig", Language::Zig}, {"x.lua", Language::Lua}, {"x.php", Language::Php},
  };
  for (const auto& [path, language] : paths) {
    require(ckgit::languageForPath(path) == language, std::string(path) + " is " + std::string(ckgit::languageName(language)));
  }
  const std::pair<std::string_view, Language> shebangs[] = {
      {"#!/usr/bin/env node\n", Language::JavaScript}, {"#!/usr/bin/env -S deno run --allow-read=/tmp\n", Language::JavaScript},
      {"#!/usr/bin/env bun\n", Language::JavaScript}, {"#!/usr/bin/ruby -w\n", Language::Ruby},
      {"#!/usr/bin/perl\n", Language::Perl}, {"#!/usr/bin/env perl5.36\n", Language::Perl},
      {"#!/usr/bin/php\n", Language::Php}, {"#!/usr/bin/env lua5.4\n", Language::Lua},
      {"#!/usr/bin/env Rscript\n", Language::R}, {"#!/usr/bin/env pwsh\n", Language::PowerShell},
      {"#!/usr/bin/python3.12\n", Language::Python}, {"#! /bin/bash\n", Language::Shell},
      {"#!/usr/bin/awk -f\n", Language::Awk}, {"#!/usr/bin/make -f\n", Language::Makefile},
      {"#!/usr/bin/env unknown\n", Language::None}, {"#!\n", Language::None},
  };
  for (const auto& [line, language] : shebangs) {
    require(ckgit::languageForShebang(line) == language, "shebang " + std::string(line));
  }
}

void testCatalogue() {
  const auto languages = ckgit::highlightLanguages();
  const auto last = static_cast<int>(Language::Http);
  require(static_cast<int>(languages.size()) == last, "every language but None is listed once");
  std::set<std::string_view> names;
  std::set<std::string_view> aliases;
  std::set<std::string_view> extensions;
  for (std::size_t k = 0; k < languages.size(); ++k) {
    const auto& language = languages[k];
    require(static_cast<int>(language.language) == static_cast<int>(k) + 1, "languages are listed in enumerator order");
    require(!language.name.empty() && ckgit::languageName(language.language) == language.name && names.insert(language.name).second,
            "each language has a unique display name");
    require(!language.aliases.empty(), std::string(language.name) + " has fence names");
    for (const auto alias : language.aliases) {
      require(aliases.insert(alias).second, "the fence name " + std::string(alias) + " is unique");
      require(ckgit::languageForName(alias) == language.language, "the fence name " + std::string(alias) + " round-trips");
    }
    for (const auto extension : language.extensions) {
      require(extensions.insert(extension).second, "the extension " + std::string(extension) + " is unique");
      require(ckgit::languageForPath("file." + std::string(extension)) == language.language,
              "the extension " + std::string(extension) + " round-trips");
    }
    for (const auto file : language.file_names) {
      require(ckgit::languageForPath("dir/" + std::string(file)) == language.language, "the file name " + std::string(file) + " round-trips");
    }
  }
  require(ckgit::languageName(Language::None).empty(), "None has no display name");
  require(ckgit::highlightClass(TokenKind::Type) == "hl-t" && ckgit::highlightClass(TokenKind::Function) == "hl-f" &&
              ckgit::highlightClass(TokenKind::Inserted) == "hl-i" && ckgit::highlightClass(TokenKind::Deleted) == "hl-d" &&
              ckgit::highlightClass(TokenKind::Comment) == "hl-c" && ckgit::highlightClass(TokenKind::Variable) == "hl-v",
          "token kinds render with their classes");
}

void testCpp() {
  const auto html = ckgit::highlightHtml(
      "#include <x>\nint main() { // hi\n  return 0x1F; /* multi\nline */ const char* s = \"a\\\"b\"; }\n", Language::Cpp);
  require(html ==
              "<span class=\"hl-p\">#include &lt;x&gt;</span>\n"
              "<span class=\"hl-k\">int</span> <span class=\"hl-f\">main</span>() { <span class=\"hl-c\">// hi</span>\n"
              "  <span class=\"hl-k\">return</span> <span class=\"hl-n\">0x1F</span>; <span class=\"hl-c\">/* multi</span>\n"
              "<span class=\"hl-c\">line */</span> <span class=\"hl-k\">const</span> <span class=\"hl-k\">char</span>* s = "
              "<span class=\"hl-s\">&quot;a\\&quot;b&quot;</span>; }\n",
          "C++ directives, keywords, functions, numbers, line and block comments, strings; spans split at newlines");
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
  require(ckgit::highlightHtml("std::string s; f (x); sizeof(int)", Language::Cpp) ==
              "std::<span class=\"hl-t\">string</span> s; f (x); <span class=\"hl-k\">sizeof</span>(<span class=\"hl-k\">int</span>)",
          "standard types; a function name needs its parenthesis right after it; keywords stay keywords");
  const auto tokens = ckgit::highlightTokens("int x; // c", Language::Cpp);
  require(tokens.size() == 2 && tokens[0].begin == 0 && tokens[0].end == 3 && tokens[0].kind == TokenKind::Keyword &&
              tokens[1].begin == 7 && tokens[1].end == 11 && tokens[1].kind == TokenKind::Comment,
          "tokens carry byte ranges and kinds in order");
  expectHtml(Language::C, "int main(void) { size_t n; }",
             R"x(<span class="hl-k">int</span> <span class="hl-f">main</span>(<span class="hl-k">void</span>) { <span class="hl-t">size_t</span> n; })x",
             "C keywords, functions and standard types");
  expectHtml(Language::ObjectiveC, "@interface A\nNSString *s = @\"x\";",
             "<span class=\"hl-k\">@interface</span> A\n<span class=\"hl-t\">NSString</span> *s = <span class=\"hl-s\">@&quot;x&quot;</span>;",
             "Objective-C directives and @\"strings\"");
}

void testPython() {
  require(ckgit::highlightHtml("@dataclass\ndef f(x):  # c\n    return f\"{x}\" + '''a\nb''' + 1.5e3\n", Language::Python) ==
              "<span class=\"hl-a\">@dataclass</span>\n"
              "<span class=\"hl-k\">def</span> <span class=\"hl-f\">f</span>(x):  <span class=\"hl-c\"># c</span>\n"
              "    <span class=\"hl-k\">return</span> <span class=\"hl-s\">f&quot;{x}&quot;</span> + "
              "<span class=\"hl-s\">&#39;&#39;&#39;a</span>\n<span class=\"hl-s\">b&#39;&#39;&#39;</span> + "
              "<span class=\"hl-n\">1.5e3</span>\n",
          "Python decorators, keywords, functions, comments, prefixed and triple-quoted strings, numbers");
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

void testMarkup() {
  expectHtml(Language::Html,
             "<!DOCTYPE html>\n<p class=x id=\"a\">A &amp; B<!-- c --></p>\n<script>let a = 1; // c\n</script><style>p { color: #fff; }</style>\n",
             R"x(<span class="hl-p">&lt;!DOCTYPE html&gt;</span>
&lt;<span class="hl-k">p</span> <span class="hl-a">class</span>=<span class="hl-s">x</span> <span class="hl-a">id</span>=<span class="hl-s">&quot;a&quot;</span>&gt;A <span class="hl-v">&amp;amp;</span> B<span class="hl-c">&lt;!-- c --&gt;</span>&lt;/<span class="hl-k">p</span>&gt;
&lt;<span class="hl-k">script</span>&gt;<span class="hl-k">let</span> a = <span class="hl-n">1</span>; <span class="hl-c">// c</span>
&lt;/<span class="hl-k">script</span>&gt;&lt;<span class="hl-k">style</span>&gt;<span class="hl-k">p</span> { <span class="hl-a">color</span>: <span class="hl-n">#fff</span>; }&lt;/<span class="hl-k">style</span>&gt;
)x",
             "tags, attributes, values, entities, comments, doctype, and script and style bodies in their languages");
  expectHtml(Language::Html, "<script type=\"text/template\"><b>x</b></script><script type=\"application/ld+json\">{\"a\":1}</SCRIPT>",
             R"x(&lt;<span class="hl-k">script</span> <span class="hl-a">type</span>=<span class="hl-s">&quot;text/template&quot;</span>&gt;&lt;b&gt;x&lt;/b&gt;&lt;/<span class="hl-k">script</span>&gt;&lt;<span class="hl-k">script</span> <span class="hl-a">type</span>=<span class="hl-s">&quot;application/ld+json&quot;</span>&gt;{<span class="hl-a">&quot;a&quot;</span>:<span class="hl-n">1</span>}&lt;/<span class="hl-k">SCRIPT</span>&gt;)x",
             "a template script stays plain, a JSON one is JSON, and the closing tag matches in any case");
  expectHtml(Language::Xml, "<?xml version=\"1.0\"?>\n<a b='1'><![CDATA[x]]><script>x</script></a>",
             R"x(<span class="hl-p">&lt;?xml version=&quot;1.0&quot;?&gt;</span>
&lt;<span class="hl-k">a</span> <span class="hl-a">b</span>=<span class="hl-s">&#39;1&#39;</span>&gt;<span class="hl-s">&lt;![CDATA[x]]&gt;</span>&lt;<span class="hl-k">script</span>&gt;x&lt;/<span class="hl-k">script</span>&gt;&lt;/<span class="hl-k">a</span>&gt;)x",
             "processing instructions, CDATA, and no script language in XML");
  expectHtml(Language::Html, "a < b && <x", "a &lt; b &amp;&amp; &lt;<span class=\"hl-k\">x</span>",
             "a bare < and & are text; an unclosed tag ends with the input");
}

void testSheets() {
  expectHtml(Language::Css, "@media screen { a:hover, .b #c::after { margin: -2px 1.5em !important; color: #0f0f; } }",
             R"x(<span class="hl-k">@media</span> <span class="hl-k">screen</span> { <span class="hl-k">a</span><span class="hl-p">:hover</span>, <span class="hl-t">.b</span> <span class="hl-t">#c</span><span class="hl-p">::after</span> { <span class="hl-a">margin</span>: <span class="hl-n">-2px</span> <span class="hl-n">1.5em</span> <span class="hl-k">!important</span>; <span class="hl-a">color</span>: <span class="hl-n">#0f0f</span>; } })x",
             "at-rules, selectors, pseudo-classes, properties, numbers with units, !important and hex colours");
  expectHtml(Language::Css, ":root { --x: 1px; width: calc(var(--x) * 2); background: url(http://a/b.png); }",
             R"x(<span class="hl-p">:root</span> { <span class="hl-a">--x</span>: <span class="hl-n">1px</span>; <span class="hl-a">width</span>: <span class="hl-f">calc</span>(<span class="hl-f">var</span>(<span class="hl-v">--x</span>) * <span class="hl-n">2</span>); <span class="hl-a">background</span>: <span class="hl-f">url</span>(<span class="hl-s">http://a/b.png</span>); })x",
             "custom properties, functions, and an unquoted url() whose // is no comment");
  expectHtml(Language::Scss, "$w: 2px; // c\n.a { &:hover { width: #{$w}px; } }",
             R"x(<span class="hl-v">$w</span>: <span class="hl-n">2px</span>; <span class="hl-c">// c</span>
<span class="hl-t">.a</span> { &amp;<span class="hl-p">:hover</span> { <span class="hl-a">width</span>: <span class="hl-v">#{$w}</span>px; } })x",
             "SCSS variables, line comments, nesting and interpolation");
  expectHtml(Language::Less, "@w: 1px; .a { width: @w; }",
             R"x(<span class="hl-v">@w</span>: <span class="hl-n">1px</span>; <span class="hl-t">.a</span> { <span class="hl-a">width</span>: <span class="hl-v">@w</span>; })x",
             "Less variables are no at-rules");
}

void testMarkdown() {
  expectHtml(Language::Markdown, "# Title\nText *em* **b** `x` [l](u) <https://a.b>\n- item\n> q\n```js\nlet a = 1;\n```\n",
             R"x(<span class="hl-k"># Title</span>
Text <span class="hl-v">*em*</span> <span class="hl-k">**b**</span> <span class="hl-s">`x`</span> <span class="hl-a">[l]</span><span class="hl-s">(u)</span> <span class="hl-s">&lt;https://a.b&gt;</span>
<span class="hl-k">-</span> item
<span class="hl-k">&gt;</span> q
<span class="hl-p">```js</span>
<span class="hl-k">let</span> a = <span class="hl-n">1</span>;
<span class="hl-p">```</span>
)x",
             "headings, emphasis, code, links, autolinks, list and quote markers, and a fenced block in its language");
  expectHtml(Language::Markdown, "---\ntitle: x\n---\nSetext\n===\nsnake_case_name * a\n```\nplain *x*\n```\n<!-- c -->",
             R"x(<span class="hl-k">---</span>
<span class="hl-a">title</span>: x
<span class="hl-k">---</span>
Setext
<span class="hl-k">===</span>
snake_case_name * a
<span class="hl-p">```</span>
plain *x*
<span class="hl-p">```</span>
<span class="hl-c">&lt;!-- c --&gt;</span>)x",
             "YAML front matter, setext underlines, intraword underscores, plain fences and HTML comments");
}

void testConfiguration() {
  expectHtml(Language::Ini, "; c\n[s]\nk = v ; t\nb=true\npath = ${HOME}/x\n",
             R"x(<span class="hl-c">; c</span>
<span class="hl-k">[s]</span>
<span class="hl-a">k</span> = v <span class="hl-c">; t</span>
<span class="hl-a">b</span>=<span class="hl-k">true</span>
<span class="hl-a">path</span> = <span class="hl-v">${HOME}</span>/x
)x",
             "INI sections, keys, values, booleans, variables and comments");
  expectHtml(Language::Toml, "[t]\na = \"s\" # c\nd = 1979-05-27\n[[x]]\ny = { z = 1 }\nm = \"\"\"\ntwo\n\"\"\"\n",
             R"x(<span class="hl-k">[t]</span>
<span class="hl-a">a</span> = <span class="hl-s">&quot;s&quot;</span> <span class="hl-c"># c</span>
<span class="hl-a">d</span> = <span class="hl-n">1979-05-27</span>
<span class="hl-k">[[x]]</span>
<span class="hl-a">y</span> = { <span class="hl-a">z</span> = <span class="hl-n">1</span> }
<span class="hl-a">m</span> = <span class="hl-s">&quot;&quot;&quot;</span>
<span class="hl-s">two</span>
<span class="hl-s">&quot;&quot;&quot;</span>
)x",
             "TOML tables, keys, inline tables, dates and multi-line strings");
}

void testDiff() {
  expectHtml(Language::Diff, "--- a/f\n+++ b/f\n@@ -1,2 +1,2 @@ fn\n x\n--- y\n+z\n\\ No newline at end of file\n",
             R"x(<span class="hl-k">--- a/f</span>
<span class="hl-k">+++ b/f</span>
<span class="hl-p">@@ -1,2 +1,2 @@</span> fn
 x
<span class="hl-d">--- y</span>
<span class="hl-i">+z</span>
<span class="hl-c">\ No newline at end of file</span>
)x",
             "headers, hunk headers, and a removed line that looks like a header inside a counted hunk");
  const auto tokens = ckgit::highlightTokens("+a\n-b\n", Language::Diff);
  require(tokens.size() == 2 && tokens[0].kind == TokenKind::Inserted && tokens[1].kind == TokenKind::Deleted,
          "a fragment without headers still marks added and removed lines");
}

void testBuildFiles() {
  expectHtml(Language::Dockerfile, "FROM a AS b\nRUN echo $X \\\n  && ls # c\nCOPY --from=b /a /b\n",
             R"x(<span class="hl-k">FROM</span> a <span class="hl-k">AS</span> b
<span class="hl-k">RUN</span> echo <span class="hl-v">$X</span> \
  &amp;&amp; ls <span class="hl-c"># c</span>
<span class="hl-k">COPY</span> <span class="hl-a">--from</span>=b /a /b
)x",
             "instructions, shell code across continuation lines, flags and variables");
  expectHtml(Language::Makefile, "X := $(Y:.c=.o)\n.PHONY: all\nall: x # c\n\t@echo $@ $$p \"q\"\n",
             R"x(<span class="hl-v">X</span> := <span class="hl-v">$(Y:.c=.o)</span>
<span class="hl-k">.PHONY</span>: all
<span class="hl-f">all</span>: x <span class="hl-c"># c</span>
	<span class="hl-p">@</span>echo <span class="hl-v">$@</span> <span class="hl-v">$$p</span> <span class="hl-s">&quot;q&quot;</span>
)x",
             "assignments, substitution references, special and ordinary targets, and recipes as shell");
  expectHtml(Language::Makefile, "ifeq ($(OS),x)\n  EXT = .exe\nendif\nall: \\#x\n",
             R"x(<span class="hl-k">ifeq</span> (<span class="hl-v">$(OS)</span>,x)
  <span class="hl-v">EXT</span> = .exe
<span class="hl-k">endif</span>
<span class="hl-f">all</span>: \#x
)x",
             "conditionals and an escaped # in a rule");
  expectHtml(Language::CMake, "add_executable(app ${SRC}) # c\nif(NOT WIN32)\n#[[ x\n]]",
             R"x(<span class="hl-f">add_executable</span>(app <span class="hl-v">${SRC}</span>) <span class="hl-c"># c</span>
<span class="hl-k">if</span>(<span class="hl-k">NOT</span> WIN32)
<span class="hl-c">#[[ x</span>
<span class="hl-c">]]</span>)x",
             "CMake commands, variables, case-insensitive keywords and bracket comments");
}

void testDocumentsAndScripts() {
  expectHtml(Language::Latex, "\\section{A} % c\n$x$ \\begin{verbatim}\n$y\n\\end{verbatim}",
             R"x(<span class="hl-k">\section</span>{A} <span class="hl-c">% c</span>
<span class="hl-s">$x$</span> <span class="hl-k">\begin</span>{<span class="hl-t">verbatim</span>}
<span class="hl-s">$y</span>
<span class="hl-k">\end</span>{<span class="hl-t">verbatim</span>})x",
             "commands, comments, math, environments and verbatim text");
  expectHtml(Language::Batch, "@echo off\nrem c\n:l\nset A=%B% %1 %%i !C! %~dp0\n",
             R"x(<span class="hl-p">@</span><span class="hl-k">echo</span> <span class="hl-k">off</span>
<span class="hl-c">rem c</span>
<span class="hl-f">:l</span>
<span class="hl-k">set</span> A=<span class="hl-v">%B%</span> <span class="hl-v">%1</span> <span class="hl-v">%%i</span> <span class="hl-v">!C!</span> <span class="hl-v">%~dp0</span>
)x",
             "batch echo suppression, REM, labels and every variable form");
  expectHtml(Language::Http, "GET /a HTTP/1.1\nAccept: */*\nContent-Type: application/json\n\n{\"a\": 1}\n###\nHTTP/1.1 404 Not Found\n",
             R"x(<span class="hl-k">GET</span> <span class="hl-s">/a</span> <span class="hl-k">HTTP/1.1</span>
<span class="hl-a">Accept</span>: */*
<span class="hl-a">Content-Type</span>: application/json

{<span class="hl-a">&quot;a&quot;</span>: <span class="hl-n">1</span>}
<span class="hl-c">###</span>
<span class="hl-k">HTTP/1.1</span> <span class="hl-n">404</span> Not Found
)x",
             "request and status lines, headers, a JSON body and a second message");
}

void testGenericLanguages() {
  expectHtml(Language::JavaScript, "const r = /a+/g, d = a / b; let t = `x ${y + 1} z`;",
             R"x(<span class="hl-k">const</span> r = <span class="hl-s">/a+/g</span>, d = a / b; <span class="hl-k">let</span> t = <span class="hl-s">`x </span><span class="hl-v">${</span>y + <span class="hl-n">1</span><span class="hl-v">}</span><span class="hl-s"> z`</span>;)x",
             "regular expressions where an operand is expected, division elsewhere, and template interpolation");
  expectHtml(Language::JavaScript, "`a\n${`b ${c}`}`",
             "<span class=\"hl-s\">`a</span>\n<span class=\"hl-v\">${</span><span class=\"hl-s\">`b </span><span class=\"hl-v\">${</span>c"
             "<span class=\"hl-v\">}</span><span class=\"hl-s\">`</span><span class=\"hl-v\">}</span><span class=\"hl-s\">`</span>",
             "template literals span lines and nest");
  expectHtml(Language::TypeScript, "type A = { b: number };",
             R"x(<span class="hl-k">type</span> <span class="hl-t">A</span> = { b: <span class="hl-t">number</span> };)x",
             "TypeScript type definitions and built-in types");
  expectHtml(Language::Rust, "fn f<'a>(x: &'a str) -> u8 { let c = 'c'; let r = r#\"x\"#; println!(\"{x}\"); 0 }",
             R"x(<span class="hl-k">fn</span> <span class="hl-f">f</span>&lt;<span class="hl-t">&#39;a</span>&gt;(x: &amp;<span class="hl-t">&#39;a</span> <span class="hl-t">str</span>) -&gt; <span class="hl-t">u8</span> { <span class="hl-k">let</span> c = <span class="hl-s">&#39;c&#39;</span>; <span class="hl-k">let</span> r = <span class="hl-s">r#&quot;x&quot;#</span>; <span class="hl-p">println!</span>(<span class="hl-s">&quot;{x}&quot;</span>); <span class="hl-n">0</span> })x",
             "Rust lifetimes, characters, raw strings and macros");
  expectHtml(Language::Rust, "/* a /* b */ c */ #[derive(Debug)] x",
             "<span class=\"hl-c\">/* a /* b */ c */</span> <span class=\"hl-p\">#[derive(Debug)]</span> x",
             "nested block comments and attributes");
  expectHtml(Language::Go, "func main() { s := `r\n` }",
             "<span class=\"hl-k\">func</span> <span class=\"hl-f\">main</span>() { s := <span class=\"hl-s\">`r</span>\n<span class=\"hl-s\">`</span> }",
             "Go raw strings span lines");
  expectHtml(Language::CSharp, "#region R\nvar s = $\"a {b} {{c}}\"; var v = @\"\\d\";",
             R"x(<span class="hl-p">#region R</span>
<span class="hl-k">var</span> s = <span class="hl-s">$&quot;a </span><span class="hl-v">{</span>b<span class="hl-v">}</span><span class="hl-s"> {{c}}&quot;</span>; <span class="hl-k">var</span> v = <span class="hl-s">@&quot;\d&quot;</span>;)x",
             "C# directives, interpolated strings with escaped braces, and verbatim strings");
  expectHtml(Language::Java, "@Override String f() { return \"x\"; }",
             R"x(<span class="hl-p">@Override</span> <span class="hl-t">String</span> <span class="hl-f">f</span>() { <span class="hl-k">return</span> <span class="hl-s">&quot;x&quot;</span>; })x",
             "Java annotations, types and methods");
  expectHtml(Language::Kotlin, "fun f(n: Int) = \"v $n ${n + 1}\"",
             R"x(<span class="hl-k">fun</span> <span class="hl-f">f</span>(n: <span class="hl-t">Int</span>) = <span class="hl-s">&quot;v </span><span class="hl-v">$n</span><span class="hl-s"> </span><span class="hl-v">${</span>n + <span class="hl-n">1</span><span class="hl-v">}</span><span class="hl-s">&quot;</span>)x",
             "Kotlin string templates");
  expectHtml(Language::Swift, "let s = \"a \\(b) c\"",
             R"x(<span class="hl-k">let</span> s = <span class="hl-s">&quot;a </span><span class="hl-v">\(</span>b<span class="hl-v">)</span><span class="hl-s"> c&quot;</span>)x",
             "Swift interpolation");
  expectHtml(Language::Ruby, "def f(x) = puts \"#{x}\" :sym key: 1\n=begin\nc\n=end",
             R"x(<span class="hl-k">def</span> <span class="hl-f">f</span>(x) = puts <span class="hl-s">&quot;</span><span class="hl-v">#{</span>x<span class="hl-v">}</span><span class="hl-s">&quot;</span> <span class="hl-a">:sym</span> <span class="hl-a">key</span>: <span class="hl-n">1</span>
<span class="hl-c">=begin</span>
<span class="hl-c">c</span>
<span class="hl-c">=end</span>)x",
             "Ruby interpolation, symbols, keys and =begin comments");
  expectHtml(Language::Perl, "my $x = qw(a b); s/a/b/g; print \"$x\\n\";",
             R"x(<span class="hl-k">my</span> <span class="hl-v">$x</span> = <span class="hl-s">qw(a b)</span>; <span class="hl-s">s/a/b/g</span>; <span class="hl-k">print</span> <span class="hl-s">&quot;</span><span class="hl-v">$x</span><span class="hl-s">\n&quot;</span>;)x",
             "Perl sigils, quote-like operators, substitutions and interpolation");
  expectHtml(Language::Php, "<?php $a = \"v $a\"; # c",
             R"x(<span class="hl-p">&lt;?php</span> <span class="hl-v">$a</span> = <span class="hl-s">&quot;v </span><span class="hl-v">$a</span><span class="hl-s">&quot;</span>; <span class="hl-c"># c</span>)x",
             "PHP tags, variables and both comment forms");
  expectHtml(Language::Lua, "local s = [[x]] --[[ c ]] -- d",
             R"x(<span class="hl-k">local</span> s = <span class="hl-s">[[x]]</span> <span class="hl-c">--[[ c ]]</span> <span class="hl-c">-- d</span>)x",
             "Lua long strings and comments");
  expectHtml(Language::Sql, "SELECT count(*) FROM t WHERE a = 'b''c' -- c",
             R"x(<span class="hl-k">SELECT</span> <span class="hl-f">count</span>(*) <span class="hl-k">FROM</span> t <span class="hl-k">WHERE</span> a = <span class="hl-s">&#39;b&#39;</span><span class="hl-s">&#39;c&#39;</span> <span class="hl-c">-- c</span>)x",
             "SQL keywords in any case, functions, doubled quotes and comments");
  expectHtml(Language::Haskell, "{-# LANGUAGE X #-}\nmain :: IO () -- c\nmain = print 'x' {- a {- b -} -}",
             R"x(<span class="hl-p">{-# LANGUAGE X #-}</span>
main :: <span class="hl-t">IO</span> () <span class="hl-c">-- c</span>
main = print <span class="hl-s">&#39;x&#39;</span> <span class="hl-c">{- a {- b -} -}</span>)x",
             "Haskell pragmas, types and nested comments");
  expectHtml(Language::OCaml, "let f (x : 'a list) = (* c (* d *) *) x",
             R"x(<span class="hl-k">let</span> f (x : <span class="hl-t">&#39;a</span> <span class="hl-t">list</span>) = <span class="hl-c">(* c (* d *) *)</span> x)x",
             "OCaml type variables and nested comments");
  expectHtml(Language::Elixir, "def f(x), do: :ok",
             R"x(<span class="hl-k">def</span> <span class="hl-f">f</span>(x), <span class="hl-a">do</span>: <span class="hl-a">:ok</span>)x",
             "Elixir keyword lists and atoms");
  expectHtml(Language::Erlang, "-module(m). f(X) -> X. % c",
             R"x(<span class="hl-p">-module</span>(m). <span class="hl-f">f</span>(<span class="hl-v">X</span>) -&gt; <span class="hl-v">X</span>. <span class="hl-c">% c</span>)x",
             "Erlang attributes, variables and comments");
  expectHtml(Language::Clojure, "(defn f [x] (inc x)) ; c",
             R"x((<span class="hl-k">defn</span> <span class="hl-f">f</span> [x] (<span class="hl-f">inc</span> x)) <span class="hl-c">; c</span>)x",
             "Clojure definitions and calls");
  expectHtml(Language::PowerShell, "Get-Item -Path $p # c",
             R"x(<span class="hl-f">Get-Item</span> <span class="hl-a">-Path</span> <span class="hl-v">$p</span> <span class="hl-c"># c</span>)x",
             "PowerShell commands, parameters and variables");
  expectHtml(Language::Nginx, "listen 80; root /var;",
             R"x(<span class="hl-k">listen</span> <span class="hl-n">80</span>; <span class="hl-k">root</span> /var;)x",
             "nginx directives");
  expectHtml(Language::Assembly, "_start: mov eax, 1 ; c",
             R"x(<span class="hl-f">_start:</span> <span class="hl-k">mov</span> <span class="hl-v">eax</span>, <span class="hl-n">1</span> <span class="hl-c">; c</span>)x",
             "assembly labels, mnemonics, registers and comments");
  expectHtml(Language::Hcl, "name = \"a-${b}\" # c",
             R"x(<span class="hl-a">name</span> = <span class="hl-s">&quot;a-</span><span class="hl-v">${</span>b<span class="hl-v">}</span><span class="hl-s">&quot;</span> <span class="hl-c"># c</span>)x",
             "HCL keys and interpolation");
  expectHtml(Language::GraphQL, "query Q($id: ID!) { a }",
             R"x(<span class="hl-k">query</span> <span class="hl-f">Q</span>(<span class="hl-v">$id</span>: <span class="hl-t">ID</span>!) { a })x",
             "GraphQL operations, variables and scalar types");
  expectHtml(Language::Matlab, "y = x'; s = 'a';", R"x(y = x&#39;; s = <span class="hl-s">&#39;a&#39;</span>;)x",
             "a MATLAB apostrophe after an operand transposes");
  expectHtml(Language::VisualBasic, "Dim x As Integer ' c",
             R"x(<span class="hl-k">Dim</span> x <span class="hl-k">As</span> <span class="hl-t">Integer</span> <span class="hl-c">&#39; c</span>)x",
             "Visual Basic keywords in any case and apostrophe comments");
  expectHtml(Language::Pascal, "{$R+} begin WriteLn('x') end.",
             R"x(<span class="hl-p">{$R+}</span> <span class="hl-k">begin</span> <span class="hl-f">WriteLn</span>(<span class="hl-s">&#39;x&#39;</span>) <span class="hl-k">end</span>.)x",
             "Pascal directives");
  expectHtml(Language::Verilog, "assign a = 8'hFF; `define X",
             R"x(<span class="hl-k">assign</span> a = <span class="hl-n">8&#39;hFF</span>; <span class="hl-p">`define</span> X)x",
             "Verilog sized literals and compiler directives");
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
  // Every language keeps the token guarantees on awkward text, including
  // UTF-8 right after quotes and inside would-be character literals.
  const std::string awkward = "\"a\n'b\n<<\n$\n#\n/*\nR\"(\n'\xc3\xa9\n\xe2\x82\xac'x ${ #{ `\n@@ -1 +1 @@\n<a href='\n";
  for (const auto& entry : ckgit::highlightLanguages()) {
    std::size_t last = 0;
    for (const auto& token : ckgit::highlightTokens(awkward, entry.language)) {
      require(token.begin >= last && token.end > token.begin && token.end <= awkward.size(),
              std::string(entry.name) + ": tokens are ordered, non-empty, in range");
      require((static_cast<unsigned char>(awkward[token.begin]) & 0xc0) != 0x80 &&
                  (token.end == awkward.size() || (static_cast<unsigned char>(awkward[token.end]) & 0xc0) != 0x80),
              std::string(entry.name) + ": tokens never split a UTF-8 sequence");
      last = token.end;
    }
    const auto html = ckgit::highlightHtml(awkward, entry.language);
    for (auto at = html.find("<span"); at != std::string::npos; at = html.find("<span", at + 1)) {
      require(html.substr(at, html.find("</span>", at) - at).find('\n') == std::string::npos,
              std::string(entry.name) + ": no span contains a newline");
    }
  }
}

// Hostile input the size of the bound, for every language: unclosed comment
// and string openers, interpolations and brackets by the ten thousand. A
// quadratic scan would take minutes here; a linear one milliseconds.
void testLinearTime() {
  const std::string_view units[] = {"/*", "\"", "${", "'x ", "<a [`*#{(*\\"};
  for (const auto unit : units) {
    std::string input;
    while (input.size() + unit.size() <= ckgit::kMaximumHighlightBytes) input += unit;
    for (const auto& entry : ckgit::highlightLanguages()) {
      const auto start = std::chrono::steady_clock::now();
      static_cast<void>(ckgit::highlightHtml(input, entry.language));
      const auto elapsed = std::chrono::steady_clock::now() - start;
      require(elapsed < std::chrono::seconds(2),
              std::string(entry.name) + " stays linear on 128 KiB of " + std::string(unit));
    }
  }
}

}  // namespace

void testHighlight() {
  testDetection();
  testNames();
  testPaths();
  testCatalogue();
  testCpp();
  testPython();
  testShell();
  testYaml();
  testJson();
  testMarkup();
  testSheets();
  testMarkdown();
  testConfiguration();
  testDiff();
  testBuildFiles();
  testDocumentsAndScripts();
  testGenericLanguages();
  testBounds();
  testLinearTime();
}
