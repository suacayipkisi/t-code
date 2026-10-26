/*
 * The find bar over an editor's top right corner: the text to find
 * (Match Case, Whole Word and Regex toggles), "3 of 12", previous / next, and a second row to
 * replace one match or all of them. Every match in the file is highlighted while it shows.
 */
#pragma once

#include "editor/editorview.h"

/* Shows the editor's find bar, with the replace row when replace, and the keyboard in it. A
 * selection on one line becomes the text to find. */
void findbar_show(Tab *tab, gboolean replace);

/* The next (backward: previous) match of the find bar's text from the cursor, shown selected.
 * Opens the bar first when nothing was searched in this editor yet. */
void findbar_next(Tab *tab, gboolean backward);

/* Frees a find bar (its widgets go with the page). NULL is fine. */
void findbar_free(FindBar *bar);
