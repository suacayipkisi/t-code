/*
 * TeX math in the preview (see state.h): $...$, $$...$$ and ```math blocks. A formula is parsed into a
 * tree of atoms and laid out the way TeX sets it (sizes by style, the space between atoms by their
 * class, scripts, fractions, radicals, big operators and their limits, delimiters stretched to what
 * they enclose, matrices and aligned equations), then drawn with Pango and cairo: no TeX installation
 * and no browser engine.
 */
#include "editor/preview/state.h"

#include "window/palette.h"

#include <pango/pangocairo.h>
#include <string.h>

#define MAX_DEPTH 48 /* groups inside groups: deeper ones are left out (no stack overflow on {{{{...) */
#define SERIF "Latin Modern Roman, STIX Two Text, STIX Two Math, Noto Serif, Noto Sans Math, serif"

/* The tree ----------------------------------------------------------------------------------- */

/* An atom's class decides the space between it and its neighbors, as in TeX. */
typedef enum { ORD, OP, BIN, REL, OPEN, CLOSE, PUNCT, INNER } AtomClass;

/* Display and text style have the same size; scripts are smaller, scripts of scripts smaller still. */
typedef enum { STYLE_DISPLAY, STYLE_TEXT, STYLE_SCRIPT, STYLE_SCRIPTSCRIPT } Style;

typedef enum {
  FONT_ITALIC,
  FONT_ROMAN,
  FONT_BOLD,
  FONT_BOLD_ITALIC,
  FONT_SANS,
  FONT_MONO,
  FONT_BB, /* the letters become Unicode's double-struck, script and fraktur ones */
  FONT_CAL,
  FONT_FRAK,
  FONT_ERROR, /* an unknown command, shown as typed */
} Font;

typedef enum {
  NODE_LIST,    /* kids in a row */
  NODE_SYMBOL,  /* text in a font */
  NODE_FRAC,    /* kids[0] over kids[1] */
  NODE_SQRT,    /* kids[0] under a radical sign, kids[1] its index (an empty list: none) */
  NODE_SCRIPTS, /* kids[0] with kids[1] above (superscript) and kids[2] below (subscript); either may be NULL */
  NODE_FENCED,  /* kids[0] between delimiters stretched to its height */
  NODE_DELIM,   /* a delimiter of a fixed height (\big, \Big, ...) */
  NODE_ACCENT,  /* kids[0] with an accent, a line or a brace over or under it */
  NODE_MATRIX,  /* rows (lists) of cells (lists) */
  NODE_SPACE,
  NODE_STYLE, /* the style of what follows it in its list (\displaystyle, ...) */
} NodeKind;

typedef enum {
  ACCENT_GLYPH,
  ACCENT_WIDE,
  ACCENT_ARROW,
  ACCENT_OVERLINE,
  ACCENT_UNDERLINE,
  ACCENT_OVERBRACE,
  ACCENT_UNDERBRACE
} Accent;

/* Where a big operator's scripts go: beside it, above and below it in display style, or always. */
typedef enum { LIMITS_NONE, LIMITS_DISPLAY, LIMITS_ALWAYS } Limits;

typedef struct Node Node;
struct Node {
  NodeKind kind;
  AtomClass cls;
  Font font;
  char *text;      /* SYMBOL: its text; FENCED, DELIM: the (opening) delimiter; ACCENT: its glyph; MATRIX:
                      the columns' alignment (l, c, r), repeated */
  char *close;     /* FENCED: the closing delimiter */
  double size;     /* SPACE: width in mu (1/18 em); DELIM: height in em; ACCENT: the glyph's size (1: the
                      text's); MATRIX: the space between columns in em */
  int style;       /* STYLE: the style; FRAC, MATRIX: the style inside it (-1: from around it) */
  Accent accent;   /* ACCENT */
  Limits limits;   /* SYMBOL, SCRIPTS */
  gboolean big;    /* SYMBOL: a big operator (∑, ∫): larger in display style, centered on the axis */
  gboolean bar;    /* FRAC: a bar between its parts (binomials have none) */
  gboolean pairs;  /* MATRIX: columns in right / left aligned pairs (aligned equations) */
  GPtrArray *kids; /* Node */
};

#define KID(n, i) ((Node *)g_ptr_array_index((n)->kids, (i)))

static void node_free(gpointer data)
{
  Node *n = data;

  if (!n)
    return;
  g_free(n->text);
  g_free(n->close);
  g_ptr_array_free(n->kids, TRUE);
  g_free(n);
}

static Node *node_new(NodeKind kind, AtomClass cls)
{
  Node *n = g_new0(Node, 1);

  n->kind = kind;
  n->cls = cls;
  n->style = -1;
  n->size = 1;
  n->kids = g_ptr_array_new_with_free_func(node_free);
  return n;
}

static Node *symbol_new(const char *text, AtomClass cls, Font font)
{
  Node *n = node_new(NODE_SYMBOL, cls);

  n->text = g_strdup(text);
  n->font = font;
  return n;
}

static Node *space_new(double mu)
{
  Node *n = node_new(NODE_SPACE, ORD);

  n->size = mu;
  return n;
}

static Node *node_with(NodeKind kind, AtomClass cls, Node *a, Node *b)
{
  Node *n = node_new(kind, cls);

  g_ptr_array_add(n->kids, a);
  if (b)
    g_ptr_array_add(n->kids, b);
  return n;
}

/* Symbols ------------------------------------------------------------------------------------ */

typedef struct {
  const char *name, *text;
  AtomClass cls;
} Symbol;

/* clang-format off */
static const Symbol symbols[] = {
  { "alpha", "α", ORD }, { "beta", "β", ORD }, { "gamma", "γ", ORD }, { "delta", "δ", ORD },
  { "epsilon", "ϵ", ORD }, { "varepsilon", "ε", ORD }, { "zeta", "ζ", ORD }, { "eta", "η", ORD },
  { "theta", "θ", ORD }, { "vartheta", "ϑ", ORD }, { "iota", "ι", ORD }, { "kappa", "κ", ORD },
  { "lambda", "λ", ORD }, { "mu", "μ", ORD }, { "nu", "ν", ORD }, { "xi", "ξ", ORD }, { "omicron", "ο", ORD },
  { "pi", "π", ORD }, { "varpi", "ϖ", ORD }, { "rho", "ρ", ORD }, { "varrho", "ϱ", ORD }, { "sigma", "σ", ORD },
  { "varsigma", "ς", ORD }, { "tau", "τ", ORD }, { "upsilon", "υ", ORD }, { "phi", "ϕ", ORD },
  { "varphi", "φ", ORD }, { "chi", "χ", ORD }, { "psi", "ψ", ORD }, { "omega", "ω", ORD },
  { "Gamma", "Γ", ORD }, { "Delta", "Δ", ORD }, { "Theta", "Θ", ORD }, { "Lambda", "Λ", ORD }, { "Xi", "Ξ", ORD },
  { "Pi", "Π", ORD }, { "Sigma", "Σ", ORD }, { "Upsilon", "Υ", ORD }, { "Phi", "Φ", ORD }, { "Psi", "Ψ", ORD },
  { "Omega", "Ω", ORD },
  { "infty", "∞", ORD }, { "partial", "∂", ORD }, { "nabla", "∇", ORD }, { "hbar", "ℏ", ORD }, { "ell", "ℓ", ORD },
  { "Re", "ℜ", ORD }, { "Im", "ℑ", ORD }, { "aleph", "ℵ", ORD }, { "wp", "℘", ORD }, { "emptyset", "∅", ORD },
  { "varnothing", "∅", ORD }, { "forall", "∀", ORD }, { "exists", "∃", ORD }, { "nexists", "∄", ORD },
  { "neg", "¬", ORD }, { "lnot", "¬", ORD }, { "top", "⊤", ORD }, { "bot", "⊥", ORD }, { "angle", "∠", ORD },
  { "triangle", "△", ORD }, { "prime", "′", ORD }, { "degree", "°", ORD }, { "ldots", "…", INNER },
  { "dots", "…", INNER }, { "dotsc", "…", INNER }, { "dotsb", "⋯", INNER }, { "cdots", "⋯", INNER },
  { "vdots", "⋮", ORD }, { "ddots", "⋱", INNER }, { "imath", "ı", ORD }, { "jmath", "ȷ", ORD },
  { "clubsuit", "♣", ORD }, { "diamondsuit", "♢", ORD }, { "heartsuit", "♡", ORD }, { "spadesuit", "♠", ORD },
  { "checkmark", "✓", ORD }, { "S", "§", ORD }, { "P", "¶", ORD }, { "backslash", "\\", ORD },
  { "%", "%", ORD }, { "#", "#", ORD }, { "$", "$", ORD }, { "&", "&", ORD }, { "_", "_", ORD },
  { "{", "{", OPEN }, { "}", "}", CLOSE }, { "lbrace", "{", OPEN }, { "rbrace", "}", CLOSE },
  { "langle", "⟨", OPEN }, { "rangle", "⟩", CLOSE }, { "lfloor", "⌊", OPEN }, { "rfloor", "⌋", CLOSE },
  { "lceil", "⌈", OPEN }, { "rceil", "⌉", CLOSE }, { "lbrack", "[", OPEN }, { "rbrack", "]", CLOSE },
  { "vert", "|", ORD }, { "Vert", "‖", ORD }, { "|", "‖", ORD }, { "lvert", "|", OPEN }, { "rvert", "|", CLOSE },
  { "lVert", "‖", OPEN }, { "rVert", "‖", CLOSE },
  { "pm", "±", BIN }, { "mp", "∓", BIN }, { "times", "×", BIN }, { "div", "÷", BIN }, { "cdot", "⋅", BIN },
  { "ast", "∗", BIN }, { "star", "⋆", BIN }, { "circ", "∘", BIN }, { "bullet", "∙", BIN }, { "oplus", "⊕", BIN },
  { "ominus", "⊖", BIN }, { "otimes", "⊗", BIN }, { "oslash", "⊘", BIN }, { "odot", "⊙", BIN },
  { "cup", "∪", BIN }, { "cap", "∩", BIN }, { "sqcup", "⊔", BIN }, { "sqcap", "⊓", BIN }, { "vee", "∨", BIN },
  { "lor", "∨", BIN }, { "wedge", "∧", BIN }, { "land", "∧", BIN }, { "setminus", "∖", BIN }, { "wr", "≀", BIN },
  { "amalg", "⨿", BIN }, { "uplus", "⊎", BIN }, { "diamond", "⋄", BIN }, { "dagger", "†", BIN },
  { "ddagger", "‡", BIN }, { "bigtriangleup", "△", BIN }, { "bigtriangledown", "▽", BIN },
  { "triangleleft", "◁", BIN }, { "triangleright", "▷", BIN },
  { "leq", "≤", REL }, { "le", "≤", REL }, { "geq", "≥", REL }, { "ge", "≥", REL }, { "neq", "≠", REL },
  { "ne", "≠", REL }, { "equiv", "≡", REL }, { "approx", "≈", REL }, { "cong", "≅", REL }, { "sim", "∼", REL },
  { "simeq", "≃", REL }, { "propto", "∝", REL }, { "ll", "≪", REL }, { "gg", "≫", REL }, { "prec", "≺", REL },
  { "succ", "≻", REL }, { "preceq", "⪯", REL }, { "succeq", "⪰", REL }, { "subset", "⊂", REL },
  { "supset", "⊃", REL }, { "subseteq", "⊆", REL }, { "supseteq", "⊇", REL }, { "subsetneq", "⊊", REL },
  { "supsetneq", "⊋", REL }, { "sqsubseteq", "⊑", REL }, { "sqsupseteq", "⊒", REL }, { "in", "∈", REL },
  { "ni", "∋", REL }, { "notin", "∉", REL }, { "perp", "⊥", REL }, { "parallel", "∥", REL }, { "mid", "∣", REL },
  { "nmid", "∤", REL }, { "vdash", "⊢", REL }, { "dashv", "⊣", REL }, { "models", "⊨", REL },
  { "asymp", "≍", REL }, { "doteq", "≐", REL }, { "leqslant", "⩽", REL }, { "geqslant", "⩾", REL },
  { "lesssim", "≲", REL }, { "gtrsim", "≳", REL }, { "coloneqq", "≔", REL }, { "triangleq", "≜", REL },
  { "to", "→", REL }, { "rightarrow", "→", REL }, { "leftarrow", "←", REL }, { "gets", "←", REL },
  { "leftrightarrow", "↔", REL }, { "Rightarrow", "⇒", REL }, { "Leftarrow", "⇐", REL },
  { "Leftrightarrow", "⇔", REL }, { "implies", "⟹", REL }, { "impliedby", "⟸", REL }, { "iff", "⟺", REL },
  { "mapsto", "↦", REL }, { "longrightarrow", "⟶", REL }, { "longleftarrow", "⟵", REL },
  { "longleftrightarrow", "⟷", REL }, { "Longrightarrow", "⟹", REL }, { "Longleftarrow", "⟸", REL },
  { "Longleftrightarrow", "⟺", REL }, { "longmapsto", "⟼", REL }, { "uparrow", "↑", REL },
  { "downarrow", "↓", REL }, { "updownarrow", "↕", REL }, { "Uparrow", "⇑", REL }, { "Downarrow", "⇓", REL },
  { "nearrow", "↗", REL }, { "searrow", "↘", REL }, { "swarrow", "↙", REL }, { "nwarrow", "↖", REL },
  { "hookrightarrow", "↪", REL }, { "hookleftarrow", "↩", REL }, { "rightharpoonup", "⇀", REL },
  { "leftharpoondown", "↽", REL }, { "rightleftharpoons", "⇌", REL }, { "leadsto", "⇝", REL },
  { "colon", ":", PUNCT }, { "ldotp", ".", PUNCT }, { "cdotp", "⋅", PUNCT },
};

