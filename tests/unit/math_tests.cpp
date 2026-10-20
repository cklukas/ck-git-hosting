// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/math.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("math: " + message);
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// The rendering between <semantics><mrow> and </mrow><annotation…>, which is
// what a formula's assertions are about; the wrapper is checked separately.
std::string body(std::string_view tex, bool display = false) {
  const auto html = ckgit::renderTexMath(tex, display).html;
  const std::string open = "<semantics><mrow>";
  const auto start = html.find(open);
  const auto end = html.rfind("</mrow><annotation");
  if (start == std::string::npos || end == std::string::npos || end < start) return "(malformed) " + html;
  return html.substr(start + open.size(), end - start - open.size());
}

void expectBody(std::string_view tex, std::string_view expected, const std::string& what, bool display = false) {
  const auto actual = body(tex, display);
  require(actual == expected, what + " — " + std::string(tex) + "\n  expected " + std::string(expected) + "\n  actual   " +
                                  actual);
}

// The single error a formula reports.
std::string onlyError(std::string_view tex, bool display = false) {
  const auto result = ckgit::renderTexMath(tex, display);
  return result.errors.size() == 1 ? result.errors.front() : "(" + std::to_string(result.errors.size()) + " errors)";
}

void testWrapper() {
  require(ckgit::renderTexMath("x^2", false).html ==
              "<math><semantics><mrow><msup><mi>x</mi><mn>2</mn></msup></mrow>"
              "<annotation encoding=\"application/x-tex\">x^2</annotation></semantics></math>",
          "inline math is one <math> with the source as a TeX annotation");
  require(ckgit::renderTexMath("x", true).html ==
              "<math display=\"block\"><semantics><mrow><mi>x</mi></mrow>"
              "<annotation encoding=\"application/x-tex\">x</annotation></semantics></math>",
          "display math is a block <math>");
  require(ckgit::renderTexMath("x", false).errors.empty(), "an understood formula reports no errors");
}

void testEscaping() {
  require(ckgit::renderTexMath("a<b \\text{\"&'}", false).html ==
              "<math><semantics><mrow><mi>a</mi><mo>&lt;</mo><mi>b</mi><mtext>&quot;&amp;’</mtext></mrow>"
              "<annotation encoding=\"application/x-tex\">a&lt;b \\text{&quot;&amp;&#39;}</annotation></semantics></math>",
          "< & \" are escaped in the MathML and in the annotation");
  expectBody("a > b", "<mi>a</mi><mo>&gt;</mo><mi>b</mi>", "> is escaped");
  expectBody("\\color{#F00}{x}", "<mstyle mathcolor=\"#f00\"><mi>x</mi></mstyle>",
             "a colour switch applies to what follows; hex colours are normalised");
  const auto bad = ckgit::renderTexMath("x\x01y\xff", false);
  require(contains(bad.html, "<mi>\uFFFD</mi>") && contains(bad.html, "x\uFFFDy\uFFFD</annotation>") &&
              !contains(bad.html, "\x01"),
          "control bytes and invalid UTF-8 become U+FFFD in both the math and the annotation");
  const auto injected = ckgit::renderTexMath("\\color{red\" onclick=\"x}{a} \\text{<script>}", false);
  require(!contains(injected.html, "onclick=\"") && !contains(injected.html, "<script") &&
              contains(injected.html, "&lt;script&gt;") && injected.errors.size() == 1,
          "an invalid colour is refused and text cannot carry markup");
}

