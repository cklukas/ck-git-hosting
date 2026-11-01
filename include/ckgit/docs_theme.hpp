// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

namespace ckgit {

// The stylesheet every ckdocs page embeds: the dashboard's colour tokens and
// typography (copied, so the tool never depends on web/style.hpp), a
// header / sidebar + main / footer grid, tabs, native <details> groups, the
// "On this page" outline, alerts, task lists, syntax highlighting, a
// portal's <details> site selector and welcome-page cards, the language
// menu, the home layout's hero and feature cards, tag lists, and a sidebar
// that collapses behind a checkbox toggle under 700px. No script. The rules
// for what Markdown itself renders (code groups, footnotes, math, ...) are
// kMarkdownStyles, which every page embeds after these.
inline constexpr std::string_view kDocsStyles = R"CSS(
:root{color-scheme:light dark;--bg:#fafbfd;--panel:#fff;--text:#18212b;--muted:#59636d;--line:#d9e0e7;--accent:#145ea8;--tint:#edf4fb}
@media(prefers-color-scheme:dark){:root{--bg:#131920;--panel:#1b242e;--text:#dce5ed;--muted:#a5b1be;--line:#344251;--accent:#7db8ee;--tint:#263e53}}
*{box-sizing:border-box}
body{margin:0;font:15px/1.6 system-ui,sans-serif;background:var(--bg);color:var(--text);min-height:100vh;display:flex;flex-direction:column}
a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
a:focus-visible,summary:focus-visible,input:focus-visible{outline:3px solid var(--accent);outline-offset:3px}
.skip-link{position:absolute;left:1rem;top:-5rem;background:var(--panel);padding:.6rem;z-index:2}.skip-link:focus{top:.5rem}
header.site{position:relative;display:flex;align-items:center;gap:1rem;flex-wrap:wrap;padding:.9rem 2rem;border-bottom:1px solid var(--line);background:var(--panel)}
.brand{display:inline-flex;align-items:center;gap:.6rem;font-weight:700;font-size:1.15rem;color:var(--text);letter-spacing:-.02em}.brand:hover{text-decoration:none}.brand img{height:1.8rem;width:auto}
.site-select{position:relative;font-size:.92rem}.site-select>summary{display:inline-flex;align-items:center;gap:.4rem;padding:.25rem .75rem;border:1px solid var(--line);border-radius:1rem;background:var(--panel);color:var(--text);cursor:pointer;list-style:none;white-space:nowrap}
.site-select>summary::-webkit-details-marker{display:none}.site-select>summary:after{content:"▾";color:var(--muted);font-size:.8em}.site-select[open]>summary:after{content:"▴"}.site-select>summary:hover{background:var(--tint)}
.site-select ul{position:absolute;left:0;top:calc(100% + .35rem);z-index:6;min-width:100%;width:max-content;max-width:min(22rem,90vw);margin:0;padding:.3rem;list-style:none;background:var(--panel);border:1px solid var(--line);border-radius:.6rem;box-shadow:0 .6rem 1.8rem #0003}
.site-select li a{display:block;padding:.4rem .7rem;border-radius:.4rem;color:var(--text)}.site-select li a:hover,.site-select li a:focus-visible{background:var(--tint);text-decoration:none}.site-select li a[aria-current=true]{color:var(--accent);font-weight:600}
nav.tabs{display:flex;gap:.8rem;flex-wrap:wrap;align-items:center}.tab-link,.tab-menu>summary{padding:.4rem .15rem;color:var(--text);border-bottom:2px solid transparent}.tab-link[aria-current=page],.tab-menu.is-active>summary{font-weight:600;color:var(--accent);border-bottom-color:var(--accent)}
nav.links{margin-left:auto;display:flex;gap:1rem;flex-wrap:wrap;font-size:.92rem}.search~nav.links{margin-left:0}
.page{display:grid;grid-template-columns:minmax(230px,17rem) minmax(0,1fr);gap:2.5rem;max-width:1440px;width:100%;margin:0 auto;padding:1.5rem 2rem;flex:1}
aside.sidebar{font-size:.92rem;align-self:start;position:sticky;top:1rem;max-height:calc(100vh - 2rem);overflow:auto}
.side-title{font-size:.78rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:1.4rem 0 .4rem;font-weight:600}aside .side-title:first-child{margin-top:0}
aside ul{list-style:none;margin:0;padding:0}aside li{margin:0}aside li ul{padding-left:.8rem;margin:0}
aside a{display:block;padding:.28rem .55rem;border-radius:.3rem;color:var(--text)}aside a:hover{background:var(--tint);text-decoration:none}aside a[aria-current=page]{background:var(--tint);color:var(--accent);font-weight:600}
.nav-group{margin:.4rem 0;padding:0;border:0;background:none}.nav-group>ul{margin:0;padding-left:.8rem}.nav-group>summary{display:flex;align-items:center;justify-content:space-between;gap:.75rem;padding:.35rem .55rem;border-radius:.35rem;cursor:pointer;font-weight:600;list-style:none;color:var(--text)}.nav-group>summary::-webkit-details-marker{display:none}.nav-group>summary:hover{background:var(--tint)}.nav-group[open]>summary{margin:0 0 .15rem}.nav-chevron{display:block;width:14px;height:14px;flex-shrink:0;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;color:var(--muted)}.nav-group[open]>summary .nav-chevron{transform:rotate(90deg)}
.nav-switch,.nav-toggle{display:none}
main{min-width:0}
nav.crumbs ol{list-style:none;display:flex;flex-wrap:wrap;gap:.4rem;margin:0 0 1rem;padding:0;font-size:.9rem;color:var(--muted)}nav.crumbs li+li:before{content:"›";margin-right:.4rem}
article{overflow-wrap:anywhere}article>:first-child{margin-top:0}
h1{font-size:2rem;letter-spacing:-.03em;line-height:1.2;margin:.2rem 0 1rem}h2{font-size:1.4rem;margin-top:2.2rem;padding-top:.7rem;border-top:1px solid var(--line)}h3{font-size:1.1rem;margin-top:1.6rem}
.byline{margin:0 0 1.25rem;color:var(--muted);font-size:.9rem}h1+.byline{margin-top:-.5rem}
h1,h2,h3,h4,h5,h6{scroll-margin-top:1rem}article :target{background:var(--tint)}
code,pre{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:.88em}
pre{overflow:auto;white-space:pre;tab-size:4;background:var(--panel);border:1px solid var(--line);border-radius:.4rem;padding:1rem}
code{background:var(--tint);padding:.1rem .3rem;border-radius:.25rem}pre code{background:none;padding:0}
blockquote{border-left:3px solid var(--line);margin:1rem 0;padding-left:1rem;color:var(--muted)}
kbd{font:.85em ui-monospace,SFMono-Regular,Consolas,monospace;padding:.1em .4em;border:1px solid var(--line);border-bottom-width:2px;border-radius:.3em;background:var(--tint)}
article details{margin:1rem 0;padding:.5rem 1rem;border:1px solid var(--line);border-radius:.4rem}article details>summary{cursor:pointer;font-weight:600}article details[open]>summary{margin-bottom:.5rem}
img{max-width:100%;height:auto}
figure.mermaid{margin:1.5rem 0;padding:1rem;overflow:auto;border:1px solid var(--line);border-radius:.6rem}
figure.mermaid img{display:block;margin:auto}
article img[src*="images/web-"]{display:block;margin:1.25rem 0;border:1px solid var(--line);border-radius:.75rem;box-shadow:0 .25rem .9rem #0002}
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
@media(prefers-color-scheme:dark){html:not([data-theme=light]) .alert-note{--alert:#4493f8}html:not([data-theme=light]) .alert-tip{--alert:#3fb950}html:not([data-theme=light]) .alert-important{--alert:#ab7df8}html:not([data-theme=light]) .alert-warning{--alert:#d29922}html:not([data-theme=light]) .alert-caution{--alert:#f85149}html:not([data-theme=light]) .hl-c{color:#9198a1}html:not([data-theme=light]) .hl-s{color:#a5d6ff}html:not([data-theme=light]) .hl-k{color:#ff7b72}html:not([data-theme=light]) .hl-n{color:#79c0ff}html:not([data-theme=light]) .hl-a{color:#7ee787}html:not([data-theme=light]) .hl-p{color:#d2a8ff}html:not([data-theme=light]) .hl-v{color:#ffa657}}
)CSS" R"CSS(:root[data-theme=dark] .alert-note{--alert:#4493f8}:root[data-theme=dark] .alert-tip{--alert:#3fb950}:root[data-theme=dark] .alert-important{--alert:#ab7df8}:root[data-theme=dark] .alert-warning{--alert:#d29922}:root[data-theme=dark] .alert-caution{--alert:#f85149}:root[data-theme=dark] .hl-c{color:#9198a1}:root[data-theme=dark] .hl-s{color:#a5d6ff}:root[data-theme=dark] .hl-k{color:#ff7b72}:root[data-theme=dark] .hl-n{color:#79c0ff}:root[data-theme=dark] .hl-a{color:#7ee787}:root[data-theme=dark] .hl-p{color:#d2a8ff}:root[data-theme=dark] .hl-v{color:#ffa657}
nav.pager{display:flex;justify-content:space-between;gap:1rem;margin:2.5rem 0 0;padding-top:1rem;border-top:1px solid var(--line);font-size:.95rem}nav.pager a{max-width:48%}nav.pager .next{margin-left:auto;text-align:right}nav.pager small{display:block;color:var(--muted);font-size:.8rem}
.search{position:relative;margin-left:auto;display:flex;align-items:center;gap:.5rem}.search [hidden]{display:none!important}.search input{font:inherit;padding:.35rem .8rem;border:1px solid var(--line);border-radius:1rem;background:var(--panel);color:var(--text);width:11rem;max-width:40vw}.search input:focus-visible{outline:3px solid var(--accent);outline-offset:2px}
.search-results{position:absolute;right:0;top:calc(100% + .4rem);width:22rem;max-width:85vw;max-height:70vh;overflow:auto;background:var(--panel);border:1px solid var(--line);border-radius:.6rem;box-shadow:0 .6rem 1.8rem #0003;padding:.35rem;z-index:5;text-align:left}
.search-result{display:block;padding:.5rem .6rem;border-radius:.4rem;color:var(--text)}.search-result:hover,.search-result:focus-visible{background:var(--tint);text-decoration:none}.search-result b{color:var(--accent)}.search-result span{display:block;margin-top:.15rem;font-size:.85rem;color:var(--muted);font-weight:400}
@media(max-width:700px){.search input{width:9rem;max-width:60vw}.search-results{width:90vw;right:-1rem}}
footer.site{border-top:1px solid var(--line);padding:1rem 2rem;font-size:.85rem;color:var(--muted);display:flex;flex-wrap:wrap;gap:.5rem 1.5rem;max-width:1440px;width:100%;margin:0 auto}
.page.portal{display:block;max-width:1040px}.portal-description{font-size:1.1rem;color:var(--muted);margin:0 0 1.5rem}
.site-cards{list-style:none;margin:2rem 0 0;padding:0;display:grid;grid-template-columns:repeat(auto-fill,minmax(16rem,1fr));gap:1rem}.site-cards li{padding:1rem 1.2rem;border:1px solid var(--line);border-radius:.6rem;background:var(--panel)}.site-cards a{font-weight:600;font-size:1.1rem}.site-cards p{margin:.4rem 0 0;color:var(--muted)}
.site-index ul{padding-left:1.2rem}.site-index li{margin:.2rem 0}.site-index .index-headings{color:var(--muted);font-size:.9rem}.site-index .index-description{margin:.1rem 0 .3rem;color:var(--muted)}
@media(max-width:700px){header.site{padding:.8rem 1rem;gap:.6rem 1rem}nav.links{margin-left:0}.page{display:block;padding:1rem}
.nav-switch{display:block;position:absolute;opacity:0;width:1px;height:1px}.nav-toggle{display:inline-block;border:1px solid var(--line);border-radius:.4rem;padding:.45rem .8rem;margin-bottom:1rem;background:var(--panel);cursor:pointer}.nav-switch:focus-visible+.nav-toggle{outline:3px solid var(--accent)}
.site-select ul{max-width:calc(100vw - 2rem)}.site-cards{grid-template-columns:minmax(0,1fr)}
aside.sidebar{display:none;position:static;max-height:none;margin-bottom:1.5rem;padding:1rem;border:1px solid var(--line);border-radius:.5rem;background:var(--panel)}.nav-switch:checked~aside.sidebar{display:block}footer.site{padding:1rem}}
/* Data-only home pages and native section menus. */
.tab-group{display:flex;align-items:center;position:relative}
.tab-menu{margin:0;padding:0;border:0;background:none}.tab-menu>summary{display:flex;align-items:center;gap:.4rem;list-style:none;cursor:pointer;font-weight:inherit;white-space:nowrap}.tab-menu>summary::-webkit-details-marker{display:none}.tab-menu[open]>summary{margin:0}.tab-menu>summary:hover,.tab-link:hover{color:var(--accent);text-decoration:none}.tab-menu>summary .nav-chevron{width:12px;height:12px;transform:rotate(90deg)}.tab-menu[open]>summary .nav-chevron{transform:rotate(-90deg)}
.tab-menu>ul{position:absolute;left:0;top:100%;z-index:7;list-style:none;min-width:16rem;max-width:min(24rem,90vw);max-height:75vh;overflow:auto;background:var(--panel);border:1px solid var(--line);border-radius:.65rem;box-shadow:0 .6rem 1.8rem #0003;padding:.5rem;margin:.3rem 0}
.tab-menu li{list-style:none}.tab-menu li a{display:block;padding:.4rem .6rem;border-radius:.35rem;color:var(--text)}.tab-menu li a:hover,.tab-menu li a[aria-current=page]{background:var(--tint);text-decoration:none}.tab-menu li a[aria-current=page]{color:var(--accent);font-weight:600}.tab-menu li ul{padding-left:.8rem}.tab-menu .nav-group>summary{padding:.4rem .6rem}
.page.landing{display:block;max-width:1440px;padding-top:3rem}.landing>.sidebar,.landing>.nav-toggle,.landing>.nav-switch,.landing main>.pager{display:none}.landing article>h2,.landing article>p,.landing article>ul,.landing article>figure{max-width:70rem;margin-left:auto;margin-right:auto}
.home-hero{display:grid;grid-template-columns:minmax(0,1.15fr) minmax(0,1fr);align-items:center;gap:3rem;padding:1rem 0 3rem}.home-hero h1{font-size:clamp(2.4rem,4vw,4rem);line-height:1.15;letter-spacing:-.04em}.home-name{display:block;color:var(--accent);margin-bottom:.5rem}.home-tagline{font-size:clamp(1.1rem,1.7vw,1.6rem);line-height:1.55;color:var(--muted)}.home-image img{display:block;width:100%;max-height:30rem;object-fit:contain}.home-actions{display:flex;flex-wrap:wrap;gap:.75rem;margin-top:1.8rem}.home-action{display:inline-block;padding:.6rem 1.1rem;border-radius:2rem;background:var(--tint);font-weight:600}.home-action.brand{background:var(--accent);color:var(--bg);font-size:inherit}.home-features{display:grid;grid-template-columns:repeat(auto-fit,minmax(15rem,1fr));gap:1rem;margin:1rem 0 3rem}.home-feature{padding:1.5rem;border-radius:.8rem;background:var(--tint)}.home-feature h2{font-size:1rem;margin:0;padding:0;border:0}.home-feature p{color:var(--muted);margin:.6rem 0 0}
@media(max-width:700px){.tab-group{position:static}.tab-menu>ul{left:1rem;right:1rem;top:auto;max-width:none}}
@media(max-width:800px){.home-hero{grid-template-columns:minmax(0,1fr);gap:1.5rem}.home-image{grid-row:1}.home-image img{max-height:18rem}.page.landing{padding-top:1.5rem}}

:root[data-theme=light]{color-scheme:light;--bg:#fafbfd;--panel:#fff;--text:#18212b;--muted:#59636d;--line:#d9e0e7;--accent:#145ea8;--tint:#edf4fb}
:root[data-theme=dark]{color-scheme:dark;--bg:#131920;--panel:#1b242e;--text:#dce5ed;--muted:#a5b1be;--line:#344251;--accent:#7db8ee;--tint:#263e53}
.theme-toggle,.diagram-expand,.diagram-close{font:inherit;color:var(--text);background:var(--panel);border:1px solid var(--line);border-radius:.5rem;padding:.3rem .6rem;cursor:pointer}button:focus-visible{outline:3px solid var(--accent);outline-offset:3px}
figure.mermaid{position:relative}.diagram-expand{display:block;margin:0 0 .5rem auto}.diagram-dialog{color:var(--text);background:var(--bg);border:1px solid var(--line);border-radius:.7rem;padding:1rem;width:calc(100vw - 2rem);height:calc(100vh - 2rem);max-width:none;max-height:none}.diagram-dialog::backdrop{background:#0009}.diagram-close{display:block;margin-left:auto}.diagram-dialog picture{display:flex;align-items:center;justify-content:center;height:calc(100% - 3rem);overflow:auto}.diagram-dialog img{width:100%;height:100%;object-fit:contain;max-width:none}.diagram-open{font-size:.85rem}

.theme-picker:not([hidden]){display:block;position:relative;margin:0;padding:0;border:0;background:none;flex-shrink:0}
)CSS" R"CSS(.theme-trigger{display:flex;align-items:center;justify-content:center;list-style:none;width:30px;height:30px;padding:6px;border:1px solid var(--line);border-radius:50%;background:var(--tint);color:var(--muted);cursor:pointer}.theme-trigger::-webkit-details-marker{display:none}.theme-picker[open]>.theme-trigger{margin-bottom:0;color:var(--accent)}.theme-trigger:hover{color:var(--text);background:var(--panel)}
.theme-options{position:absolute;right:0;top:calc(100% + .45rem);z-index:8;display:flex;gap:2px;padding:3px;border:1px solid var(--line);border-radius:999px;background:var(--panel);box-shadow:0 .25rem 1rem #0002}
.theme-toggle,.icon-button{display:inline-flex;align-items:center;justify-content:center;width:30px;height:30px;padding:6px;border:0;border-radius:50%;background:transparent;color:var(--muted);cursor:pointer;line-height:1;transition:background .15s,color .15s,box-shadow .15s}
.theme-trigger svg,.theme-toggle svg,.icon-button svg{display:block;width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round;pointer-events:none}
.header-icon{background:var(--tint);border:1px solid var(--line)}.github-link svg{fill:currentColor;stroke:none}
.theme-toggle:hover,.icon-button:hover{background:var(--panel);color:var(--text);text-decoration:none}.theme-toggle[aria-pressed=true]{background:var(--panel);color:var(--accent);box-shadow:0 1px 4px #0002}
figure.mermaid{padding-top:3rem}.diagram-tools{display:flex;gap:3px;position:absolute;right:.55rem;top:.5rem}.diagram-tools .icon-button{margin:0;background:var(--tint);border-radius:.45rem}.diagram-tools .icon-button:hover{background:var(--line);color:var(--text)}.diagram-close{display:flex;width:34px;height:34px;margin:0 0 .5rem auto;background:var(--tint);border-radius:.5rem}
@media(prefers-reduced-motion:reduce){.theme-toggle,.icon-button{transition:none}}
.site-select,.lang-select{margin:0;padding:0;border:0;border-radius:0}.lang-select{position:relative;font-size:.92rem}.lang-select>summary{display:inline-flex;align-items:center;gap:.35rem;padding:.25rem .7rem;border:1px solid var(--line);border-radius:1rem;cursor:pointer;list-style:none;white-space:nowrap;color:var(--text)}
.lang-select>summary::-webkit-details-marker{display:none}.lang-select>summary:hover{background:var(--tint)}
.lang-select ul{position:absolute;right:0;top:calc(100% + .35rem);z-index:6;min-width:100%;width:max-content;margin:0;padding:.3rem;list-style:none;background:var(--panel);border:1px solid var(--line);border-radius:.6rem;box-shadow:0 .6rem 1.8rem #0003}
.lang-select li a{display:block;padding:.4rem .7rem;border-radius:.4rem;color:var(--text)}.lang-select li a:hover,.lang-select li a:focus-visible{background:var(--tint);text-decoration:none}.lang-select li a[aria-current=true]{color:var(--accent);font-weight:600}
.page.no-sidebar{grid-template-columns:minmax(0,1fr);max-width:1100px}.layout-home .page.no-sidebar{max-width:1240px}
.fallback-notice{margin:0 0 1.25rem;padding:.6rem .9rem;border:1px dashed var(--line);border-radius:.4rem;color:var(--muted);font-size:.92rem}
nav.page-tags{display:flex;flex-wrap:wrap;align-items:baseline;gap:.5rem;margin:2rem 0 0;font-size:.9rem;color:var(--muted)}nav.page-tags ul{display:flex;flex-wrap:wrap;gap:.4rem;margin:0;padding:0;list-style:none}
nav.page-tags a,.tag-list a{display:inline-block;padding:.1rem .6rem;border:1px solid var(--line);border-radius:1rem;background:var(--panel)}nav.page-tags a:hover,.tag-list a:hover{background:var(--tint);text-decoration:none}
.tag-list{list-style:none;padding:0;display:flex;flex-wrap:wrap;gap:.6rem 1rem}.tag-count{color:var(--muted);font-size:.85rem}.tagged-pages li{margin:.4rem 0}
[dir=rtl] nav.links,[dir=rtl] .search{margin-left:0;margin-right:auto}[dir=rtl] aside li ul,[dir=rtl] .nav-group>ul{padding-left:0;padding-right:.8rem}[dir=rtl] .tab-menu>ul{left:auto;right:0}[dir=rtl] blockquote,[dir=rtl] .alert{border-left:0;border-right:4px solid var(--alert,var(--line));padding-left:1rem;padding-right:1rem}[dir=rtl] nav.pager .next{margin-left:0;margin-right:auto;text-align:left}[dir=rtl] .lang-select ul,[dir=rtl] .search-results{right:auto;left:0}
@media(max-width:700px){.lang-select ul{right:auto;left:0}}
@media print{header.site,.site-select,.lang-select,.theme-picker,aside.sidebar,nav.pager,footer.site,.nav-toggle,.skip-link{display:none}.page{display:block;padding:0}}
.home-feature-icon{display:block;font-size:1.6rem;line-height:1;margin-bottom:.6rem}a.home-feature{display:block;color:var(--text);border:1px solid transparent}a.home-feature:hover{border-color:var(--accent);text-decoration:none}
)CSS";

// Progressive enhancement: native disclosure menus need no script; these
// controls add persisted colour choice and a keyboard-accessible diagram dialog.
inline constexpr std::string_view kDocsUiScript = R"JS(
(function(){
  var root=document.documentElement,picker=document.querySelector('.theme-picker'),buttons=document.querySelectorAll('[data-theme-choice]');
  var menus=document.querySelectorAll('header.site .tab-menu');
  menus.forEach(function(menu){menu.addEventListener('toggle',function(){if(menu.open)menus.forEach(function(other){if(other!==menu)other.open=false;});});});
  document.addEventListener('click',function(event){menus.forEach(function(menu){if(menu.open&&!menu.contains(event.target))menu.open=false;});});
  document.addEventListener('keydown',function(event){if(event.key==='Escape')menus.forEach(function(menu){if(menu.open){menu.open=false;menu.querySelector('summary').focus();}});});
  function theme(value){
    if(value==='light'||value==='dark')root.setAttribute('data-theme',value);
    else root.removeAttribute('data-theme');
    document.querySelectorAll('source[media][data-theme-media]').forEach(function(source){
      source.media=value==='dark'?'all':value==='light'?'not all':source.getAttribute('data-theme-media');
    });
    buttons.forEach(function(button){
      var selected=button.getAttribute('data-theme-choice')===value;
      button.setAttribute('aria-pressed',String(selected));
      if(selected&&picker){var trigger=picker.querySelector('summary'),label=(picker.getAttribute('data-label')||'Colour theme')+': '+(button.title||value);trigger.replaceChildren(button.querySelector('svg').cloneNode(true));trigger.setAttribute('aria-label',label);trigger.title=label;}
    });
  }
  document.querySelectorAll('source[media="(prefers-color-scheme: dark)"]').forEach(function(source){source.setAttribute('data-theme-media',source.media);});
  var choice='system';try{choice=localStorage.getItem('ckdocs-theme')||'system';}catch(e){}
  if(['system','light','dark'].indexOf(choice)<0)choice='system';
  theme(choice);
  if(picker){picker.hidden=false;buttons.forEach(function(button){button.onclick=function(){choice=button.getAttribute('data-theme-choice');theme(choice);picker.open=false;picker.querySelector('summary').focus();try{localStorage.setItem('ckdocs-theme',choice);}catch(e){}};});}
  document.querySelectorAll('figure.mermaid').forEach(function(figure){
    var picture=figure.querySelector('picture');if(!picture||!window.HTMLDialogElement)return;
    var body=document.body,expandLabel=body.getAttribute('data-diagram-expand')||'Expand diagram',closeLabel=body.getAttribute('data-diagram-close')||'Close diagram';
    var expand=document.createElement('button');expand.type='button';expand.className='icon-button diagram-expand';expand.setAttribute('aria-label',expandLabel);expand.title=expandLabel;expand.innerHTML='<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M8 3H3v5m0-5 7 7m6-7h5v5m0-5-7 7M3 16v5h5m-5 0 7-7m11 2v5h-5m5 0-7-7"/></svg>';
    expand.onclick=function(){
      var dialog=document.createElement('dialog');dialog.className='diagram-dialog';dialog.setAttribute('aria-label',expandLabel);
      var close=document.createElement('button');close.type='button';close.className='icon-button diagram-close';close.setAttribute('aria-label',closeLabel);close.title=closeLabel;close.innerHTML='<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m6 6 12 12M18 6 6 18"/></svg>';
      close.onclick=function(){dialog.close();};dialog.appendChild(close);dialog.appendChild(picture.cloneNode(true));document.body.appendChild(dialog);
      dialog.addEventListener('close',function(){dialog.remove();expand.focus();},{once:true});dialog.showModal();close.focus();
    };figure.querySelector('.diagram-tools').appendChild(expand);
  });
})();
)JS";

// The opt-in search box's own behaviour (site.search / `search: true`):
// fetches the generated search-index.json lazily (on first focus or
// keystroke, not on page load), filters it client-side, and lists matches
// linking straight to their page and section; a page whose title,
// description or tags match is listed first. Inline, dependency-free, and
// needs no nonce -- ckdocs pages carry no CSP that would require one. The
// input itself starts `hidden`; this script is the only thing that reveals
// it, so a visitor with scripting disabled never sees a non-functional box
// (the page's own <noscript> link to the site index covers them instead).
inline constexpr std::string_view kDocsSearchScript = R"JS(
(function(){
  var input=document.getElementById('ckdocs-search');
  if(!input)return;
  var form=input.closest('form'),toggle=form.querySelector('.search-toggle');
  if(!toggle)return;
  toggle.hidden=false;
  var openLabel=toggle.getAttribute('data-open-label')||'Open search',closeLabel=toggle.getAttribute('data-close-label')||'Close search';
  function open(){input.hidden=false;toggle.setAttribute('aria-expanded','true');toggle.setAttribute('aria-label',closeLabel);input.focus();load().then(function(){render(input.value)});}
  function close(focus){input.hidden=true;toggle.setAttribute('aria-expanded','false');toggle.setAttribute('aria-label',openLabel);clear();if(focus)toggle.focus();}
  toggle.addEventListener('click',function(){if(input.hidden)open();else close(true);});
  document.addEventListener('keydown',function(event){
    if((event.metaKey||event.ctrlKey)&&event.key.toLowerCase()==='k'){event.preventDefault();open();}
    else if(event.key==='Escape'&&!input.hidden){event.preventDefault();close(true);}
  });
  var root=input.getAttribute('data-index').replace('search-index.json','');
  var results=document.getElementById('ckdocs-search-results');
  var data=null,loading=null;
  function load(){
    if(data||loading)return loading;
    loading=fetch(input.getAttribute('data-index')).then(function(r){return r.json()})
      .then(function(j){data=j.pages||[]})['catch'](function(){data=[]});
    return loading;
  }
  function clear(){results.textContent='';results.hidden=true}
  function add(href,text,summary){
    var link=document.createElement('a');
    link.className='search-result';
    link.href=root+href;
    var title=document.createElement('b');
    title.textContent=text;
    var excerpt=document.createElement('span');
    excerpt.textContent=summary;
    link.appendChild(title);
    link.appendChild(excerpt);
    results.appendChild(link);
  }
  function render(query){
    clear();
    if(input.hidden||!query||!data)return;
    var q=query.toLowerCase(),shown=0;
    for(var i=0;i<data.length&&shown<20;i++){
      var page=data[i],sections=page.sections||[],desc=page.description||'',tags=(page.tags||[]).join(', ');
      var whole=(desc||tags)&&(page.title+' '+desc+' '+tags).toLowerCase().indexOf(q)!==-1;
      if(whole){add(page.url,page.title,desc||tags);shown++}
      for(var j=0;j<sections.length&&shown<20;j++){
        var s=sections[j];
        if(whole&&!s.heading)continue;
        if((s.heading+' '+s.excerpt).toLowerCase().indexOf(q)===-1)continue;
        add(page.url+(s.anchor?'#'+s.anchor:''),s.heading?page.title+' – '+s.heading:page.title,s.excerpt||desc);
        shown++;
      }
    }
    results.hidden=shown===0;
  }
  input.addEventListener('focus',load);
  input.addEventListener('input',function(){
    var query=input.value;
    load().then(function(){render(query)});
  });
  document.addEventListener('click',function(e){
    if(!form.contains(e.target)&&!input.hidden)close(false);
  });
})();
)JS";

// Every page's small behaviour script: a copy button on each code block of
// the article. The buttons are made here, so a visitor without scripting
// never sees one that does nothing. The copied text is the code as written:
// without line numbers, and without lines a diff marks as removed. Where the
// browser refuses to copy, the code is selected instead. The labels come
// from the page's own data attributes (its language's text).
inline constexpr std::string_view kDocsCodeScript = R"JS(
(function(){
  var body=document.body,label=body.getAttribute('data-copy-label')||'Copy code',done=body.getAttribute('data-copied-label')||'Copied';
  function text(pre){
    var code=pre.querySelector('code')||pre,lines=code.querySelectorAll('.line');
    if(!lines.length)return code.textContent;
    var out=[];
    for(var i=0;i<lines.length;i++){
      if(lines[i].classList.contains('remove'))continue;
      var copy=lines[i].cloneNode(true),number=copy.querySelector('.line-number');
      if(number)number.parentNode.removeChild(number);
      out.push(copy.textContent);
    }
    return out.join('\n')+'\n';
  }
  // The selection-based copy, for pages the Clipboard API is not open to
  // (file://, or a browser that refuses it).
  function legacy(value){
    return new Promise(function(resolve,reject){
      var area=document.createElement('textarea');
      area.value=value;area.setAttribute('readonly','');area.style.position='fixed';area.style.opacity='0';
      body.appendChild(area);area.select();
      try{document.execCommand('copy')?resolve():reject(new Error('copy'))}catch(e){reject(e)}
      body.removeChild(area);
    });
  }
  function write(value){
    if(navigator.clipboard&&window.isSecureContext)return navigator.clipboard.writeText(value)['catch'](function(){return legacy(value)});
    return legacy(value);
  }
  var blocks=document.querySelectorAll('article pre');
  for(var i=0;i<blocks.length;i++){
    (function(pre){
      var holder=pre.parentNode;
      if(!holder.classList||!holder.classList.contains('code-block')){
        holder=document.createElement('div');holder.className='code-wrap';
        pre.parentNode.insertBefore(holder,pre);holder.appendChild(pre);
      }
      var button=document.createElement('button'),timer=0;
      button.type='button';button.className='code-copy';button.textContent=label;
      button.addEventListener('click',function(){
        write(text(pre)).then(function(){
          button.textContent=done;button.classList.add('copied');
          clearTimeout(timer);timer=setTimeout(function(){button.textContent=label;button.classList.remove('copied')},2000);
        },function(){
          // Copying is refused here: select the code, so the reader's own
          // copy shortcut takes it.
          var range=document.createRange(),selection=window.getSelection();
          range.selectNodeContents(pre.querySelector('code')||pre);selection.removeAllRanges();selection.addRange(range);
        });
      });
      holder.appendChild(button);
    })(blocks[i]);
  }
})();
)JS";

}  // namespace ckgit
