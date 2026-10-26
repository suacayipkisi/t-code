/*
 * Selection highlight: when the selection is a whole name ("count", by a double click
 * or otherwise), its other occurrences in the file get a faint background, and the strip right of
 * the minimap marks their lines. An edit clears them until the selection changes again.
 */
#pragma once

#include "app.h"

typedef struct Occurrences Occurrences;

/* Starts following a new editor's selection; the state goes to tab->view->occurrences. */
void occurrences_attach(Tab *tab);

/* Appends the lines of the highlighted occurrences (the selection's own too) to lines. */
void occurrences_lines(Tab *tab, GArray *lines);

/* Stops following the buffer and frees the state (NULL is fine). */
void occurrences_free(Occurrences *o);

/* The highlight in the color theme's colors (again when it changes). */
void occurrences_recolor(Tab *tab);