void testBasics() {
  expectBody("x^2", "<msup><mi>x</mi><mn>2</mn></msup>", "superscript");
  expectBody("a_i^2", "<msubsup><mi>a</mi><mi>i</mi><mn>2</mn></msubsup>", "subscript and superscript");
  expectBody("a^2_i", "<msubsup><mi>a</mi><mi>i</mi><mn>2</mn></msubsup>", "scripts in either order");
  expectBody("x^{10}_{n+1}", "<msubsup><mi>x</mi><mrow><mi>n</mi><mo>+</mo><mn>1</mn></mrow><mn>10</mn></msubsup>",
             "braced scripts");
  expectBody("x^12", "<msup><mi>x</mi><mn>1</mn></msup><mn>2</mn>", "an unbraced script is one token");
  expectBody("\\frac{a}{b}", "<mfrac><mi>a</mi><mi>b</mi></mfrac>", "fraction");
  expectBody("\\frac12", "<mfrac><mn>1</mn><mn>2</mn></mfrac>", "fraction with single-token arguments");
  expectBody("\\sqrt[3]{x}", "<mroot><mi>x</mi><mn>3</mn></mroot>", "root with an index");
  expectBody("\\sqrt{x+1}", "<msqrt><mrow><mi>x</mi><mo>+</mo><mn>1</mn></mrow></msqrt>", "square root");
  expectBody("3.14 + .5 + 1,000",
             "<mn>3.14</mn><mo>+</mo><mn>.5</mn><mo>+</mo><mn>1</mn><mo>,</mo><mn>000</mn>",
             "numbers keep their decimal point; a comma separates");
  expectBody("ab", "<mi>a</mi><mi>b</mi>", "each letter is its own identifier");
  expectBody("f'(x) + g''", "<msup><mi>f</mi><mo>′</mo></msup><mo stretchy=\"false\">(</mo><mi>x</mi>"
                            "<mo stretchy=\"false\">)</mo><mo>+</mo><msup><mi>g</mi><mo>″</mo></msup>",
             "primes are superscripts; brackets outside \\left do not stretch");
  expectBody("x'^2", "<msup><mi>x</mi><mrow><mo>′</mo><mn>2</mn></mrow></msup>", "a prime then a superscript");
  expectBody("-a = -b + c - d",
             "<mo form=\"prefix\">−</mo><mi>a</mi><mo>=</mo><mo form=\"prefix\">−</mo><mi>b</mi><mo>+</mo><mi>c</mi>"
             "<mo>−</mo><mi>d</mi>",
             "a sign at the start or after a relation is unary");
  expectBody("(-1)", "<mo stretchy=\"false\">(</mo><mo form=\"prefix\">−</mo><mn>1</mn><mo stretchy=\"false\">)</mo>",
             "a sign after an opening bracket is unary");
  expectBody("{}^{14}C", "<msup><mrow></mrow><mn>14</mn></msup><mi>C</mi>", "an empty group as a base");
  expectBody("a{=}b", "<mi>a</mi><mrow><mo>=</mo></mrow><mi>b</mi>", "a braced operator is ordinary");
  expectBody("a % comment\n+ b", "<mi>a</mi><mo>+</mo><mi>b</mi>", "comments run to the end of the line");
  expectBody("{a \\over b}", "<mfrac><mi>a</mi><mi>b</mi></mfrac>", "\\over");
  expectBody("{n \\choose k}",
             "<mrow><mo fence=\"true\" form=\"prefix\" stretchy=\"true\">(</mo><mfrac linethickness=\"0\"><mi>n</mi>"
             "<mi>k</mi></mfrac><mo fence=\"true\" form=\"postfix\" stretchy=\"true\">)</mo></mrow>",
             "\\choose");
  expectBody("\\binom{n}{k}",
             "<mrow><mo fence=\"true\" form=\"prefix\" stretchy=\"true\">(</mo><mfrac linethickness=\"0\"><mi>n</mi>"
             "<mi>k</mi></mfrac><mo fence=\"true\" form=\"postfix\" stretchy=\"true\">)</mo></mrow>",
             "\\binom");
  expectBody("\\dfrac12", "<mstyle displaystyle=\"true\" scriptlevel=\"0\"><mfrac><mn>1</mn><mn>2</mn></mfrac></mstyle>",
             "\\dfrac");
  expectBody("\\tfrac12", "<mstyle displaystyle=\"false\" scriptlevel=\"0\"><mfrac><mn>1</mn><mn>2</mn></mfrac></mstyle>",
             "\\tfrac");
  expectBody("\\alpha\\Gamma\\varGamma\\nabla\\infty",
             "<mi>α</mi><mi mathvariant=\"normal\">Γ</mi><mi>Γ</mi><mi mathvariant=\"normal\">∇</mi><mi>∞</mi>",
             "Greek capitals and nabla are upright, their var- forms italic");
  expectBody("\\{ \\} \\$ \\% \\# \\& \\_ \\backslash \\|",
             "<mo stretchy=\"false\">{</mo><mo stretchy=\"false\">}</mo><mi>$</mi><mi>%</mi><mi>#</mi><mi>&amp;</mi>"
             "<mi>_</mi><mo>\\</mo><mo stretchy=\"false\">‖</mo>",
             "escaped specials");
  expectBody("αβ ≤ ⟨x⟩ x̂", "<mi>α</mi><mi>β</mi><mo>≤</mo><mo stretchy=\"false\">⟨</mo><mi>x</mi>"
                           "<mo stretchy=\"false\">⟩</mo><mi>x̂</mi>",
             "Unicode typed directly, combining marks kept with their letter");
  expectBody("x \\leq y \\to z \\in A \\cup B \\times C",
             "<mi>x</mi><mo>≤</mo><mi>y</mi><mo>→</mo><mi>z</mi><mo>∈</mo><mi>A</mi><mo>∪</mo><mi>B</mi><mo>×</mo>"
             "<mi>C</mi>",
             "relations, arrows and binary operators");
  expectBody("a \\cdots b \\ldots", "<mi>a</mi><mo>⋯</mo><mi>b</mi><mo>…</mo>", "dots");
}

