/*
 * While the program is paused, the pointer resting on a name in the editor shows its value
 * ("sum = 245"): a GtkSourceHoverProvider next to the language server's (lsp/inline/hover.c),
 * whose part goes first. On a member ("p->name", "s.x") the whole chain is evaluated; inside a
 * selection, the selected text.
 */
#pragma once

#include "app.h"

/* Adds the provider to tab's hover (editorview.c). */
void debug_hover_attach(Tab *tab);
