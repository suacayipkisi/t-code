/*
 * The Hierarchy panel: who calls the function at the cursor and what it calls
 * (textDocument/prepareCallHierarchy, callHierarchy/incomingCalls and outgoingCalls), or a type's
 * supertypes and subtypes (prepareTypeHierarchy, typeHierarchy/supertypes and subtypes), as a tree
 * whose rows ask the server for their own children when shown; a click opens a symbol.
 */
#pragma once

#include "app.h"

/* Creates the panel. */
GtkWidget *hierarchy_panel(App *t);

/* Shift+Alt+H, "Show Type Hierarchy": the hierarchy of the symbol at tab's cursor, in the panel
 * (shown). */
void hierarchy_show(Tab *tab, gboolean types);

/* The title, a note when there is one, then the tree as far as it was asked for, indented two
 * spaces a level (for the tests). */
char *hierarchy_text(void);

/* Up: callers (or supertypes); else callees (or subtypes). The tree starts again. */
void hierarchy_set_up(gboolean up);