void testOperators() {
  expectBody("\\sum_{i=1}^n i",
             "<msubsup><mo>∑</mo><mrow><mi>i</mi><mo>=</mo><mn>1</mn></mrow><mi>n</mi></msubsup><mi>i</mi>",
             "inline sums take their limits as scripts");
  expectBody("\\sum_{i=1}^n i",
             "<munderover><mo>∑</mo><mrow><mi>i</mi><mo>=</mo><mn>1</mn></mrow><mi>n</mi></munderover><mi>i</mi>",
             "display sums take their limits under and over", true);
  expectBody("\\sum\\limits_a^b", "<munderover><mo movablelimits=\"false\">∑</mo><mi>a</mi><mi>b</mi></munderover>",
             "\\limits keeps the limits under and over inline");
  expectBody("\\sum\\nolimits_a^b", "<msubsup><mo>∑</mo><mi>a</mi><mi>b</mi></msubsup>",
             "\\nolimits puts them at the side in display style", true);
  expectBody("\\int_0^1", "<msubsup><mo>∫</mo><mn>0</mn><mn>1</mn></msubsup>", "integrals keep side limits", true);
  expectBody("\\frac{\\sum_a^b}{2}", "<mfrac><msubsup><mo>∑</mo><mi>a</mi><mi>b</mi></msubsup><mn>2</mn></mfrac>",
             "a fraction's parts are in text style", true);
  expectBody("\\sin x + \\sin(x)",
             "<mi>sin</mi><mo rspace=\"0.1667em\">\u2061</mo><mi>x</mi><mo>+</mo><mi>sin</mi><mo>\u2061</mo>"
             "<mo stretchy=\"false\">(</mo><mi>x</mi><mo stretchy=\"false\">)</mo>",
             "functions apply with a thin space before an ordinary argument and none before a bracket");
  expectBody("\\lim_{n\\to\\infty} a_n",
             "<munder><mi>lim</mi><mrow><mi>n</mi><mo>→</mo><mi>∞</mi></mrow></munder><mo rspace=\"0.1667em\">\u2061</mo>"
             "<msub><mi>a</mi><mi>n</mi></msub>",
             "\\lim has limits under it in display style", true);
  expectBody("\\lim_{x} f", "<msub><mi>lim</mi><mi>x</mi></msub><mo rspace=\"0.1667em\">\u2061</mo><mi>f</mi>",
             "and at the side inline");
  expectBody("\\log_2 n", "<msub><mi>log</mi><mn>2</mn></msub><mo rspace=\"0.1667em\">\u2061</mo><mi>n</mi>",
             "a function with a subscript");
  expectBody("\\operatorname{sgn} x", "<mi>sgn</mi><mo rspace=\"0.1667em\">\u2061</mo><mi>x</mi>", "\\operatorname");
  expectBody("\\operatorname*{argmax}_x f",
             "<munder><mi>argmax</mi><mi>x</mi></munder><mo rspace=\"0.1667em\">\u2061</mo><mi>f</mi>",
             "\\operatorname* takes limits", true);
  expectBody("\\sin -x", "<mi>sin</mi><mo rspace=\"0.1667em\">\u2061</mo><mo form=\"prefix\">−</mo><mi>x</mi>",
             "a sign after a function is unary");
  expectBody("a \\bmod b", "<mi>a</mi><mo lspace=\"0.2222em\" rspace=\"0.2222em\">mod</mo><mi>b</mi>", "\\bmod");
  expectBody("a \\pmod{n}",
             "<mi>a</mi><mrow><mspace width=\"0.4444em\"></mspace><mo stretchy=\"false\">(</mo><mi>mod</mi>"
             "<mspace width=\"0.3333em\"></mspace><mi>n</mi><mo stretchy=\"false\">)</mo></mrow>",
             "\\pmod");
  expectBody("\\liminf", "<mi>lim\u2009inf</mi>", "two-word operator names");
}