/* Big operators: their scripts go above and below in display style, an integral's beside it. */
static const struct { const char *name, *text; gboolean limits; } big_operators[] = {
  { "sum", "∑", TRUE }, { "prod", "∏", TRUE }, { "coprod", "∐", TRUE }, { "bigcup", "⋃", TRUE },
  { "bigcap", "⋂", TRUE }, { "bigsqcup", "⨆", TRUE }, { "bigvee", "⋁", TRUE }, { "bigwedge", "⋀", TRUE },
  { "bigoplus", "⨁", TRUE }, { "bigotimes", "⨂", TRUE }, { "bigodot", "⨀", TRUE }, { "biguplus", "⨄", TRUE },
  { "int", "∫", FALSE }, { "iint", "∬", FALSE }, { "iiint", "∭", FALSE }, { "oint", "∮", FALSE },
  { "oiint", "∯", FALSE },
};

/* Function names, upright; lim and the like take their scripts below in display style. */
static const struct { const char *name, *text; gboolean limits; } functions[] = {
  { "arccos", "arccos", FALSE }, { "arcsin", "arcsin", FALSE }, { "arctan", "arctan", FALSE },
  { "arg", "arg", FALSE }, { "cos", "cos", FALSE }, { "cosh", "cosh", FALSE }, { "cot", "cot", FALSE },
  { "coth", "coth", FALSE }, { "csc", "csc", FALSE }, { "deg", "deg", FALSE }, { "dim", "dim", FALSE },
  { "exp", "exp", FALSE }, { "hom", "hom", FALSE }, { "ker", "ker", FALSE }, { "lg", "lg", FALSE },
  { "ln", "ln", FALSE }, { "log", "log", FALSE }, { "sec", "sec", FALSE }, { "sin", "sin", FALSE },
  { "sinh", "sinh", FALSE }, { "tan", "tan", FALSE }, { "tanh", "tanh", FALSE }, { "det", "det", TRUE },
  { "gcd", "gcd", TRUE }, { "inf", "inf", TRUE }, { "lim", "lim", TRUE }, { "liminf", "lim inf", TRUE },
  { "limsup", "lim sup", TRUE }, { "max", "max", TRUE }, { "min", "min", TRUE }, { "Pr", "Pr", TRUE },
  { "sup", "sup", TRUE }, { "argmax", "arg max", TRUE }, { "argmin", "arg min", TRUE },
};

static const struct { const char *name; double mu; } spaces[] = {
  { ",", 3 }, { ":", 4 }, { ">", 4 }, { ";", 5 }, { "!", -3 }, { " ", 6 }, { "quad", 18 }, { "qquad", 36 },
  { "enspace", 9 }, { "thinspace", 3 }, { "medspace", 4 }, { "thickspace", 5 }, { "negthinspace", -3 },
  { "negmedspace", -4 }, { "negthickspace", -5 }, { "nobreakspace", 6 },
};

static const struct { const char *name, *glyph; Accent accent; double size; } accents[] = {
  { "hat", "ˆ", ACCENT_GLYPH, 1 }, { "widehat", "ˆ", ACCENT_WIDE, 1 }, { "check", "ˇ", ACCENT_GLYPH, 1 },
  { "widecheck", "ˇ", ACCENT_WIDE, 1 }, { "tilde", "˜", ACCENT_GLYPH, 1 }, { "widetilde", "˜", ACCENT_WIDE, 1 },
  { "acute", "´", ACCENT_GLYPH, 1 }, { "grave", "`", ACCENT_GLYPH, 1 }, { "dot", "˙", ACCENT_GLYPH, 1 },
  { "ddot", "¨", ACCENT_GLYPH, 1 }, { "breve", "˘", ACCENT_GLYPH, 1 }, { "bar", "¯", ACCENT_GLYPH, 1 },
  { "mathring", "˚", ACCENT_GLYPH, 1 }, { "vec", "→", ACCENT_GLYPH, 0.7 },
  { "overline", NULL, ACCENT_OVERLINE, 1 }, { "underline", NULL, ACCENT_UNDERLINE, 1 },
  { "overbrace", NULL, ACCENT_OVERBRACE, 1 }, { "underbrace", NULL, ACCENT_UNDERBRACE, 1 },
  { "overrightarrow", "→", ACCENT_ARROW, 1 }, { "overleftarrow", "←", ACCENT_ARROW, 1 },
  { "overleftrightarrow", "↔", ACCENT_ARROW, 1 }, { "vv", "→", ACCENT_ARROW, 1 },
};

static const struct { const char *name; Font font; } fonts[] = {
  { "mathrm", FONT_ROMAN }, { "mathup", FONT_ROMAN }, { "mathbf", FONT_BOLD }, { "mathit", FONT_ITALIC },
  { "mathnormal", FONT_ITALIC }, { "mathsf", FONT_SANS }, { "mathtt", FONT_MONO }, { "mathbb", FONT_BB },
  { "mathcal", FONT_CAL }, { "mathscr", FONT_CAL }, { "mathfrak", FONT_FRAK }, { "boldsymbol", FONT_BOLD_ITALIC },
  { "bm", FONT_BOLD_ITALIC }, { "pmb", FONT_BOLD_ITALIC },
};

/* \rm, \bf, ...: the font of the rest of the group */
static const struct { const char *name; Font font; } font_switches[] = {
  { "rm", FONT_ROMAN }, { "bf", FONT_BOLD }, { "it", FONT_ITALIC }, { "sf", FONT_SANS }, { "tt", FONT_MONO },
  { "cal", FONT_CAL },
};

static const struct { const char *name; Font font; } texts[] = {
  { "text", FONT_ROMAN }, { "textrm", FONT_ROMAN }, { "textnormal", FONT_ROMAN }, { "mbox", FONT_ROMAN },
  { "textup", FONT_ROMAN }, { "textbf", FONT_BOLD }, { "textit", FONT_ITALIC }, { "textsf", FONT_SANS },
  { "texttt", FONT_MONO }, { "emph", FONT_ITALIC }, { "hbox", FONT_ROMAN },
};

static const struct { const char *name; AtomClass cls; } classes[] = {
  { "mathord", ORD }, { "mathop", OP }, { "mathbin", BIN }, { "mathrel", REL }, { "mathopen", OPEN },
  { "mathclose", CLOSE }, { "mathpunct", PUNCT }, { "mathinner", INNER },
};

/* Commands that show nothing: their arguments are skipped. */
static const struct { const char *name; int arguments; } ignored[] = {
  { "label", 1 }, { "tag", 1 }, { "nonumber", 0 }, { "notag", 0 }, { "hline", 0 }, { "cline", 1 },
  { "relax", 0 }, { "allowbreak", 0 }, { "nolinebreak", 0 }, { "displaybreak", 0 }, { "color", 1 },
  { "phantom", 1 }, { "hphantom", 1 }, { "vphantom", 1 }, { "strut", 0 }, { "newline", 0 },
};

typedef struct {
  const char *name, *open, *close, *align;
  double gap;  /* between columns, in em */
  int style;   /* the cells' style, -1: the surrounding one */
  gboolean pairs;
} Environment;

