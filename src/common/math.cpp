// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/math.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The converter reads TeX once, left to right, into a small tree of MathML
// nodes and then writes the tree out. Every recursive step (a group, an
// argument, an environment, a \left…\right body, a \text body) passes a depth
// guard, so the stack and the tree are both bounded by kMaximumMathDepth, and
// every source byte is looked at a constant number of times. The symbol and
// command tables below were written for this file from the Unicode charts.

namespace ckgit {
namespace {

// ------------------------------------------------------------------ UTF-8

void appendUtf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

// The code point at s[i], advancing past it. Malformed sequences (overlong
// forms, surrogates, values above U+10FFFF, truncation) decode as U+FFFD and
// consume a single byte, so every byte is visited once.
char32_t decodeUtf8(std::string_view s, std::size_t& i) {
  const auto lead = static_cast<unsigned char>(s[i]);
  if (lead < 0x80) {
    ++i;
    return lead;
  }
  std::size_t length = 0;
  char32_t c = 0;
  char32_t minimum = 0;
  if ((lead & 0xE0) == 0xC0) {
    length = 2, c = lead & 0x1F, minimum = 0x80;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3, c = lead & 0x0F, minimum = 0x800;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4, c = lead & 0x07, minimum = 0x10000;
  } else {
    ++i;
    return 0xFFFD;
  }
  if (i + length > s.size()) {
    ++i;
    return 0xFFFD;
  }
  for (std::size_t k = 1; k < length; ++k) {
    const auto byte = static_cast<unsigned char>(s[i + k]);
    if ((byte & 0xC0) != 0x80) {
      ++i;
      return 0xFFFD;
    }
    c = (c << 6) | (byte & 0x3F);
  }
  if (c < minimum || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
    ++i;
    return 0xFFFD;
  }
  i += length;
  return c;
}

bool isControl(char32_t c) { return c < 0x20 || c == 0x7F; }

bool isCombining(char32_t c) {
  return (c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x20D0 && c <= 0x20FF) ||
         (c >= 0xFE20 && c <= 0xFE2F);
}

bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

void appendEscaped(std::string& out, std::string_view text) {
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
}

// The source as annotation text: valid UTF-8, control bytes other than tab,
// newline and carriage return replaced by U+FFFD, then escaped.
void appendAnnotation(std::string& out, std::string_view source) {
  std::string clean;
  clean.reserve(source.size());
  for (std::size_t i = 0; i < source.size();) {
    const char32_t c = decodeUtf8(source, i);
    appendUtf8(clean, isControl(c) && c != '\t' && c != '\n' && c != '\r' ? 0xFFFD : c);
  }
  appendEscaped(out, clean);
}

std::string clip(std::string_view text) {
  constexpr std::size_t kLimit = 48;
  if (text.size() <= kLimit) return std::string(text);
  std::size_t end = kLimit;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  return std::string(text.substr(0, end)) + "…";
}

// ------------------------------------------------------------------- fonts

enum class Font : unsigned char {
  Default,
  Roman,
  Italic,
  Bold,
  BoldItalic,
  BoldSymbol,  // \boldsymbol: bold italic letters, bold upright capital Greek
  Script,
  BoldScript,
  Fraktur,
  BoldFraktur,
  DoubleStruck,
  Sans,
  SansBold,
  SansItalic,
  SansBoldItalic,
  Mono,
};

// The first capital of each Latin alphabet in Mathematical Alphanumeric
// Symbols (each alphabet is 52 letters, capitals then small letters).
char32_t latinBase(Font font) {
  switch (font) {
    case Font::Bold: return 0x1D400;
    case Font::Italic: return 0x1D434;
    case Font::BoldItalic:
    case Font::BoldSymbol: return 0x1D468;
    case Font::Script: return 0x1D49C;
    case Font::BoldScript: return 0x1D4D0;
    case Font::Fraktur: return 0x1D504;
    case Font::DoubleStruck: return 0x1D538;
    case Font::BoldFraktur: return 0x1D56C;
    case Font::Sans: return 0x1D5A0;
    case Font::SansBold: return 0x1D5D4;
    case Font::SansItalic: return 0x1D608;
    case Font::SansBoldItalic: return 0x1D63C;
    case Font::Mono: return 0x1D670;
    default: return 0;
  }
}

// Letters Unicode had already encoded in Letterlike Symbols, which are
// therefore reserved holes in the alphanumeric block.
char32_t latinHole(Font font, char letter) {
  switch (font) {
    case Font::Italic: return letter == 'h' ? 0x210E : 0;
    case Font::Script:
      switch (letter) {
        case 'B': return 0x212C;
        case 'E': return 0x2130;
        case 'F': return 0x2131;
        case 'H': return 0x210B;
        case 'I': return 0x2110;
        case 'L': return 0x2112;
        case 'M': return 0x2133;
        case 'R': return 0x211B;
        case 'e': return 0x212F;
        case 'g': return 0x210A;
        case 'o': return 0x2134;
        default: return 0;
      }
    case Font::Fraktur:
      switch (letter) {
        case 'C': return 0x212D;
        case 'H': return 0x210C;
        case 'I': return 0x2111;
        case 'R': return 0x211C;
        case 'Z': return 0x2128;
        default: return 0;
      }
    case Font::DoubleStruck:
      switch (letter) {
        case 'C': return 0x2102;
        case 'H': return 0x210D;
        case 'N': return 0x2115;
        case 'P': return 0x2119;
        case 'Q': return 0x211A;
        case 'R': return 0x211D;
        case 'Z': return 0x2124;
        default: return 0;
      }
    default: return 0;
  }
}

char32_t digitBase(Font font) {
  switch (font) {
    case Font::Bold:
    case Font::BoldItalic:
    case Font::BoldSymbol:
    case Font::BoldScript:
    case Font::BoldFraktur: return 0x1D7CE;
    case Font::DoubleStruck: return 0x1D7D8;
    case Font::Sans:
    case Font::SansItalic: return 0x1D7E2;
    case Font::SansBold:
    case Font::SansBoldItalic: return 0x1D7EC;
    case Font::Mono: return 0x1D7F6;
    default: return 0;
  }
}

// The position of a Greek letter in the 58-character mathematical Greek
// alphabets: 25 capitals (with capital theta symbol at 17), nabla, 25 small
// letters (final sigma at 43), then partial, epsilon, theta, kappa, phi, rho
// and pi symbols. -1 for anything else.
int greekIndex(char32_t c) {
  if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return static_cast<int>(c - 0x391);
  if (c >= 0x3B1 && c <= 0x3C9) return 26 + static_cast<int>(c - 0x3B1);
  switch (c) {
    case 0x3F4: return 17;
    case 0x2207: return 25;
    case 0x2202: return 51;
    case 0x3F5: return 52;
    case 0x3D1: return 53;
    case 0x3F0: return 54;
    case 0x3D5: return 55;
    case 0x3F1: return 56;
    case 0x3D6: return 57;
    default: return -1;
  }
}

char32_t greekBase(Font font, bool capital) {
  switch (font) {
    case Font::Bold: return 0x1D6A8;
    case Font::Italic: return 0x1D6E2;
    case Font::BoldItalic: return 0x1D71C;
    case Font::BoldSymbol: return capital ? 0x1D6A8 : 0x1D71C;
    case Font::SansBold: return 0x1D756;
    case Font::SansBoldItalic: return 0x1D790;
    default: return 0;
  }
}

// The character `c` in `font`, or `c` itself when the font has no such
// character (Unicode has no double-struck Greek, for example).
char32_t styled(char32_t c, Font font) {
  if (c < 0x80 && isLetter(static_cast<char>(c))) {
    const char32_t base = latinBase(font);
    if (base == 0) return c;
    if (const char32_t hole = latinHole(font, static_cast<char>(c))) return hole;
    return base + (c <= 'Z' ? c - 'A' : 26 + (c - 'a'));
  }
  if (c >= '0' && c <= '9') {
    const char32_t base = digitBase(font);
    return base == 0 ? c : base + (c - '0');
  }
  if ((c == 0x131 || c == 0x237) && font == Font::Italic) return c == 0x131 ? 0x1D6A4 : 0x1D6A5;
  const int index = greekIndex(c);
  if (index >= 0) {
    const char32_t base = greekBase(font, index <= 25);
    return base == 0 ? c : base + static_cast<char32_t>(index);
  }
  return c;
}

// Characters a browser italicises in a one-character <mi>.
bool autoItalic(char32_t c) {
  return (c < 0x80 && isLetter(static_cast<char>(c))) || greekIndex(c) >= 0 || c == 0x131 || c == 0x237;
}

// ------------------------------------------------------------------- nodes

// Atom classes in TeX's sense, as far as spacing and unary signs need them.
// Space marks explicit spacing, which is transparent to both.
enum class Cls : unsigned char { Ord, Op, Bin, Rel, Open, Close, Punct, Inner, Func, Space };
enum class Limits : unsigned char { Side, Movable, Always };

using Attributes = std::vector<std::pair<std::string_view, std::string>>;

struct Node {
  std::string_view tag;
  std::string text;
  Attributes attributes;
  std::vector<Node> children;
  Cls cls = Cls::Ord;
  Limits limits = Limits::Side;
};

Node token(std::string_view tag, std::string text, Cls cls = Cls::Ord) {
  Node node;
  node.tag = tag;
  node.text = std::move(text);
  node.cls = cls;
  return node;
}

Node element(std::string_view tag, std::vector<Node> children, Cls cls = Cls::Ord) {
  Node node;
  node.tag = tag;
  node.children = std::move(children);
  node.cls = cls;
  return node;
}

Node with(Node node, std::string_view name, std::string value) {
  node.attributes.emplace_back(name, std::move(value));
  return node;
}

template <typename... Nodes>
std::vector<Node> nodes(Nodes&&... items) {
  std::vector<Node> list;
  list.reserve(sizeof...(items));
  (list.push_back(std::forward<Nodes>(items)), ...);
  return list;
}

// A list as one node: its only item, or an <mrow>.
Node rowOf(std::vector<Node> items) {
  if (items.size() == 1) return std::move(items.front());
  return element("mrow", std::move(items));
}

// A `{…}` group as an atom: like rowOf, except that a lone operator stays in
// an <mrow>, as TeX makes `{=}` an ordinary symbol without relation spacing.
Node groupOf(std::vector<Node> items) {
  if (items.size() == 1 && items.front().tag != "mo") return std::move(items.front());
  return element("mrow", std::move(items));
}

Node space(std::string width) { return with(token("mspace", "", Cls::Space), "width", std::move(width)); }

// An operator that must keep its natural size: brackets and bars outside
// \left…\right (MathML would otherwise stretch them to the row).
Node fixed(std::string text, Cls cls) { return with(token("mo", std::move(text), cls), "stretchy", "false"); }

Node fence(std::string text, std::string_view form) {
  Node node = token("mo", std::move(text), form == "prefix" ? Cls::Open : Cls::Close);
  node.attributes.emplace_back("fence", "true");
  node.attributes.emplace_back("form", std::string(form));
  node.attributes.emplace_back("stretchy", "true");
  return node;
}

void write(std::string& out, const Node& node) {
  out += '<';
  out += node.tag;
  for (const auto& [name, value] : node.attributes) {
    out += ' ';
    out += name;
    out += "=\"";
    appendEscaped(out, value);
    out += '"';
  }
  out += '>';
  appendEscaped(out, node.text);
  for (const auto& child : node.children) write(out, child);
  out += "</";
  out += node.tag;
  out += '>';
}

// The identifier for one symbol in `font`. A single character the font can
// restyle becomes that mathematical alphanumeric character; otherwise the
// character stays and mathvariant="normal" keeps it upright where a browser
// would italicise it (\mathrm, capital Greek, \nabla).
Node identifier(std::string_view text, Font font, bool upright) {
  std::size_t end = 0;
  const char32_t c = text.empty() ? 0 : decodeUtf8(text, end);
  if (text.empty() || end != text.size()) return token("mi", std::string(text));
  const char32_t restyled = styled(c, font);
  if (restyled != c) {
    std::string out;
    appendUtf8(out, restyled);
    return token("mi", out);
  }
  Node node = token("mi", std::string(text));
  if (autoItalic(c) && (font == Font::Roman || upright)) node.attributes.emplace_back("mathvariant", "normal");
  return node;
}

// ----------------------------------------------------------------- tables

enum class K : unsigned char {
  Ident,         // <mi>
  Upright,       // <mi mathvariant="normal">
  Ord,           // ordinary <mo>
  Fence,         // a bar used as an ordinary symbol: non-stretchy <mo>
  Bin,
  Rel,
  Open,
  Close,
  Punct,         // n = 1: TeX punctuation spacing (\colon)
  Large,         // limits under/over in display style
  Integral,      // limits at the side
  Function,      // upright name, limits at the side
  LimitFunction, // upright name, limits under/over in display style
  Frac,          // n: 0 frac, 1 dfrac, 2 tfrac, 3 cfrac
  Binom,         // n: 0 binom, 1 dbinom, 2 tbinom
  Infix,         // n: 0 over, 1 atop, 2 choose
  Sqrt,
  Left,
  Big,           // n: size * 4 + form (0 none, 1 l, 2 r, 3 m)
  Accent,        // text: the mark; n: bit 0 stretchy, bit 1 under
  Brace,         // text: the brace; n: 0 over, 1 under
  ExtensibleArrow,
  OverUnderSet,  // n: 0 overset, 1 underset, 2 stackrel
  Font,          // n: Font, applied to the argument
  FontSwitch,    // n: Font, for the rest of the group
  Text,          // n: TextChange
  OperatorName,
  MathClass,     // n: Cls
  Mod,           // n: 0 bmod, 1 pmod, 2 mod, 3 pod
  Space,         // text: width
  HSpace,
  Kern,          // n: 1 when the unit must be mu
  Style,         // n: 0 display, 1 text, 2 script, 3 scriptscript
  Color,
  TextColor,
  ColorBox,
  Phantom,       // n: 0 phantom, 1 hphantom, 2 vphantom
  Smash,
  Strut,         // n: 0 strut, 1 mathstrut
  Not,
  Boxed,
  Begin,
  Substack,
  Tag,
  Limits,        // n: Limits
  SkipArgument,
  Ignore,
};

struct Entry {
  std::string_view name;
  K kind;
  std::string_view text = {};
  int n = 0;
};

// How a text command changes the text font.
enum TextChange : int { kTextKeep, kTextBold, kTextItalic, kTextSans, kTextMono, kTextRoman, kTextUpright, kTextNormal };

const Entry kEntries[] = {
    // Greek, small letters italic as in TeX; \epsilon is the lunate form.
    {"alpha", K::Ident, "α"}, {"beta", K::Ident, "β"}, {"gamma", K::Ident, "γ"}, {"delta", K::Ident, "δ"},
    {"epsilon", K::Ident, "ϵ"}, {"varepsilon", K::Ident, "ε"}, {"zeta", K::Ident, "ζ"}, {"eta", K::Ident, "η"},
    {"theta", K::Ident, "θ"}, {"vartheta", K::Ident, "ϑ"}, {"iota", K::Ident, "ι"}, {"kappa", K::Ident, "κ"},
    {"varkappa", K::Ident, "ϰ"}, {"lambda", K::Ident, "λ"}, {"mu", K::Ident, "μ"}, {"nu", K::Ident, "ν"},
    {"xi", K::Ident, "ξ"}, {"omicron", K::Ident, "ο"}, {"pi", K::Ident, "π"}, {"varpi", K::Ident, "ϖ"},
    {"rho", K::Ident, "ρ"}, {"varrho", K::Ident, "ϱ"}, {"sigma", K::Ident, "σ"}, {"varsigma", K::Ident, "ς"},
    {"tau", K::Ident, "τ"}, {"upsilon", K::Ident, "υ"}, {"phi", K::Ident, "ϕ"}, {"varphi", K::Ident, "φ"},
    {"chi", K::Ident, "χ"}, {"psi", K::Ident, "ψ"}, {"omega", K::Ident, "ω"}, {"digamma", K::Ident, "ϝ"},
    // Capitals upright, their var- forms italic.
    {"Alpha", K::Upright, "Α"}, {"Beta", K::Upright, "Β"}, {"Gamma", K::Upright, "Γ"}, {"Delta", K::Upright, "Δ"},
    {"Epsilon", K::Upright, "Ε"}, {"Zeta", K::Upright, "Ζ"}, {"Eta", K::Upright, "Η"}, {"Theta", K::Upright, "Θ"},
    {"Iota", K::Upright, "Ι"}, {"Kappa", K::Upright, "Κ"}, {"Lambda", K::Upright, "Λ"}, {"Mu", K::Upright, "Μ"},
    {"Nu", K::Upright, "Ν"}, {"Xi", K::Upright, "Ξ"}, {"Omicron", K::Upright, "Ο"}, {"Pi", K::Upright, "Π"},
    {"Rho", K::Upright, "Ρ"}, {"Sigma", K::Upright, "Σ"}, {"Tau", K::Upright, "Τ"}, {"Upsilon", K::Upright, "Υ"},
    {"Phi", K::Upright, "Φ"}, {"Chi", K::Upright, "Χ"}, {"Psi", K::Upright, "Ψ"}, {"Omega", K::Upright, "Ω"},
    {"varGamma", K::Ident, "Γ"}, {"varDelta", K::Ident, "Δ"}, {"varTheta", K::Ident, "Θ"},
    {"varLambda", K::Ident, "Λ"}, {"varXi", K::Ident, "Ξ"}, {"varPi", K::Ident, "Π"}, {"varSigma", K::Ident, "Σ"},
    {"varUpsilon", K::Ident, "Υ"}, {"varPhi", K::Ident, "Φ"}, {"varPsi", K::Ident, "Ψ"}, {"varOmega", K::Ident, "Ω"},
    // Letter-like and ordinary symbols.
    {"ell", K::Ident, "ℓ"}, {"imath", K::Ident, "ı"}, {"jmath", K::Ident, "ȷ"}, {"partial", K::Ident, "∂"},
    {"hbar", K::Ident, "ℏ"}, {"hslash", K::Ident, "ℏ"}, {"wp", K::Ident, "℘"}, {"aleph", K::Ident, "ℵ"},
    {"beth", K::Ident, "ℶ"}, {"gimel", K::Ident, "ℷ"}, {"daleth", K::Ident, "ℸ"}, {"infty", K::Ident, "∞"},
    {"emptyset", K::Ident, "∅"}, {"varnothing", K::Ident, "∅"}, {"Re", K::Ident, "ℜ"}, {"Im", K::Ident, "ℑ"},
    {"mho", K::Ident, "℧"}, {"eth", K::Ident, "ð"}, {"Bbbk", K::Ident, "𝕜"}, {"complement", K::Ident, "∁"},
    {"nabla", K::Upright, "∇"}, {"angle", K::Ident, "∠"}, {"measuredangle", K::Ident, "∡"},
    {"sphericalangle", K::Ident, "∢"}, {"triangle", K::Ident, "△"}, {"vartriangle", K::Ident, "△"},
    {"triangledown", K::Ident, "▽"}, {"blacktriangle", K::Ident, "▲"}, {"blacktriangledown", K::Ident, "▼"},
    {"square", K::Ident, "□"}, {"Box", K::Ident, "□"}, {"blacksquare", K::Ident, "■"}, {"Diamond", K::Ident, "◇"},
    {"lozenge", K::Ident, "◊"}, {"blacklozenge", K::Ident, "⧫"}, {"diamondsuit", K::Ident, "♢"},
    {"heartsuit", K::Ident, "♡"}, {"clubsuit", K::Ident, "♣"}, {"spadesuit", K::Ident, "♠"}, {"flat", K::Ident, "♭"},
    {"natural", K::Ident, "♮"}, {"sharp", K::Ident, "♯"}, {"top", K::Ident, "⊤"}, {"bot", K::Ident, "⊥"},
    {"degree", K::Ident, "°"}, {"checkmark", K::Ident, "✓"}, {"maltese", K::Ident, "✠"}, {"S", K::Ident, "§"},
    {"P", K::Ident, "¶"}, {"dag", K::Ident, "†"}, {"ddag", K::Ident, "‡"}, {"copyright", K::Ident, "©"},
    {"pounds", K::Ident, "£"}, {"yen", K::Ident, "¥"}, {"euro", K::Ident, "€"}, {"circledR", K::Ident, "®"},
    {"circledS", K::Ident, "Ⓢ"}, {"surd", K::Ident, "√"}, {"backprime", K::Ident, "‵"}, {"$", K::Ident, "$"},
    {"%", K::Ident, "%"}, {"#", K::Ident, "#"}, {"&", K::Ident, "&"}, {"_", K::Ident, "_"},
    {"forall", K::Ord, "∀"}, {"exists", K::Ord, "∃"}, {"nexists", K::Ord, "∄"}, {"neg", K::Ord, "¬"},
    {"lnot", K::Ord, "¬"}, {"prime", K::Ord, "′"}, {"backslash", K::Ord, "\\"},
    {"ldots", K::Ord, "…"}, {"dots", K::Ord, "…"}, {"dotso", K::Ord, "…"}, {"dotsc", K::Ord, "…"},
    {"cdots", K::Ord, "⋯"}, {"dotsb", K::Ord, "⋯"}, {"dotsm", K::Ord, "⋯"}, {"dotsi", K::Ord, "⋯"},
    {"vdots", K::Ord, "⋮"}, {"ddots", K::Ord, "⋱"}, {"iddots", K::Ord, "⋰"}, {"adots", K::Ord, "⋰"},
    {"|", K::Fence, "‖"}, {"vert", K::Fence, "|"}, {"Vert", K::Fence, "‖"},
    // Delimiters.
    {"{", K::Open, "{"}, {"lbrace", K::Open, "{"}, {"langle", K::Open, "⟨"}, {"lfloor", K::Open, "⌊"},
    {"lceil", K::Open, "⌈"}, {"lvert", K::Open, "|"}, {"lVert", K::Open, "‖"}, {"lbrack", K::Open, "["},
    {"llbracket", K::Open, "⟦"}, {"lgroup", K::Open, "⟮"}, {"lmoustache", K::Open, "⎰"},
    {"ulcorner", K::Open, "⌜"}, {"llcorner", K::Open, "⌞"},
    {"}", K::Close, "}"}, {"rbrace", K::Close, "}"}, {"rangle", K::Close, "⟩"}, {"rfloor", K::Close, "⌋"},
    {"rceil", K::Close, "⌉"}, {"rvert", K::Close, "|"}, {"rVert", K::Close, "‖"}, {"rbrack", K::Close, "]"},
    {"rrbracket", K::Close, "⟧"}, {"rgroup", K::Close, "⟯"}, {"rmoustache", K::Close, "⎱"},
    {"urcorner", K::Close, "⌝"}, {"lrcorner", K::Close, "⌟"},
    {"colon", K::Punct, ":", 1}, {"ldotp", K::Punct, "."}, {"cdotp", K::Punct, "⋅"},
    // Binary operators.
    {"pm", K::Bin, "±"}, {"mp", K::Bin, "∓"}, {"times", K::Bin, "×"}, {"div", K::Bin, "÷"}, {"cdot", K::Bin, "⋅"},
    {"centerdot", K::Bin, "⋅"}, {"ast", K::Bin, "∗"}, {"star", K::Bin, "⋆"}, {"circ", K::Bin, "∘"},
    {"bullet", K::Bin, "∙"}, {"oplus", K::Bin, "⊕"}, {"ominus", K::Bin, "⊖"}, {"otimes", K::Bin, "⊗"},
    {"oslash", K::Bin, "⊘"}, {"odot", K::Bin, "⊙"}, {"cup", K::Bin, "∪"}, {"cap", K::Bin, "∩"},
    {"sqcup", K::Bin, "⊔"}, {"sqcap", K::Bin, "⊓"}, {"uplus", K::Bin, "⊎"}, {"setminus", K::Bin, "∖"},
    {"smallsetminus", K::Bin, "∖"}, {"wedge", K::Bin, "∧"}, {"land", K::Bin, "∧"}, {"vee", K::Bin, "∨"},
    {"lor", K::Bin, "∨"}, {"wr", K::Bin, "≀"}, {"amalg", K::Bin, "⨿"}, {"triangleleft", K::Bin, "◁"},
    {"triangleright", K::Bin, "▷"}, {"bigtriangleup", K::Bin, "△"}, {"bigtriangledown", K::Bin, "▽"},
    {"dagger", K::Bin, "†"}, {"ddagger", K::Bin, "‡"}, {"diamond", K::Bin, "⋄"}, {"bigcirc", K::Bin, "◯"},
    {"lhd", K::Bin, "⊲"}, {"rhd", K::Bin, "⊳"}, {"unlhd", K::Bin, "⊴"}, {"unrhd", K::Bin, "⊵"},
    {"boxplus", K::Bin, "⊞"}, {"boxminus", K::Bin, "⊟"}, {"boxtimes", K::Bin, "⊠"}, {"boxdot", K::Bin, "⊡"},
    {"divideontimes", K::Bin, "⋇"}, {"ltimes", K::Bin, "⋉"}, {"rtimes", K::Bin, "⋊"},
    {"leftthreetimes", K::Bin, "⋋"}, {"rightthreetimes", K::Bin, "⋌"}, {"curlywedge", K::Bin, "⋏"},
    {"curlyvee", K::Bin, "⋎"}, {"dotplus", K::Bin, "∔"}, {"intercal", K::Bin, "⊺"}, {"barwedge", K::Bin, "⊼"},
    {"veebar", K::Bin, "⊻"}, {"circledast", K::Bin, "⊛"}, {"circledcirc", K::Bin, "⊚"},
    {"circleddash", K::Bin, "⊝"}, {"Cap", K::Bin, "⋒"}, {"Cup", K::Bin, "⋓"}, {"doublecap", K::Bin, "⋒"},
    {"doublecup", K::Bin, "⋓"}, {"And", K::Bin, "&"}, {"lessdot", K::Bin, "⋖"}, {"gtrdot", K::Bin, "⋗"},
    // Relations.
    {"leq", K::Rel, "≤"}, {"le", K::Rel, "≤"}, {"geq", K::Rel, "≥"}, {"ge", K::Rel, "≥"}, {"leqq", K::Rel, "≦"},
    {"geqq", K::Rel, "≧"}, {"leqslant", K::Rel, "⩽"}, {"geqslant", K::Rel, "⩾"}, {"eqslantless", K::Rel, "⪕"},
    {"eqslantgtr", K::Rel, "⪖"}, {"lesssim", K::Rel, "≲"}, {"gtrsim", K::Rel, "≳"}, {"lessapprox", K::Rel, "⪅"},
    {"gtrapprox", K::Rel, "⪆"}, {"lessgtr", K::Rel, "≶"}, {"gtrless", K::Rel, "≷"}, {"lesseqgtr", K::Rel, "⋚"},
    {"gtreqless", K::Rel, "⋛"}, {"ll", K::Rel, "≪"}, {"gg", K::Rel, "≫"}, {"lll", K::Rel, "⋘"},
    {"ggg", K::Rel, "⋙"}, {"lneq", K::Rel, "⪇"}, {"gneq", K::Rel, "⪈"}, {"lneqq", K::Rel, "≨"},
    {"gneqq", K::Rel, "≩"}, {"lnsim", K::Rel, "⋦"}, {"gnsim", K::Rel, "⋧"}, {"nless", K::Rel, "≮"},
    {"ngtr", K::Rel, "≯"}, {"nleq", K::Rel, "≰"}, {"ngeq", K::Rel, "≱"}, {"neq", K::Rel, "≠"}, {"ne", K::Rel, "≠"},
    {"prec", K::Rel, "≺"}, {"succ", K::Rel, "≻"}, {"preceq", K::Rel, "⪯"}, {"succeq", K::Rel, "⪰"},
    {"preccurlyeq", K::Rel, "≼"}, {"succcurlyeq", K::Rel, "≽"}, {"precsim", K::Rel, "≾"}, {"succsim", K::Rel, "≿"},
    {"nprec", K::Rel, "⊀"}, {"nsucc", K::Rel, "⊁"}, {"sim", K::Rel, "∼"}, {"nsim", K::Rel, "≁"},
    {"backsim", K::Rel, "∽"}, {"backsimeq", K::Rel, "⋍"}, {"eqsim", K::Rel, "≂"}, {"thicksim", K::Rel, "∼"},
    {"simeq", K::Rel, "≃"}, {"approx", K::Rel, "≈"}, {"thickapprox", K::Rel, "≈"}, {"approxeq", K::Rel, "≊"},
    {"cong", K::Rel, "≅"}, {"ncong", K::Rel, "≇"}, {"equiv", K::Rel, "≡"}, {"propto", K::Rel, "∝"},
    {"varpropto", K::Rel, "∝"}, {"asymp", K::Rel, "≍"}, {"bumpeq", K::Rel, "≏"}, {"Bumpeq", K::Rel, "≎"},
    {"circeq", K::Rel, "≗"}, {"eqcirc", K::Rel, "≖"}, {"triangleq", K::Rel, "≜"}, {"doteq", K::Rel, "≐"},
    {"doteqdot", K::Rel, "≑"}, {"Doteq", K::Rel, "≑"}, {"fallingdotseq", K::Rel, "≒"},
    {"risingdotseq", K::Rel, "≓"}, {"coloneqq", K::Rel, "≔"}, {"eqqcolon", K::Rel, "≕"},
    {"in", K::Rel, "∈"}, {"notin", K::Rel, "∉"}, {"ni", K::Rel, "∋"}, {"owns", K::Rel, "∋"},
    {"notni", K::Rel, "∌"}, {"subset", K::Rel, "⊂"}, {"supset", K::Rel, "⊃"}, {"subseteq", K::Rel, "⊆"},
    {"supseteq", K::Rel, "⊇"}, {"subsetneq", K::Rel, "⊊"}, {"supsetneq", K::Rel, "⊋"}, {"subseteqq", K::Rel, "⫅"},
    {"supseteqq", K::Rel, "⫆"}, {"nsubseteq", K::Rel, "⊈"}, {"nsupseteq", K::Rel, "⊉"}, {"Subset", K::Rel, "⋐"},
    {"Supset", K::Rel, "⋑"}, {"sqsubset", K::Rel, "⊏"}, {"sqsupset", K::Rel, "⊐"}, {"sqsubseteq", K::Rel, "⊑"},
    {"sqsupseteq", K::Rel, "⊒"}, {"models", K::Rel, "⊨"}, {"vdash", K::Rel, "⊢"}, {"dashv", K::Rel, "⊣"},
    {"vDash", K::Rel, "⊨"}, {"Vdash", K::Rel, "⊩"}, {"Vvdash", K::Rel, "⊪"}, {"nvdash", K::Rel, "⊬"},
    {"nvDash", K::Rel, "⊭"}, {"nVdash", K::Rel, "⊮"}, {"perp", K::Rel, "⊥"}, {"parallel", K::Rel, "∥"},
    {"nparallel", K::Rel, "∦"}, {"mid", K::Rel, "∣"}, {"nmid", K::Rel, "∤"}, {"shortmid", K::Rel, "∣"},
    {"shortparallel", K::Rel, "∥"}, {"smile", K::Rel, "⌣"}, {"frown", K::Rel, "⌢"}, {"bowtie", K::Rel, "⋈"},
    {"Join", K::Rel, "⋈"}, {"between", K::Rel, "≬"}, {"pitchfork", K::Rel, "⋔"}, {"therefore", K::Rel, "∴"},
    {"because", K::Rel, "∵"}, {"multimap", K::Rel, "⊸"}, {"trianglelefteq", K::Rel, "⊴"},
    {"trianglerighteq", K::Rel, "⊵"}, {"vartriangleleft", K::Rel, "⊲"}, {"vartriangleright", K::Rel, "⊳"},
    {"ntriangleleft", K::Rel, "⋪"}, {"ntriangleright", K::Rel, "⋫"}, {"ntrianglelefteq", K::Rel, "⋬"},
    {"ntrianglerighteq", K::Rel, "⋭"},
    // Arrows.
    {"to", K::Rel, "→"}, {"rightarrow", K::Rel, "→"}, {"gets", K::Rel, "←"}, {"leftarrow", K::Rel, "←"},
    {"leftrightarrow", K::Rel, "↔"}, {"Rightarrow", K::Rel, "⇒"}, {"Leftarrow", K::Rel, "⇐"},
    {"Leftrightarrow", K::Rel, "⇔"}, {"implies", K::Rel, "⟹"}, {"impliedby", K::Rel, "⟸"}, {"iff", K::Rel, "⟺"},
    {"longrightarrow", K::Rel, "⟶"}, {"longleftarrow", K::Rel, "⟵"}, {"longleftrightarrow", K::Rel, "⟷"},
    {"Longrightarrow", K::Rel, "⟹"}, {"Longleftarrow", K::Rel, "⟸"}, {"Longleftrightarrow", K::Rel, "⟺"},
    {"mapsto", K::Rel, "↦"}, {"longmapsto", K::Rel, "⟼"}, {"uparrow", K::Rel, "↑"}, {"downarrow", K::Rel, "↓"},
    {"updownarrow", K::Rel, "↕"}, {"Uparrow", K::Rel, "⇑"}, {"Downarrow", K::Rel, "⇓"},
    {"Updownarrow", K::Rel, "⇕"}, {"nearrow", K::Rel, "↗"}, {"searrow", K::Rel, "↘"}, {"swarrow", K::Rel, "↙"},
    {"nwarrow", K::Rel, "↖"}, {"hookrightarrow", K::Rel, "↪"}, {"hookleftarrow", K::Rel, "↩"},
    {"rightharpoonup", K::Rel, "⇀"}, {"rightharpoondown", K::Rel, "⇁"}, {"leftharpoonup", K::Rel, "↼"},
    {"leftharpoondown", K::Rel, "↽"}, {"rightleftharpoons", K::Rel, "⇌"}, {"leftrightharpoons", K::Rel, "⇋"},
    {"upharpoonleft", K::Rel, "↿"}, {"upharpoonright", K::Rel, "↾"}, {"downharpoonleft", K::Rel, "⇃"},
    {"downharpoonright", K::Rel, "⇂"}, {"leadsto", K::Rel, "⇝"}, {"rightsquigarrow", K::Rel, "⇝"},
    {"leftrightsquigarrow", K::Rel, "↭"}, {"twoheadrightarrow", K::Rel, "↠"}, {"twoheadleftarrow", K::Rel, "↞"},
    {"rightarrowtail", K::Rel, "↣"}, {"leftarrowtail", K::Rel, "↢"}, {"circlearrowleft", K::Rel, "↺"},
    {"circlearrowright", K::Rel, "↻"}, {"curvearrowleft", K::Rel, "↶"}, {"curvearrowright", K::Rel, "↷"},
    {"rightrightarrows", K::Rel, "⇉"}, {"leftleftarrows", K::Rel, "⇇"}, {"rightleftarrows", K::Rel, "⇄"},
    {"leftrightarrows", K::Rel, "⇆"}, {"Lsh", K::Rel, "↰"}, {"Rsh", K::Rel, "↱"}, {"looparrowleft", K::Rel, "↫"},
    {"looparrowright", K::Rel, "↬"}, {"nleftarrow", K::Rel, "↚"}, {"nrightarrow", K::Rel, "↛"},
    {"nLeftarrow", K::Rel, "⇍"}, {"nRightarrow", K::Rel, "⇏"}, {"nleftrightarrow", K::Rel, "↮"},
    {"nLeftrightarrow", K::Rel, "⇎"}, {"dashrightarrow", K::Rel, "⇢"}, {"dashleftarrow", K::Rel, "⇠"},
    {"Lleftarrow", K::Rel, "⇚"}, {"Rrightarrow", K::Rel, "⇛"},
    // Big operators.
    {"sum", K::Large, "∑"}, {"prod", K::Large, "∏"}, {"coprod", K::Large, "∐"}, {"bigcup", K::Large, "⋃"},
    {"bigcap", K::Large, "⋂"}, {"bigsqcup", K::Large, "⨆"}, {"biguplus", K::Large, "⨄"},
    {"bigoplus", K::Large, "⨁"}, {"bigotimes", K::Large, "⨂"}, {"bigodot", K::Large, "⨀"},
    {"bigvee", K::Large, "⋁"}, {"bigwedge", K::Large, "⋀"}, {"int", K::Integral, "∫"}, {"intop", K::Large, "∫"},
    {"smallint", K::Integral, "∫"}, {"iint", K::Integral, "∬"}, {"iiint", K::Integral, "∭"},
    {"iiiint", K::Integral, "⨌"}, {"oint", K::Integral, "∮"}, {"oiint", K::Integral, "∯"},
    {"oiiint", K::Integral, "∰"},
    // Named functions.
    {"sin", K::Function, "sin"}, {"cos", K::Function, "cos"}, {"tan", K::Function, "tan"},
    {"cot", K::Function, "cot"}, {"sec", K::Function, "sec"}, {"csc", K::Function, "csc"},
    {"arcsin", K::Function, "arcsin"}, {"arccos", K::Function, "arccos"}, {"arctan", K::Function, "arctan"},
    {"sinh", K::Function, "sinh"}, {"cosh", K::Function, "cosh"}, {"tanh", K::Function, "tanh"},
    {"coth", K::Function, "coth"}, {"arg", K::Function, "arg"}, {"deg", K::Function, "deg"},
    {"dim", K::Function, "dim"}, {"exp", K::Function, "exp"}, {"hom", K::Function, "hom"},
    {"ker", K::Function, "ker"}, {"lg", K::Function, "lg"}, {"ln", K::Function, "ln"}, {"log", K::Function, "log"},
    {"lim", K::LimitFunction, "lim"}, {"liminf", K::LimitFunction, "lim inf"},
    {"limsup", K::LimitFunction, "lim sup"}, {"injlim", K::LimitFunction, "inj lim"},
    {"projlim", K::LimitFunction, "proj lim"}, {"max", K::LimitFunction, "max"},
    {"min", K::LimitFunction, "min"}, {"sup", K::LimitFunction, "sup"}, {"inf", K::LimitFunction, "inf"},
    {"det", K::LimitFunction, "det"}, {"gcd", K::LimitFunction, "gcd"}, {"Pr", K::LimitFunction, "Pr"},
    {"operatorname", K::OperatorName},
    {"bmod", K::Mod, {}, 0}, {"pmod", K::Mod, {}, 1}, {"mod", K::Mod, {}, 2}, {"pod", K::Mod, {}, 3},
    {"mathord", K::MathClass, {}, static_cast<int>(Cls::Ord)},
    {"mathop", K::MathClass, {}, static_cast<int>(Cls::Op)},
    {"mathbin", K::MathClass, {}, static_cast<int>(Cls::Bin)},
    {"mathrel", K::MathClass, {}, static_cast<int>(Cls::Rel)},
    {"mathopen", K::MathClass, {}, static_cast<int>(Cls::Open)},
    {"mathclose", K::MathClass, {}, static_cast<int>(Cls::Close)},
    {"mathpunct", K::MathClass, {}, static_cast<int>(Cls::Punct)},
    {"mathinner", K::MathClass, {}, static_cast<int>(Cls::Inner)},
    // Structure.
    {"frac", K::Frac, {}, 0}, {"dfrac", K::Frac, {}, 1}, {"tfrac", K::Frac, {}, 2}, {"cfrac", K::Frac, {}, 3},
    {"binom", K::Binom, {}, 0}, {"dbinom", K::Binom, {}, 1}, {"tbinom", K::Binom, {}, 2},
    {"over", K::Infix, {}, 0}, {"atop", K::Infix, {}, 1}, {"choose", K::Infix, {}, 2},
    {"sqrt", K::Sqrt}, {"left", K::Left},
    {"big", K::Big, {}, 0}, {"bigl", K::Big, {}, 1}, {"bigr", K::Big, {}, 2}, {"bigm", K::Big, {}, 3},
    {"Big", K::Big, {}, 4}, {"Bigl", K::Big, {}, 5}, {"Bigr", K::Big, {}, 6}, {"Bigm", K::Big, {}, 7},
    {"bigg", K::Big, {}, 8}, {"biggl", K::Big, {}, 9}, {"biggr", K::Big, {}, 10}, {"biggm", K::Big, {}, 11},
    {"Bigg", K::Big, {}, 12}, {"Biggl", K::Big, {}, 13}, {"Biggr", K::Big, {}, 14}, {"Biggm", K::Big, {}, 15},
    {"hat", K::Accent, "^", 0}, {"widehat", K::Accent, "^", 1}, {"check", K::Accent, "ˇ", 0},
    {"widecheck", K::Accent, "ˇ", 1}, {"tilde", K::Accent, "~", 0}, {"widetilde", K::Accent, "~", 1},
    {"acute", K::Accent, "´", 0}, {"grave", K::Accent, "`", 0}, {"dot", K::Accent, "˙", 0},
    {"ddot", K::Accent, "¨", 0}, {"breve", K::Accent, "˘", 0}, {"bar", K::Accent, "¯", 0},
    {"vec", K::Accent, "→", 0}, {"mathring", K::Accent, "˚", 0}, {"overline", K::Accent, "‾", 1},
    {"overrightarrow", K::Accent, "→", 1}, {"overleftarrow", K::Accent, "←", 1},
    {"overleftrightarrow", K::Accent, "↔", 1}, {"Overrightarrow", K::Accent, "⇒", 1},
    {"underline", K::Accent, "_", 3}, {"underrightarrow", K::Accent, "→", 3},
    {"underleftarrow", K::Accent, "←", 3}, {"underleftrightarrow", K::Accent, "↔", 3},
    {"utilde", K::Accent, "~", 3},
    {"overbrace", K::Brace, "⏞", 0}, {"underbrace", K::Brace, "⏟", 1}, {"overparen", K::Brace, "⏜", 0},
    {"underparen", K::Brace, "⏝", 1}, {"overbracket", K::Brace, "⎴", 0}, {"underbracket", K::Brace, "⎵", 1},
    {"xrightarrow", K::ExtensibleArrow, "→"}, {"xleftarrow", K::ExtensibleArrow, "←"},
    {"xRightarrow", K::ExtensibleArrow, "⇒"}, {"xLeftarrow", K::ExtensibleArrow, "⇐"},
    {"xleftrightarrow", K::ExtensibleArrow, "↔"}, {"xLeftrightarrow", K::ExtensibleArrow, "⇔"},
    {"xmapsto", K::ExtensibleArrow, "↦"}, {"xhookrightarrow", K::ExtensibleArrow, "↪"},
    {"xhookleftarrow", K::ExtensibleArrow, "↩"}, {"xrightharpoonup", K::ExtensibleArrow, "⇀"},
    {"xrightharpoondown", K::ExtensibleArrow, "⇁"}, {"xleftharpoonup", K::ExtensibleArrow, "↼"},
    {"xleftharpoondown", K::ExtensibleArrow, "↽"}, {"xrightleftharpoons", K::ExtensibleArrow, "⇌"},
    {"xleftrightharpoons", K::ExtensibleArrow, "⇋"}, {"xtwoheadrightarrow", K::ExtensibleArrow, "↠"},
    {"xtwoheadleftarrow", K::ExtensibleArrow, "↞"}, {"xlongequal", K::ExtensibleArrow, "="},
    {"overset", K::OverUnderSet, {}, 0}, {"underset", K::OverUnderSet, {}, 1}, {"stackrel", K::OverUnderSet, {}, 2},
    // Fonts.
    {"mathrm", K::Font, {}, static_cast<int>(Font::Roman)},
    {"mathup", K::Font, {}, static_cast<int>(Font::Roman)},
    {"mathit", K::Font, {}, static_cast<int>(Font::Italic)},
    {"mathnormal", K::Font, {}, static_cast<int>(Font::Default)},
    {"mathbf", K::Font, {}, static_cast<int>(Font::Bold)},
    {"mathbfit", K::Font, {}, static_cast<int>(Font::BoldItalic)},
    {"boldsymbol", K::Font, {}, static_cast<int>(Font::BoldSymbol)},
    {"bm", K::Font, {}, static_cast<int>(Font::BoldSymbol)},
    {"pmb", K::Font, {}, static_cast<int>(Font::BoldSymbol)},
    {"mathsf", K::Font, {}, static_cast<int>(Font::Sans)},
    {"mathsfit", K::Font, {}, static_cast<int>(Font::SansItalic)},
    {"mathtt", K::Font, {}, static_cast<int>(Font::Mono)},
    {"mathcal", K::Font, {}, static_cast<int>(Font::Script)},
    {"mathscr", K::Font, {}, static_cast<int>(Font::Script)},
    {"mathfrak", K::Font, {}, static_cast<int>(Font::Fraktur)},
    {"mathbb", K::Font, {}, static_cast<int>(Font::DoubleStruck)},
    {"Bbb", K::Font, {}, static_cast<int>(Font::DoubleStruck)},
    {"rm", K::FontSwitch, {}, static_cast<int>(Font::Roman)},
    {"it", K::FontSwitch, {}, static_cast<int>(Font::Italic)},
    {"bf", K::FontSwitch, {}, static_cast<int>(Font::Bold)},
    {"sf", K::FontSwitch, {}, static_cast<int>(Font::Sans)},
    {"tt", K::FontSwitch, {}, static_cast<int>(Font::Mono)},
    {"cal", K::FontSwitch, {}, static_cast<int>(Font::Script)},
    {"text", K::Text, {}, kTextKeep}, {"textrm", K::Text, {}, kTextRoman}, {"textup", K::Text, {}, kTextUpright},
    {"textnormal", K::Text, {}, kTextNormal}, {"mbox", K::Text, {}, kTextKeep}, {"hbox", K::Text, {}, kTextKeep},
    {"textbf", K::Text, {}, kTextBold}, {"textit", K::Text, {}, kTextItalic}, {"textsl", K::Text, {}, kTextItalic},
    {"emph", K::Text, {}, kTextItalic}, {"textsf", K::Text, {}, kTextSans}, {"texttt", K::Text, {}, kTextMono},
    {"fbox", K::Text, {}, kTextKeep},
    // Spacing and styles.
    {",", K::Space, "0.1667em"}, {"thinspace", K::Space, "0.1667em"}, {":", K::Space, "0.2222em"},
    {">", K::Space, "0.2222em"}, {"medspace", K::Space, "0.2222em"}, {";", K::Space, "0.2778em"},
    {"thickspace", K::Space, "0.2778em"}, {"!", K::Space, "-0.1667em"}, {"negthinspace", K::Space, "-0.1667em"},
    {"negmedspace", K::Space, "-0.2222em"}, {"negthickspace", K::Space, "-0.2778em"}, {" ", K::Space, "0.3333em"},
    {"enspace", K::Space, "0.5em"}, {"enskip", K::Space, "0.5em"}, {"quad", K::Space, "1em"},
    {"qquad", K::Space, "2em"}, {"hspace", K::HSpace}, {"kern", K::Kern, {}, 0}, {"hskip", K::Kern, {}, 0},
    {"mkern", K::Kern, {}, 1}, {"mskip", K::Kern, {}, 1},
    {"displaystyle", K::Style, {}, 0}, {"textstyle", K::Style, {}, 1}, {"scriptstyle", K::Style, {}, 2},
    {"scriptscriptstyle", K::Style, {}, 3},
    {"color", K::Color}, {"textcolor", K::TextColor}, {"colorbox", K::ColorBox},
    {"phantom", K::Phantom, {}, 0}, {"hphantom", K::Phantom, {}, 1}, {"vphantom", K::Phantom, {}, 2},
    {"smash", K::Smash}, {"strut", K::Strut, {}, 0}, {"mathstrut", K::Strut, {}, 1},
    {"not", K::Not}, {"boxed", K::Boxed}, {"begin", K::Begin}, {"substack", K::Substack}, {"tag", K::Tag},
    {"limits", K::Limits, {}, static_cast<int>(Limits::Always)},
    {"nolimits", K::Limits, {}, static_cast<int>(Limits::Side)},
    {"displaylimits", K::Limits, {}, static_cast<int>(Limits::Movable)},
    {"label", K::SkipArgument}, {"nonumber", K::Ignore}, {"notag", K::Ignore}, {"hline", K::Ignore},
    {"hdashline", K::Ignore}, {"relax", K::Ignore}, {"allowbreak", K::Ignore}, {"nobreak", K::Ignore},
};

const Entry* lookup(std::string_view name) {
  static const auto* const table = [] {
    auto* map = new std::unordered_map<std::string_view, const Entry*>();
    for (const auto& entry : kEntries) map->emplace(entry.name, &entry);
    return map;
  }();
  const auto found = table->find(name);
  return found == table->end() ? nullptr : found->second;
}

// \not followed by one of these becomes the precomposed negation.
std::string_view negation(std::string_view name) {
  static const std::pair<std::string_view, std::string_view> kNegations[] = {
      {"=", "≠"}, {"<", "≮"}, {">", "≯"}, {"in", "∉"}, {"ni", "∌"}, {"owns", "∌"}, {"leq", "≰"}, {"le", "≰"},
      {"geq", "≱"}, {"ge", "≱"}, {"subset", "⊄"}, {"supset", "⊅"}, {"subseteq", "⊈"}, {"supseteq", "⊉"},
      {"equiv", "≢"}, {"sim", "≁"}, {"simeq", "≄"}, {"approx", "≉"}, {"cong", "≇"}, {"mid", "∤"},
      {"parallel", "∦"}, {"exists", "∄"}, {"prec", "⊀"}, {"succ", "⊁"}, {"preceq", "⋠"}, {"succeq", "⋡"},
      {"vdash", "⊬"}, {"models", "⊭"}, {"vDash", "⊭"}, {"Vdash", "⊮"}, {"leftarrow", "↚"}, {"gets", "↚"},
      {"rightarrow", "↛"}, {"to", "↛"}, {"Leftarrow", "⇍"}, {"Rightarrow", "⇏"}, {"leftrightarrow", "↮"},
      {"Leftrightarrow", "⇎"}, {"asymp", "≭"}, {"sqsubseteq", "⋢"}, {"sqsupseteq", "⋣"}, {"lesssim", "≴"},
      {"gtrsim", "≵"}, {"lessgtr", "≸"}, {"gtrless", "≹"}, {"trianglelefteq", "⋬"}, {"trianglerighteq", "⋭"},
      {"vartriangleleft", "⋪"}, {"vartriangleright", "⋫"},
  };
  for (const auto& [from, to] : kNegations) {
    if (from == name) return to;
  }
  return {};
}

// CSS colour names accepted by \color and friends (case-insensitive).
bool isColourName(std::string_view name) {
  static const std::string_view kNames[] = {
      "aqua", "black", "blue", "brown", "chocolate", "coral", "crimson", "cyan", "darkblue", "darkcyan",
      "darkgray", "darkgreen", "darkgrey", "darkmagenta", "darkorange", "darkred", "darkviolet", "deeppink",
      "dodgerblue", "firebrick", "forestgreen", "fuchsia", "gold", "goldenrod", "gray", "green", "grey",
      "hotpink", "indigo", "khaki", "lightblue", "lightgray", "lightgreen", "lightgrey", "lime", "limegreen",
      "magenta", "maroon", "navy", "olive", "orange", "orangered", "orchid", "pink", "plum", "purple", "red",
      "royalblue", "salmon", "seagreen", "sienna", "silver", "skyblue", "slateblue", "slategray", "steelblue",
      "tan", "teal", "tomato", "turquoise", "violet", "white", "yellow", "yellowgreen",
  };
  return std::find(std::begin(kNames), std::end(kNames), name) != std::end(kNames);
}

bool isHex(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}

std::string lower(std::string_view text) {
  std::string out(text);
  for (auto& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
  return text;
}

// A colour as a CSS value: a known name, #rgb or #rrggbb, or with the
// xcolor model "HTML" six hex digits. nullopt for anything else.
std::optional<std::string> cssColour(std::string_view model, std::string_view value) {
  value = trim(value);
  if (model == "HTML") {
    if (value.size() == 6 && isHex(value)) return "#" + lower(value);
    return std::nullopt;
  }
  if (!model.empty()) return std::nullopt;
  if (!value.empty() && value.front() == '#') {
    const auto digits = value.substr(1);
    if ((digits.size() == 3 || digits.size() == 6) && isHex(digits)) return lower(value);
    return std::nullopt;
  }
  auto name = lower(value);
  if (isColourName(name)) return name;
  return std::nullopt;
}

std::string formatNumber(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%.4f", value);
  std::string text(buffer);
  while (!text.empty() && text.back() == '0') text.pop_back();
  if (!text.empty() && text.back() == '.') text.pop_back();
  if (text == "-0") text = "0";
  return text;
}

// A TeX length as a CSS length: an optional sign, at most six digits on
// either side of an optional point, and one of the units em ex pt px pc cm
// mm in bp mu. Math units become em (18 mu to the em) and big points become
// CSS points, which have the same size. When `mu_only`, mu is required.
std::optional<std::string> cssLength(std::string_view text, bool mu_only) {
  text = trim(text);
  std::size_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
  double value = 0;
  std::size_t whole = 0;
  std::size_t fraction = 0;
  while (i < text.size() && isDigit(text[i])) {
    value = value * 10 + (text[i++] - '0');
    if (++whole > 6) return std::nullopt;
  }
  if (i < text.size() && (text[i] == '.' || text[i] == ',')) {
    ++i;
    double scale = 0.1;
    while (i < text.size() && isDigit(text[i])) {
      value += scale * (text[i++] - '0');
      scale /= 10;
      if (++fraction > 6) return std::nullopt;
    }
  }
  if (whole + fraction == 0) return std::nullopt;
  const auto unit = trim(text.substr(i));
  if (negative) value = -value;
  if (unit == "mu") return formatNumber(value / 18) + "em";
  if (mu_only) return std::nullopt;
  if (unit == "bp") return formatNumber(value) + "pt";
  for (const std::string_view known : {"em", "ex", "pt", "px", "pc", "cm", "mm", "in"}) {
    if (unit == known) return formatNumber(value) + std::string(known);
  }
  return std::nullopt;
}

// Unicode typed directly: identifiers, or operators with a TeX class.
struct Classified {
  bool operator_ = false;
  Cls cls = Cls::Ord;
  Limits limits = Limits::Side;
  bool fixed = false;  // a bracket that must not stretch
};

Classified classify(char32_t c) {
  const auto op = [](Cls cls, Limits limits = Limits::Side, bool fixed = false) {
    return Classified{true, cls, limits, fixed};
  };
  switch (c) {
    case 0x221E: case 0x2202: case 0x2205: case 0x2207: case 0x2220: case 0x2221: case 0x2222:
      return {};
    case 0x2200: case 0x2203: case 0x2204: case 0xAC: case 0x2026: case 0x22EE: case 0x22EF: case 0x22F0:
    case 0x22F1: case 0x2057:
      return op(Cls::Ord);
    case 0x2016: return op(Cls::Ord, Limits::Side, true);
    case 0xB1: case 0xD7: case 0xF7: case 0xB7: case 0x2212: case 0x2213: case 0x2214: case 0x2216: case 0x2217:
    case 0x2218: case 0x2219: case 0x2227: case 0x2228: case 0x2229: case 0x222A: case 0x228E: case 0x22C4:
    case 0x22C5: case 0x22C6:
      return op(Cls::Bin);
    case 0x2211: case 0x220F: case 0x2210: case 0x22C0: case 0x22C1: case 0x22C2: case 0x22C3:
      return op(Cls::Op, Limits::Movable);
    case 0x2308: case 0x230A: case 0x2329: case 0x27E6: case 0x27E8: case 0x27EA: case 0x27EC: case 0x27EE:
    case 0x2983: case 0x2985: case 0x2987: case 0x2989: case 0x298B:
      return op(Cls::Open, Limits::Side, true);
    case 0x2309: case 0x230B: case 0x232A: case 0x27E7: case 0x27E9: case 0x27EB: case 0x27ED: case 0x27EF:
    case 0x2984: case 0x2986: case 0x2988: case 0x298A: case 0x298C:
      return op(Cls::Close, Limits::Side, true);
    default: break;
  }
  if (c >= 0x2032 && c <= 0x2037) return op(Cls::Ord);
  if (c >= 0x222B && c <= 0x2233) return op(Cls::Op);
  if (c >= 0x2A00 && c <= 0x2A0A) return op(Cls::Op, Limits::Movable);
  if (c >= 0x2A0B && c <= 0x2A1C) return op(Cls::Op);
  if (c >= 0x2293 && c <= 0x22A1) return op(Cls::Bin);
  if ((c >= 0x2190 && c <= 0x22FF) || (c >= 0x27C0 && c <= 0x27FF) || (c >= 0x2900 && c <= 0x2AFF) ||
      (c >= 0x2B00 && c <= 0x2BFF)) {
    return op(Cls::Rel);
  }
  return {};
}

// ------------------------------------------------------------------ lists

bool isSign(std::string_view text) { return text == "+" || text == "−" || text == "±" || text == "∓"; }

// Builds one row of math: decides unary signs, adds the function application
// operator after named functions, and wraps the parts of the row that follow
// a style or colour switch in <mstyle>. Each switch closes the previous part
// and opens a sibling that carries every attribute in force, so a run of
// switches never nests.
class ListBuilder {
 public:
  explicit ListBuilder(Cls before = Cls::Open) : last_(before) {}

  void push(Node node) {
    if (node.cls == Cls::Space) {
      if (pending_function_) apply(false);
      current_.push_back(std::move(node));
      return;
    }
    if (node.tag == "mo" && node.cls == Cls::Bin && isSign(node.text)) {
      // A sign is unary where TeX would make it ordinary: at the start, after
      // an operator, relation, opening bracket, punctuation or function.
      switch (last_) {
        case Cls::Op: case Cls::Bin: case Cls::Rel: case Cls::Open: case Cls::Punct: case Cls::Func:
          node.cls = Cls::Ord;
          node.attributes.emplace_back("form", "prefix");
          break;
        default:
          // A binary sign that starts a row (the right half of an aligned
          // pair) would otherwise be taken for a prefix by the browser.
          if (empty_) node.attributes.emplace_back("form", "infix");
      }
    }
    if (pending_function_) apply(node.cls == Cls::Ord || node.cls == Cls::Op || node.cls == Cls::Func ||
                                 node.cls == Cls::Inner);
    pending_function_ = node.cls == Cls::Func;
    last_ = node.cls;
    empty_ = false;
    current_.push_back(std::move(node));
  }

  void restyle(const Attributes& changes) {
    close();
    for (const auto& [name, value] : changes) {
      const auto found = std::find_if(style_.begin(), style_.end(), [&](const auto& a) { return a.first == name; });
      if (found == style_.end()) style_.emplace_back(name, value);
      else found->second = value;
    }
  }

  const Attributes& style() const { return style_; }

  // A function name that ends the row gets no application operator: within
  // a group (\underset{x}{\min}) the row that continues past the group
  // supplies it.
  std::vector<Node> finish() {
    pending_function_ = false;
    close();
    return std::move(done_);
  }

 private:
  // U+2061 FUNCTION APPLICATION; before an ordinary operand it carries TeX's
  // thin space between an operator name and its argument, which browsers do
  // not add by themselves.
  void apply(bool spaced) {
    Node node = token("mo", "⁡");
    if (spaced) node.attributes.emplace_back("rspace", "0.1667em");
    current_.push_back(std::move(node));
    pending_function_ = false;
  }

  void close() {
    if (current_.empty()) return;
    if (style_.empty()) {
      for (auto& node : current_) done_.push_back(std::move(node));
    } else {
      Node wrapper = element("mstyle", std::move(current_));
      wrapper.attributes = style_;
      done_.push_back(std::move(wrapper));
    }
    current_.clear();
  }

  std::vector<Node> done_;
  std::vector<Node> current_;
  Attributes style_;
  Cls last_;
  bool empty_ = true;
  bool pending_function_ = false;
};

// ------------------------------------------------------------ environments

enum class Columns : unsigned char { Centered, Left, Pairs, Specified };

struct Environment {
  std::string_view name;
  Columns columns = Columns::Centered;
  std::string_view open = {};
  std::string_view close = {};
  bool display = false;
  bool small = false;
  bool argument = false;  // array/subarray column spec, alignat column count
  bool single = false;    // no `&` columns
};

const Environment kEnvironments[] = {
    {"matrix"},
    {"pmatrix", Columns::Centered, "(", ")"},
    {"bmatrix", Columns::Centered, "[", "]"},
    {"Bmatrix", Columns::Centered, "{", "}"},
    {"vmatrix", Columns::Centered, "|", "|"},
    {"Vmatrix", Columns::Centered, "‖", "‖"},
    {"smallmatrix", Columns::Centered, {}, {}, false, true},
    {"cases", Columns::Left, "{", {}},
    {"dcases", Columns::Left, "{", {}, true},
    {"rcases", Columns::Left, {}, "}"},
    {"drcases", Columns::Left, {}, "}", true},
    {"aligned", Columns::Pairs, {}, {}, true},
    {"align", Columns::Pairs, {}, {}, true},
    {"align*", Columns::Pairs, {}, {}, true},
    {"flalign", Columns::Pairs, {}, {}, true},
    {"flalign*", Columns::Pairs, {}, {}, true},
    {"split", Columns::Pairs, {}, {}, true},
    {"alignat", Columns::Pairs, {}, {}, true, false, true},
    {"alignat*", Columns::Pairs, {}, {}, true, false, true},
    {"alignedat", Columns::Pairs, {}, {}, true, false, true},
    {"gathered", Columns::Centered, {}, {}, true, false, false, true},
    {"gather", Columns::Centered, {}, {}, true, false, false, true},
    {"gather*", Columns::Centered, {}, {}, true, false, false, true},
    {"multline", Columns::Centered, {}, {}, true, false, false, true},
    {"multline*", Columns::Centered, {}, {}, true, false, false, true},
    {"equation", Columns::Centered, {}, {}, true, false, false, true},
    {"equation*", Columns::Centered, {}, {}, true, false, false, true},
    {"array", Columns::Specified, {}, {}, false, false, true},
    {"subarray", Columns::Specified, {}, {}, false, true, true},
};

const Environment* findEnvironment(std::string_view name) {
  for (const auto& environment : kEnvironments) {
    if (environment.name == name) return &environment;
  }
  return nullptr;
}

// Rows of cells, each cell a list of nodes.
struct Table {
  std::vector<std::vector<std::vector<Node>>> rows;
  std::size_t columns = 0;
};

// How a table ends: at the end of the source (a formula of its own), at
// \end{name}, or at the `}` of \substack.
enum class Close : unsigned char { Input, End, Brace };

constexpr unsigned kStopBrace = 1;    // `}`
constexpr unsigned kStopBracket = 2;  // `]` of an optional argument
constexpr unsigned kStopRight = 4;    // \middle, \right
constexpr unsigned kStopCell = 8;     // `&`, `\\`, \cr, \newline, \end
constexpr unsigned kStopDollar = 16;  // `$` closing math inside \text

struct DepthExceeded {};

struct TextFont {
  bool bold = false;
  bool italic = false;
  int family = 0;  // 0 roman, 1 sans-serif, 2 monospace
};

TextFont changeTextFont(TextFont font, int change) {
  switch (change) {
    case kTextBold: font.bold = true; break;
    case kTextItalic: font.italic = true; break;
    case kTextSans: font.family = 1; break;
    case kTextMono: font.family = 2; break;
    case kTextRoman: font.family = 0; break;
    case kTextUpright: font.italic = false; break;
    case kTextNormal: font = TextFont{}; break;
    default: break;
  }
  return font;
}

Font mathFont(TextFont font) {
  if (font.family == 2) return Font::Mono;
  if (font.family == 1) {
    if (font.bold) return font.italic ? Font::SansBoldItalic : Font::SansBold;
    return font.italic ? Font::SansItalic : Font::Sans;
  }
  if (font.bold) return font.italic ? Font::BoldItalic : Font::Bold;
  return font.italic ? Font::Italic : Font::Default;
}

// Collects the text of a \text argument into <mtext> runs, with nested math
// between them. Runs of white space become one space; a space at either end
// of a run becomes U+00A0, which a browser does not trim.
class TextBuilder {
 public:
  void add(std::string_view text) { buffer_ += text; }
  void addCodePoint(char32_t c, TextFont font) { appendUtf8(buffer_, styled(c, mathFont(font))); }
  void space() {
    if (buffer_.empty() || buffer_.back() != ' ') buffer_ += ' ';
  }
  void push(Node node) {
    flush();
    nodes_.push_back(std::move(node));
  }
  std::vector<Node> finish() {
    flush();
    return std::move(nodes_);
  }

 private:
  void flush() {
    if (buffer_.empty()) return;
    std::string text;
    if (buffer_.front() == ' ') {
      text = " ";
      buffer_.erase(0, 1);
    }
    const bool trailing = !buffer_.empty() && buffer_.back() == ' ';
    if (trailing) buffer_.pop_back();
    text += buffer_;
    if (trailing) text += " ";
    nodes_.push_back(token("mtext", std::move(text)));
    buffer_.clear();
  }

  std::string buffer_;
  std::vector<Node> nodes_;
};

// ----------------------------------------------------------------- parser

class Parser {
 public:
  Parser(std::string_view source, bool display, std::vector<std::string>& errors)
      : s_(source), display_(display), errors_(errors) {}

  Node parseFormula();

 private:
  struct State {
    Font font = Font::Default;
    bool display = false;
  };

  class Guard {
   public:
    explicit Guard(Parser& parser) : parser_(parser) {
      if (++parser_.depth_ > kMaximumMathDepth) {
        --parser_.depth_;
        throw DepthExceeded{};
      }
    }
    ~Guard() { --parser_.depth_; }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

   private:
    Parser& parser_;
  };

  // Lexing.
  bool atEnd() const { return i_ >= s_.size(); }
  bool at(char c) const { return i_ < s_.size() && s_[i_] == c; }
  void skipSpace();
  std::string_view peekCommand() const;
  std::string_view readCommand();
  std::optional<std::string_view> readRawGroup();
  std::optional<std::string_view> readRawBracket();
  std::optional<std::string> readEnvironmentName();

  // Errors.
  void error(std::string message);
  static Node errorNode(std::string_view shown) {
    return element("merror", nodes(token("mtext", std::string(shown))));
  }
  Node fail(std::string message, std::string_view shown) {
    error(std::move(message));
    return errorNode(shown);
  }

  // Math.
  std::vector<Node> parseList(unsigned stops, State state, Cls before = Cls::Open);
  void parseScripted(ListBuilder& list, const State& state);
  std::optional<Node> parseAtom(const State& state, bool single);
  std::optional<Node> parseCommand(const State& state);
  Node parseArgument(const State& state, std::string_view what);
  void closeGroup(std::vector<Node>& items);
  Node parseLetters(const State& state, bool single);
  Node parseNumber(const State& state, bool single);
  Node parseUnicode(const State& state);
  std::optional<std::string> parseDelimiter(std::string_view command);
  Node parseLeft(const State& state);
  Node parseNot(const State& state);
  std::optional<std::string> parseColourArgument(std::string_view command);
  std::optional<Node> parseOptionalScript(const State& state, std::string_view what);

  // Text.
  std::vector<Node> parseTextArgument(TextFont font, std::string_view command, std::string_view prefix = {},
                                      std::string_view suffix = {});
  void parseTextBody(TextBuilder& text, TextFont font);

  // Environments.
  Node parseEnvironment(const Environment& environment, std::string_view name, const State& state);
  Table parseRows(const Environment& environment, std::string_view name, const State& outer, Close close);
  std::vector<std::string_view> parseColumnSpec();
  Node buildTable(const Environment& environment, Table table, const std::vector<std::string_view>& aligns);

  std::string_view s_;
  std::size_t i_ = 0;
  std::size_t depth_ = 0;
  bool display_;
  std::vector<std::string>& errors_;
  std::optional<Node> tag_;
};

void Parser::error(std::string message) {
  if (errors_.size() < kMaximumMathErrors) errors_.push_back(std::move(message));
  else if (errors_.size() == kMaximumMathErrors) errors_.push_back("further errors omitted");
}

// White space and `%` comments, which math mode ignores.
void Parser::skipSpace() {
  while (i_ < s_.size()) {
    if (isSpace(s_[i_])) {
      ++i_;
    } else if (s_[i_] == '%') {
      while (i_ < s_.size() && s_[i_] != '\n') ++i_;
    } else {
      break;
    }
  }
}

// The name of the command at the cursor without consuming it: a run of ASCII
// letters, or the single character after the backslash (`\\`, `\,`, `\{`; a
// backslash before white space is a control space). Empty when the cursor is
// not at a command, or at a backslash that ends the source.
std::string_view Parser::peekCommand() const {
  if (!at('\\') || i_ + 1 >= s_.size()) return {};
  std::size_t end = i_ + 1;
  if (isLetter(s_[end])) {
    while (end < s_.size() && isLetter(s_[end])) ++end;
    return s_.substr(i_ + 1, end - i_ - 1);
  }
  if (isSpace(s_[end])) return " ";
  std::size_t next = end;
  decodeUtf8(s_, next);
  return s_.substr(end, next - end);
}

std::string_view Parser::readCommand() {
  const auto name = peekCommand();
  if (!at('\\')) return {};
  ++i_;
  i_ += name == " " ? 1 : name.size();
  return name;
}

// The text of a balanced `{…}` group, consumed whole; nullopt when the
// cursor (after white space) is not at `{`.
std::optional<std::string_view> Parser::readRawGroup() {
  skipSpace();
  if (!at('{')) return std::nullopt;
  const std::size_t start = ++i_;
  std::size_t depth = 0;
  while (i_ < s_.size()) {
    const char c = s_[i_];
    if (c == '\\') {
      i_ += 2;
      continue;
    }
    if (c == '{') ++depth;
    if (c == '}') {
      if (depth == 0) {
        const auto text = s_.substr(start, i_ - start);
        ++i_;
        return text;
      }
      --depth;
    }
    ++i_;
  }
  i_ = s_.size();
  error("missing }");
  return s_.substr(start);
}

// An optional `[…]` argument read as text; nullopt when there is none.
std::optional<std::string_view> Parser::readRawBracket() {
  skipSpace();
  if (!at('[')) return std::nullopt;
  const std::size_t start = ++i_;
  const auto end = s_.find(']', start);
  if (end == std::string_view::npos) {
    i_ = s_.size();
    error("missing ]");
    return s_.substr(start);
  }
  i_ = end + 1;
  return s_.substr(start, end - start);
}

std::optional<std::string> Parser::readEnvironmentName() {
  const auto raw = readRawGroup();
  if (!raw) return std::nullopt;
  const auto name = trim(*raw);
  if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return isLetter(c) || c == '*'; })) {
    return std::nullopt;
  }
  return std::string(name);
}