void testFences() {
  expectBody("\\left(\\frac{a}{b}\\right)",
             "<mrow><mo fence=\"true\" form=\"prefix\" stretchy=\"true\">(</mo><mfrac><mi>a</mi><mi>b</mi></mfrac>"
             "<mo fence=\"true\" form=\"postfix\" stretchy=\"true\">)</mo></mrow>",
             "\\left and \\right stretch");
  expectBody("\\left\\{ x \\middle| x>0 \\right.",
             "<mrow><mo fence=\"true\" form=\"prefix\" stretchy=\"true\">{</mo><mi>x</mi>"
             "<mo fence=\"true\" stretchy=\"true\" lspace=\"0.1667em\" rspace=\"0.1667em\">|</mo><mi>x</mi><mo>&gt;</mo>"
             "<mn>0</mn></mrow>",
             "\\middle, and `.` as no delimiter");
  expectBody("\\left\\langle x \\right\\rVert",
             "<mrow><mo fence=\"true\" form=\"prefix\" stretchy=\"true\">⟨</mo><mi>x</mi>"
             "<mo fence=\"true\" form=\"postfix\" stretchy=\"true\">‖</mo></mrow>",
             "named delimiters");
  expectBody("\\left< x \\right>",
             "<mrow><mo fence=\"true\" form=\"prefix\" stretchy=\"true\">⟨</mo><mi>x</mi>"
             "<mo fence=\"true\" form=\"postfix\" stretchy=\"true\">⟩</mo></mrow>",
             "< and > as delimiters are angle brackets");
  expectBody("\\left(x\\right)^2",
             "<msup><mrow><mo fence=\"true\" form=\"prefix\" stretchy=\"true\">(</mo><mi>x</mi>"
             "<mo fence=\"true\" form=\"postfix\" stretchy=\"true\">)</mo></mrow><mn>2</mn></msup>",
             "a fenced group takes scripts");
  expectBody("\\bigl( x \\Bigr]",
             "<mo form=\"prefix\" stretchy=\"true\" minsize=\"1.2em\" maxsize=\"1.2em\">(</mo><mi>x</mi>"
             "<mo form=\"postfix\" stretchy=\"true\" minsize=\"1.8em\" maxsize=\"1.8em\">]</mo>",
             "\\big sizes");
}

