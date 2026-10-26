/*
 * Merge conflicts in an editor: the blocks git leaves between "<<<<<<<", "=======" and ">>>>>>>"
 * lines (with "|||||||" before the common ancestor's lines in the diff3 style). Their two sides are
 * tinted (green the current change, blue the incoming one), and links after the "<<<<<<<" line
 * keep one side or both in place of the block, in one undo step. Looked for again after the text
 * changes.
 */
#pragma once

#include "app.h"

typedef struct Conflicts Conflicts;

/* Starts looking for conflicts in a new editor; the state goes to tab->view->conflicts. */
void conflicts_attach(Tab *tab);

/* Stops, removing the links (NULL is fine). */
void conflicts_free(Conflicts *conflicts);

/* Tints the conflicts' lines (under the text) and keeps the links beside them. */
void conflicts_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot);