Node Parser::parseFormula() {
  Environment formula{"", Columns::Centered, {}, {}, display_, false, false, true};
  Table table = parseRows(formula, "", State{Font::Default, display_}, Close::Input);
  if (table.rows.size() == 1 && table.rows.front().size() == 1) {
    return element("mrow", std::move(table.rows.front().front()));
  }
  return element("mrow", nodes(buildTable(formula, std::move(table), {})));
}

std::vector<Node> Parser::parseList(unsigned stops, State state, Cls before) {
  const Guard guard(*this);
  ListBuilder list(before);
  std::optional<std::vector<Node>> numerator;
  int infix = 0;
  for (;;) {
    skipSpace();
    if (atEnd()) break;
    const char c = s_[i_];
    if (c == '}') {
      if (stops & kStopBrace) break;
      ++i_;
      list.push(fail("unmatched }", "}"));
      continue;
    }
    if (c == ']' && (stops & kStopBracket)) break;
    if (c == '$') {
      if (stops & kStopDollar) break;
      ++i_;
      list.push(fail("unexpected $ in math", "$"));
      continue;
    }
    if (c == '&') {
      if (stops & kStopCell) break;
      ++i_;
      list.push(fail("misplaced &", "&"));
      continue;
    }
    if (c == '\\') {
      const auto name = peekCommand();
      if (name == "\\" || name == "cr" || name == "newline" || name == "end") {
        if (stops & kStopCell) break;
        readCommand();
        if (name == "end") {
          const auto ended = readEnvironmentName();
          list.push(fail("\\end{" + ended.value_or("") + "} without \\begin", "\\end"));
        } else {
          list.push(fail("misplaced \\" + std::string(name), "\\" + std::string(name)));
        }
        continue;
      }
      if (name == "right" || name == "middle") {
        if (stops & kStopRight) break;
        readCommand();
        const std::string command = "\\" + std::string(name);
        parseDelimiter(command);
        list.push(fail(command + " without \\left", command));
        continue;
      }
      if (const Entry* entry = lookup(name)) {
        if (entry->kind == K::Infix) {
          readCommand();
          if (numerator) {
            list.push(fail("ambiguous fraction: \\" + std::string(name) + " after another", "\\" + std::string(name)));
            continue;
          }
          numerator = list.finish();
          infix = entry->n;
          const Attributes style = list.style();
          list = ListBuilder(Cls::Open);
          list.restyle(style);
          continue;
        }
        if (entry->kind == K::Style) {
          readCommand();
          static const char* const kLevels[] = {"0", "0", "1", "2"};
          state.display = entry->n == 0;
          list.restyle({{"displaystyle", state.display ? "true" : "false"}, {"scriptlevel", kLevels[entry->n]}});
          continue;
        }
        if (entry->kind == K::Color) {
          readCommand();
          if (const auto colour = parseColourArgument("\\color")) list.restyle({{"mathcolor", *colour}});
          continue;
        }
        if (entry->kind == K::FontSwitch) {
          readCommand();
          state.font = static_cast<Font>(entry->n);
          continue;
        }
      }
    }
    parseScripted(list, state);
  }
  auto items = list.finish();
  if (!numerator) return items;
  Node fraction = element("mfrac", nodes(rowOf(std::move(*numerator)), rowOf(std::move(items))));
  if (infix != 0) fraction.attributes.emplace_back("linethickness", "0");
  if (infix == 2) return nodes(element("mrow", nodes(fence("(", "prefix"), std::move(fraction), fence(")", "postfix"))));
  return nodes(std::move(fraction));
}