void testFonts() {
  expectBody("\\mathbb{R}", "<mi>ℝ</mi>", "double-struck R is the letterlike ℝ");
  expectBody("\\mathbb{A1}", "<mrow><mi>𝔸</mi><mn>𝟙</mn></mrow>", "double-struck letters and digits");
  expectBody("\\mathcal{L} \\mathcal{A} \\mathscr{B}", "<mi>ℒ</mi><mi>𝒜</mi><mi>ℬ</mi>", "script letters");
  expectBody("\\mathfrak{gC}", "<mi>𝔤ℭ</mi>", "a run of letters in one font is one identifier");
  expectBody("\\mathbf{v} + \\mathbf{12}", "<mi>𝐯</mi><mo>+</mo><mn>𝟏𝟐</mn>", "bold letters and digits");
  expectBody("\\boldsymbol{\\alpha\\Gamma x}", "<mrow><mi>𝜶</mi><mi>𝚪</mi><mi>𝒙</mi></mrow>",
             "\\boldsymbol: bold italic, capital Greek bold upright");
  expectBody("\\mathit{h}", "<mi>ℎ</mi>", "italic h is the Planck constant sign");
  expectBody("\\mathrm{d}x", "<mi mathvariant=\"normal\">d</mi><mi>x</mi>", "an upright single letter");
  expectBody("\\mathrm{max}", "<mi>max</mi>", "an upright word");
  expectBody("\\mathsf{A}\\mathtt{b}", "<mi>𝖠</mi><mi>𝚋</mi>", "sans-serif and monospace");
  expectBody("{\\bf x} y", "<mi>𝐱</mi><mi>y</mi>", "a font switch lasts to the end of its group");
}

void testText() {
  expectBody("\\text{if } x", "<mtext>if\u00a0</mtext><mi>x</mi>", "an edge space survives as U+00A0");
  expectBody("\\text{for all $x>0$}",
             "<mrow><mtext>for all\u00a0</mtext><mrow><mi>x</mi><mo>&gt;</mo><mn>0</mn></mrow></mrow>",
             "text may contain math");
  expectBody("\\text{a -- b --- ``c'' d~e}", "<mtext>a – b — “c” d\u00a0e</mtext>", "dashes, quotes and ties");
  expectBody("\\text{x   y}", "<mtext>x y</mtext>", "white space collapses");
  expectBody("\\textbf{ab} \\textit{c} \\texttt{d}", "<mtext>𝐚𝐛</mtext><mtext>𝑐</mtext><mtext>𝚍</mtext>",
             "text fonts use the mathematical alphabets");
  expectBody("\\text{50\\% \\textbf{x} y}", "<mrow><mtext>50%\u00a0</mtext><mtext>𝐱</mtext><mtext>\u00a0y</mtext></mrow>",
             "escapes and nested text commands in text");
}

