/*
 * The Outline panel: the current editor's symbols (lsp/symbols/symbols.h) as an indented list, the one
 * around the cursor selected; a click goes to a symbol.
 */
#pragma once

#include "app.h"

/* Creates the panel. */
GtkWidget *outline_panel(App *t);

/* Another editor is the current one (or none): its symbols. */
void outline_show_tab(App *t);

/* tab's symbols came (shown when it is the current editor). */
void outline_symbols_changed(Tab *tab);

/* tab's cursor moved: the symbol around it is selected. */
void outline_cursor_moved(Tab *tab);

/* Fills the list with roots for tab (outline_symbols_changed's work; the tests give their own). */
void outline_fill(App *t, Tab *tab, GPtrArray *roots);

/* The rows' text, one line each, indented two spaces a level (for the tests). */
char *outline_text(App *t);