std::string primeText(std::size_t count) {
  switch (count) {
    case 1: return "′";
    case 2: return "″";
    case 3: return "‴";
    case 4: return "⁗";
    default: {
      std::string text;
      for (std::size_t k = 0; k < count; ++k) text += "′";
      return text;
    }
  }
}

void Parser::parseScripted(ListBuilder& list, const State& state) {
  std::optional<Node> base;
  if (!at('^') && !at('_') && !at('\'')) {
    base = parseAtom(state, false);
    if (!base) return;
  }
  std::optional<Node> sub;
  std::optional<Node> sup;
  std::size_t primes = 0;
  std::vector<Node> stray;
  State script = state;
  script.display = false;
  for (;;) {
    skipSpace();
    if (atEnd()) break;
    const char c = s_[i_];
    if (c == '^' || c == '_') {
      ++i_;
      const bool upper = c == '^';
      Node argument = parseArgument(script, upper ? "superscript" : "subscript");
      auto& slot = upper ? sup : sub;
      if (slot) {
        // Shown after the atom rather than dropped, next to the error.
        stray.push_back(fail(upper ? "double superscript" : "double subscript", upper ? "^" : "_"));
        stray.push_back(std::move(argument));
      } else {
        slot = std::move(argument);
      }
      continue;
    }
    if (c == '\'') {
      if (sup) {
        ++i_;
        stray.push_back(fail("double superscript", "′"));
        continue;
      }
      while (at('\'')) ++primes, ++i_;
      continue;
    }
    if (c == '\\') {
      const auto name = peekCommand();
      const Entry* entry = lookup(name);
      if (entry && entry->kind == K::Limits) {
        readCommand();
        if (base && (base->cls == Cls::Op || base->cls == Cls::Func) && !sub && !sup && primes == 0) {
          base->limits = static_cast<Limits>(entry->n);
        } else {
          stray.push_back(fail("\\" + std::string(name) + " must follow an operator", "\\" + std::string(name)));
        }
        continue;
      }
    }
    break;
  }
  Node node = base ? std::move(*base) : element("mrow", {});
  if (primes > 0) {
    Node mark = token("mo", primeText(primes));
    sup = sup ? element("mrow", nodes(std::move(mark), std::move(*sup))) : std::move(mark);
  }
  if (sub || sup) {
    const Cls cls = node.cls;
    const bool under = node.limits == Limits::Always || (node.limits == Limits::Movable && state.display);
    // A browser moves the limits of ∑ and friends to the side outside display
    // style by itself; \limits has to say that they stay where they are.
    if (node.limits == Limits::Always && node.tag == "mo") node.attributes.emplace_back("movablelimits", "false");
    std::string_view tag;
    if (sub && sup) tag = under ? "munderover" : "msubsup";
    else if (sub) tag = under ? "munder" : "msub";
    else tag = under ? "mover" : "msup";
    std::vector<Node> children = nodes(std::move(node));
    if (sub) children.push_back(std::move(*sub));
    if (sup) children.push_back(std::move(*sup));
    node = element(tag, std::move(children), cls);
  }
  list.push(std::move(node));
  for (auto& extra : stray) list.push(std::move(extra));
}

