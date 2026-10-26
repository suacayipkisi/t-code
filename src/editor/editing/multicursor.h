/*
 * Multiple cursors: Ctrl+D adds the next occurrence of the selection, Ctrl+Shift+L selects them all,
 * Ctrl+Shift+Up / Down add a cursor on the line above / below, Alt+Click adds (or removes) one and
 * Shift+Alt+drag selects a column. While there is more than one, typing, Backspace, Delete, Enter,
 * Tab, the arrows, Home / End and copy / cut / paste act on all of them, as one undo step; Escape or a
 * plain click goes back to one. The extra cursors are pairs of marks; the buffer's own cursor is the
 * first.
 */
#pragma once

#include "app.h"

typedef struct MultiCursor MultiCursor;

/* How the cursors move (the arrows, Home / End, Ctrl+arrows). */
typedef enum {
  MULTI_LEFT,
  MULTI_RIGHT,
  MULTI_UP,
  MULTI_DOWN,
  MULTI_HOME,
  MULTI_END,
  MULTI_WORD_LEFT,
  MULTI_WORD_RIGHT,
} MultiMove;

/* Adds the keys, clicks and drags to a new editor; the state goes to tab->view->multi. */
void multicursor_attach(Tab *tab);

/* Frees the state and its marks (NULL is fine). */
void multicursor_free(MultiCursor *m);

/* Whether the editor has more than one cursor. */
gboolean multicursor_active(Tab *tab);

/* Back to the buffer's own cursor only. */
void multicursor_clear(Tab *tab);

/* Ctrl+D: without a selection, selects the name at the cursor; with one, adds the next occurrence of
 * its text (from the last one added, round to the top) as another selection. */
void multicursor_add_next(Tab *tab);

/* Ctrl+Shift+L: every occurrence of the selection's text (or of the name at the cursor). */
void multicursor_select_all(Tab *tab);

/* Ctrl+Shift+Up / Down: a cursor on the line above / below the last one added, in the same column
 * (or the line's end). */
void multicursor_add_line(Tab *tab, gboolean below);

/* Alt+Click: a cursor at iter, or none there any more when one was. */
void multicursor_toggle_at(Tab *tab, const GtkTextIter *iter);

/* A column selection: one per line from line0 to line1 (0-based), from column col0 to col1
 * (characters, cut at each line's end); the buffer's cursor takes the first line. */
void multicursor_box(Tab *tab, int line0, int col0, int line1, int col1);

/* Every cursor: text typed (replacing its selection), a move. */
void multicursor_type(Tab *tab, const char *text);
void multicursor_move(Tab *tab, MultiMove move, gboolean extend);

/* The selections' text in the file's order, a line each (what Ctrl+C copies); NULL when none of
 * them selects anything. */
char *multicursor_selected_text(Tab *tab);

/* Pastes text at every cursor: one line each when it has as many lines as there are cursors. */
void multicursor_paste(Tab *tab, const char *text);

/* Draws the extra cursors' selections (below the text) or the cursors themselves (above it). */
void multicursor_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot, gboolean above);
