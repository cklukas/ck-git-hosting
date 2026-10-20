// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ckgit {

// Server-side TeX math for documentation pages: the LaTeX math notation that
// scientific and engineering documents are written in, converted to static
// MathML Core, which current browsers lay out natively. There is no script on
// the page and no macro language: one pass over the source with recursion
// bounded by nesting, and output that grows linearly with the input.
//
// What it understands:
//   - letters (one <mi> each), numbers with a decimal point (<mn>), ASCII
//     operators and relations, `'` primes, `{}` groups, `^`/`_` scripts in
//     either order, `%` comments, `~` and the escapes \{ \} \$ \% \# \& \_ \|;
//   - Greek letters (with the var- forms and upright capitals), the usual
//     symbols, binary operators, relations, negated relations, arrows, dots
//     and delimiters, and Unicode characters typed directly;
//   - \frac \dfrac \tfrac \cfrac, \binom \dbinom \tbinom, \over \atop
//     \choose, \sqrt and \sqrt[n];
//   - big operators (\sum \prod \int \oint \bigcup …) with limits under and
//     over in display style and as scripts inline, \limits \nolimits
//     \displaylimits; named functions (\sin … \lim \max \Pr) as upright
//     identifiers followed by U+2061 FUNCTION APPLICATION, \operatorname and
//     \operatorname*, \bmod \pmod \mod \pod, \mathop \mathbin \mathrel …;
//   - \left \middle \right with every common delimiter, \big \Big \bigg
//     \Bigg and their l/r/m forms;
//   - accents (\hat \widehat \bar \overline \underline \vec \tilde \dot
//     \ddot \acute \grave \breve \check \mathring, arrows over and under),
//     \overbrace \underbrace (labels via ^ and _), \overset \underset
//     \stackrel, \xrightarrow and its relatives, \substack;
//   - fonts \mathrm \mathit \mathbf \mathbfit \boldsymbol \bm \mathsf \mathtt
//     \mathcal \mathscr \mathfrak \mathbb (and the switches \rm \bf \it …),
//     emitted as the Unicode mathematical alphanumeric characters, because
//     MathML Core honours mathvariant="normal" only;
//   - \text \textrm \textbf \textit \textsf \texttt \emph \mbox as <mtext>,
//     with `--`, `---`, TeX quotes, `~` and nested `$…$` math;
//   - spacing \, \: \; \! \  \quad \qquad \hspace \kern \mkern and friends,
//     \phantom \hphantom \vphantom \smash \strut, \displaystyle \textstyle
//     \scriptstyle \scriptscriptstyle, \color (a switch) \textcolor
//     \colorbox with a validated colour, \not;
//   - the environments matrix pmatrix bmatrix Bmatrix vmatrix Vmatrix
//     smallmatrix cases dcases rcases drcases aligned align(*) alignat(*)
//     alignedat split gathered gather(*) multline(*) equation(*) array and
//     subarray, and `\\` rows in a formula of its own.
//
// Renderings that cannot be exact in MathML Core, chosen to stay honest:
//   - \boxed and \fbox draw their content without the frame (Core has no
//     element that draws a border, and no style attribute is emitted);
//   - \tag{x} (display only) follows its row after a 2em space as "(x)"
//     rather than sitting at the right margin; \tag* omits the parentheses;
//     nothing is numbered automatically, \label \nonumber \notag are ignored;
//   - array column rules (`|`, `:`) and \hline are ignored; columns align;
//   - \text may contain `$…$`, which renders as math inside the text run;
//   - \textbf and similar use the mathematical bold, italic, sans-serif or
//     monospace letters, since Core has no other way to restyle <mtext>;
//   - \! and other negative spaces emit a negative <mspace> width, which a
//     browser that clamps negative widths renders as no space at all.
//
// Unknown commands, unbalanced braces, \left without \right, misplaced `&`
// and the like become an <merror> in place and a message in `errors`, so a
// documentation build can warn and the rest of the formula still renders.
// Input over kMaximumMathSourceBytes, or nested deeper than
// kMaximumMathDepth groups, becomes a single <merror> for the whole formula.

inline constexpr std::size_t kMaximumMathSourceBytes = 16 * 1024;
// Nested groups, arguments, environments, \left…\right pairs and \text
// bodies together.
inline constexpr std::size_t kMaximumMathDepth = 64;
// `errors` keeps this many messages, then one saying that more were dropped.
inline constexpr std::size_t kMaximumMathErrors = 16;

struct MathRendering {
  std::string html;
  std::vector<std::string> errors;  // empty when the formula was understood in full
};

// One <math> element — <math display="block"> when `display` — wrapping
// <semantics><mrow>…</mrow><annotation encoding="application/x-tex">source
// </annotation></semantics>. Only MathML Core elements are emitted, every
// text node and attribute value is HTML-escaped, invalid UTF-8 and control
// bytes become U+FFFD, and the only attributes are ones the converter itself
// writes from fixed values (display, mathvariant, stretchy, fence, form,
// movablelimits, accent, accentunder, lspace, rspace, minsize, maxsize, width,
// height, depth, linethickness, columnalign, columnspacing, scriptlevel,
// displaystyle, encoding) or from validated input (lengths; mathcolor and
// mathbackground from a CSS colour name, #rgb or #rrggbb). An oversize
// source is not copied into the annotation. Never throws.
MathRendering renderTexMath(std::string_view tex, bool display);

}  // namespace ckgit