void Parser::closeGroup(std::vector<Node>& items) {
  if (at('}')) {
    ++i_;
    return;
  }
  items.push_back(fail("missing }", "missing }"));
}

// One argument: a `{…}` group, or a single token — one letter, one digit,
// one symbol, or one command together with its own arguments, as in
// \frac12 or x^\alpha.
Node Parser::parseArgument(const State& state, std::string_view what) {
  skipSpace();
  const auto name = peekCommand();
  if (atEnd() || at('}') || at('&') || at('$') || at('^') || at('_') || name == "\\" || name == "cr" ||
      name == "newline" || name == "end" || name == "right" || name == "middle") {
    return fail("missing argument for " + std::string(what), "missing argument");
  }
  if (at('{')) {
    ++i_;
    auto items = parseList(kStopBrace, state);
    closeGroup(items);
    return rowOf(std::move(items));
  }
  const Guard guard(*this);
  auto atom = parseAtom(state, true);
  return atom ? std::move(*atom) : element("mrow", {});
}

std::optional<Node> Parser::parseAtom(const State& state, bool single) {
  skipSpace();
  if (atEnd()) return std::nullopt;
  const char c = s_[i_];
  if (c == '{') {
    ++i_;
    auto items = parseList(kStopBrace, state);
    closeGroup(items);
    return groupOf(std::move(items));
  }
  if (c == '\\') return parseCommand(state);
  if (isLetter(c)) return parseLetters(state, single);
  if (isDigit(c) || (c == '.' && i_ + 1 < s_.size() && isDigit(s_[i_ + 1]))) return parseNumber(state, single);
  if (static_cast<unsigned char>(c) >= 0x80) return parseUnicode(state);
  ++i_;
  switch (c) {
    case '+': return token("mo", "+", Cls::Bin);
    case '-': return token("mo", "−", Cls::Bin);
    case '*': return token("mo", "∗", Cls::Bin);
    case '=': case '<': case '>': case ':': return token("mo", std::string(1, c), Cls::Rel);
    case ',': case ';': return token("mo", std::string(1, c), Cls::Punct);
    case '!': case '?': return token("mo", std::string(1, c), Cls::Close);
    case '(': case '[': return fixed(std::string(1, c), Cls::Open);
    case ')': case ']': return fixed(std::string(1, c), Cls::Close);
    case '|': case '/': return fixed(std::string(1, c), Cls::Ord);
    case '\'': return token("mo", "′");
    case '`': return token("mo", "‘");
    case '~': return space("0.3333em");
    case '#': return fail("unexpected #", "#");
    case '^': case '_': case '&': case '$': case '}':
      return fail(std::string("unexpected ") + c, std::string(1, c));
    default:
      if (isControl(static_cast<unsigned char>(c))) return token("mi", "�");
      return token("mi", std::string(1, c));  // . @ " and friends are ordinary
  }
}

