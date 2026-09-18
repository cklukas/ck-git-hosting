// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

namespace ckgit {

// The stylesheet every ckdocs page embeds: the dashboard's colour tokens and
// typography (copied, so the tool never depends on web/style.hpp), a
// header / sidebar + main / footer grid, tabs, native <details> groups, the
// "On this page" outline, alerts, task lists, syntax highlighting, and a
// sidebar that collapses behind a checkbox toggle under 700px. No script.
inline constexpr std::string_view kDocsStyles = R"CSS(
:root{color-scheme:light dark;--bg:#fafbfd;--panel:#fff;--text:#18212b;--muted:#59636d;--line:#d9e0e7;--accent:#145ea8;--tint:#edf4fb}
@media(prefers-color-scheme:dark){:root{--bg:#131920;--panel:#1b242e;--text:#dce5ed;--muted:#a5b1be;--line:#344251;--accent:#7db8ee;--tint:#263e53}}
*{box-sizing:border-box}
body{margin:0;font:15px/1.6 system-ui,sans-serif;background:var(--bg);color:var(--text);min-height:100vh;display:flex;flex-direction:column}
a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
a:focus-visible,summary:focus-visible,input:focus-visible{outline:3px solid var(--accent);outline-offset:3px}
.skip-link{position:absolute;left:1rem;top:-5rem;background:var(--panel);padding:.6rem;z-index:2}.skip-link:focus{top:.5rem}
header.site{display:flex;align-items:center;gap:1.5rem;flex-wrap:wrap;padding:.9rem 2rem;border-bottom:1px solid var(--line);background:var(--panel)}
.brand{display:inline-flex;align-items:center;gap:.6rem;font-weight:700;font-size:1.15rem;color:var(--text);letter-spacing:-.02em}.brand:hover{text-decoration:none}.brand img{height:1.8rem;width:auto}
nav.tabs{display:flex;gap:1.2rem;flex-wrap:wrap}nav.tabs a{padding:.3rem 0;color:var(--text)}nav.tabs a[aria-current=page]{font-weight:700;color:var(--accent);text-decoration:underline;text-underline-offset:.5rem;text-decoration-thickness:2px}
nav.links{margin-left:auto;display:flex;gap:1rem;flex-wrap:wrap;font-size:.92rem}
.page{display:grid;grid-template-columns:minmax(230px,17rem) minmax(0,1fr);gap:2.5rem;max-width:1440px;width:100%;margin:0 auto;padding:1.5rem 2rem;flex:1}
aside.sidebar{font-size:.92rem;align-self:start;position:sticky;top:1rem;max-height:calc(100vh - 2rem);overflow:auto}
.side-title{font-size:.78rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:1.4rem 0 .4rem;font-weight:600}aside .side-title:first-child{margin-top:0}
aside ul{list-style:none;margin:0;padding:0}aside li{margin:0}aside li ul{padding-left:.9rem;margin-left:.5rem;border-left:1px solid var(--line)}
aside a{display:block;padding:.28rem .55rem;border-radius:.3rem;color:var(--text)}aside a:hover{background:var(--tint);text-decoration:none}aside a[aria-current=page]{background:var(--tint);color:var(--accent);font-weight:600}
aside details>summary{cursor:pointer;padding:.28rem .55rem;font-weight:600;list-style:none;color:var(--text)}aside details>summary::-webkit-details-marker{display:none}
aside details>summary:before{content:"▸ ";color:var(--muted)}aside details[open]>summary:before{content:"▾ "}aside details>summary a{display:inline;padding:0}
.nav-switch,.nav-toggle{display:none}
main{min-width:0}
nav.crumbs ol{list-style:none;display:flex;flex-wrap:wrap;gap:.4rem;margin:0 0 1rem;padding:0;font-size:.9rem;color:var(--muted)}nav.crumbs li+li:before{content:"›";margin-right:.4rem}
article{overflow-wrap:anywhere}article>:first-child{margin-top:0}
h1{font-size:2rem;letter-spacing:-.03em;line-height:1.2;margin:.2rem 0 1rem}h2{font-size:1.4rem;margin-top:2.2rem;padding-top:.7rem;border-top:1px solid var(--line)}h3{font-size:1.1rem;margin-top:1.6rem}
h1,h2,h3,h4,h5,h6{scroll-margin-top:1rem}article :target{background:var(--tint)}
code,pre{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:.88em}
pre{overflow:auto;white-space:pre;tab-size:4;background:var(--panel);border:1px solid var(--line);border-radius:.4rem;padding:1rem}
code{background:var(--tint);padding:.1rem .3rem;border-radius:.25rem}pre code{background:none;padding:0}
blockquote{border-left:3px solid var(--line);margin:1rem 0;padding-left:1rem;color:var(--muted)}
img{max-width:100%;height:auto}
.table-wrap,article table{display:block;overflow:auto;max-width:100%}article table{border-collapse:collapse}th,td{text-align:left;padding:.5rem .7rem;border:1px solid var(--line);vertical-align:top}th{background:var(--tint)}
/* article's own overflow-wrap:anywhere (below) exists for long unbroken
   strings such as URLs and paths; inside a table cell it instead fractures
   ordinary short code words like "version" once a narrow column squeezes
   them, so cells opt back out -- the table already scrolls horizontally. */
article th,article td{overflow-wrap:normal;word-break:normal}
hr{border:0;border-top:1px solid var(--line);margin:2rem 0}
.alert{--alert:var(--accent);margin:1rem 0;padding:.7rem 1rem;border-left:4px solid var(--alert);border-radius:.25rem;background:var(--tint)}.alert>:last-child{margin-bottom:0}.alert-title{margin:0 0 .35rem;font-weight:600;color:var(--alert)}
.alert-note{--alert:#0969da}.alert-tip{--alert:#1a7f37}.alert-important{--alert:#8250df}.alert-warning{--alert:#9a6700}.alert-caution{--alert:#cf222e}
.contains-task-list{list-style:none;padding-left:.4rem}.task-list-item input{margin:0 .45rem 0 0;vertical-align:-.1rem}
.hl-c{color:#59636e;font-style:italic}.hl-s{color:#0a3069}.hl-k{color:#cf222e}.hl-n{color:#0550ae}.hl-a{color:#116329}.hl-p{color:#8250df}.hl-v{color:#953800}
@media(prefers-color-scheme:dark){.alert-note{--alert:#4493f8}.alert-tip{--alert:#3fb950}.alert-important{--alert:#ab7df8}.alert-warning{--alert:#d29922}.alert-caution{--alert:#f85149}.hl-c{color:#9198a1}.hl-s{color:#a5d6ff}.hl-k{color:#ff7b72}.hl-n{color:#79c0ff}.hl-a{color:#7ee787}.hl-p{color:#d2a8ff}.hl-v{color:#ffa657}}
nav.pager{display:flex;justify-content:space-between;gap:1rem;margin:2.5rem 0 0;padding-top:1rem;border-top:1px solid var(--line);font-size:.95rem}nav.pager a{max-width:48%}nav.pager .next{margin-left:auto;text-align:right}nav.pager small{display:block;color:var(--muted);font-size:.8rem}
footer.site{border-top:1px solid var(--line);padding:1rem 2rem;font-size:.85rem;color:var(--muted);display:flex;flex-wrap:wrap;gap:.5rem 1.5rem;max-width:1440px;width:100%;margin:0 auto}
.site-index ul{padding-left:1.2rem}.site-index li{margin:.2rem 0}.site-index .index-headings{color:var(--muted);font-size:.9rem}
@media(max-width:700px){header.site{padding:.8rem 1rem;gap:.6rem 1rem}nav.links{margin-left:0}.page{display:block;padding:1rem}
.nav-switch{display:block;position:absolute;opacity:0;width:1px;height:1px}.nav-toggle{display:inline-block;border:1px solid var(--line);border-radius:.4rem;padding:.45rem .8rem;margin-bottom:1rem;background:var(--panel);cursor:pointer}.nav-switch:focus-visible+.nav-toggle{outline:3px solid var(--accent)}
aside.sidebar{display:none;position:static;max-height:none;margin-bottom:1.5rem;padding:1rem;border:1px solid var(--line);border-radius:.5rem;background:var(--panel)}.nav-switch:checked~aside.sidebar{display:block}footer.site{padding:1rem}}
@media print{header.site,aside.sidebar,nav.pager,footer.site,.nav-toggle,.skip-link{display:none}.page{display:block;padding:0}}
)CSS";

}  // namespace ckgit