static const Environment environments[] = {
  { "matrix", ".", ".", "c", 1, STYLE_TEXT, FALSE }, { "pmatrix", "(", ")", "c", 1, STYLE_TEXT, FALSE },
  { "bmatrix", "[", "]", "c", 1, STYLE_TEXT, FALSE }, { "Bmatrix", "{", "}", "c", 1, STYLE_TEXT, FALSE },
  { "vmatrix", "|", "|", "c", 1, STYLE_TEXT, FALSE }, { "Vmatrix", "‖", "‖", "c", 1, STYLE_TEXT, FALSE },
  { "smallmatrix", ".", ".", "c", 0.4, STYLE_SCRIPT, FALSE }, { "cases", "{", ".", "ll", 1, STYLE_TEXT, FALSE },
  { "dcases", "{", ".", "ll", 1, STYLE_DISPLAY, FALSE }, { "rcases", ".", "}", "ll", 1, STYLE_TEXT, FALSE },
  { "aligned", ".", ".", "rl", 2, -1, TRUE }, { "align", ".", ".", "rl", 2, -1, TRUE },
  { "align*", ".", ".", "rl", 2, -1, TRUE }, { "alignat", ".", ".", "rl", 0, -1, TRUE },
  { "alignat*", ".", ".", "rl", 0, -1, TRUE }, { "alignedat", ".", ".", "rl", 0, -1, TRUE },
  { "split", ".", ".", "rl", 2, -1, TRUE }, { "flalign", ".", ".", "rl", 2, -1, TRUE },
  { "flalign*", ".", ".", "rl", 2, -1, TRUE }, { "eqnarray", ".", ".", "rcl", 0.3, -1, FALSE },
  { "eqnarray*", ".", ".", "rcl", 0.3, -1, FALSE }, { "gathered", ".", ".", "c", 0, -1, FALSE },
  { "gather", ".", ".", "c", 0, -1, FALSE }, { "gather*", ".", ".", "c", 0, -1, FALSE },
  { "equation", ".", ".", "c", 0, -1, FALSE }, { "equation*", ".", ".", "c", 0, -1, FALSE },
  { "multline", ".", ".", "c", 0, -1, FALSE }, { "multline*", ".", ".", "c", 0, -1, FALSE },
  { "array", ".", ".", "c", 1, STYLE_TEXT, FALSE }, { "darray", ".", ".", "c", 1, STYLE_DISPLAY, FALSE },
  { "subarray", ".", ".", "c", 0.4, STYLE_SCRIPT, FALSE },
};
/* clang-format on */

static const Symbol *find_symbol(const char *name)
{
  for (gsize i = 0; i < G_N_ELEMENTS(symbols); i++)
    if (strcmp(symbols[i].name, name) == 0)
      return &symbols[i];
  return NULL;
}

/* A character typed as itself (≤, α, ...): the class of the command that stands for it. */
static AtomClass class_of_text(const char *text, AtomClass otherwise)
{
  for (gsize i = 0; i < G_N_ELEMENTS(symbols); i++)
    if (strcmp(symbols[i].text, text) == 0)
      return symbols[i].cls;
  return otherwise;
}

/* A letter or digit in \mathbb, \mathcal or \mathfrak: Unicode's styled one (some have older code points). */
static gunichar styled_char(Font font, gunichar c)
{
  /* clang-format off */
  static const struct { Font font; char c; gunichar code; } older[] = {
    { FONT_BB, 'C', 0x2102 }, { FONT_BB, 'H', 0x210D }, { FONT_BB, 'N', 0x2115 }, { FONT_BB, 'P', 0x2119 },
    { FONT_BB, 'Q', 0x211A }, { FONT_BB, 'R', 0x211D }, { FONT_BB, 'Z', 0x2124 }, { FONT_CAL, 'B', 0x212C },
    { FONT_CAL, 'E', 0x2130 }, { FONT_CAL, 'F', 0x2131 }, { FONT_CAL, 'H', 0x210B }, { FONT_CAL, 'I', 0x2110 },
    { FONT_CAL, 'L', 0x2112 }, { FONT_CAL, 'M', 0x2133 }, { FONT_CAL, 'R', 0x211B }, { FONT_CAL, 'e', 0x212F },
    { FONT_CAL, 'g', 0x210A }, { FONT_CAL, 'o', 0x2134 }, { FONT_FRAK, 'C', 0x212D }, { FONT_FRAK, 'H', 0x210C },
    { FONT_FRAK, 'I', 0x2111 }, { FONT_FRAK, 'R', 0x211C }, { FONT_FRAK, 'Z', 0x2128 },
  };
  /* clang-format on */
  gunichar upper = font == FONT_BB ? 0x1D538 : font == FONT_CAL ? 0x1D49C : 0x1D504;

  for (gsize i = 0; i < G_N_ELEMENTS(older); i++)
    if (older[i].font == font && (gunichar)older[i].c == c)
      return older[i].code;
  if (c >= 'A' && c <= 'Z')
    return upper + (c - 'A');
  if (c >= 'a' && c <= 'z')
    return upper + 26 + (c - 'a');
  if (font == FONT_BB && c >= '0' && c <= '9')
    return 0x1D7D8 + (c - '0');
  return c;
}

/* Parsing ------------------------------------------------------------------------------------ */

typedef struct {
  const char *s;
  Font font; /* letters' font in the current group */
  int depth;
} Parser;

/* What a list stops at: what it was parsed until, or the end of a cell or row. */
typedef enum { UNTIL_END, UNTIL_BRACE, UNTIL_BRACKET, UNTIL_RIGHT, UNTIL_ENV } Until;
typedef enum { STOP_END, STOP_BRACE, STOP_BRACKET, STOP_RIGHT, STOP_ENV, STOP_CELL, STOP_ROW } Stop;

static Node *parse_list(Parser *ps, Until until, gboolean cells, Stop *stop);
static Node *parse_command(Parser *ps, const char *name);

static void skip_spaces(Parser *ps)
{
  while (g_ascii_isspace(*ps->s))
    ps->s++;
}

/* A command's name after its backslash: letters, or one character. */
static void read_name(Parser *ps, char *name, gsize size)
{
  gsize len = 0;

  ps->s++; /* the backslash */
  if (g_ascii_isalpha(*ps->s)) {
    while (g_ascii_isalpha(*ps->s)) {
      if (len + 1 < size)
        name[len++] = *ps->s;
      ps->s++;
    }
  } else if (*ps->s) {
    const char *next = g_utf8_next_char(ps->s);
    while (ps->s < next && *ps->s) {
      if (len + 1 < size)
        name[len++] = *ps->s;
      ps->s++;
    }
  }
  name[len] = '\0';
}

/* An argument as it is written: a group's text, a command, or a character. */
static char *read_raw(Parser *ps)
{
  const char *start;
  int depth = 1;

  skip_spaces(ps);
  if (*ps->s != '{') {
    start = ps->s;
    if (*ps->s == '\\') {
      char name[32];
      read_name(ps, name, sizeof name);
    } else if (*ps->s) {
      ps->s = g_utf8_next_char(ps->s);
    }
    return g_strndup(start, ps->s - start);
  }
  start = ++ps->s;
  for (; *ps->s; ps->s++) {
    if (*ps->s == '\\' && ps->s[1])
      ps->s++;
    else if (*ps->s == '{')
      depth++;
    else if (*ps->s == '}' && --depth == 0)
      break;
  }
  char *raw = g_strndup(start, ps->s - start);
  if (*ps->s)
    ps->s++;
  return raw;
}

/* \text's argument as plain text: escapes undone, commands and braces left out. */
static char *plain_text(const char *raw)
{
  GString *text = g_string_new(NULL);

  for (const char *c = raw; *c; c++) {
    if (*c == '\\' && g_ascii_isalpha(c[1])) {
      while (g_ascii_isalpha(c[1]))
        c++;
    } else if (*c == '\\' && c[1]) {
      c++;
      g_string_append_c(text, *c == '\\' ? ' ' : *c);
    } else if (*c == '~') {
      g_string_append(text, " ");
    } else if (*c != '{' && *c != '}' && *c != '$') {
      g_string_append_c(text, g_ascii_isspace(*c) ? ' ' : *c);
    }
  }
  return g_string_free(text, FALSE);
}

/* A delimiter after \left, \right, \big, ...; "." is none. */
static char *read_delim(Parser *ps)
{
  static const char *const names[][2] = {
    { "{", "{" },          { "}", "}" },      { "|", "‖" },      { "lbrace", "{" },     { "rbrace", "}" },
    { "backslash", "\\" }, { "lgroup", "(" }, { "rgroup", ")" }, { "lmoustache", "(" },
  };
  char name[32];

  skip_spaces(ps);
  if (*ps->s == '\\') {
    read_name(ps, name, sizeof name);
    for (gsize i = 0; i < G_N_ELEMENTS(names); i++)
      if (strcmp(name, names[i][0]) == 0)
        return g_strdup(names[i][1]);
    const Symbol *symbol = find_symbol(name);
    return g_strdup(symbol ? symbol->text : ".");
  }
  if (!*ps->s)
    return g_strdup(".");
  const char *start = ps->s;
  ps->s = g_utf8_next_char(ps->s);
  if (*start == '<')
    return g_strdup("⟨");
  if (*start == '>')
    return g_strdup("⟩");
  return g_strndup(start, ps->s - start);
}

/* Letters, digits and other characters typed as themselves (number: digits group into one number). */
static Node *parse_char(Parser *ps, gboolean number)
{
  /* clang-format off */
  static const struct { char c; const char *text; AtomClass cls; } ascii[] = {
    { '+', "+", BIN }, { '-', "−", BIN }, { '*', "∗", BIN }, { '=', "=", REL }, { '<', "<", REL },
    { '>', ">", REL }, { ':', ":", REL }, { ',', ",", PUNCT }, { ';', ";", PUNCT }, { '(', "(", OPEN },
    { '[', "[", OPEN }, { ')', ")", CLOSE }, { ']', "]", CLOSE }, { '!', "!", CLOSE }, { '?', "?", CLOSE },
  };
  /* clang-format on */
  const char *start = ps->s;
  gunichar c = g_utf8_get_char_validated(ps->s, -1);
  Font font = ps->font;

  if (c == (gunichar)-1 || c == (gunichar)-2) { /* not UTF-8 */
    ps->s++;
    return node_new(NODE_LIST, ORD);
  }
  ps->s = g_utf8_next_char(ps->s);
  if (c == '~')
    return space_new(6);
  if (g_ascii_isdigit(c) || (g_ascii_isalpha(c) && c < 128)) {
    GString *text = g_string_new(NULL);
    if (number && g_ascii_isdigit(c))
      while (g_ascii_isdigit(*ps->s) || (*ps->s == '.' && g_ascii_isdigit(ps->s[1])))
        ps->s++;
    for (const char *each = start; each < ps->s; each++)
      g_string_append_unichar(text, font >= FONT_BB && font <= FONT_FRAK ? styled_char(font, *each) : (gunichar)*each);
    if (font >= FONT_BB && font <= FONT_FRAK)
      font = FONT_ROMAN;
    else if (g_ascii_isdigit(c) && font == FONT_ITALIC)
      font = FONT_ROMAN; /* digits are upright */
    else if (g_ascii_isdigit(c) && font == FONT_BOLD_ITALIC)
      font = FONT_BOLD;
    Node *n = symbol_new(text->str, ORD, font);
    g_string_free(text, TRUE);
    return n;
  }
  for (gsize i = 0; i < G_N_ELEMENTS(ascii); i++)
    if ((gunichar)ascii[i].c == c)
      return symbol_new(ascii[i].text, ascii[i].cls, FONT_ROMAN);
  char *text = g_strndup(start, ps->s - start);
  gboolean letter = c >= 128 && g_unichar_isalpha(c);
  Node *n = symbol_new(text, class_of_text(text, ORD), letter && font == FONT_ITALIC ? FONT_ITALIC : FONT_ROMAN);
  g_free(text);
  return n;
}

