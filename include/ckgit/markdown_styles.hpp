// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

namespace ckgit {

// The rules for what renderMarkdown writes beyond plain CommonMark --
// footnotes, definition lists, `:::` containers, code titles and marked
// lines, tabs and code groups, math, and the highlighter's newer token
// classes -- shared by the dashboard's stylesheet and every ckdocs page so a
// document looks the same in both. It uses the colour tokens both define
// (--bg, --panel, --text, --muted, --line, --accent, --tint). Tabs are radio
// inputs and labels: the checked input's panel shows, so they need no
// script; a set holds at most 16 tabs (kMaximumTabs in markdown.cpp), which
// is how many panel rules there are below.
inline constexpr std::string_view kMarkdownStyles = R"CSS(
.footnote-ref{font-size:.75em;line-height:0;vertical-align:super}.footnote-ref a{padding:0 .1em}
section.footnotes{margin-top:2.5rem;padding-top:.5rem;border-top:1px solid var(--line);font-size:.9rem;color:var(--muted)}section.footnotes ol{padding-left:1.4rem}section.footnotes li:target{background:var(--tint)}.footnote-backref{text-decoration:none}
:is(.readme,article) dl{display:block;margin:1rem 0}:is(.readme,article) dt{font-weight:600;margin-top:.75rem}:is(.readme,article) dd{margin:.2rem 0 0 1.5rem}:is(.readme,article) dd>p:first-child{margin-top:0}
.alert-info{--alert:var(--muted)}.alert-danger{--alert:#cf222e}
details.details-container{margin:1rem 0;padding:.5rem 1rem;border:1px solid var(--line);border-radius:.4rem}
.code-block{margin:1rem 0;border:1px solid var(--line);border-radius:.4rem;overflow:hidden;background:var(--panel)}.code-block>pre{margin:0;border:0;border-radius:0}
.code-title{padding:.4rem 1rem;font:600 .82rem/1.4 ui-monospace,SFMono-Regular,Consolas,monospace;color:var(--muted);background:var(--tint);border-bottom:1px solid var(--line)}
pre.code-lines{padding:1rem 0}pre.code-lines>code{display:inline-block;min-width:100%}pre.code-lines .line{display:inline-block;min-width:100%;padding:0 1rem;position:relative}
.line-number{display:inline-block;min-width:2.2em;margin-right:1.2em;text-align:right;color:var(--muted);opacity:.75;user-select:none;-webkit-user-select:none}
pre.code-lines .line.highlighted{background:#f6c94326;box-shadow:inset 3px 0 #d4a72c}
pre.has-diff .line{padding-left:1.8rem}pre.code-lines .line.diff:before{position:absolute;left:.65rem;font-weight:700}
pre.code-lines .line.diff.add{background:#2da44e1f}pre.code-lines .line.diff.add:before{content:"+";color:#1a7f37}
pre.code-lines .line.diff.remove{background:#cf222e1a;opacity:.85}pre.code-lines .line.diff.remove:before{content:"\2212";color:#cf222e}
pre.code-lines .line.warning{background:#d2992229}pre.code-lines .line.error{background:#f8514926}
pre.has-focused .line:not(.focused){filter:blur(.08rem);opacity:.6;transition:filter .3s,opacity .3s}pre.has-focused:hover .line:not(.focused){filter:none;opacity:1}
.tabbed{margin:1rem 0;border:1px solid var(--line);border-radius:.4rem;background:var(--panel);overflow:hidden}
.tabbed>.tab-input{position:absolute;opacity:0;width:1px;height:1px;pointer-events:none}
.tabbed>.tab-label{display:inline-block;padding:.45rem 1rem;cursor:pointer;color:var(--muted);font-size:.9rem;border-bottom:2px solid transparent;user-select:none}
.tabbed>.tab-label:hover{color:var(--text)}.tabbed>.tab-input:checked+.tab-label{color:var(--accent);border-bottom-color:var(--accent);font-weight:600}
.tabbed>.tab-input:focus-visible+.tab-label{outline:3px solid var(--accent);outline-offset:-3px}
.tab-panels{border-top:1px solid var(--line)}.tab-panel{display:none;padding:0 1rem}.tab-panel>:first-child{margin-top:1rem}
.code-group .tab-panel{padding:0}.code-group .tab-panel>:first-child{margin-top:0}.code-group .tab-panel>pre,.code-group .tab-panel>.code-wrap>pre{margin:0;border:0;border-radius:0}
.tabbed>.tab-input:nth-of-type(1):checked~.tab-panels>.tab-panel:nth-child(1),.tabbed>.tab-input:nth-of-type(2):checked~.tab-panels>.tab-panel:nth-child(2),.tabbed>.tab-input:nth-of-type(3):checked~.tab-panels>.tab-panel:nth-child(3),.tabbed>.tab-input:nth-of-type(4):checked~.tab-panels>.tab-panel:nth-child(4),
.tabbed>.tab-input:nth-of-type(5):checked~.tab-panels>.tab-panel:nth-child(5),.tabbed>.tab-input:nth-of-type(6):checked~.tab-panels>.tab-panel:nth-child(6),.tabbed>.tab-input:nth-of-type(7):checked~.tab-panels>.tab-panel:nth-child(7),.tabbed>.tab-input:nth-of-type(8):checked~.tab-panels>.tab-panel:nth-child(8),
.tabbed>.tab-input:nth-of-type(9):checked~.tab-panels>.tab-panel:nth-child(9),.tabbed>.tab-input:nth-of-type(10):checked~.tab-panels>.tab-panel:nth-child(10),.tabbed>.tab-input:nth-of-type(11):checked~.tab-panels>.tab-panel:nth-child(11),.tabbed>.tab-input:nth-of-type(12):checked~.tab-panels>.tab-panel:nth-child(12),
.tabbed>.tab-input:nth-of-type(13):checked~.tab-panels>.tab-panel:nth-child(13),.tabbed>.tab-input:nth-of-type(14):checked~.tab-panels>.tab-panel:nth-child(14),.tabbed>.tab-input:nth-of-type(15):checked~.tab-panels>.tab-panel:nth-child(15),.tabbed>.tab-input:nth-of-type(16):checked~.tab-panels>.tab-panel:nth-child(16){display:block}
p.tab-label{font-weight:600;margin:1.2rem 0 .3rem}
math{font-family:"STIX Two Math","Cambria Math","Latin Modern Math","Noto Sans Math",math;font-size:1.08em}
math[display=block]{display:block math;margin:1rem 0;padding:.25rem 0;overflow-x:auto;overflow-y:hidden}
merror{color:#cf222e;background:#cf222e14;outline:1px dashed #cf222e80}
.code-wrap{position:relative}.code-block{position:relative}
.code-copy{position:absolute;top:.45rem;right:.45rem;z-index:1;padding:.2rem .55rem;font:500 .78rem/1.4 system-ui,sans-serif;color:var(--muted);background:var(--panel);border:1px solid var(--line);border-radius:.35rem;cursor:pointer;opacity:0;transition:opacity .15s}
.code-block>.code-copy{top:.3rem}.code-wrap:hover>.code-copy,.code-block:hover>.code-copy,.code-copy:focus-visible,.code-copy.copied{opacity:1}.code-copy:hover{color:var(--text)}.code-copy.copied{color:var(--accent)}
@media(hover:none){.code-copy{opacity:1}}
.hl-t{color:#953800}.hl-f{color:#6639ba}.hl-i{color:#116329;background:#2da44e1a}.hl-d{color:#82071e;background:#cf222e14}
@media(prefers-color-scheme:dark){html:not([data-theme=light]) .alert-danger{--alert:#f85149}html:not([data-theme=light]) pre.code-lines .line.highlighted{background:#bb800926;box-shadow:inset 3px 0 #d29922}html:not([data-theme=light]) pre.code-lines .line.diff.add:before{color:#3fb950}html:not([data-theme=light]) pre.code-lines .line.diff.remove:before{color:#f85149}html:not([data-theme=light]) merror{color:#f85149}html:not([data-theme=light]) .hl-t{color:#ffa657}html:not([data-theme=light]) .hl-f{color:#d2a8ff}html:not([data-theme=light]) .hl-i{color:#aff5b4;background:#2ea0431f}html:not([data-theme=light]) .hl-d{color:#ffdcd7;background:#f8514926}}
:root[data-theme=dark] .alert-danger{--alert:#f85149}:root[data-theme=dark] pre.code-lines .line.highlighted{background:#bb800926;box-shadow:inset 3px 0 #d29922}:root[data-theme=dark] pre.code-lines .line.diff.add:before{color:#3fb950}:root[data-theme=dark] pre.code-lines .line.diff.remove:before{color:#f85149}:root[data-theme=dark] merror{color:#f85149}:root[data-theme=dark] .hl-t{color:#ffa657}:root[data-theme=dark] .hl-f{color:#d2a8ff}:root[data-theme=dark] .hl-i{color:#aff5b4;background:#2ea0431f}:root[data-theme=dark] .hl-d{color:#ffdcd7;background:#f8514926}
@media print{.code-copy{display:none}.tab-panel{display:block!important}.tabbed>.tab-label{display:none}}
)CSS";

}  // namespace ckgit
