/*
 * The breadcrumbs over an editor: the file's folders and name, then the symbols around the cursor
 * (lsp/symbols/symbols.h: "Widget › draw"). A symbol's crumb goes to it. Off with the "breadcrumbs"
 * setting.
 */
#pragma once

#include "app.h"

/* Makes a new editor's bar, at the top of its page (tab->view->breadcrumbs). */
void breadcrumbs_attach(Tab *tab);

/* Fills the bar again for the cursor (and shows or hides it as the setting says). */
void breadcrumbs_update(Tab *tab);

/* What the bar says, the crumbs joined with " › " (for the tests). */
char *breadcrumbs_text(Tab *tab);