/* One argument: a group, a command or a character. */
static Node *parse_argument(Parser *ps)
{
  Stop stop;

  skip_spaces(ps);
  if (*ps->s == '{') {
    ps->s++;
    return parse_list(ps, UNTIL_BRACE, FALSE, &stop);
  }
  if (*ps->s == '\\' && ps->s[1] != '\\') {
    char name[32];
    read_name(ps, name, sizeof name);
    Node *n = parse_command(ps, name);
    return n ? n : node_new(NODE_LIST, ORD);
  }
  if (!*ps->s || strchr("}&^_\\", *ps->s))
    return node_new(NODE_LIST, ORD);
  return parse_char(ps, FALSE);
}

/* x^a, x_b: the script goes on the atom before it (an empty one when there is none). */
static void attach(Node *list, Node *script, gboolean sup)
{
  Node *last = list->kids->len ? KID(list, list->kids->len - 1) : NULL;
  int slot = sup ? 1 : 2;

  if (last && last->kind == NODE_SCRIPTS && !g_ptr_array_index(last->kids, slot)) {
    last->kids->pdata[slot] = script;
    return;
  }
  Node *base = last && last->kind != NODE_SPACE && last->kind != NODE_STYLE
                   ? g_ptr_array_steal_index(list->kids, list->kids->len - 1)
                   : node_new(NODE_LIST, ORD);
  Node *n = node_new(NODE_SCRIPTS, base->cls);
  n->limits = base->limits;
  g_ptr_array_add(n->kids, base);
  g_ptr_array_add(n->kids, sup ? script : NULL);
  g_ptr_array_add(n->kids, sup ? NULL : script);
  g_ptr_array_add(list->kids, n);
}

/* Rows of cells (& between cells, \\ between rows) into m, until the end of the environment. */
static void parse_rows(Parser *ps, Node *m, Until until)
{
  Stop stop;

  do {
    Node *row = node_new(NODE_LIST, ORD);
    do
      g_ptr_array_add(row->kids, parse_list(ps, until, TRUE, &stop));
    while (stop == STOP_CELL);
    g_ptr_array_add(m->kids, row);
  } while (stop == STOP_ROW);
  Node *last = KID(m, m->kids->len - 1);
  if (m->kids->len > 1 && last->kids->len == 1 && KID(last, 0)->kids->len == 0) /* a \\ after the last row */
    g_ptr_array_remove_index(m->kids, m->kids->len - 1);
}

/* A delimited matrix or aligned equations, as one atom. */
static Node *fence(Node *inner, const char *open, const char *close)
{
  Node *n;

  if (strcmp(open, ".") == 0 && strcmp(close, ".") == 0)
    return inner;
  n = node_with(NODE_FENCED, INNER, inner, NULL);
  n->text = g_strdup(open);
  n->close = g_strdup(close);
  return n;
}

static Node *parse_environment(Parser *ps, const char *name)
{
  const Environment *env = &environments[0];
  Node *m = node_new(NODE_MATRIX, ORD);

  for (gsize i = 0; i < G_N_ELEMENTS(environments); i++)
    if (strcmp(environments[i].name, name) == 0)
      env = &environments[i];
  m->text = g_strdup(env->align);
  m->size = env->gap;
  m->style = env->style;
  m->pairs = env->pairs;
  if (g_str_has_suffix(env->name, "array")) { /* its columns: {lcr|...} */
    char *spec = read_raw(ps);
    GString *align = g_string_new(NULL);
    for (const char *c = spec; *c; c++)
      if (*c == 'l' || *c == 'c' || *c == 'r')
        g_string_append_c(align, *c);
    if (align->len)
      g_set_str(&m->text, align->str);
    g_string_free(align, TRUE);
    g_free(spec);
  } else if (g_str_has_prefix(env->name, "alignat") || strcmp(env->name, "alignedat") == 0) {
    g_free(read_raw(ps));
  }
  parse_rows(ps, m, UNTIL_ENV);
  return fence(m, env->open, env->close);
}

/* \big( ... \Biggr]: a delimiter of a fixed size. */
static Node *parse_big(Parser *ps, const char *name)
{
  static const struct {
    const char *name;
    double size;
  } sizes[] = {
    { "big", 1.2 },
    { "Big", 1.8 },
    { "bigg", 2.4 },
    { "Bigg", 3.0 },
  };
  gsize len = strlen(name);
  char last = len ? name[len - 1] : 0;
  gboolean side = last == 'l' || last == 'r' || last == 'm';

  for (gsize i = 0; i < G_N_ELEMENTS(sizes); i++) {
    gsize size_len = strlen(sizes[i].name);
    if (strncmp(name, sizes[i].name, size_len) != 0 || len != size_len + side)
      continue;
    Node *n = node_new(NODE_DELIM, last == 'l' ? OPEN : last == 'r' ? CLOSE : last == 'm' ? REL : ORD);
    n->size = sizes[i].size;
    n->text = read_delim(ps);
    return n;
  }
  return NULL;
}

static Node *parse_command(Parser *ps, const char *name)
{
  const Symbol *symbol;
  Node *n;

  for (gsize i = 0; i < G_N_ELEMENTS(spaces); i++)
    if (strcmp(spaces[i].name, name) == 0)
      return space_new(spaces[i].mu);
  if ((symbol = find_symbol(name))) {
    gunichar c = g_utf8_get_char(symbol->text);
    gboolean lower_greek = (c >= 0x3B1 && c <= 0x3C9) || (c >= 0x3D1 && c <= 0x3F5);
    Font font = ps->font == FONT_BOLD || ps->font == FONT_BOLD_ITALIC ? FONT_BOLD : FONT_ROMAN;
    if (lower_greek && (ps->font == FONT_ITALIC || ps->font == FONT_BOLD_ITALIC))
      font = ps->font;
    return symbol_new(symbol->text, symbol->cls, font);
  }
  for (gsize i = 0; i < G_N_ELEMENTS(big_operators); i++)
    if (strcmp(big_operators[i].name, name) == 0) {
      n = symbol_new(big_operators[i].text, OP, FONT_ROMAN);
      n->big = TRUE;
      n->limits = big_operators[i].limits ? LIMITS_DISPLAY : LIMITS_NONE;
      return n;
    }
  for (gsize i = 0; i < G_N_ELEMENTS(functions); i++)
    if (strcmp(functions[i].name, name) == 0) {
      n = symbol_new(functions[i].text, OP, FONT_ROMAN);
      n->limits = functions[i].limits ? LIMITS_DISPLAY : LIMITS_NONE;
      return n;
    }
  for (gsize i = 0; i < G_N_ELEMENTS(fonts); i++)
    if (strcmp(fonts[i].name, name) == 0) {
      Font old = ps->font;
      ps->font = fonts[i].font;
      n = parse_argument(ps);
      ps->font = old;
      return n;
    }
  for (gsize i = 0; i < G_N_ELEMENTS(font_switches); i++)
    if (strcmp(font_switches[i].name, name) == 0) {
      ps->font = font_switches[i].font;
      return NULL;
    }
  for (gsize i = 0; i < G_N_ELEMENTS(texts); i++)
    if (strcmp(texts[i].name, name) == 0) {
      char *raw = read_raw(ps), *text = plain_text(raw);
      n = symbol_new(text, ORD, texts[i].font);
      g_free(text);
      g_free(raw);
      return n;
    }
  for (gsize i = 0; i < G_N_ELEMENTS(accents); i++)
    if (strcmp(accents[i].name, name) == 0) {
      n = node_with(NODE_ACCENT, ORD, parse_argument(ps), NULL);
      n->text = g_strdup(accents[i].glyph);
      n->accent = accents[i].accent;
      n->size = accents[i].size;
      if (n->accent == ACCENT_OVERBRACE || n->accent == ACCENT_UNDERBRACE)
        n->limits = LIMITS_ALWAYS; /* \overbrace{...}^{text}: the text over the brace */
      return n;
    }
  for (gsize i = 0; i < G_N_ELEMENTS(classes); i++)
    if (strcmp(classes[i].name, name) == 0) {
      n = parse_argument(ps);
      n->cls = classes[i].cls;
      if (n->cls == OP)
        n->limits = LIMITS_DISPLAY;
      return n;
    }
  for (gsize i = 0; i < G_N_ELEMENTS(ignored); i++)
    if (strcmp(ignored[i].name, name) == 0) {
      for (int a = 0; a < ignored[i].arguments; a++)
        g_free(read_raw(ps));
      return NULL;
    }
  if ((n = parse_big(ps, name)))
    return n;

  if (strcmp(name, "frac") == 0 || strcmp(name, "dfrac") == 0 || strcmp(name, "tfrac") == 0 ||
      strcmp(name, "cfrac") == 0 || g_str_has_suffix(name, "binom")) {
    Node *num = parse_argument(ps);
    n = node_with(NODE_FRAC, INNER, num, parse_argument(ps));
    n->bar = !g_str_has_suffix(name, "binom");
    if (name[0] == 'd' || name[0] == 'c')
      n->style = STYLE_DISPLAY;
    else if (name[0] == 't')
      n->style = STYLE_TEXT;
    return n->bar ? n : fence(n, "(", ")");
  }
  if (strcmp(name, "sqrt") == 0) {
    Stop stop;
    Node *index;
    skip_spaces(ps);
    if (*ps->s == '[') {
      ps->s++;
      index = parse_list(ps, UNTIL_BRACKET, FALSE, &stop);
    } else {
      index = node_new(NODE_LIST, ORD);
    }
    Node *radicand = parse_argument(ps);
    return node_with(NODE_SQRT, ORD, radicand, index);
  }
  if (strcmp(name, "left") == 0) {
    Stop stop;
    char *open = read_delim(ps);
    Node *inner = parse_list(ps, UNTIL_RIGHT, FALSE, &stop);
    char *close = stop == STOP_RIGHT ? read_delim(ps) : g_strdup(".");
    n = node_with(NODE_FENCED, INNER, inner, NULL);
    n->text = open;
    n->close = close;
    return n;
  }
  if (strcmp(name, "middle") == 0) {
    char *delim = read_delim(ps);
    n = symbol_new(delim, REL, FONT_ROMAN);
    g_free(delim);
    return n;
  }
  if (strcmp(name, "begin") == 0) {
    char *env = read_raw(ps);
    n = parse_environment(ps, env);
    g_free(env);
    return n;
  }
  if (g_str_has_suffix(name, "style")) {
    static const char *const styles[] = { "displaystyle", "textstyle", "scriptstyle", "scriptscriptstyle" };
    for (int i = 0; i < 4; i++)
      if (strcmp(name, styles[i]) == 0) {
        n = node_new(NODE_STYLE, ORD);
        n->style = i;
        return n;
      }
  }
  if (strcmp(name, "operatorname") == 0) {
    gboolean limits = *ps->s == '*';
    ps->s += limits;
    char *raw = read_raw(ps), *text = plain_text(raw);
    n = symbol_new(text, OP, FONT_ROMAN);
    n->limits = limits ? LIMITS_DISPLAY : LIMITS_NONE;
    g_free(text);
    g_free(raw);
    return n;
  }
  if (strcmp(name, "not") == 0) {
    n = parse_argument(ps);
    if (n->kind == NODE_SYMBOL) {
      char *crossed = g_strconcat(n->text, "̸", NULL);
      g_free(n->text);
      n->text = crossed;
    }
    n->cls = REL;
    return n;
  }
  if (strcmp(name, "stackrel") == 0 || strcmp(name, "overset") == 0 || strcmp(name, "underset") == 0) {
    Node *script = parse_argument(ps), *base = parse_argument(ps);
    gboolean under = name[0] == 'u';
    n = node_with(NODE_SCRIPTS, base->cls, base, under ? NULL : script);
    g_ptr_array_add(n->kids, under ? script : NULL);
    n->limits = LIMITS_ALWAYS;
    return n;
  }
  if (strcmp(name, "substack") == 0) {
    n = node_new(NODE_MATRIX, ORD);
    n->text = g_strdup("c");
    n->size = 0;
    skip_spaces(ps);
    if (*ps->s == '{') {
      ps->s++;
      parse_rows(ps, n, UNTIL_BRACE);
    }
    return n;
  }
  if (strcmp(name, "textcolor") == 0 || strcmp(name, "colorbox") == 0) {
    g_free(read_raw(ps));
    return parse_argument(ps);
  }
  if (strcmp(name, "boxed") == 0 || strcmp(name, "fbox") == 0 || strcmp(name, "displaylines") == 0)
    return parse_argument(ps);
  if (strcmp(name, "hspace") == 0 || strcmp(name, "mspace") == 0 || strcmp(name, "kern") == 0) {
    char *raw = read_raw(ps), *unit;
    double value = g_ascii_strtod(raw, &unit), mu = value * 18;
    if (g_str_has_prefix(unit, "mu"))
      mu = value;
    else if (g_str_has_prefix(unit, "ex"))
      mu = value * 8;
    else if (g_str_has_prefix(unit, "pt") || g_str_has_prefix(unit, "px"))
      mu = value * 1.8;
    g_free(raw);
    return space_new(mu);
  }
  if (strcmp(name, "bmod") == 0)
    return symbol_new("mod", BIN, FONT_ROMAN);
  if (strcmp(name, "pmod") == 0 || strcmp(name, "pod") == 0 || strcmp(name, "mod") == 0) {
    n = node_new(NODE_LIST, ORD);
    g_ptr_array_add(n->kids, space_new(name[0] == 'm' ? 12 : 8));
    if (name[0] == 'p')
      g_ptr_array_add(n->kids, symbol_new("(", OPEN, FONT_ROMAN));
    if (name[1] != 'o' || name[0] == 'm') {
      g_ptr_array_add(n->kids, symbol_new("mod", ORD, FONT_ROMAN));
      g_ptr_array_add(n->kids, space_new(6));
    }
    if (name[0] == 'p') {
      g_ptr_array_add(n->kids, parse_argument(ps));
      g_ptr_array_add(n->kids, symbol_new(")", CLOSE, FONT_ROMAN));
    }
    return n;
  }
  char *shown = g_strconcat("\\", name, NULL); /* unknown: as it is written */
  n = symbol_new(shown, ORD, FONT_ERROR);
  g_free(shown);
  return n;
}

