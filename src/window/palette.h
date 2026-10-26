/*
 * Color themes: each one is a few base colors (background, frame, text, accent, the code's colors and
 * the terminal's 16) from which every color of t-code is worked out, or taken as the theme gives it.
 * The panels' CSS uses them as variables (var(--tc-fg), ...), the editors get a style scheme made
 * from them, and what t-code draws itself (guides, sticky lines, folds, the diff gutter) asks for
 * them here. The desktop's light or dark setting is not followed: the theme says which.
 */
#pragma once

#include "app.h"

typedef enum {
  /* The panels */
  PAL_FRAME,         /* title bar, status bar, the gaps between the panels */
  PAL_PANEL,         /* panels and editors */
  PAL_RAISED,        /* popovers, tooltips, the find bar */
  PAL_INPUT,         /* text fields */
  PAL_BORDER,        /* lines between things */
  PAL_BORDER_STRONG, /* around popups and fields */
  PAL_FG,            /* text */
  PAL_FG_BRIGHT,     /* headings */
  PAL_FG_STRONG,     /* the current tab, bold text */
  PAL_FG_DIM,        /* notes, inactive tabs */
  PAL_FG_FAINT,      /* folders beside names, placeholders */
  PAL_ACCENT,        /* buttons, the current tab's line, focused fields */
  PAL_ACCENT_HOVER,
  PAL_ACCENT_FG,          /* text on the accent */
  PAL_SELECTION,          /* the selected row of a focused list */
  PAL_INACTIVE_SELECTION, /* ... of a list without focus */
  PAL_HOVER,              /* a row under the pointer */
  PAL_ERROR,
  PAL_WARNING,
  PAL_INFO,
  PAL_WARNING_BG, /* the save conflict bar */
  PAL_WARNING_BORDER,
  PAL_MATCH, /* search matches in the Search panel */
  PAL_LINK,
  /* Git */
  PAL_GIT_MODIFIED,
  PAL_GIT_ADDED,
  PAL_GIT_UNTRACKED,
  PAL_GIT_DELETED,
  PAL_GIT_CONFLICT,
  PAL_GIT_IGNORED,
  /* The editor */
  PAL_EDITOR_SELECTION,
  PAL_SELECTION_UNFOCUSED,
  PAL_CURRENT_LINE,
  PAL_LINE_NUMBER,
  PAL_CURSOR,
  PAL_BRACKET,
  PAL_SEARCH,
  PAL_GUIDE,
  PAL_GUIDE_ACTIVE,
  PAL_PAIR1, /* bracket pair colors by depth */
  PAL_PAIR2,
  PAL_PAIR3,
  PAL_OCCURRENCE,
  PAL_DIFF_ADDED,
  PAL_DIFF_MODIFIED,
  PAL_DIFF_DELETED,
  /* The code */
  PAL_COMMENT,
  PAL_KEYWORD,
  PAL_CONTROL,
  PAL_TYPE,
  PAL_FUNCTION,
  PAL_VARIABLE,
  PAL_CONSTANT,
  PAL_STRING,
  PAL_NUMBER,
  PAL_ESCAPE,
  PAL_OPERATOR,
  PAL_COUNT
} PalColor;

/* Picks the theme id (an unknown one: Dark Classic): the panels' CSS variables and GTK's light or
 * dark. Before any window; window/theme.c's color_theme_apply() switches while t-code runs. */
void palette_init(App *t, const char *id);

/* Adds a panel's CSS (which uses the palette's var(--tc-...) colors) for the whole display, at
 * application priority; the same string again does nothing, so a widget made many times may call
 * it each time. */
void palette_add_css(const char *css);

/* The current theme's color, as a GdkRGBA and as "#rrggbb". */
const GdkRGBA *palette_rgba(PalColor color);
const char *palette_hex(PalColor color);

/* The terminal's 16 colors. */
const GdkRGBA *palette_terminal(void);

/* The id of the theme id names now (a theme's old id gives its new one; an unknown one Dark Classic's). */
const char *palette_known_id(const char *id);

/* The current theme's id, and whether it is a light one. */
const char *palette_id(void);
gboolean palette_light(void);

/* The editor style scheme for the current theme (made from styles/dark-classic.xml with its colors). */
GtkSourceStyleScheme *palette_scheme(App *t);

/* The themes, for the Settings window and the picker: how many, and the n-th one's id and name. */
int palette_count(void);
const char *palette_nth_id(int n);
const char *palette_nth_name(int n);