void testEnvironments() {
  expectBody("\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}",
             "<mrow><mo fence=\"true\" form=\"prefix\" stretchy=\"true\">(</mo><mtable><mtr><mtd><mi>a</mi></mtd>"
             "<mtd><mi>b</mi></mtd></mtr><mtr><mtd><mi>c</mi></mtd><mtd><mi>d</mi></mtd></mtr></mtable>"
             "<mo fence=\"true\" form=\"postfix\" stretchy=\"true\">)</mo></mrow>",
             "pmatrix");
  expectBody("\\begin{matrix} 1 \\\\ \\end{matrix}", "<mtable><mtr><mtd><mn>1</mn></mtd></mtr></mtable>",
             "a trailing \\\\ adds no row");
  expectBody("\\begin{vmatrix} a \\end{vmatrix}",
             "<mrow><mo fence=\"true\" form=\"prefix\" stretchy=\"true\">|</mo><mtable><mtr><mtd><mi>a</mi></mtd></mtr>"
             "</mtable><mo fence=\"true\" form=\"postfix\" stretchy=\"true\">|</mo></mrow>",
             "vmatrix");
  expectBody("f = \\begin{cases} 1 & x > 0 \\\\ -1 & \\text{otherwise} \\end{cases}",
             "<mi>f</mi><mo>=</mo><mrow><mo fence=\"true\" form=\"prefix\" stretchy=\"true\">{</mo>"
             "<mtable columnspacing=\"1em\"><mtr><mtd columnalign=\"left\"><mn>1</mn></mtd><mtd columnalign=\"left\">"
             "<mi>x</mi><mo>&gt;</mo><mn>0</mn></mtd></mtr><mtr><mtd columnalign=\"left\"><mo form=\"prefix\">−</mo>"
             "<mn>1</mn></mtd><mtd columnalign=\"left\"><mtext>otherwise</mtext></mtd></mtr></mtable></mrow>",
             "cases");
  expectBody("\\begin{aligned} a &= b + c \\\\ &+ d \\end{aligned}",
             "<mtable displaystyle=\"true\" columnspacing=\"0em\"><mtr><mtd columnalign=\"right\"><mi>a</mi></mtd>"
             "<mtd columnalign=\"left\"><mo>=</mo><mi>b</mi><mo>+</mo><mi>c</mi></mtd></mtr><mtr>"
             "<mtd columnalign=\"right\"></mtd><mtd columnalign=\"left\"><mo form=\"infix\">+</mo><mi>d</mi></mtd>"
             "</mtr></mtable>",
             "aligned: right/left pairs, a sign opening the right half stays binary");
  expectBody("\\begin{array}{l|cr} 1 & 2 & 3 \\\\ \\hline 4 & 5 & 6 \\end{array}",
             "<mtable><mtr><mtd columnalign=\"left\"><mn>1</mn></mtd><mtd><mn>2</mn></mtd><mtd columnalign=\"right\">"
             "<mn>3</mn></mtd></mtr><mtr><mtd columnalign=\"left\"><mn>4</mn></mtd><mtd><mn>5</mn></mtd>"
             "<mtd columnalign=\"right\"><mn>6</mn></mtd></mtr></mtable>",
             "array columns align; rules and \\hline are ignored");
  expectBody("a \\\\ b", "<mtable displaystyle=\"true\"><mtr><mtd><mi>a</mi></mtd></mtr><mtr><mtd><mi>b</mi></mtd></mtr></mtable>",
             "\\\\ in a formula of its own makes centred rows", true);
  expectBody("\\tag{1} E = mc^2",
             "<mi>E</mi><mo>=</mo><mi>m</mi><msup><mi>c</mi><mn>2</mn></msup><mspace width=\"2em\"></mspace>"
             "<mtext>(1)</mtext>",
             "\\tag follows the formula", true);
  require(onlyError("\\tag{1} x") == "\\tag needs display math", "\\tag is refused inline");
  expectBody("\\sum_{\\substack{i<n \\\\ j}}",
             "<msub><mo>∑</mo><mtable><mtr><mtd><mi>i</mi><mo>&lt;</mo><mi>n</mi></mtd></mtr><mtr><mtd><mi>j</mi></mtd>"
             "</mtr></mtable></msub>",
             "\\substack");
}

void testAccents() {
  expectBody("\\hat{x}", "<mover accent=\"true\"><mi>x</mi><mo stretchy=\"false\">^</mo></mover>", "\\hat");
  expectBody("\\vec v", "<mover accent=\"true\"><mi>v</mi><mo stretchy=\"false\">→</mo></mover>", "\\vec");
  expectBody("\\overline{AB}", "<mover accent=\"true\"><mrow><mi>A</mi><mi>B</mi></mrow><mo stretchy=\"true\">‾</mo></mover>",
             "\\overline stretches");
  expectBody("\\underline{x}", "<munder accentunder=\"true\"><mi>x</mi><mo stretchy=\"true\">_</mo></munder>",
             "\\underline");
  expectBody("\\widetilde{xy}",
             "<mover accent=\"true\"><mrow><mi>x</mi><mi>y</mi></mrow><mo stretchy=\"true\">~</mo></mover>",
             "\\widetilde");
  expectBody("\\overbrace{a+b}^{n}",
             "<mover><mover><mrow><mi>a</mi><mo>+</mo><mi>b</mi></mrow><mo stretchy=\"true\">⏞</mo></mover><mi>n</mi>"
             "</mover>",
             "a brace takes its label over it");
  expectBody("\\underbrace{c}_{m}",
             "<munder><munder><mi>c</mi><mo stretchy=\"true\">⏟</mo></munder><mi>m</mi></munder>", "and under it");
  expectBody("\\overset{!}{=}", "<mover><mo>=</mo><mo>!</mo></mover>", "\\overset");
  expectBody("\\underset{x}{\\min} f",
             "<munder><mi>min</mi><mi>x</mi></munder><mo rspace=\"0.1667em\">\u2061</mo><mi>f</mi>", "\\underset");
  expectBody("\\xrightarrow{f}",
             "<mover><mo stretchy=\"true\">→</mo><mrow><mspace width=\"0.2778em\"></mspace><mi>f</mi>"
             "<mspace width=\"0.2778em\"></mspace></mrow></mover>",
             "\\xrightarrow");
}