/* Atoms until a closing brace (or bracket, \right, \end), the end, or (cells) the end of a cell or row. */
static Node *parse_list(Parser *ps, Until until, gboolean cells, Stop *stop)
{
  Node *list = node_new(NODE_LIST, ORD);
  Font font = ps->font;

  *stop = STOP_END;
  if (++ps->depth > MAX_DEPTH) {
    ps->s += strlen(ps->s);
    ps->depth--;
    return list;
  }
  for (;;) {
    skip_spaces(ps);
    char c = *ps->s;
    if (!c)
      break;
    if (c == '}') {
      ps->s++;
      if (until == UNTIL_BRACE) {
        *stop = STOP_BRACE;
        break;
      }
    } else if (c == ']' && until == UNTIL_BRACKET) {
      ps->s++;
      *stop = STOP_BRACKET;
      break;
    } else if (c == '&') {
      ps->s++;
      if (cells) {
        *stop = STOP_CELL;
        break;
      }
    } else if (c == '\\' && ps->s[1] == '\\') {
      ps->s += 2;
      skip_spaces(ps);
      if (*ps->s == '[') /* \\[4pt] */
        while (*ps->s && *ps->s++ != ']')
          ;
      if (cells) {
        *stop = STOP_ROW;
        break;
      }
    } else if (c == '^' || c == '_') {
      ps->s++;
      attach(list, parse_argument(ps), c == '^');
    } else if (c == '\'') {
      GString *primes = g_string_new(NULL);
      while (*ps->s == '\'') {
        g_string_append(primes, "′");
        ps->s++;
      }
      g_ptr_array_add(list->kids, symbol_new(primes->str, ORD, FONT_ROMAN));
      g_string_free(primes, TRUE);
    } else if (c == '{') {
      Stop inner;
      ps->s++;
      g_ptr_array_add(list->kids, parse_list(ps, UNTIL_BRACE, FALSE, &inner));
    } else if (c == '\\') {
      char name[32];
      read_name(ps, name, sizeof name);
      if (strcmp(name, "right") == 0) {
        if (until == UNTIL_RIGHT) {
          *stop = STOP_RIGHT;
          break;
        }
        g_free(read_delim(ps));
      } else if (strcmp(name, "end") == 0) {
        g_free(read_raw(ps));
        if (until == UNTIL_ENV) {
          *stop = STOP_ENV;
          break;
        }
      } else if (strcmp(name, "limits") == 0 || strcmp(name, "nolimits") == 0) {
        if (list->kids->len)
          KID(list, list->kids->len - 1)->limits = name[0] == 'l' ? LIMITS_ALWAYS : LIMITS_NONE;
      } else {
        Node *n = parse_command(ps, name);
        if (n)
          g_ptr_array_add(list->kids, n);
      }
    } else {
      g_ptr_array_add(list->kids, parse_char(ps, TRUE));
    }
  }
  ps->font = font;
  ps->depth--;
  return list;
}

/* A whole formula: rows (\\) and aligned cells (&) at its top make it a gathered or aligned one. */
static Node *parse_formula(const char *tex)
{
  Parser ps = { tex, FONT_ITALIC, 0 };
  Node *m = node_new(NODE_MATRIX, ORD), *only;
  gboolean cells = FALSE;

  parse_rows(&ps, m, UNTIL_END);
  for (guint i = 0; i < m->kids->len; i++)
    cells |= KID(m, i)->kids->len > 1;
  if (m->kids->len == 1 && !cells) {
    only = g_ptr_array_steal_index(KID(m, 0)->kids, 0);
    node_free(m);
    return only;
  }
  m->text = g_strdup(cells ? "rl" : "c");
  m->pairs = cells;
  m->size = cells ? 2 : 0;
  return m;
}

/* Boxes -------------------------------------------------------------------------------------- */

/* ceil() and round() without libm. */
static double ceiling(double x)
{
  double whole = (double)(long long)x;

  return whole < x ? whole + 1 : whole;
}

static double nearest(double x)
{
  return (double)(long long)(x >= 0 ? x + 0.5 : x - 0.5);
}

typedef enum { ITEM_TEXT, ITEM_RULE, ITEM_RADICAL, ITEM_DELIM } ItemKind;

/* What a formula draws, from its baseline's left (y grows down). */
typedef struct {
  ItemKind kind;
  double x, y, w, h; /* TEXT: its baseline's left, stretched w times wide and h times high; others: their box */
  double base;       /* TEXT: its baseline below the layout's top; RADICAL: the bar's length right of the sign */
  double t;          /* RADICAL, DELIM: the lines' thickness */
  gunichar ch;       /* DELIM: the delimiter, arrow or brace (⏞ ⏟) */
  gboolean error;    /* TEXT: drawn in the error color */
  PangoLayout *layout;
} Item;

typedef struct {
  double w, a, d; /* width, height above the baseline, depth below it */
  double italic;  /* how far its last letter leans out right: a superscript moves by it */
  GArray *items;  /* Item, from the box's baseline's left */
} Box;

typedef struct {
  double x, y, w, h;
} Rect;

static Box box_new(void)
{
  return (Box){ .items = g_array_new(FALSE, FALSE, sizeof(Item)) };
}

static void box_free(Box *box)
{
  for (guint i = 0; i < box->items->len; i++)
    g_clear_object(&g_array_index(box->items, Item, i).layout);
  g_array_free(box->items, TRUE);
}

static void box_add(Box *box, Item item)
{
  g_array_append_val(box->items, item);
}

/* Moves child's items into box, child's baseline's left at (x, y); child is used up. */
static void box_put(Box *box, Box *child, double x, double y)
{
  for (guint i = 0; i < child->items->len; i++) {
    Item item = g_array_index(child->items, Item, i);
    item.x += x;
    item.y += y;
    box_add(box, item);
  }
  g_array_free(child->items, TRUE);
  child->items = NULL;
}

/* Moves a box's contents down by dy. */
static void box_lower(Box *box, double dy)
{
  for (guint i = 0; i < box->items->len; i++)
    g_array_index(box->items, Item, i).y += dy;
  box->a -= dy;
  box->d += dy;
}

/* The context every formula's text is measured in: unhinted, so sizes scale smoothly. */
static PangoContext *math_context(void)
{
  static PangoContext *context;

  if (!context) {
    cairo_font_options_t *options = cairo_font_options_create();
    context = pango_font_map_create_context(pango_cairo_font_map_get_default());
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(context, options);
    cairo_font_options_destroy(options);
    pango_context_set_round_glyph_positions(context, FALSE);
  }
  return context;
}

