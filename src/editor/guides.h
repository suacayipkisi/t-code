/*
 * Indent guides, drawn under the text: a line at each indentation level, and the one of
 * the bracket pair around the cursor in its bracket pair color, and the ruler (Settings). The editor's view is a GtkSourceView
 * with that one more layer.
 */
#pragma once

#include "app.h"

/* The editor's view on buffer, drawing the guides when the setting is on. */
GtkSourceView *guides_view_new(Tab *tab, GtkSourceBuffer *buffer);
