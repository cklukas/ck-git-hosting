---
description: What a ckdocs page is made of and how to change its look — header, navigation, colour themes, logo and favicon, your own stylesheet, diagrams, small screens, print, and accessibility.
---
# Look and feel

## The parts of a page

| Part | What it holds |
|---|---|
| Header | The brand (logo and name, leading to the home page — or, in a [portal](portals.md), to the welcome page — and the site selector), the tabs, the search button, the header links, the [language menu](translations.md) and the colour theme picker. |
| Sidebar | The current tab's pages, then "On this page": the page's headings from level 2 to 3, or the levels its [front matter](front-matter.md) or [`toc`](configuration.md#ckdocsyml-reference) asks for. |
| Main | Breadcrumbs, the page itself with its byline (author and date), its [tags](front-matter.md#tags), and previous/next links in reading order. |
| Footer | The site's footer text, "Built with ckdocs", and links to the [site index](search.md#the-site-index) and to the page's [Markdown version](search.md#llmstxt-and-markdown-versions). |

A page's `layout` (see [Page metadata](front-matter.md)) changes this: `page`
shows the content alone, full width, without sidebar, breadcrumbs and pager;
`home` turns the home page into a [landing page](front-matter.md#home-layout)
with a hero and feature cards. `sidebar: false`, `toc: false` and
`pager: false` remove one part each.

## Navigation

Category tabs open a dropdown when you click anywhere on their label or
chevron; individual page tabs remain links. The category's overview page is
inside the dropdown. Escape closes the menu and returns focus to its label;
clicking outside also closes it. Native disclosure controls work without JavaScript.

Sidebar categories use borderless disclosure rows. The category containing
the current page starts expanded; other categories start collapsed. If an
index page would repeat its category's name, its navigation label becomes
**Overview** (in the site's [language](configuration.md#interface-text)). Its page title and
breadcrumbs remain unchanged.

On a narrow screen the sidebar folds away behind a **Menu** button, the tabs
wrap, and a tab's menu spans the width of the screen.

## Colour themes

Every page has a light and a dark colour scheme and follows the reader's
system setting. The theme picker in the header lets a reader choose system,
light or dark instead; the choice is kept in the browser (`localStorage`,
under `ckdocs-theme`) for the whole site, and diagrams and the landing page's
image switch with it. Code highlighting, alerts and all other colours follow
the chosen theme. Without scripting the picker is not shown and the system
setting applies.

## Branding

| Setting | Effect |
|---|---|
| `site.title`, `site.brand` | The name in the header (`brand` when it should differ from the title) and in every browser tab. |
| `site.logo` | An image (SVG or PNG) before the brand, about 1.8 rem high. |
| `site.favicon` | The browser tab icon (SVG, PNG or ICO); it may be the logo itself. |
| `site.links` | Up to 8 header links; `icon: github` shows a round GitHub icon instead of the text, which then names it for screen readers and as a tooltip. |
| `site.footer` | A line of footer text, such as a copyright notice. |

In a portal each site may name its own logo, brand and favicon, which then
replace the portal's on its pages; see [Several sites in one build](portals.md#the-header-of-a-sites-pages).

## Your own stylesheet

`site.stylesheet` names a CSS file that is copied into the site and linked
after the built-in styles, so its rules win. The built-in styles are built on
seven colour tokens; changing them recolours everything, in both themes:

| Token | Used for |
|---|---|
| `--bg` | The page background. |
| `--panel` | The header, code blocks, cards and menus. |
| `--text` | Body text. |
| `--muted` | Secondary text: bylines, the outline, footers, captions. |
| `--line` | Borders and rules. |
| `--accent` | Links, the current tab and page, buttons. |
| `--tint` | Highlighted backgrounds: the current page in the sidebar, inline code, alerts. |

A dark value applies when the reader's system is dark and they have not
chosen light, or when they have chosen dark:

```css
:root { --accent: #8a3ffc; }
@media (prefers-color-scheme: dark) {
  html:not([data-theme=light]) { --accent: #be95ff; }
}
:root[data-theme=dark] { --accent: #be95ff; }
```

Every `<body>` carries `layout-doc`, `layout-page` or `layout-home`, plus
the classes a page's front matter names in `page_class`, so a stylesheet can
style one kind of page or one page alone. The page parts carry stable classes
(`header.site`, `aside.sidebar`, `nav.outline`, `nav.crumbs`, `article`,
`nav.pager`, `footer.site`) and the Markdown constructs theirs (`.alert-tip`,
`.tabbed`, `.code-block`, `.footnotes`, …).

## Diagrams and code

A [Mermaid diagram](mermaid.md) carries two small buttons: one shows it in a
full-window dialog (Escape closes it), the other opens the drawn SVG on its
own, which also works without scripting. Each [code block](code.md) gets a
copy button when the reader points at it.

## Printing

Printed pages leave out the header, sidebar, pager and footer, show every
panel of a tab set one after the other, and drop the copy buttons.

## Accessibility and pages without scripts

- A **Skip to content** link comes first on every page; the header, the
  sidebar's page list, the outline, the breadcrumbs and the pager are labelled
  navigation regions, in the page's language.
- Every control works with the keyboard: tabs of a tab set are radio buttons
  (arrow keys), menus are native disclosures, Cmd/Ctrl+K opens search and
  Escape closes it or the open menu.
- Focus is always visible, and the controls' animations stop when the
  reader's system asks for reduced motion.
- The pages are complete without scripting: navigation, menus, tabs, the
  outline and every link work as plain HTML and CSS. The small inline scripts
  add only what needs one — search, the theme picker, copy buttons and the
  diagram dialog — and their controls do not appear without them.