void testNegation() {
  expectBody("a \\not= b", "<mi>a</mi><mo>≠</mo><mi>b</mi>", "\\not= is ≠");
  expectBody("x \\not\\in A", "<mi>x</mi><mo>∉</mo><mi>A</mi>", "\\not\\in is ∉");
  expectBody("\\not\\perp", "<mo>⊥\u0338</mo>", "otherwise a combining long solidus");
  expectBody("\\neq \\notin \\nleq", "<mo>≠</mo><mo>∉</mo><mo>≰</mo>", "named negations");
}

void testColourAndSpacing() {
  expectBody("\\color{red} x + \\textcolor{#00f}{y} \\color{blue} z",
             "<mstyle mathcolor=\"red\"><mi>x</mi><mo>+</mo><mstyle mathcolor=\"#00f\"><mi>y</mi></mstyle></mstyle>"
             "<mstyle mathcolor=\"blue\"><mi>z</mi></mstyle>",
             "colour switches make sibling parts, \\textcolor wraps its argument");
  expectBody("\\displaystyle \\color{Red} x",
             "<mstyle displaystyle=\"true\" scriptlevel=\"0\" mathcolor=\"red\"><mi>x</mi></mstyle>",
             "a part carries every switch in force");
  require(onlyError("\\color{notacolor} x") == "unsupported colour notacolor", "unknown colours are reported");
  expectBody("x\\,y\\quad z\\!w\\hspace{2mu}v\\kern1em u",
             "<mi>x</mi><mspace width=\"0.1667em\"></mspace><mi>y</mi><mspace width=\"1em\"></mspace><mi>z</mi>"
             "<mspace width=\"-0.1667em\"></mspace><mi>w</mi><mspace width=\"0.1111em\"></mspace><mi>v</mi>"
             "<mspace width=\"1em\"></mspace><mi>u</mi>",
             "spacing commands and lengths (18 mu to the em)");
  require(onlyError("\\hspace{3furlongs}") == "invalid length after \\hspace", "unknown units are refused");
  expectBody("a~b", "<mi>a</mi><mspace width=\"0.3333em\"></mspace><mi>b</mi>", "a tie is an interword space");
  expectBody("\\phantom{x}\\vphantom{y}",
             "<mphantom><mi>x</mi></mphantom><mpadded width=\"0\"><mphantom><mi>y</mi></mphantom></mpadded>",
             "phantoms");
  expectBody("\\boxed{x}", "<mi>x</mi>", "\\boxed keeps its content without a frame");
}