/* text in a font at size pixels: its layout, ink extents, width and baseline (from its top). */
static PangoLayout *text_layout(const char *text, Font font, double size, Rect *ink, double *width, double *base)
{
  PangoLayout *layout = pango_layout_new(math_context());
  PangoFontDescription *desc = pango_font_description_new();
  PangoRectangle i, l;

  pango_font_description_set_family(desc, font == FONT_SANS ? "sans-serif" : font == FONT_MONO ? "monospace" : SERIF);
  if (font == FONT_ITALIC || font == FONT_BOLD_ITALIC)
    pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
  if (font == FONT_BOLD || font == FONT_BOLD_ITALIC)
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
  pango_layout_set_font_description(layout, desc);
  pango_font_description_free(desc);
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_extents(layout, &i, &l);
  *ink = (Rect){ pango_units_to_double(i.x), pango_units_to_double(i.y), pango_units_to_double(i.width),
                 pango_units_to_double(i.height) };
  *width = pango_units_to_double(l.width);
  *base = pango_units_to_double(pango_layout_get_baseline(layout));
  return layout;
}

/* Text as a box: as high and deep as its ink, as wide as its advance. */
static Box text_box(const char *text, Font font, double size)
{
  Rect ink;
  double width, base;
  PangoLayout *layout = text_layout(text, font, size, &ink, &width, &base);
  Box box = box_new();

  box.w = width;
  if (ink.w > 0 && ink.h > 0) {
    box.a = MAX(base - ink.y, 0);
    box.d = MAX(ink.y + ink.h - base, 0);
  }
  if (font == FONT_ITALIC || font == FONT_BOLD_ITALIC)
    box.italic = MAX(ink.x + ink.w - width, 0);
  box_add(&box,
          (Item){ .kind = ITEM_TEXT, .w = 1, .h = 1, .base = base, .error = font == FONT_ERROR, .layout = layout });
  return box;
}

/* Layout ------------------------------------------------------------------------------------- */

static double style_size(double em, Style style)
{
  static const double factor[] = { 1, 1, 0.7, 0.5 };

  return em * factor[style];
}

/* The math axis: fraction bars and centered operators are on it (the minus sign's height). */
static double axis(double size)
{
  return 0.25 * size;
}

/* A fraction bar's, an overline's and a radical's thickness. */
static double rule(double size)
{
  return MAX(1.0, 0.055 * size);
}

/* TeX's space between neighbor atoms, in mu (1/18 em); negative: only in display and text style. */
static double atom_space(AtomClass left, AtomClass right, Style style)
{
  /* clang-format off */
  static const signed char mu[8][8] = {
    /*           ORD  OP  BIN  REL OPEN CLOSE PUNCT INNER */
    /* ORD   */ {  0,  3,  -4,  -5,  0,   0,    0,   -3 },
    /* OP    */ {  3,  3,   0,  -5,  0,   0,    0,   -3 },
    /* BIN   */ { -4, -4,   0,   0, -4,   0,    0,   -4 },
    /* REL   */ { -5, -5,   0,   0, -5,   0,    0,   -5 },
    /* OPEN  */ {  0,  0,   0,   0,  0,   0,    0,    0 },
    /* CLOSE */ {  0,  3,  -4,  -5,  0,   0,    0,   -3 },
    /* PUNCT */ { -3, -3,   0,  -3, -3,  -3,   -3,   -3 },
    /* INNER */ { -3,  3,  -4,  -5, -3,   0,   -3,   -3 },
  };
  /* clang-format on */
  int space = mu[left][right];

  return space >= 0 ? space : style <= STYLE_TEXT ? -space : 0;
}

static Box layout_node(double em, Node *n, Style style);

/* A row of atoms, spaced by their classes. after_ord: as if an ordinary atom were before the first
 * (the right side of an aligned equation keeps the space before its "="). */
static Box layout_list(double em, GPtrArray *kids, Style style, gboolean after_ord)
{
  Box box = box_new();
  int n = kids->len, prev = after_ord ? ORD : -1, last = -1;
  int *cls = g_new(int, MAX(n, 1));
  double x = 0;

  for (int i = 0; i < n; i++) {
    Node *kid = g_ptr_array_index(kids, i);
    cls[i] = kid->kind == NODE_SPACE || kid->kind == NODE_STYLE ? -1 : (int)kid->cls;
  }
  /* A binary operator with nothing on one side is an ordinary atom: the minus in -x. */
  for (int i = 0; i < n; i++) {
    if (cls[i] < 0)
      continue;
    if (cls[i] == BIN && (prev < 0 || prev == BIN || prev == OP || prev == REL || prev == OPEN || prev == PUNCT))
      cls[i] = ORD;
    if ((cls[i] == REL || cls[i] == CLOSE || cls[i] == PUNCT) && prev == BIN)
      cls[last] = ORD;
    prev = cls[i];
    last = i;
  }
  if (prev == BIN && last >= 0)
    cls[last] = ORD;

  prev = after_ord ? ORD : -1;
  for (int i = 0; i < n; i++) {
    Node *kid = g_ptr_array_index(kids, i);
    if (kid->kind == NODE_STYLE) {
      style = kid->style;
      continue;
    }
    if (cls[i] >= 0 && prev >= 0)
      x += atom_space(prev, cls[i], style) * style_size(em, style) / 18;
    Box child = layout_node(em, kid, style);
    double w = child.w;
    box.a = MAX(box.a, child.a);
    box.d = MAX(box.d, child.d);
    box.italic = child.italic;
    box_put(&box, &child, x, 0);
    x += w;
    if (cls[i] >= 0)
      prev = cls[i];
  }
  box.w = x;
  g_free(cls);
  return box;
}

static Box layout_symbol(double em, Node *n, Style style)
{
  double size = style_size(em, style);
  gunichar c = g_utf8_get_char(n->text);
  gboolean integral = c >= 0x222B && c <= 0x2233; /* the fonts' integrals are smaller than their sums */

  if (!n->big)
    return text_box(n->text, n->font, size);
  Box box =
      text_box(n->text, n->font, size * (style == STYLE_DISPLAY ? (integral ? 2.1 : 1.55) : (integral ? 1.3 : 1.15)));
  box_lower(&box, (box.a - box.d) / 2 - axis(size)); /* centered on the axis */
  return box;
}

static Box layout_frac(double em, Node *n, Style style)
{
  if (n->style >= 0)
    style = n->style;
  double size = style_size(em, style), t = rule(size), ax = axis(size), pad = 0.12 * size, gap, u, v;
  Style inner = style == STYLE_DISPLAY ? STYLE_TEXT : style == STYLE_TEXT ? STYLE_SCRIPT : STYLE_SCRIPTSCRIPT;
  Box num = layout_node(em, KID(n, 0), inner), den = layout_node(em, KID(n, 1), inner), box = box_new();

  if (n->bar) {
    gap = style == STYLE_DISPLAY ? MAX(3 * t, 0.15 * size) : MAX(t, 0.09 * size);
    u = ax + t / 2 + gap + num.d;
    v = t / 2 + gap + den.a - ax;
  } else {
    gap = style == STYLE_DISPLAY ? 0.5 * size : 0.25 * size;
    u = ax + gap / 2 + num.d;
    v = gap / 2 + den.a - ax;
  }
  box.w = MAX(num.w, den.w) + 2 * pad;
  box.a = u + num.a;
  box.d = v + den.d;
  double nw = num.w, dw = den.w;
  box_put(&box, &num, (box.w - nw) / 2, -u);
  box_put(&box, &den, (box.w - dw) / 2, v);
  if (n->bar)
    box_add(&box, (Item){ .kind = ITEM_RULE, .x = pad / 2, .y = -ax - t / 2, .w = box.w - pad, .h = t });
  return box;
}

static Box layout_sqrt(double em, Node *n, Style style)
{
  double size = style_size(em, style), t = rule(size), clear = t + 0.1 * size;
  Box inner = layout_node(em, KID(n, 0), style), box = box_new();
  double ia = MAX(inner.a, 0.68 * size), id = MAX(inner.d, 0.12 * size);
  double top = -(ia + clear + t), bottom = id, h = bottom - top, sign = 0.5 * size + 0.08 * h, x = 0.05 * size;
  double iw = inner.w;
  Node *index = KID(n, 1);

  if (index->kids->len) { /* its right edge over the middle of the sign, its bottom above the tick */
    Box ib = layout_node(em, index, STYLE_SCRIPTSCRIPT);
    double iwidth = ib.w, y = bottom - 0.5 * h - ib.d;
    x = MAX(x, iwidth - 0.5 * sign);
    box.a = -y + ib.a;
    box_put(&box, &ib, x + 0.5 * sign - iwidth, y);
  }
  box_add(&box, (Item){ .kind = ITEM_RADICAL, .x = x, .y = top, .w = sign, .h = h, .base = iw + 0.1 * size, .t = t });
  box_put(&box, &inner, x + sign, 0);
  box.w = x + sign + iw + 0.15 * size;
  box.a = MAX(box.a, -top + t);
  box.d = bottom;
  return box;
}

static Box layout_scripts(double em, Node *n, Style style)
{
  Node *base = KID(n, 0), *sup = KID(n, 1), *sub = KID(n, 2);
  double size = style_size(em, style), t = rule(size);
  Style ss = style <= STYLE_TEXT ? STYLE_SCRIPT : STYLE_SCRIPTSCRIPT;
  Box b = layout_node(em, base, style), up = { 0 }, down = { 0 }, box = box_new();
  double bw = b.w, italic = b.italic, u = 0, v = 0;

  if (sup)
    up = layout_node(em, sup, ss);
  if (sub)
    down = layout_node(em, sub, ss);
  if (n->limits == LIMITS_ALWAYS || (n->limits == LIMITS_DISPLAY && style == STYLE_DISPLAY)) {
    double gap = 0.12 * size, w = MAX(bw, MAX(sup ? up.w : 0, sub ? down.w : 0));
    box.w = w;
    box.a = b.a;
    box.d = b.d;
    if (sup) {
      double y = -(b.a + gap + up.d), uw = up.w;
      box.a = -y + up.a + 0.05 * size;
      box_put(&box, &up, (w - uw) / 2, y);
    }
    if (sub) {
      double y = b.d + gap + down.a, dw = down.w;
      box.d = y + down.d + 0.05 * size;
      box_put(&box, &down, (w - dw) / 2, y);
    }
    box_put(&box, &b, (w - bw) / 2, 0);
    return box;
  }

  gboolean letter = base->kind == NODE_SYMBOL && !base->big; /* scripts are placed from the letter's size */
  double ssize = style_size(em, ss);
  if (sup) {
    u = letter ? 0 : b.a - 0.39 * ssize;
    u = MAX(u, (style == STYLE_DISPLAY ? 0.41 : 0.36) * size);
    u = MAX(u, up.d + 0.11 * size);
  }
  if (sub) {
    v = letter ? 0 : b.d + 0.05 * ssize;
    v = MAX(v, 0.15 * size);
    v = MAX(v, down.a - 0.36 * size);
  }
  if (sup && sub) {
    v = MAX(v, 0.25 * size);
    double gap = (u - up.d) - (down.a - v);
    if (gap < 4 * t)
      v += 4 * t - gap;
  }
  box.a = b.a;
  box.d = b.d;
  box.w = bw + MAX(sup ? up.w + italic : 0, sub ? down.w : 0) + 0.05 * size;
  box_put(&box, &b, 0, 0);
  if (sup) {
    box.a = MAX(box.a, u + up.a);
    box_put(&box, &up, bw + italic, -u);
  }
  if (sub) {
    box.d = MAX(box.d, v + down.d);
    box_put(&box, &down, bw, v);
  }
  return box;
}