// Letters are separate identifiers; under a font other than the default, a
// run of letters is one identifier (a word such as \mathrm{max} or
// \mathbf{AB}).
Node Parser::parseLetters(const State& state, bool single) {
  if (state.font == Font::Default || single) {
    Node node = identifier(s_.substr(i_++, 1), state.font, false);
    // Combining marks typed after a letter (x̂) belong to it.
    while (i_ < s_.size()) {
      std::size_t next = i_;
      const char32_t mark = decodeUtf8(s_, next);
      if (!isCombining(mark)) break;
      appendUtf8(node.text, mark);
      i_ = next;
    }
    return node;
  }
  const std::size_t start = i_;
  while (i_ < s_.size() && isLetter(s_[i_])) ++i_;
  if (i_ - start == 1) return identifier(s_.substr(start, 1), state.font, false);
  std::string text;
  for (std::size_t k = start; k < i_; ++k) appendUtf8(text, styled(static_cast<unsigned char>(s_[k]), state.font));
  return token("mi", std::move(text));
}

Node Parser::parseNumber(const State& state, bool single) {
  std::string text;
  const auto take = [&] { appendUtf8(text, styled(static_cast<unsigned char>(s_[i_++]), state.font)); };
  if (single) {
    take();
    return token("mn", std::move(text));
  }
  while (i_ < s_.size() && isDigit(s_[i_])) take();
  if (at('.') && i_ + 1 < s_.size() && isDigit(s_[i_ + 1])) {
    take();
    while (i_ < s_.size() && isDigit(s_[i_])) take();
  }
  return token("mn", std::move(text));
}

