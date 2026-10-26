/*
 * Inlay hints (textDocument/inlayHint): parameter names before arguments and deduced types after
 * names, shown inside the line in a dim color without touching the text. The gap a hint needs is
 * made with a letter-spacing tag on the character next to it (a space when there is one beside
 * the hint; otherwise the character before, drawn again at its cell's start: Pango centers the
 * glyph in the gap, under the hint's opaque box), and the hint is drawn in it. Asked for the shown
 * lines and some around them, again after an edit or a scroll past them; off with the
 * "inlay-hints" setting. Holding Ctrl+Alt alone turns them the other way while it is held.
 */
#pragma once

#include "app.h"

typedef struct Inlay Inlay;

/* Starts showing a new editor's hints; the state goes to tab->view->inlay. */
void inlay_attach(Tab *tab);

/* Stops, removing its tags (NULL is fine). */
void inlay_free(Inlay *inlay);

/* The text changed, the server has the file again or asks for its hints again: asked again soon. */
void inlay_update(Tab *tab);

/* The setting changed: the hints come or go. */
void inlay_apply_setting(Tab *tab);

/* Ctrl+Alt held (on) or let go: every editor's hints the other way round from the setting, or back. */
void inlay_peek(App *t, gboolean on);

/* Watches the window's keys for Ctrl+Alt held alone. */
void inlay_peek_keys(App *t, GtkWindow *window);

/* Draws the hints (above the text). */
void inlay_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot);

/* The hints shown, "line:column text" each on a line (for the tests). */
char *inlay_text(Tab *tab);