/* A delimiter at least height high, centered on the axis: the font's glyph when that is high enough,
 * otherwise drawn. */
static Box layout_delim(const char *text, double height, double size)
{
  static const char *const drawn = "()[]{}|‖⟨⟩⌊⌋⌈⌉/\\";
  gunichar c = g_utf8_get_char(text);
  double t = MAX(1.0, 0.065 * size), w, margin = 0.06 * size;
  Box box;

  if (!*text || strcmp(text, ".") == 0) {
    box = box_new();
    box.w = 0.12 * size;
    return box;
  }
  box = text_box(text, FONT_ROMAN, size);
  if (box.a + box.d >= height * 0.98 || !g_utf8_strchr(drawn, -1, c))
    return box;
  box_free(&box);
  switch (c) {
    case '(':
    case ')': w = MIN(0.28 * size + 0.05 * height, 0.6 * size); break;
    case '{':
    case '}': w = MIN(0.3 * size + 0.05 * height, 0.6 * size); break;
    case '|': w = 0.15 * size; break;
    case 0x2016: w = 0.3 * size; break;
    case 0x27E8:
    case 0x27E9: w = MIN(0.25 * size + 0.08 * height, 0.6 * size); break;
    case '/':
    case '\\': w = 0.3 * height; break;
    default: w = MIN(0.25 * size + 0.02 * height, 0.4 * size); break;
  }
  box = box_new();
  box.w = w + 2 * margin;
  box.a = axis(size) + height / 2;
  box.d = height / 2 - axis(size);
  box_add(&box, (Item){ .kind = ITEM_DELIM, .x = margin, .y = -box.a, .w = w, .h = height, .t = t, .ch = c });
  return box;
}

static Box layout_fenced(double em, Node *n, Style style)
{
  double size = style_size(em, style), ax = axis(size);
  Box inner = layout_node(em, KID(n, 0), style), box = box_new();
  double delta = MAX(inner.a - ax, inner.d + ax), height = MAX(delta * 1.802, 2 * delta - 0.5 * size);
  Box open = layout_delim(n->text, height, size), close = layout_delim(n->close, height, size);
  double x = 0, w;

  box.a = MAX(inner.a, MAX(open.a, close.a));
  box.d = MAX(inner.d, MAX(open.d, close.d));
  w = open.w;
  box_put(&box, &open, x, 0);
  x += w;
  w = inner.w;
  box_put(&box, &inner, x, 0);
  x += w;
  w = close.w;
  box_put(&box, &close, x, 0);
  box.w = x + w;
  return box;
}

static Box layout_accent(double em, Node *n, Style style)
{
  double size = style_size(em, style), t = rule(size), gap = MAX(2 * t, 0.08 * size);
  Node *kid = KID(n, 0);
  Box inner = layout_node(em, kid, style), box = box_new();
  double iw = inner.w, ia = inner.a, id = inner.d;
  gboolean slanted = kid->kind == NODE_SYMBOL && (kid->font == FONT_ITALIC || kid->font == FONT_BOLD_ITALIC);
  double skew = slanted ? 0.06 * size + inner.italic / 2 : 0;

  box.w = iw;
  box.a = ia;
  box.d = id;
  box_put(&box, &inner, 0, 0);
  switch (n->accent) {
    case ACCENT_OVERLINE:
      box_add(&box, (Item){ .kind = ITEM_RULE, .y = -(ia + gap + t), .w = iw, .h = t });
      box.a = ia + gap + 2 * t;
      break;
    case ACCENT_UNDERLINE:
      box_add(&box, (Item){ .kind = ITEM_RULE, .y = id + gap, .w = iw, .h = t });
      box.d = id + gap + 2 * t;
      break;
    case ACCENT_OVERBRACE:
    case ACCENT_UNDERBRACE: {
      double h = 0.35 * size, y = n->accent == ACCENT_OVERBRACE ? -(ia + gap + h) : id + gap;
      box_add(&box, (Item){ .kind = ITEM_DELIM,
                            .y = y,
                            .w = iw,
                            .h = h,
                            .t = t * 1.2,
                            .ch = n->accent == ACCENT_OVERBRACE ? 0x23DE : 0x23DF });
      if (n->accent == ACCENT_OVERBRACE)
        box.a = ia + gap + h;
      else
        box.d = id + gap + h;
      break;
    }
    case ACCENT_ARROW: {
      double h = 0.3 * size, y = -(MAX(ia, 0.45 * size) + gap + h);
      box_add(&box, (Item){ .kind = ITEM_DELIM, .y = y, .w = iw, .h = h, .t = t, .ch = g_utf8_get_char(n->text) });
      box.a = -y;
      break;
    }
    case ACCENT_GLYPH:
    case ACCENT_WIDE: { /* the glyph's ink centered over the text, a little above it */
      Rect ink;
      double width, base, top = MAX(ia, 0.45 * size) + 0.06 * size;
      PangoLayout *layout = text_layout(n->text, FONT_ROMAN, size * n->size, &ink, &width, &base);
      double sx = n->accent == ACCENT_WIDE && ink.w > 0 ? MAX(1, iw * 0.9 / ink.w) : 1, cx = iw / 2 + skew;
      box_add(&box, (Item){ .kind = ITEM_TEXT,
                            .x = cx - sx * (ink.x + ink.w / 2),
                            .y = -top - (ink.y + ink.h - base),
                            .w = sx,
                            .h = 1,
                            .base = base,
                            .layout = layout });
      box.a = MAX(box.a, top + ink.h);
      break;
    }
  }
  return box;
}

static Box layout_matrix(double em, Node *n, Style style)
{
  Style st = n->style < 0 ? style : MAX(style, (Style)n->style);
  double s = style_size(em, st), ax = axis(style_size(em, style)), row_gap = (n->pairs ? 0.3 : 0.25) * s;
  guint rows = n->kids->len, cols = 0;
  gsize align_len = n->text && *n->text ? strlen(n->text) : 0;
  Box box = box_new();

  for (guint r = 0; r < rows; r++)
    cols = MAX(cols, KID(n, r)->kids->len);
  if (!rows || !cols)
    return box;
  Box *cells = g_new0(Box, rows * cols);
  double *ra = g_new0(double, rows), *rd = g_new0(double, rows), *cw = g_new0(double, cols);
  for (guint r = 0; r < rows; r++) {
    Node *row = KID(n, r);
    ra[r] = 0.7 * s; /* a strut: rows as high as lines of text */
    rd[r] = 0.3 * s;
    for (guint j = 0; j < row->kids->len; j++) {
      Node *cell = KID(row, j);
      Box *b = &cells[r * cols + j];
      *b = layout_list(em, cell->kids, st, n->pairs && j % 2 == 1 && cell->kids->len);
      ra[r] = MAX(ra[r], b->a);
      rd[r] = MAX(rd[r], b->d);
      cw[j] = MAX(cw[j], b->w);
    }
  }
  double height = row_gap * (rows - 1), x, y;
  for (guint r = 0; r < rows; r++)
    height += ra[r] + rd[r];
  y = rows == 1 ? -ra[0] : -(ax + height / 2); /* centered on the axis (one row stays on the baseline) */
  box.a = -y;
  box.d = height + y;
  for (guint r = 0; r < rows; r++) {
    y += ra[r];
    x = 0;
    for (guint j = 0; j < cols; j++) {
      Box *b = &cells[r * cols + j];
      char align = align_len ? n->text[j % align_len] : 'c';
      if (b->items) {
        double offset = align == 'l' ? 0 : align == 'r' ? cw[j] - b->w : (cw[j] - b->w) / 2;
        box_put(&box, b, x + offset, y);
      }
      x += cw[j];
      if (j + 1 < cols)
        x += n->pairs && j % 2 == 0 ? 0 : n->size * s;
    }
    box.w = MAX(box.w, x);
    y += rd[r] + row_gap;
  }
  g_free(cells);
  g_free(ra);
  g_free(rd);
  g_free(cw);
  return box;
}

static Box layout_node(double em, Node *n, Style style)
{
  Box box;

  switch (n->kind) {
    case NODE_LIST: return layout_list(em, n->kids, style, FALSE);
    case NODE_SYMBOL: return layout_symbol(em, n, style);
    case NODE_FRAC: return layout_frac(em, n, style);
    case NODE_SQRT: return layout_sqrt(em, n, style);
    case NODE_SCRIPTS: return layout_scripts(em, n, style);
    case NODE_FENCED: return layout_fenced(em, n, style);
    case NODE_DELIM: return layout_delim(n->text, n->size * style_size(em, style), style_size(em, style));
    case NODE_ACCENT: return layout_accent(em, n, style);
    case NODE_MATRIX: return layout_matrix(em, n, style);
    case NODE_SPACE:
      box = box_new();
      box.w = n->size * style_size(em, style) / 18;
      return box;
    case NODE_STYLE: break;
  }
  return box_new();
}

/* Drawing ------------------------------------------------------------------------------------ */

/* A rule on whole device pixels: a 1 px bar stays sharp. */
static void fill_rule(cairo_t *cr, double x, double y, double w, double h)
{
  double y0 = y, y1 = y + h, ox = 0;

  cairo_user_to_device(cr, &ox, &y0);
  ox = 0;
  cairo_user_to_device(cr, &ox, &y1);
  y0 = nearest(y0);
  y1 = MAX(nearest(y1), y0 + 1);
  ox = 0;
  cairo_device_to_user(cr, &ox, &y0);
  ox = 0;
  cairo_device_to_user(cr, &ox, &y1);
  cairo_rectangle(cr, x, y0, w, y1 - y0);
  cairo_fill(cr);
}