Node Parser::parseUnicode(const State& state) {
  char32_t c = decodeUtf8(s_, i_);
  if (isControl(c)) c = 0xFFFD;
  std::string text;
  appendUtf8(text, c);
  // Combining marks typed after a character belong to it.
  while (i_ < s_.size()) {
    std::size_t next = i_;
    const char32_t mark = decodeUtf8(s_, next);
    if (!isCombining(mark)) break;
    appendUtf8(text, mark);
    i_ = next;
  }
  const Classified kind = classify(c);
  if (!kind.operator_) {
    const int greek = greekIndex(c);
    return identifier(text, state.font, greek >= 0 && greek < 25);
  }
  Node node = kind.fixed ? fixed(std::move(text), kind.cls) : token("mo", std::move(text), kind.cls);
  node.limits = kind.limits;
  return node;
}

// The delimiter after \left, \middle, \right or \big: a bracket, bar, slash,
// arrow or `.` (none, returned as an empty string). nullopt after reporting
// an error.
std::optional<std::string> Parser::parseDelimiter(std::string_view command) {
  skipSpace();
  if (atEnd()) {
    error("missing delimiter after " + std::string(command));
    return std::nullopt;
  }
  const char c = s_[i_];
  if (c == '\\') {
    const auto name = peekCommand();
    const Entry* entry = lookup(name);
    const bool arrow = name == "uparrow" || name == "downarrow" || name == "updownarrow" || name == "Uparrow" ||
                       name == "Downarrow" || name == "Updownarrow" || name == "backslash";
    if (entry && (entry->kind == K::Open || entry->kind == K::Close || entry->kind == K::Fence || arrow)) {
      readCommand();
      return std::string(entry->text);
    }
    error("\\" + clip(name) + " is not a delimiter after " + std::string(command));
    return std::nullopt;
  }
  if (c == '.' || c == '(' || c == ')' || c == '[' || c == ']' || c == '|' || c == '/') {
    ++i_;
    return c == '.' ? std::string() : std::string(1, c);
  }
  if (c == '<' || c == '>') {
    ++i_;
    return c == '<' ? "⟨" : "⟩";
  }
  if (static_cast<unsigned char>(c) >= 0x80) {
    std::size_t next = i_;
    const char32_t code = decodeUtf8(s_, next);
    if (classify(code).operator_) {
      std::string text;
      appendUtf8(text, code);
      i_ = next;
      return text;
    }
  }
  error("missing delimiter after " + std::string(command));
  return std::nullopt;
}

Node Parser::parseLeft(const State& state) {
  std::vector<Node> inner;
  const auto open = parseDelimiter("\\left");
  if (!open) inner.push_back(errorNode("\\left"));
  else if (!open->empty()) inner.push_back(fence(*open, "prefix"));
  for (;;) {
    for (auto& node : parseList(kStopRight, state)) inner.push_back(std::move(node));
    const auto name = peekCommand();
    if (name == "middle") {
      readCommand();
      const auto middle = parseDelimiter("\\middle");
      if (!middle) {
        inner.push_back(errorNode("\\middle"));
      } else if (!middle->empty()) {
        Node bar = token("mo", *middle, Cls::Rel);
        bar.attributes.emplace_back("fence", "true");
        bar.attributes.emplace_back("stretchy", "true");
        bar.attributes.emplace_back("lspace", "0.1667em");
        bar.attributes.emplace_back("rspace", "0.1667em");
        inner.push_back(std::move(bar));
      }
      continue;
    }
    if (name == "right") {
      readCommand();
      const auto close = parseDelimiter("\\right");
      if (!close) inner.push_back(errorNode("\\right"));
      else if (!close->empty()) inner.push_back(fence(*close, "postfix"));
      break;
    }
    inner.push_back(fail("\\left without matching \\right", "missing \\right"));
    break;
  }
  return element("mrow", std::move(inner), Cls::Inner);
}