void testErrors() {
  expectBody("\\foo{x}", "<merror><mtext>\\foo</mtext></merror><mi>x</mi>", "an unknown command is an error in place");
  require(onlyError("\\foo{x}") == "unknown command \\foo", "and says which command");
  expectBody("{a", "<mrow><mi>a</mi><merror><mtext>missing }</mtext></merror></mrow>", "an unclosed group");
  require(onlyError("{a") == "missing }", "is reported");
  expectBody("a}", "<mi>a</mi><merror><mtext>}</mtext></merror>", "an unmatched }");
  require(onlyError("a}") == "unmatched }", "is reported");
  expectBody("\\left( x",
             "<mrow><mo fence=\"true\" form=\"prefix\" stretchy=\"true\">(</mo><mi>x</mi>"
             "<merror><mtext>missing \\right</mtext></merror></mrow>",
             "\\left without \\right");
  require(onlyError("\\left( x") == "\\left without matching \\right", "is reported");
  require(onlyError("x \\right)") == "\\right without \\left", "\\right without \\left is reported");
  require(onlyError("a & b") == "misplaced &", "& outside a table is reported");
  require(onlyError("x^") == "missing argument for superscript", "a script needs an argument");
  expectBody("x_a_b", "<msub><mi>x</mi><mi>a</mi></msub><merror><mtext>_</mtext></merror><mi>b</mi>",
             "a double subscript is reported and its argument kept");
  require(onlyError("\\begin{matrix} a \\end{pmatrix}") == "\\begin{matrix} ended by \\end{pmatrix}",
          "mismatched environments");
  require(onlyError("\\begin{foo} a \\end{foo}") == "unknown environment foo", "unknown environments");
  require(onlyError("\\begin{matrix} a") == "missing \\end{matrix}", "unterminated environments");

  const std::string oversize(ckgit::kMaximumMathSourceBytes + 1, 'x');
  const auto too_long = ckgit::renderTexMath(oversize, false);
  require(too_long.html == "<math><merror><mtext>formula too long</mtext></merror></math>" &&
              too_long.errors.size() == 1,
          "oversize input is one error and is not copied into the page");
  require(ckgit::renderTexMath(std::string(ckgit::kMaximumMathSourceBytes, 'x'), false).errors.empty(),
          "input at the bound is converted");

  std::string deep;
  for (std::size_t k = 0; k < ckgit::kMaximumMathDepth; ++k) deep += "{";
  const auto nested = ckgit::renderTexMath(deep + "x" + std::string(ckgit::kMaximumMathDepth, '}'), false);
  require(contains(nested.html, "<mrow><merror><mtext>formula nested too deeply</mtext></merror></mrow>") &&
              nested.errors.size() == 1 && nested.errors.front() == "formula nested deeper than 64 levels",
          "nesting beyond the bound is one error for the whole formula");
  std::string shallow;
  for (int k = 0; k < 30; ++k) shallow += "{";
  require(ckgit::renderTexMath(shallow + "x" + std::string(30, '}'), false).errors.empty(),
          "moderate nesting is fine");

  std::string many;
  for (int k = 0; k < 40; ++k) many += "\\bad ";
  const auto capped = ckgit::renderTexMath(many, false);
  require(capped.errors.size() == ckgit::kMaximumMathErrors + 1 && capped.errors.back() == "further errors omitted",
          "error messages are capped");
}

// Pathological input of the maximum size must finish quickly with output
// bounded linearly in the input.
void testLinearTime() {
  for (const std::string_view unit : {"{", "}", "^", "\\frac", "\\left(", "\\not", "x'", "&", "\\\\", "a_b^c",
                                       "\\sqrt[", "\\text{", "\\begin{matrix}", "\\color{red}"}) {
    std::string input;
    while (input.size() + unit.size() <= ckgit::kMaximumMathSourceBytes) input += unit;
    for (const bool display : {false, true}) {
      const auto start = std::chrono::steady_clock::now();
      const auto result = ckgit::renderTexMath(input, display);
      const auto elapsed = std::chrono::steady_clock::now() - start;
      require(elapsed < std::chrono::seconds(2), "16 KiB of " + std::string(unit) + " converts quickly");
      require(result.html.size() <= 128 * input.size() && result.errors.size() <= ckgit::kMaximumMathErrors + 1,
              "16 KiB of " + std::string(unit) + " stays bounded");
      require(result.html.rfind("<math", 0) == 0 && contains(result.html, "</math>"),
              "16 KiB of " + std::string(unit) + " is still one <math> element");
    }
  }
}

}  // namespace

void testMath() {
  testWrapper();
  testEscaping();
  testBasics();
  testOperators();
  testFences();
  testFonts();
  testText();
  testEnvironments();
  testAccents();
  testNegation();
  testColourAndSpacing();
  testErrors();
  testLinearTime();
}
