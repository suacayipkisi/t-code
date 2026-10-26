/*
 * Code folding: an arrow in the gutter beside each { } block's first line (shown while the pointer
 * is over the gutter, always on a folded one) hides the lines inside the block, keeping its head
 * ("void f() {") and its closing line; a "⋯" after the head marks it. The cursor going into folded
 * lines (a search, Go to Definition) unfolds them.
 */
#pragma once

#include "app.h"

typedef struct Folding Folding;

/* Puts the arrows into tab's gutter (editorview.c). */
void folding_attach(Tab *tab);

/* Frees it once the view is gone; NULL is fine. */
void folding_free(Folding *f);

/* The file was loaded again: nothing is folded. */
void folding_reset(Tab *tab);

/* Draws the "⋯" after each folded block's head (guides.c, under the text). */
void folding_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot);

/* The file's { } blocks (editor/text/blocks.h), worked out again only when the text changed. Owned by
 * the editor: valid until its next change. */
GArray *folding_blocks(Tab *tab);

/* The line numbers' renderer (sticky scroll puts its numbers where they are). */
GtkWidget *folding_line_numbers(Tab *tab);

/* Folds the innermost block around the cursor, or unfolds it (Ctrl+Shift+[ and ]). */
void fold_at_cursor(Tab *tab);
void unfold_at_cursor(Tab *tab);

/* Folds every block, or unfolds everything. */
void fold_all(Tab *tab);
void unfold_all(Tab *tab);