Node Parser::parseNot(const State& state) {
  skipSpace();
  if (at('=') || at('<') || at('>')) {
    return token("mo", std::string(negation(s_.substr(i_++, 1))), Cls::Rel);
  }
  const auto name = peekCommand();
  if (const auto negated = negation(name); !name.empty() && !negated.empty()) {
    readCommand();
    return token("mo", std::string(negated), name == "exists" ? Cls::Ord : Cls::Rel);
  }
  const Guard guard(*this);  // \not\not\not… recurses here
  auto atom = parseAtom(state, true);
  if (atom && atom->children.empty() && !atom->text.empty() &&
      (atom->tag == "mo" || atom->tag == "mi" || atom->tag == "mn")) {
    atom->text += "̸";  // COMBINING LONG SOLIDUS OVERLAY
    return std::move(*atom);
  }
  Node shown = errorNode("\\not");
  error("\\not must precede a symbol");
  if (!atom) return shown;
  return element("mrow", nodes(std::move(shown), std::move(*atom)));
}

std::optional<std::string> Parser::parseColourArgument(std::string_view command) {
  const auto model = readRawBracket();
  const auto value = readRawGroup();
  if (!value) {
    error("missing colour after " + std::string(command));
    return std::nullopt;
  }
  auto colour = cssColour(model ? trim(*model) : std::string_view(), *value);
  if (!colour) error("unsupported colour " + clip(*value));
  return colour;
}

// The `[…]` below an extensible arrow, as script-style math.
std::optional<Node> Parser::parseOptionalScript(const State& state, std::string_view what) {
  skipSpace();
  if (!at('[')) return std::nullopt;
  ++i_;
  auto items = parseList(kStopBracket, state);
  if (at(']')) ++i_;
  else items.push_back(fail("missing ] after " + std::string(what), "missing ]"));
  return rowOf(std::move(items));
}

std::optional<Node> Parser::parseCommand(const State& state) {
  const auto name = readCommand();
  if (name.empty()) {
    ++i_;
    return fail("lone backslash at the end", "\\");
  }
  const std::string command = "\\" + std::string(name);
  const Entry* entry = lookup(name);
  if (!entry) return fail("unknown command " + clip(command), clip(command));
  State script = state;
  script.display = false;
  switch (entry->kind) {
    case K::Ident: return identifier(entry->text, state.font, false);
    case K::Upright: return identifier(entry->text, state.font, true);
    case K::Ord: return token("mo", std::string(entry->text));
    case K::Fence: return fixed(std::string(entry->text), Cls::Ord);
    case K::Bin: return token("mo", std::string(entry->text), Cls::Bin);
    case K::Rel: return token("mo", std::string(entry->text), Cls::Rel);
    case K::Open: return fixed(std::string(entry->text), Cls::Open);
    case K::Close: return fixed(std::string(entry->text), Cls::Close);
    case K::Punct: {
      Node node = token("mo", std::string(entry->text), Cls::Punct);
      if (entry->n == 1) {
        node.attributes.emplace_back("lspace", "0");
        node.attributes.emplace_back("rspace", "0.1667em");
      }
      return node;
    }
    case K::Large:
    case K::Integral: {
      Node node = token("mo", std::string(entry->text), Cls::Op);
      node.limits = entry->kind == K::Large ? Limits::Movable : Limits::Side;
      return node;
    }
    case K::Function:
    case K::LimitFunction: {
      Node node = token("mi", std::string(entry->text), Cls::Func);
      node.limits = entry->kind == K::LimitFunction ? Limits::Movable : Limits::Side;
      return node;
    }
    case K::Frac: {
      // Fraction parts are in text style (limits at the side) unless \cfrac,
      // whose parts are in display style and stay full size.
      State part = state;
      part.display = entry->n == 3;
      if (entry->n == 3) readRawBracket();
      Node numerator = parseArgument(part, command);
      Node denominator = parseArgument(part, command);
      const auto displayed = [](Node node) {
        return with(with(element("mstyle", nodes(std::move(node))), "displaystyle", "true"), "scriptlevel", "0");
      };
      if (entry->n == 3) {
        numerator = displayed(std::move(numerator));
        denominator = displayed(std::move(denominator));
      }
      Node fraction = element("mfrac", nodes(std::move(numerator), std::move(denominator)));
      if (entry->n == 1 || entry->n == 3) return displayed(std::move(fraction));
      if (entry->n == 2) {
        return with(with(element("mstyle", nodes(std::move(fraction))), "displaystyle", "false"), "scriptlevel", "0");
      }
      return fraction;
    }
    case K::Binom: {
      Node top = parseArgument(script, command);
      Node bottom = parseArgument(script, command);
      Node binomial = element("mrow", nodes(fence("(", "prefix"),
                                            with(element("mfrac", nodes(std::move(top), std::move(bottom))),
                                                 "linethickness", "0"),
                                            fence(")", "postfix")));
      if (entry->n == 0) return binomial;
      const bool display = entry->n == 1;
      return with(with(element("mstyle", nodes(std::move(binomial))), "displaystyle", display ? "true" : "false"),
                  "scriptlevel", "0");
    }
    case K::Sqrt: {
      std::optional<Node> index = parseOptionalScript(script, command);
      Node radicand = parseArgument(state, command);
      if (index) return element("mroot", nodes(std::move(radicand), std::move(*index)));
      return element("msqrt", nodes(std::move(radicand)));
    }
    case K::Left: return parseLeft(state);
    case K::Big: {
      static const char* const kSizes[] = {"1.2em", "1.8em", "2.4em", "3em"};
      static const Cls kClasses[] = {Cls::Ord, Cls::Open, Cls::Close, Cls::Rel};
      static const char* const kForms[] = {nullptr, "prefix", "postfix", "infix"};
      const auto delimiter = parseDelimiter(command);
      if (!delimiter) return errorNode(command);
      if (delimiter->empty()) return std::nullopt;
      const int form = entry->n % 4;
      Node node = token("mo", *delimiter, kClasses[form]);
      if (kForms[form]) node.attributes.emplace_back("form", kForms[form]);
      node.attributes.emplace_back("stretchy", "true");
      node.attributes.emplace_back("minsize", kSizes[entry->n / 4]);
      node.attributes.emplace_back("maxsize", kSizes[entry->n / 4]);
      return node;
    }
    case K::Accent: {
      Node base = parseArgument(state, command);
      Node mark = with(token("mo", std::string(entry->text)), "stretchy", (entry->n & 1) ? "true" : "false");
      if (entry->n & 2) return with(element("munder", nodes(std::move(base), std::move(mark))), "accentunder", "true");
      return with(element("mover", nodes(std::move(base), std::move(mark))), "accent", "true");
    }
    case K::Brace: {
      Node base = parseArgument(state, command);
      Node brace = with(token("mo", std::string(entry->text)), "stretchy", "true");
      Node node = element(entry->n == 0 ? "mover" : "munder", nodes(std::move(base), std::move(brace)));
      node.limits = Limits::Always;  // a following ^ or _ labels the brace
      return node;
    }
    case K::ExtensibleArrow: {
      const auto padded = [](Node label) {
        return element("mrow", nodes(space("0.2778em"), std::move(label), space("0.2778em")));
      };
      std::optional<Node> below = parseOptionalScript(script, command);
      Node above = parseArgument(script, command);
      Node arrow = with(token("mo", std::string(entry->text)), "stretchy", "true");
      if (below) {
        return element("munderover", nodes(std::move(arrow), padded(std::move(*below)), padded(std::move(above))),
                       Cls::Rel);
      }
      return element("mover", nodes(std::move(arrow), padded(std::move(above))), Cls::Rel);
    }
    case K::OverUnderSet: {
      Node annotation = parseArgument(script, command);
      Node base = parseArgument(state, command);
      const Cls cls = entry->n == 2 ? Cls::Rel : base.cls;
      return element(entry->n == 1 ? "munder" : "mover", nodes(std::move(base), std::move(annotation)), cls);
    }
    case K::Font: {
      State styled_state = state;
      styled_state.font = static_cast<Font>(entry->n);
      return parseArgument(styled_state, command);
    }
    case K::FontSwitch:
    case K::Style:
      // Only reachable as a lone argument (x^\displaystyle), where the switch
      // has nothing left to apply to.
      return std::nullopt;
    case K::Color:
      parseColourArgument(command);
      return std::nullopt;
    case K::Infix: return fail("misplaced " + command, command);
    case K::Text: {
      auto parts = parseTextArgument(changeTextFont(TextFont{}, entry->n), command);
      return parts.empty() ? element("mrow", {}) : rowOf(std::move(parts));
    }
    case K::OperatorName: {
      const bool star = at('*');
      if (star) ++i_;
      State roman = state;
      roman.font = Font::Roman;
      Node node = parseArgument(roman, command);
      node.cls = Cls::Func;
      node.limits = star ? Limits::Movable : Limits::Side;
      return node;
    }
    case K::MathClass: {
      const auto cls = static_cast<Cls>(entry->n);
      Node node = parseArgument(state, command);
      const auto spaced = [&](const char* left, const char* right) {
        if (node.tag == "mo") {
          node.attributes.emplace_back("lspace", left);
          node.attributes.emplace_back("rspace", right);
          node.cls = cls;
          return std::move(node);
        }
        return element("mrow", nodes(space(left), std::move(node), space(right)), cls);
      };
      switch (cls) {
        case Cls::Op: node.cls = cls, node.limits = Limits::Movable; return node;
        case Cls::Bin: return spaced("0.2222em", "0.2222em");
        case Cls::Rel: return spaced("0.2778em", "0.2778em");
        case Cls::Punct: return spaced("0", "0.1667em");
        case Cls::Ord: case Cls::Open: case Cls::Close:
          if (node.tag == "mo") return spaced("0", "0");
          node.cls = cls;
          return node;
        default: node.cls = cls; return node;
      }
    }
    case K::Mod: {
      if (entry->n == 0) {
        return with(with(token("mo", "mod", Cls::Bin), "lspace", "0.2222em"), "rspace", "0.2222em");
      }
      Node argument = parseArgument(state, command);
      std::vector<Node> parts;
      if (entry->n == 2) {
        parts = nodes(space(state.display ? "1em" : "0.6667em"), token("mi", "mod"), space("0.3333em"),
                      std::move(argument));
      } else {
        parts.push_back(space(state.display ? "1em" : "0.4444em"));
        parts.push_back(fixed("(", Cls::Open));
        if (entry->n == 1) {
          parts.push_back(token("mi", "mod"));
          parts.push_back(space("0.3333em"));
        }
        parts.push_back(std::move(argument));
        parts.push_back(fixed(")", Cls::Close));
      }
      return element("mrow", std::move(parts));
    }
    case K::Space: return space(std::string(entry->text));
    case K::HSpace:
    case K::Kern: {
      if (entry->kind == K::HSpace && at('*')) ++i_;
      std::optional<std::string_view> raw = readRawGroup();
      if (!raw && entry->kind == K::Kern) {
        // A bare length: \kern3pt, \mkern-2mu.
        skipSpace();
        const std::size_t start = i_;
        if (at('-') || at('+')) ++i_;
        while (i_ < s_.size() && (isDigit(s_[i_]) || s_[i_] == '.')) ++i_;
        skipSpace();
        for (int k = 0; k < 2 && i_ < s_.size() && isLetter(s_[i_]); ++k) ++i_;
        raw = s_.substr(start, i_ - start);
      }
      const auto length = raw ? cssLength(*raw, entry->n == 1) : std::nullopt;
      if (!length) return fail("invalid length after " + command, command);
      Node node = space(*length);
      return node;
    }
    case K::TextColor: {
      const auto colour = parseColourArgument(command);
      Node node = parseArgument(state, command);
      if (!colour) return node;
      return with(element("mstyle", nodes(std::move(node))), "mathcolor", *colour);
    }
    case K::ColorBox: {
      const auto colour = parseColourArgument(command);
      auto parts = parseTextArgument(TextFont{}, command);
      Node node = element("mpadded", std::move(parts));
      if (colour) node.attributes.emplace_back("mathbackground", *colour);
      return node;
    }
    case K::Phantom: {
      Node phantom = element("mphantom", nodes(parseArgument(state, command)));
      if (entry->n == 0) return phantom;
      Node padded = element("mpadded", nodes(std::move(phantom)));
      if (entry->n == 1) {
        padded.attributes.emplace_back("height", "0");
        padded.attributes.emplace_back("depth", "0");
      } else {
        padded.attributes.emplace_back("width", "0");
      }
      return padded;
    }
    case K::Smash: {
      const auto which = readRawBracket();
      const bool top = !which || which->find('t') != std::string_view::npos;
      const bool bottom = !which || which->find('b') != std::string_view::npos;
      Node padded = element("mpadded", nodes(parseArgument(state, command)));
      if (top) padded.attributes.emplace_back("height", "0");
      if (bottom) padded.attributes.emplace_back("depth", "0");
      return padded;
    }
    case K::Strut: {
      Node strut = token("mspace", "", Cls::Space);
      strut.attributes.emplace_back("height", entry->n == 0 ? "0.7em" : "0.75em");
      strut.attributes.emplace_back("depth", entry->n == 0 ? "0.3em" : "0.25em");
      return strut;
    }
    case K::Not: return parseNot(state);
    case K::Boxed: return parseArgument(state, command);  // the frame is not drawn (see the header)
    case K::Begin: {
      const auto name_read = readEnvironmentName();
      if (!name_read) return fail("missing environment name after \\begin", "\\begin");
      const Environment* environment = findEnvironment(*name_read);
      if (!environment) {
        // Read the body as a matrix so that its \end still matches.
        error("unknown environment " + clip(*name_read));
        static const Environment kUnknown{"matrix"};
        Node body = parseEnvironment(kUnknown, *name_read, state);
        return element("mrow", nodes(errorNode("\\begin{" + clip(*name_read) + "}"), std::move(body)));
      }
      return parseEnvironment(*environment, *name_read, state);
    }
    case K::Substack: {
      skipSpace();
      if (!at('{')) return fail("missing { after \\substack", command);
      ++i_;
      static const Environment kSubstack{"substack"};
      return buildTable(kSubstack, parseRows(kSubstack, "substack", script, Close::Brace), {});
    }
    case K::Tag: {
      const bool star = at('*');
      if (star) ++i_;
      auto parts = parseTextArgument(TextFont{}, command, star ? "" : "(", star ? "" : ")");
      if (!display_) {
        error("\\tag needs display math");
      } else if (tag_) {
        error("more than one \\tag in a row");
      } else {
        tag_ = rowOf(std::move(parts));
      }
      return std::nullopt;
    }
    case K::Limits: return fail(command + " must follow an operator", command);
    case K::SkipArgument: readRawGroup(); return std::nullopt;
    case K::Ignore: return std::nullopt;
  }
  return std::nullopt;
}

