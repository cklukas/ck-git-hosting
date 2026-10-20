// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "ckgit/docs_site.hpp"

namespace {

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("docs features: " + message);
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

bool anyContains(const std::vector<std::string>& lines, std::string_view part) {
  return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return contains(line, part); });
}

void write(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
  require(bool(out), "write fixture " + path.string());
}

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// A scratch directory beneath TMPDIR (outside any Git work tree, so pages
// come from a directory walk), removed when the fixture goes away.
class Scratch {
 public:
  Scratch() {
    const char* configured = std::getenv("TMPDIR");
    require(configured != nullptr && *configured != '\0', "TMPDIR must be explicitly selected");
    auto pattern = (fs::path(configured) / "ckdocs-features-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(mkdtemp(writable.data()) != nullptr, "could not create the fixture directory");
    root = writable.data();
  }
  ~Scratch() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  fs::path root;
};

std::string rejection(const std::string& yaml) {
  try {
    static_cast<void>(ckgit::parseDocsConfig(yaml));
    return {};
  } catch (const std::exception& error) {
    return error.what();
  }
}

std::string loadError(const fs::path& root) {
  try {
    static_cast<void>(ckgit::loadDocsSite(root, ckgit::readDocsConfig(root), nullptr));
    return {};
  } catch (const std::exception& error) {
    return error.what();
  }
}

const ckgit::DocsPage& page(const ckgit::DocsSiteModel& model, std::string_view source) {
  for (const auto& candidate : model.pages) {
    if (candidate.source == source) return candidate;
  }
  throw std::runtime_error("docs features: no page " + std::string(source));
}

bool hasPage(const ckgit::DocsSiteModel& model, std::string_view source) {
  return std::any_of(model.pages.begin(), model.pages.end(), [&](const ckgit::DocsPage& candidate) { return candidate.source == source; });
}

const std::string kHead = "version: 1\nsite:\n  title: T\n";

void testConfig() {
  const auto config = ckgit::parseDocsConfig(
      "version: 1\nsite:\n  title: T\n  lang: de-CH\n"
      "ui:\n  on_this_page: Inhalt\ntoc:\n  min_level: 2\n  max_level: 4\nmarkdown:\n  line_numbers: true\n"
      "i18n:\n  label: Deutsch (Schweiz)\n  locales:\n"
      "    - lang: fr\n      label: Français\n      source: docs/fr\n      title: Titre\n      description: Une description.\n"
      "      fallback: false\n      nav_titles:\n        Guides: Guides FR\n      ui:\n        search: Chercher\n"
      "      nav:\n        - docs/fr/README.md\n"
      "    - lang: it\n");
  require(config.lang == "de-CH" && config.ui.size() == 1 && config.ui[0].first == "on_this_page" && config.toc_min_level == 2 &&
              config.toc_max_level == 4 && config.line_numbers && config.language_label == "Deutsch (Schweiz)",
          "site.lang, ui, toc, markdown and i18n.label");
  require(config.locales.size() == 2 && config.locales[0].lang == "fr" && config.locales[0].label == "Français" &&
              config.locales[0].source == "docs/fr" && config.locales[0].title == "Titre" && !config.locales[0].fallback &&
              config.locales[0].nav_titles.at("Guides") == "Guides FR" && config.locales[0].ui.size() == 1 &&
              config.locales[0].nav.size() == 1 && config.locales[1].lang == "it" && config.locales[1].fallback &&
              config.locales[1].source.empty(),
          "every locale key, and the defaults");
  struct Bad {
    std::string yaml;
    std::string message;
  };
  for (const Bad& bad : std::vector<Bad>{
           {kHead + "ui:\n  nope: x\n", "has no interface text 'nope'"},
           {kHead + "ui:\n  menu: ''\n", "ui.menu is empty"},
           {"version: 1\nsite:\n  title: T\n  lang: english\n", "is not a language tag"},
           {kHead + "toc:\n  max_level: 7\n", "toc.max_level must be a heading level from 1 to 6"},
           {kHead + "toc:\n  min_level: 4\n  max_level: 3\n", "toc.min_level is greater than toc.max_level"},
           {kHead + "markdown:\n  line_numbers: yes\n", "markdown.line_numbers must be true or false"},
           {kHead + "markdown:\n  math: true\n", "math"},
           {kHead + "i18n:\n  label: X\n", "i18n needs 'locales'"},
           {kHead + "i18n:\n  locales: []\n", "i18n.locales is empty"},
           {kHead + "i18n:\n  locales:\n    - label: X\n", "a locale needs 'lang'"},
           {kHead + "i18n:\n  locales:\n    - lang: de\n    - lang: DE\n", "locale 'DE' is listed twice"},
           {kHead + "i18n:\n  locales:\n    - lang: en\n", "locale 'en' is the site's own language"},
           {kHead + "i18n:\n  locales:\n    - lang: de\n      flavour: x\n", "flavour"},
           {kHead + "i18n:\n  locales:\n    - lang: de\n      source: ../x\n", "relative path inside the repository"},
       }) {
    require(contains(rejection(bad.yaml), bad.message), "rejected: " + bad.message + " (got: " + rejection(bad.yaml) + ")");
  }
}

void testFrontMatter() {
  Scratch scratch;
  const auto& root = scratch.root;
  write(root / "docs/README.md", "---\nlayout: home\noutline: deep\nhero:\n  name: N\n  text: T\n  actions:\n    - text: Go\n      link: guide/a.md\n"
                                 "features:\n  - title: F\n    icon: \":zap:\"\n    details: >\n      Folded\n      text.\n---\n# Home\n");
  write(root / "docs/guide/a.md",
        "---\ntitle: A\ntags: [one, two, one]\nkeywords: k\nsidebar_label: Alpha\nsidebar_position: 2\n"
        "outline: [2, 4]\nprev: false\nnext:\n  text: Onwards\n  link: b.md\nsearch: false\npage_class: wide special\n"
        "description: |\n  Two\n  lines.\n---\n# A\n");
  write(root / "docs/guide/b.md", "---\nslug: /elsewhere/\nhide_table_of_contents: true\nlayout: page\nsidebar: false\nnav_order: 1\n---\n# B\n");
  write(root / "docs/guide/c.md", "---\nslug: renamed\ndraft: true\n---\n# C\n");
  write(root / "docs/guide/d.md", "---\nslug: sub/dee.html\ntoc_min_heading_level: 3\ntoc_max_heading_level: 2\nlayout: blog\n"
                                  "hide_title: true\nlang: fr\nunlisted: true\ntags: [\"\"]\nprev: [x]\n---\n# D\n");
  write(root / "docs/guide/_part.md", "Partial, never a page.\n");
  write(root / "docs/_parts/x.md", "Partial too.\n");
  std::vector<std::string> warnings;
  const auto model = ckgit::loadDocsSite(root, ckgit::DocsConfig{}, &warnings);
  const auto& home = page(model, "docs/README.md");
  require(home.layout == "home" && home.landing && home.landing->name == "N" && home.landing->text == "T" &&
              home.landing->actions.size() == 1 && home.landing->actions[0].link == "guide/a.md" &&
              home.landing->actions[0].theme == "alt" && home.landing->features.size() == 1 &&
              home.landing->features[0].icon == "\xe2\x9a\xa1" && home.landing->features[0].details == "Folded text." &&
              home.toc_min_level == 2 && home.toc_max_level == 6,
          "home layout: hero, features, an emoji icon, folded text joined to one line, a deep outline");
  const auto& a = page(model, "docs/guide/a.md");
  require(a.tags == std::vector<std::string>{"one", "two"} && a.keywords == std::vector<std::string>{"k"} && a.nav_title == "Alpha" &&
              a.nav_order == 2 && a.toc_min_level == 2 && a.toc_max_level == 4 && a.prev.hidden && a.next.text == "Onwards" &&
              a.next.page.has_value() && model.pages[*a.next.page].source == "docs/guide/b.md" && !a.search &&
              a.page_class == "wide special" && a.description == "Two lines.",
          "tags (deduplicated), keywords, sidebar_label/position, outline levels, pager overrides, search, page_class");
  const auto& b = page(model, "docs/guide/b.md");
  require(b.output == "elsewhere/index.html" && !b.toc && b.layout == "page" && !b.sidebar, "an absolute slug ending in /; toc and sidebar off");
  require(!hasPage(model, "docs/guide/c.md") && model.skipped.at("docs/guide/c.md") == "is a draft (draft: true)", "a draft is left out");
  const auto& d = page(model, "docs/guide/d.md");
  require(d.output == "guide/sub/dee.html" && d.toc_min_level == 2 && d.toc_max_level == 3 && d.hide_title && d.lang == "fr" &&
              d.nav_exclude && !d.search && d.layout == "doc" && d.tags.empty() && !d.prev.hidden && d.prev.target.empty(),
          "a relative slug; crossed levels fall back to the site's; a bad layout, tag and prev are ignored");
  require(!hasPage(model, "docs/guide/_part.md") && !hasPage(model, "docs/_parts/x.md"), "files and directories starting with _ are partials");
  for (const auto* expected :
       {"docs/guide/d.md: layout 'blog' is not doc, page or home; ignored",
        "docs/guide/d.md: toc_min_heading_level is greater than toc_max_heading_level; the site's levels are used",
        "docs/guide/d.md: a tags entry is not a single line of 1 to 64 bytes; ignored",
        "docs/guide/d.md: prev must be false, a page, or {text, link}; ignored"}) {
    require(anyContains(warnings, expected), std::string("warned: ") + expected);
  }
  require(!anyContains(warnings, "docs/guide/c.md"), "a draft is not warned about");
  // In the nav: sidebar_label names a page, unlisted keeps it out.
  bool alpha = false;
  for (const auto& tab : model.nav) {
    for (const auto& item : tab.children) alpha = alpha || item.title == "Alpha";
  }
  require(alpha, "sidebar_label is the page's title in navigation");

  // Drafts build when asked; a draft home page is an error.
  auto drafts = ckgit::DocsConfig{};
  drafts.drafts = true;
  require(page(ckgit::loadDocsSite(root, drafts, nullptr), "docs/guide/c.md").output == "guide/renamed.html", "drafts build with --drafts");
  write(root / "docs/guide/e.md", "---\nslug: /elsewhere/\n---\n# E\n");
  require(contains(loadError(root), "would both be written to 'elsewhere/index.html'"), "two pages slugged to one place");
  fs::remove(root / "docs/guide/e.md");
  write(root / "docs/guide/f.md", "---\nslug: /\n---\n# F\n");
  require(contains(loadError(root), "would replace the home page"), "only the home page is the site root");
  fs::remove(root / "docs/guide/f.md");
  write(root / "docs/README.md", "---\ndraft: true\n---\n# Home\n");
  require(contains(loadError(root), "the home page 'docs/README.md' is a draft"), "the home page cannot be a draft");
}

void testBuild() {
  Scratch scratch;
  const auto& root = scratch.root;
  write(root / "ckdocs.yml", "version: 1\nsite:\n  title: Seite\n  lang: de\nsearch: true\nui:\n  menu: Navigation\n");
  write(root / "docs/README.md",
        "---\nlayout: home\nhero:\n  name: Name\n  actions:\n    - text: Los\n      link: guide/a.md#zwei\n      theme: brand\n    - text: Extern\n"
        "      link: https://example.test/\n      theme: alt\n  image:\n    src: img/logo.svg\n    alt: Logo\n"
        "features:\n  - title: Eins\n    details: Erstens.\n    link: guide/b.md\n  - title: Kaputt\n    details: Zweitens.\n    link: nirgends.md\n---\n"
        "# Willkommen\n");
  write(root / "docs/img/logo.svg", "<svg/>");
  write(root / "docs/guide/a.md",
        "---\ntags: [Einrichtung, \"C++\"]\ntoc_max_heading_level: 4\n---\n# A\n\n## Zwei\n\n### Drei\n\n#### Vier\n\n"
        "<<< ../../src/code.cpp#teil\n\n<!--@include: ../_teile/gemeinsam.md-->\n\n<<< ../../src/fehlt.cpp\n\n"
        "::: tip\nT\n:::\n");
  write(root / "docs/guide/b.md", "---\ntags: [Einrichtung]\nlayout: page\nsearch: false\nprev: a.md\n---\n# B\n\nNur Text.\n");
  write(root / "docs/_teile/gemeinsam.md", "Gemeinsam mit [Bild](../img/logo.svg).\n");
  write(root / "src/code.cpp", "// #region teil\nint x;\n// #endregion teil\n");
  const auto model = ckgit::loadDocsSite(root, ckgit::readDocsConfig(root), nullptr);
  const auto site = root / "site";
  ckgit::DocsBuildReport report;
  ckgit::buildDocsSite(model, site, ckgit::DocsBuildOptions{}, &report);
  for (const auto* file : {"index.html", "guide/a.html", "guide/b.html", "tags/index.html", "tags/einrichtung.html", "tags/c.html",
                           "img/logo.svg", "search-index.json"}) {
    require(fs::is_regular_file(site / file), std::string("written: ") + file);
  }
  std::string inputs;
  for (const auto& input : report.inputs) inputs += " " + input;
  require(report.inputs == std::vector<std::string>{"docs/_teile/gemeinsam.md", "src/code.cpp"},
          "the included files are reported as inputs, once each:" + inputs);
  require(report.broken_links.size() == 2 && anyContains(report.broken_links, "docs/guide/a.md:17: snippet '../../src/fehlt.cpp' does not exist") &&
              anyContains(report.broken_links, "docs/README.md: link target 'nirgends.md' is not a page of this site"),
          "a missing snippet and a broken feature link are reported:" + [&] {
            std::string all;
            for (const auto& line : report.broken_links) all += " | " + line;
            return all;
          }());

  const auto home = slurp(site / "index.html");
  require(contains(home, "<html lang=\"de\">") && contains(home, "<body class=\"layout-home\"") && contains(home, "Zum Inhalt springen") &&
              contains(home, "<div class=\"page landing\">") && !contains(home, "<aside"),
          "the site's language and interface text; the home layout has no sidebar");
  require(contains(home, "<section class=\"home-hero\"><div class=\"home-copy\"><h1><span class=\"home-name\">Name</span></h1>"
                         "<div class=\"home-actions\"><a class=\"home-action brand\" href=\"guide/a.html#zwei\">Los</a>"
                         "<a class=\"home-action alt\" href=\"https://example.test/\">Extern</a></div></div>"
                         "<picture class=\"home-image\"><img src=\"img/logo.svg\" alt=\"Logo\"></picture></section>"),
          "the hero, its buttons and image resolved like links");
  require(contains(home, "<section class=\"home-features\" aria-label=\"Funktionen\"><a class=\"home-feature\" href=\"guide/b.html\">"
                         "<h2>Eins</h2><p>Erstens.</p></a><div class=\"home-feature\"><h2>Kaputt</h2><p>Zweitens.</p></div></section>"),
          "a feature links where it can, and is a plain card where its link is broken");

  const auto a = slurp(site / "guide/a.html");
  require(contains(a, "<label class=\"nav-toggle\" for=\"nav-toggle\">Navigation</label>"), "a ui: override");
  require(contains(a, "<a href=\"#zwei\">Zwei</a><ul><li><a href=\"#drei\">Drei</a><ul><li><a href=\"#vier\">Vier</a></li></ul></li></ul>"),
          "the outline nests every level the page asks for");
  require(contains(a, "<p class=\"alert-title\">Tipp</p>") && contains(a, "Gemeinsam mit <a href=\"../img/logo.svg\">Bild</a>") &&
              contains(a, "<code class=\"language-cpp\">") && !contains(a, "#region"),
          "localized container titles; an included part with its link resolved from its own directory; a snippet's region");
  require(contains(a, "<nav class=\"page-tags\" aria-label=\"Schlagwörter\"><span>Schlagwörter</span><ul><li><a href=\"../tags/einrichtung.html\">"
                      "Einrichtung</a></li><li><a href=\"../tags/c.html\">C++</a></li></ul></nav>"),
          "the page's tags link to their pages");
  require(contains(a, "data-copy-label=\"Code kopieren\"") && contains(a, "function text(pre)"),
          "a page with code carries the copy script, labelled in its language");

  const auto b = slurp(site / "guide/b.html");
  require(contains(b, "<body class=\"layout-page\"") && contains(b, "<div class=\"page no-sidebar\">") && !contains(b, "class=\"crumbs\"") &&
              !contains(b, "class=\"pager\"") && !contains(b, "function text(pre)"),
          "the page layout: no sidebar, breadcrumbs or pager, and no script without code");

  const auto tags = slurp(site / "tags/einrichtung.html");
  require(contains(tags, "<h1>Schlagwort \u201eEinrichtung\u201c</h1><ul class=\"tagged-pages\"><li><a href=\"../guide/a.html\">A</a></li>"
                         "<li><a href=\"../guide/b.html\">B</a></li></ul>"),
          "a tag's page lists its pages in title order");
  require(contains(slurp(site / "tags/index.html"), "<a href=\"c.html\">C++</a> <span class=\"tag-count\">1 Seite</span>") &&
              contains(slurp(site / "tags/index.html"), "<span class=\"tag-count\">2 Seiten</span>"),
          "the tag index counts pages in the site's language");
  const auto search = slurp(site / "search-index.json");
  require(contains(search, "\"tags\":[\"Einrichtung\",\"C++\"]") && !contains(search, "guide/b.html"),
          "search knows tags and leaves out search: false pages");

  // A page whose output lies among the tag pages is an error.
  write(root / "docs/tags/x.md", "# X\n");
  ckgit::DocsBuildReport clash;
  bool refused = false;
  try {
    auto options = ckgit::DocsBuildOptions{};
    options.clean = true;
    ckgit::buildDocsSite(ckgit::loadDocsSite(root, ckgit::readDocsConfig(root), nullptr), site, options, &clash);
  } catch (const std::exception& error) {
    refused = contains(error.what(), "where the tag pages go");
  }
  require(refused, "tags/ belongs to the tag pages");
}

void testDiagramCache() {
  Scratch scratch;
  write(scratch.root / "docs/README.md", "# H\n\n```mermaid\nflowchart LR\n  A --> B\n```\n");
  const auto model = ckgit::loadDocsSite(scratch.root, ckgit::DocsConfig{}, nullptr);
  int calls = 0;
  ckgit::DocsDiagramCache cache;
  ckgit::DocsBuildOptions options;
  options.clean = true;
  options.diagram_cache = &cache;
  options.mermaid_renderer = [&](std::string_view) {
    ++calls;
    return ckgit::DocsDiagram{"<svg>light</svg>", "<svg>dark</svg>", "A to B", {}};
  };
  ckgit::buildDocsSite(model, scratch.root / "one", options, nullptr);
  ckgit::buildDocsSite(model, scratch.root / "one", options, nullptr);
  require(calls == 1 && cache.diagrams.size() == 1, "a cached diagram is not rendered again");
  require(contains(slurp(scratch.root / "one/index.html"), "alt=\"A to B\""), "the cached diagram is placed");
}

void testTranslations() {
  Scratch scratch;
  const auto& root = scratch.root;
  write(root / "ckdocs.yml",
        "version: 1\nsite:\n  title: Docs\n  url: https://example.test/\nnav:\n  - page: docs/README.md\n    title: Home\n"
        "  - title: Guides\n    pages:\n      - docs/a.md\n      - docs/b.md\n"
        "i18n:\n  locales:\n    - lang: de\n      nav_titles:\n        Home: Start\n        Guides: Anleitungen\n");
  write(root / "docs/README.md", "# Home\n");
  write(root / "docs/a.md", "# Alpha\n\n## Section\n\n[B](b.md)\n");
  write(root / "docs/b.md", "# Beta\n\n[A](a.md#section)\n");
  write(root / "docs/de/README.md", "# Startseite\n");
  write(root / "docs/de/a.md", "# Alpha DE\n\n[B](b.md)\n");
  const auto model = ckgit::loadDocsSite(root, ckgit::readDocsConfig(root), nullptr);
  require(model.lang == "en" && model.translations.size() == 1 && !hasPage(model, "docs/de/a.md"),
          "the translation's tree stays out of the default language");
  const auto& de = model.translations[0];
  require(de.lang == "de" && de.language_label == "Deutsch" && de.ui.home == "Startseite" && de.pages.size() == 3 &&
              !page(de, "docs/de/a.md").fallback && page(de, "docs/de/a.md").alias == "docs/a.md" &&
              page(de, "docs/b.md").fallback && page(de, "docs/b.md").alias == "docs/de/b.md" && page(de, "docs/b.md").output == "b.html",
          "a translated page, and a fallback for the page it lacks");
  require(de.nav.size() == 2 && de.nav[0].title == "Start" && de.nav[1].title == "Anleitungen" && de.nav[1].children.size() == 2 &&
              de.pages[*de.nav[1].children[0].page].source == "docs/de/a.md" && de.pages[*de.nav[1].children[1].page].source == "docs/b.md",
          "the default nav, mapped to the translation and its fallbacks, with translated titles");

  const auto site = root / "site";
  ckgit::DocsBuildReport report;
  ckgit::buildDocsSite(model, site, ckgit::DocsBuildOptions{}, &report);
  require(report.broken_links.empty() && report.broken_anchors.empty(), "no findings: fallback anchors are not checked again");
  const auto de_a = slurp(site / "de/a.html");
  require(contains(de_a, "<html lang=\"de\">") && contains(de_a, "<a href=\"b.html\">B</a>") &&
              contains(de_a, "<link rel=\"alternate\" hreflang=\"en\" href=\"https://example.test/a.html\">") &&
              contains(de_a, "<link rel=\"alternate\" hreflang=\"de\" href=\"https://example.test/de/a.html\">") &&
              contains(de_a, "<link rel=\"alternate\" hreflang=\"x-default\" href=\"https://example.test/a.html\">"),
          "a translated page links within its language and names its alternates");
  require(contains(de_a, "<details class=\"lang-select\">") && contains(de_a, "<a href=\"../a.html\" hreflang=\"en\" lang=\"en\">English</a>") &&
              contains(de_a, "<a href=\"a.html\" hreflang=\"de\" lang=\"de\" aria-current=\"true\">Deutsch</a>"),
          "the language menu links to the same page in each language");
  const auto de_b = slurp(site / "de/b.html");
  require(contains(de_b, "<p class=\"fallback-notice\" role=\"note\">Diese Seite wurde noch nicht übersetzt (Deutsch); sie wird in der "
                         "Originalsprache angezeigt (English).</p><article lang=\"en\" dir=\"ltr\">") &&
              contains(de_b, "<a href=\"a.html#section\">A</a>") && !contains(de_b, "hreflang=\"de\" href="),
          "a fallback page says so, keeps its language on the article, links to the translation, and is no alternate");
  require(contains(slurp(site / "b.html"), "<a href=\"de/b.html\" hreflang=\"de\" lang=\"de\">Deutsch</a>"),
          "the default language's menu links to the fallback too");
  const auto sitemap = slurp(site / "sitemap.xml");
  require(contains(sitemap, "https://example.test/de/a.html") && !contains(sitemap, "https://example.test/de/b.html") &&
              !contains(slurp(site / "llms.txt"), "/de/"),
          "the sitemap lists translated pages but not fallbacks; llms.txt keeps to the default language");
  require(contains(slurp(site / "de/site-index.html"),
                   "<a href=\"../llms.txt\" title=\"Das Dokumentationsverzeichnis für Agenten lesen\">llms.txt</a>") &&
              !fs::exists(site / "de/llms.txt"),
          "a translation's site index leads to the one llms.txt, with its own language's title");

  // Without fallback a translation holds only its own pages.
  write(root / "ckdocs.yml", "version: 1\nsite:\n  title: Docs\ni18n:\n  locales:\n    - lang: de\n      fallback: false\n");
  const auto strict = ckgit::loadDocsSite(root, ckgit::readDocsConfig(root), nullptr);
  require(strict.translations[0].pages.size() == 2 && !hasPage(strict.translations[0], "docs/b.md"), "fallback: false");
  // A translation needs its tree; a default page must not sit in its directory.
  write(root / "ckdocs.yml", "version: 1\nsite:\n  title: Docs\ni18n:\n  locales:\n    - lang: fr\n");
  require(contains(loadError(root), "locale 'fr': source directory 'docs/fr' does not exist"), "a missing translation tree");
  write(root / "ckdocs.yml", "version: 1\nsite:\n  title: Docs\ni18n:\n  locales:\n    - lang: de\n      source: translations/de\n");
  write(root / "translations/de/README.md", "# DE\n");
  require(contains(loadError(root), "inside the 'de/' directory of that translation"), "a default page in a translation's directory");
}

void testPortalSites() {
  Scratch scratch;
  const auto& root = scratch.root;
  write(root / "ckdocs.yml",
        "version: 1\nsite:\n  title: Portal\n  logo: images/portal.svg\n  favicon: images/portal.svg\n"
        "sites:\n  - id: main\n    title: Main\n    config: main.yml\n  - id: tool\n    title: Tool\n    config: docs/tool/tool.yml\n");
  write(root / "main.yml", "version: 1\nsite:\n  title: Main\nsource: docs\nexclude: [docs/tool]\n");
  write(root / "docs/tool/tool.yml",
        "version: 1\nsite:\n  title: Tool\n  brand: The tool\n  logo: docs/tool/tool.svg\n  favicon: docs/tool/tool.svg\nsource: docs/tool\n");
  write(root / "images/portal.svg", "<svg/>");
  write(root / "docs/tool/tool.svg", "<svg/>");
  write(root / "docs/README.md", "# Main\n\n[The tool](tool/guide.md#use) and [its home](tool/).\n");
  write(root / "docs/setup.md", "# Setup\n");
  write(root / "docs/tool/README.md", "# Tool\n\n[Back to setup](../setup.md).\n");
  write(root / "docs/tool/guide.md", "# Guide\n\n## Use\n");
  std::vector<std::string> warnings;
  const auto portal = ckgit::loadDocsPortal(root, ckgit::readDocsConfig(root), ckgit::DocsPortalOptions{}, &warnings);
  ckgit::DocsBuildReport report;
  ckgit::buildDocsPortal(portal, root / "site", ckgit::DocsBuildOptions{}, &report);
  require(report.broken_links.empty(), "links between the sites resolve");
  const auto main = slurp(root / "site/main/index.html");
  require(contains(main, "<a href=\"../tool/guide.html#use\">The tool</a>") && contains(main, "<a href=\"../tool/index.html\">its home</a>"),
          "a page links to another site's page and directory");
  require(contains(slurp(root / "site/tool/index.html"), "<a href=\"../main/setup.html\">Back to setup</a>"), "and back");
  require(contains(main, "<img src=\"../images/portal.svg\" alt=\"\">Portal</a>") &&
              contains(main, "<link rel=\"icon\" href=\"../images/portal.svg\">"),
          "a site without a logo of its own shows the portal's logo, brand and favicon");
  const auto tool = slurp(root / "site/tool/guide.html");
  require(contains(tool, "<a class=\"brand\" href=\"../index.html\"><img src=\"tool.svg\" alt=\"\">The tool</a>") &&
              contains(tool, "<link rel=\"icon\" href=\"tool.svg\">"),
          "a site with its own logo, brand and favicon shows them, still leading to the welcome page");
}

// Nothing outside the tree reaches a site through a symbolic link committed
// to it: not an include, a snippet, an image, a linked file, nor a logo --
// whether the link is the file itself or a directory on the way to it.
void testTreeConfinement() {
  Scratch scratch;
  const auto root = scratch.root / "repo";
  write(scratch.root / "outside/secret.md", "# OUTSIDE-SECRET\n");
  write(scratch.root / "outside/secret.txt", "OUTSIDE-SECRET\n");
  write(root / "ckdocs.yml", "version: 1\nsite:\n  title: Docs\n");
  write(root / "docs/_parts/inside.md", "Inside part.\n");
  fs::create_directory_symlink(scratch.root / "outside", root / "docs/linked");
  fs::create_symlink(scratch.root / "outside/secret.md", root / "docs/_parts/alias.md");
  write(root / "docs/README.md",
        "# Home\n\n<!--@include: _parts/inside.md-->\n\n<!--@include: linked/secret.md-->\n\n<!--@include: _parts/alias.md-->\n\n"
        "<<< linked/secret.txt\n\n![image](linked/secret.txt)\n\n[file](linked/secret.txt)\n");
  const auto model = ckgit::loadDocsSite(root, ckgit::readDocsConfig(root), nullptr);
  require(model.pages.size() == 1, "a linked directory's pages are never pages of the site");
  ckgit::DocsBuildReport report;
  const auto site = root / "site";
  ckgit::buildDocsSite(model, site, ckgit::DocsBuildOptions{}, &report);
  const auto home = slurp(site / "index.html");
  require(contains(home, "Inside part.") && !contains(home, "OUTSIDE-SECRET") && !fs::exists(site / "linked"),
          "an ordinary part is included; nothing behind a link is");
  for (const auto* finding : {"include 'linked/secret.md' is not a regular file", "include '_parts/alias.md' is not a regular file",
                              "snippet 'linked/secret.txt' is not a regular file", "image target 'linked/secret.txt' is not a regular file",
                              "link target 'linked/secret.txt' is not a regular file"}) {
    require(anyContains(report.broken_links, finding), std::string("reported: ") + finding);
  }
  write(root / "ckdocs.yml", "version: 1\nsite:\n  title: Docs\n  logo: docs/linked/secret.txt\n");
  std::string refused;
  try {
    ckgit::buildDocsSite(ckgit::loadDocsSite(root, ckgit::readDocsConfig(root), nullptr), root / "site2", ckgit::DocsBuildOptions{},
                         nullptr);
  } catch (const std::exception& error) {
    refused = error.what();
  }
  require(contains(refused, "site.logo 'docs/linked/secret.txt' is not a regular file"), "a logo behind a link is refused");
}

}  // namespace

void testDocsFeatures() {
  testConfig();
  testFrontMatter();
  testBuild();
  testDiagramCache();
  testTranslations();
  testPortalSites();
  testTreeConfinement();
}