/* A left delimiter, drawn into the box (x, y, w, h); right ones are it mirrored. */
static void draw_left_delim(cairo_t *cr, gunichar c, double x, double y, double w, double h, double t)
{
  double mid = y + h / 2;

  switch (c) {
    case '(': { /* a crescent, thickest in the middle */
      double tip = x + w * 0.9, out = x + w * 0.12, in = out + 1.6 * t;
      cairo_move_to(cr, tip, y);
      cairo_curve_to(cr, (8 * out - 2 * tip) / 6, y + 0.22 * h, (8 * out - 2 * tip) / 6, y + 0.78 * h, tip, y + h);
      cairo_curve_to(cr, (8 * in - 2 * tip) / 6, y + 0.78 * h, (8 * in - 2 * tip) / 6, y + 0.22 * h, tip, y);
      cairo_fill_preserve(cr);
      cairo_set_line_width(cr, t * 0.5);
      cairo_stroke(cr);
      break;
    }
    case '[':
    case 0x230A:   /* ⌊ */
    case 0x2308: { /* ⌈ */
      double stem = x + w * 0.3, end = x + w * 0.85;
      cairo_rectangle(cr, stem, y, 1.3 * t, h);
      if (c != 0x230A)
        cairo_rectangle(cr, stem, y, end - stem, 0.9 * t);
      if (c != 0x2308)
        cairo_rectangle(cr, stem, y + h - 0.9 * t, end - stem, 0.9 * t);
      cairo_fill(cr);
      break;
    }
    case '{': {
      double tip = x + w * 0.9, stem = x + w * 0.5, point = x + w * 0.1, r = MIN(w * 0.4, h * 0.12);
      cairo_move_to(cr, tip, y + t / 2);
      cairo_curve_to(cr, stem, y + t / 2, stem, y + t / 2, stem, y + r);
      cairo_line_to(cr, stem, mid - r);
      cairo_curve_to(cr, stem, mid, stem, mid, point, mid);
      cairo_curve_to(cr, stem, mid, stem, mid, stem, mid + r);
      cairo_line_to(cr, stem, y + h - r);
      cairo_curve_to(cr, stem, y + h - t / 2, stem, y + h - t / 2, tip, y + h - t / 2);
      cairo_set_line_width(cr, t);
      cairo_stroke(cr);
      break;
    }
    case '|':
      cairo_rectangle(cr, x + w / 2 - t / 2, y, t, h);
      cairo_fill(cr);
      break;
    case 0x2016: /* ‖ */
      cairo_rectangle(cr, x + w * 0.25 - t / 2, y, t, h);
      cairo_rectangle(cr, x + w * 0.75 - t / 2, y, t, h);
      cairo_fill(cr);
      break;
    case 0x27E8: /* ⟨ */
      cairo_move_to(cr, x + w * 0.85, y);
      cairo_line_to(cr, x + w * 0.15, mid);
      cairo_line_to(cr, x + w * 0.85, y + h);
      cairo_set_line_width(cr, t);
      cairo_stroke(cr);
      break;
    case '/':
      cairo_move_to(cr, x + w, y);
      cairo_line_to(cr, x, y + h);
      cairo_set_line_width(cr, t);
      cairo_stroke(cr);
      break;
  }
}

/* An arrow over text (→, ←, ↔) across the box, its heads h high. */
static void draw_arrow(cairo_t *cr, gunichar c, double x, double y, double w, double h, double t)
{
  double mid = y + h / 2, head = h / 2;

  cairo_set_line_width(cr, t);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_move_to(cr, x, mid);
  cairo_line_to(cr, x + w, mid);
  if (c != 0x2190) {
    cairo_move_to(cr, x + w - head, mid - head);
    cairo_line_to(cr, x + w, mid);
    cairo_line_to(cr, x + w - head, mid + head);
  }
  if (c != 0x2192) {
    cairo_move_to(cr, x + head, mid - head);
    cairo_line_to(cr, x, mid);
    cairo_line_to(cr, x + head, mid + head);
  }
  cairo_stroke(cr);
}

static void draw_delim(cairo_t *cr, const Item *item)
{
  static const gunichar mirrored[][2] = {
    { ')', '(' }, { ']', '[' }, { '}', '{' }, { 0x27E9, 0x27E8 }, { 0x230B, 0x230A }, { 0x2309, 0x2308 }, { '\\', '/' },
  };
  gunichar c = item->ch;

  cairo_save(cr);
  if (c == 0x2190 || c == 0x2192 || c == 0x2194) {
    draw_arrow(cr, c, item->x, item->y, item->w, item->h, item->t);
  } else if (c == 0x23DE || c == 0x23DF) { /* ⏞ ⏟: a brace turned on its side */
    if (c == 0x23DE) {
      cairo_translate(cr, item->x + item->w, item->y);
      cairo_rotate(cr, G_PI / 2);
    } else {
      cairo_translate(cr, item->x, item->y + item->h);
      cairo_rotate(cr, -G_PI / 2);
    }
    draw_left_delim(cr, '{', 0, 0, item->h, item->w, item->t);
  } else {
    for (gsize i = 0; i < G_N_ELEMENTS(mirrored); i++)
      if (mirrored[i][0] == c) {
        cairo_translate(cr, 2 * item->x + item->w, 0);
        cairo_scale(cr, -1, 1);
        c = mirrored[i][1];
      }
    draw_left_delim(cr, c, item->x, item->y, item->w, item->h, item->t);
  }
  cairo_restore(cr);
}

/* The sign: a short tick up, a thick stroke down, a long one up to the bar over the radicand. */
static void draw_radical(cairo_t *cr, const Item *item)
{
  double x = item->x, y = item->y, w = item->w, h = item->h, t = item->t;

  cairo_save(cr);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_width(cr, t);
  cairo_move_to(cr, x, y + 0.62 * h);
  cairo_line_to(cr, x + 0.22 * w, y + 0.52 * h);
  cairo_line_to(cr, x + 0.5 * w, y + h - t / 2);
  cairo_line_to(cr, x + w, y + t / 2);
  cairo_line_to(cr, x + w + item->base, y + t / 2);
  cairo_stroke(cr);
  cairo_set_line_width(cr, 2 * t);
  cairo_move_to(cr, x + 0.24 * w, y + 0.54 * h);
  cairo_line_to(cr, x + 0.5 * w, y + h - t);
  cairo_stroke(cr);
  cairo_restore(cr);
}

/* The paintable ---------------------------------------------------------------------------- */

#define TCODE_TYPE_MATH (tcode_math_get_type())
G_DECLARE_FINAL_TYPE(TcodeMath, tcode_math, TCODE, MATH, GObject)

struct _TcodeMath {
  GObject parent;
  GArray *items; /* Item, from the baseline's left */
  double w, a, d, pad;
};

static void tcode_math_snapshot(GdkPaintable *paintable, GdkSnapshot *snapshot, double width, double height)
{
  TcodeMath *self = TCODE_MATH(paintable);
  cairo_t *cr = gtk_snapshot_append_cairo(GTK_SNAPSHOT(snapshot), &GRAPHENE_RECT_INIT(0, 0, width, height));
  const GdkRGBA *fg = palette_rgba(PAL_FG);

  cairo_translate(cr, self->pad, ceiling(self->a));
  gdk_cairo_set_source_rgba(cr, fg);
  for (guint i = 0; i < self->items->len; i++) {
    const Item *item = &g_array_index(self->items, Item, i);
    switch (item->kind) {
      case ITEM_TEXT:
        cairo_save(cr);
        if (item->error)
          gdk_cairo_set_source_rgba(cr, palette_rgba(PAL_ERROR));
        cairo_translate(cr, item->x, item->y);
        cairo_scale(cr, item->w, item->h);
        cairo_move_to(cr, 0, -item->base);
        pango_cairo_show_layout(cr, item->layout);
        cairo_restore(cr);
        break;
      case ITEM_RULE: fill_rule(cr, item->x, item->y, item->w, item->h); break;
      case ITEM_RADICAL: draw_radical(cr, item); break;
      case ITEM_DELIM: draw_delim(cr, item); break;
    }
  }
  cairo_destroy(cr);
}

static int tcode_math_get_intrinsic_width(GdkPaintable *paintable)
{
  TcodeMath *self = TCODE_MATH(paintable);

  return (int)ceiling(self->w + 2 * self->pad);
}

static int tcode_math_get_intrinsic_height(GdkPaintable *paintable)
{
  TcodeMath *self = TCODE_MATH(paintable);

  return (int)(ceiling(self->a) + ceiling(self->d));
}

static GdkPaintableFlags tcode_math_get_flags(GdkPaintable *paintable G_GNUC_UNUSED)
{
  return GDK_PAINTABLE_STATIC_SIZE | GDK_PAINTABLE_STATIC_CONTENTS;
}

static void tcode_math_paintable_init(GdkPaintableInterface *iface)
{
  iface->snapshot = tcode_math_snapshot;
  iface->get_intrinsic_width = tcode_math_get_intrinsic_width;
  iface->get_intrinsic_height = tcode_math_get_intrinsic_height;
  iface->get_flags = tcode_math_get_flags;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(TcodeMath, tcode_math, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GDK_TYPE_PAINTABLE, tcode_math_paintable_init))

static void tcode_math_finalize(GObject *object)
{
  TcodeMath *self = TCODE_MATH(object);

  for (guint i = 0; i < self->items->len; i++)
    g_clear_object(&g_array_index(self->items, Item, i).layout);
  g_array_free(self->items, TRUE);
  G_OBJECT_CLASS(tcode_math_parent_class)->finalize(object);
}

static void tcode_math_class_init(TcodeMathClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = tcode_math_finalize;
}

static void tcode_math_init(TcodeMath *self G_GNUC_UNUSED)
{
}

GdkPaintable *preview_math_new(const char *tex, gboolean display, double size, int *depth)
{
  GString *source = g_string_new(tex);
  Node *root;
  Box box;

  /* GitHub's $`...`$ */
  g_strstrip(source->str);
  g_string_set_size(source, strlen(source->str));
  if (source->len >= 2 && source->str[0] == '`' && source->str[source->len - 1] == '`') {
    g_string_truncate(source, source->len - 1);
    g_string_erase(source, 0, 1);
  }
  root = parse_formula(source->str);
  g_string_free(source, TRUE);
  box = layout_node(size, root, display ? STYLE_DISPLAY : STYLE_TEXT);
  node_free(root);
  if (!box.items->len) {
    box_free(&box);
    return NULL;
  }

  TcodeMath *math = g_object_new(TCODE_TYPE_MATH, NULL);
  math->items = box.items;
  math->w = box.w + box.italic;
  math->a = box.a;
  math->d = box.d;
  math->pad = MAX(2, nearest(0.1 * size));
  *depth = (int)ceiling(box.d);
  return GDK_PAINTABLE(math);
}