// A text argument: a `{…}` group, or a single character.
std::vector<Node> Parser::parseTextArgument(TextFont font, std::string_view command, std::string_view prefix,
                                            std::string_view suffix) {
  skipSpace();
  TextBuilder text;
  text.add(prefix);
  if (at('{')) {
    ++i_;
    parseTextBody(text, font);
  } else if (atEnd() || at('}') || at('\\') || at('&') || at('$')) {
    text.push(fail("missing argument for " + std::string(command), "missing argument"));
  } else {
    char32_t c = decodeUtf8(s_, i_);
    text.addCodePoint(isControl(c) ? 0xFFFD : c, font);
  }
  text.add(suffix);
  return text.finish();
}

void Parser::parseTextBody(TextBuilder& text, TextFont font) {
  const Guard guard(*this);
  std::size_t braces = 0;  // plain `{…}` inside text only group
  while (i_ < s_.size()) {
    const char c = s_[i_];
    if (c == '}') {
      ++i_;
      if (braces == 0) return;
      --braces;
      continue;
    }
    if (c == '{') {
      ++braces, ++i_;
      continue;
    }
    if (isSpace(c)) {
      ++i_;
      text.space();
      continue;
    }
    if (c == '%') {
      while (i_ < s_.size() && s_[i_] != '\n') ++i_;
      if (i_ < s_.size()) ++i_;
      continue;
    }
    if (c == '$') {
      ++i_;
      auto items = parseList(kStopDollar, State{});
      if (at('$')) ++i_;
      else items.push_back(fail("missing closing $ in text", "missing $"));
      text.push(rowOf(std::move(items)));
      continue;
    }
    if (c == '~') {
      ++i_;
      text.add(" ");
      continue;
    }
    if (c == '-') {
      std::size_t dashes = 0;
      while (dashes < 3 && at('-')) ++dashes, ++i_;
      text.add(dashes == 3 ? "—" : dashes == 2 ? "–" : "-");
      continue;
    }
    if (c == '`' || c == '\'') {
      ++i_;
      const bool twice = at(c);
      if (twice) ++i_;
      text.add(c == '`' ? (twice ? "“" : "‘") : (twice ? "”" : "’"));
      continue;
    }
    if (c == '\\') {
      const auto name = readCommand();
      if (name.empty()) {
        ++i_;
        text.push(fail("lone backslash at the end", "\\"));
        continue;
      }
      // As in TeX, white space right after a command word is not text.
      if (isLetter(name.front())) {
        while (i_ < s_.size() && isSpace(s_[i_])) ++i_;
      }
      static const std::pair<std::string_view, std::string_view> kTextSymbols[] = {
          {"{", "{"}, {"}", "}"}, {"$", "$"}, {"%", "%"}, {"&", "&"}, {"#", "#"}, {"_", "_"}, {" ", " "},
          {",", " "}, {":", " "}, {";", " "}, {"!", ""}, {"quad", " "},
          {"qquad", "  "}, {"textbackslash", "\\"}, {"ldots", "…"}, {"dots", "…"},
          {"textellipsis", "…"}, {"textasciitilde", "~"}, {"textasciicircum", "^"}, {"textunderscore", "_"},
          {"textendash", "–"}, {"textemdash", "—"}, {"textquoteleft", "‘"}, {"textquoteright", "’"},
          {"textquotedblleft", "“"}, {"textquotedblright", "”"}, {"textdegree", "°"}, {"S", "§"}, {"P", "¶"},
          {"dag", "†"}, {"ddag", "‡"}, {"copyright", "©"}, {"textregistered", "®"}, {"TeX", "TeX"},
          {"LaTeX", "LaTeX"}, {"\\", " "}, {"newline", " "},
      };
      const auto symbol = std::find_if(std::begin(kTextSymbols), std::end(kTextSymbols),
                                       [&](const auto& pair) { return pair.first == name; });
      if (symbol != std::end(kTextSymbols)) {
        text.add(symbol->second);
      } else if (const Entry* entry = lookup(name); entry && entry->kind == K::Text) {
        for (auto& node : parseTextArgument(changeTextFont(font, entry->n), "\\" + std::string(name))) {
          text.push(std::move(node));
        }
      } else {
        text.push(fail("unknown command \\" + clip(name) + " in text", "\\" + clip(name)));
      }
      continue;
    }
    char32_t code = decodeUtf8(s_, i_);
    text.addCodePoint(isControl(code) ? 0xFFFD : code, font);
  }
  text.push(fail("missing } after text", "missing }"));
}

Node Parser::parseEnvironment(const Environment& environment, std::string_view name, const State& state) {
  std::vector<std::string_view> aligns;
  if (environment.columns == Columns::Specified) {
    aligns = parseColumnSpec();
  } else if (environment.argument) {
    readRawGroup();  // alignat's column count: the cells say as much
  }
  return buildTable(environment, parseRows(environment, name, state, Close::End), aligns);
}

// Column alignments from an array specification: l c r, with | and : rules
// and @{…} !{…} >{…} <{…} decorations ignored, p{…} m{…} b{…} as l, and
// *{n}{…} repeated.
std::vector<std::string_view> Parser::parseColumnSpec() {
  std::vector<std::string_view> aligns;
  const auto raw = readRawGroup();
  if (!raw) {
    error("missing column specification");
    return aligns;
  }
  const std::string_view spec = *raw;
  constexpr std::size_t kMaximumColumns = 256;
  const auto skipGroup = [&](std::size_t& k) {
    while (k + 1 < spec.size() && isSpace(spec[k + 1])) ++k;
    if (k + 1 < spec.size() && spec[k + 1] == '{') {
      const auto end = spec.find('}', k + 1);
      k = end == std::string_view::npos ? spec.size() : end;
    }
  };
  const auto add = [&](char c) {
    if (aligns.size() >= kMaximumColumns) return;
    aligns.push_back(c == 'l' ? "left" : c == 'r' ? "right" : "");
  };
  bool reported = false;
  for (std::size_t k = 0; k < spec.size(); ++k) {
    const char c = spec[k];
    if (c == 'l' || c == 'c' || c == 'r') {
      add(c);
    } else if (c == 'p' || c == 'm' || c == 'b') {
      add('l');
      skipGroup(k);
    } else if (c == '@' || c == '!' || c == '>' || c == '<') {
      skipGroup(k);
    } else if (c == '*') {
      // *{count}{columns}
      const auto open = spec.find('{', k);
      const auto close = open == std::string_view::npos ? open : spec.find('}', open);
      const auto open2 = close == std::string_view::npos ? close : spec.find('{', close);
      const auto close2 = open2 == std::string_view::npos ? open2 : spec.find('}', open2);
      if (close2 == std::string_view::npos) break;
      std::size_t count = 0;
      for (const char d : trim(spec.substr(open + 1, close - open - 1))) {
        if (isDigit(d) && count < kMaximumColumns) count = count * 10 + static_cast<std::size_t>(d - '0');
      }
      const auto body = spec.substr(open2 + 1, close2 - open2 - 1);
      for (std::size_t r = 0; r < count && aligns.size() < kMaximumColumns; ++r) {
        for (const char d : body) {
          if (d == 'l' || d == 'c' || d == 'r') add(d);
        }
      }
      k = close2;
    } else if (c != '|' && c != ':' && !isSpace(c) && !reported) {
      error("unsupported column type " + clip(spec.substr(k, 1)) + " in array");
      reported = true;
    }
  }
  return aligns;
}

Table Parser::parseRows(const Environment& environment, std::string_view name, const State& outer, Close close) {
  const Guard guard(*this);
  State state = outer;
  state.display = environment.display;
  const unsigned stops = kStopCell | (close == Close::Brace ? kStopBrace : 0u);
  const std::string ending = close == Close::Brace ? "}" : "\\end{" + std::string(name) + "}";
  Table table;
  std::vector<std::vector<Node>> row;
  for (;;) {
    // The right half of an aligned pair starts after an implicit empty
    // ordinary atom, so `&= x` and `&+ y` keep their binary spacing.
    const bool right_half = environment.columns == Columns::Pairs && row.size() % 2 == 1;
    std::vector<Node> cell = parseList(stops, state, right_half ? Cls::Ord : Cls::Open);
    enum class Next { Cell, Row, Done } next = Next::Done;
    for (;;) {
      skipSpace();
      if (atEnd()) {
        if (close != Close::Input) cell.push_back(fail("missing " + ending, "missing " + ending));
        next = Next::Done;
        break;
      }
      if (at('}')) {  // only a stop for \substack
        ++i_;
        next = Next::Done;
        break;
      }
      if (at('&')) {
        ++i_;
        if (!environment.single) {
          next = Next::Cell;
          break;
        }
        cell.push_back(fail("misplaced &", "&"));
      } else {
        const auto command = readCommand();
        if (command == "end") {
          const auto ended = readEnvironmentName().value_or("");
          if (close == Close::End) {
            if (ended != name) cell.push_back(fail("\\begin{" + std::string(name) + "} ended by \\end{" + ended + "}",
                                                   "\\end{" + ended + "}"));
            next = Next::Done;
            break;
          }
          cell.push_back(fail("\\end{" + ended + "} without \\begin", "\\end{" + ended + "}"));
        } else {
          // `\\`, \cr or \newline, perhaps with extra row space: \\[2pt]
          if (command == "\\" && at('*')) ++i_;
          if (command == "\\" && at('[')) readRawBracket();
          next = Next::Row;
          break;
        }
      }
      // A stray `&` or \end: report it and continue the same cell.
      for (auto& node : parseList(stops, state, Cls::Ord)) cell.push_back(std::move(node));
    }
    if (next != Next::Cell && tag_) {
      cell.push_back(space("2em"));
      cell.push_back(std::move(*tag_));
      tag_.reset();
    }
    row.push_back(std::move(cell));
    if (next != Next::Cell) {
      table.columns = std::max(table.columns, row.size());
      table.rows.push_back(std::move(row));
      row.clear();
    }
    if (next == Next::Done) break;
  }
  // A `\\` before \end leaves an empty last row behind.
  if (table.rows.size() > 1 && table.rows.back().size() == 1 && table.rows.back().front().empty()) {
    table.rows.pop_back();
  }
  return table;
}

Node Parser::buildTable(const Environment& environment, Table table, const std::vector<std::string_view>& aligns) {
  std::vector<Node> rows;
  rows.reserve(table.rows.size());
  for (auto& row : table.rows) {
    std::vector<Node> cells;
    cells.reserve(row.size());
    for (std::size_t column = 0; column < row.size(); ++column) {
      Node cell = element("mtd", std::move(row[column]));
      std::string_view align;
      switch (environment.columns) {
        case Columns::Centered: break;
        case Columns::Left: align = "left"; break;
        case Columns::Pairs: align = column % 2 == 0 ? "right" : "left"; break;
        case Columns::Specified: align = column < aligns.size() ? aligns[column] : ""; break;
      }
      if (!align.empty()) cell.attributes.emplace_back("columnalign", std::string(align));
      cells.push_back(std::move(cell));
    }
    rows.push_back(element("mtr", std::move(cells)));
  }
  Node node = element("mtable", std::move(rows), Cls::Inner);
  if (environment.display) node.attributes.emplace_back("displaystyle", "true");
  if (environment.columns == Columns::Pairs && table.columns > 1) {
    // No gap inside a pair, a wide one between pairs.
    std::string spacing;
    for (std::size_t gap = 0; gap + 1 < table.columns; ++gap) spacing += gap % 2 == 0 ? "0em " : "2em ";
    spacing.pop_back();
    node.attributes.emplace_back("columnspacing", std::move(spacing));
  } else if (environment.columns == Columns::Left && table.columns > 1) {
    node.attributes.emplace_back("columnspacing", "1em");
  }
  if (environment.small) node = with(element("mstyle", nodes(std::move(node)), Cls::Inner), "scriptlevel", "1");
  if (environment.open.empty() && environment.close.empty()) return node;
  std::vector<Node> parts;
  if (!environment.open.empty()) parts.push_back(fence(std::string(environment.open), "prefix"));
  parts.push_back(std::move(node));
  if (!environment.close.empty()) parts.push_back(fence(std::string(environment.close), "postfix"));
  return element("mrow", std::move(parts), Cls::Inner);
}

}  // namespace

MathRendering renderTexMath(std::string_view tex, bool display) {
  MathRendering result;
  const std::string_view open = display ? "<math display=\"block\">" : "<math>";
  try {
    if (tex.size() > kMaximumMathSourceBytes) {
      result.errors.push_back("formula longer than " + std::to_string(kMaximumMathSourceBytes) + " bytes");
      result.html = std::string(open) + "<merror><mtext>formula too long</mtext></merror></math>";
      return result;
    }
    Node body;
    try {
      Parser parser(tex, display, result.errors);
      body = parser.parseFormula();
    } catch (const DepthExceeded&) {
      result.errors.clear();
      result.errors.push_back("formula nested deeper than " + std::to_string(kMaximumMathDepth) + " levels");
      body = element("mrow", nodes(element("merror", nodes(token("mtext", "formula nested too deeply")))));
    }
    std::string html;
    html.reserve(tex.size() * 8 + 128);
    html += open;
    html += "<semantics>";
    write(html, body);
    html += "<annotation encoding=\"application/x-tex\">";
    appendAnnotation(html, tex);
    html += "</annotation></semantics></math>";
    result.html = std::move(html);
  } catch (...) {
    // Only allocation can fail here; answer with an error rather than throw.
    try {
      result.errors.assign(1, "formula could not be converted");
      result.html = std::string(open) + "<merror><mtext>formula could not be converted</mtext></merror></math>";
    } catch (...) {
      result.html.clear();
    }
  }
  return result;
}

}  // namespace ckgit
