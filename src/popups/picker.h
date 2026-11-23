/*
 * The popup list at the top of the window: a search entry and a list
 * of results. Go to File (Ctrl+P), the command palette and File History all use it; each gives
 * a query function that fills the list and a choose function for the picked row.
 */
#pragma once

#include "app.h"

/* Called with each new text; fills the list with picker_add(), now or later (results for an
 * older generation are dropped). */
typedef void (*PickerQuery)(App *t, const char *query, guint generation);
/* Called with the payload of the row the user picked. */
typedef void (*PickerChoose)(App *t, const char *payload);
/* Called when the picker closes without a pick. */
typedef void (*PickerCancel)(App *t);

/* Creates the (hidden) picker over the window's content. */
void picker_init(App *t, GtkOverlay *overlay);

/* Puts the entry into the title bar's search bar (the results dropping down from it) while the
 * search-bar setting is on, else back over the window's content. window/titlebar.c calls it. */
void picker_place(App *t);

/* Shows the picker with an empty entry and runs query for it. */
void picker_open(App *t, const char *placeholder, PickerQuery query, PickerChoose choose);

/* The same with text already typed in the entry, the cursor after it. */
void picker_open_text(App *t, const char *placeholder, const char *text, PickerQuery query, PickerChoose choose);

/* After picker_open(): preview(t, payload) runs for each row the arrow keys go to (a color theme
 * shown as it would look), cancel(t) when the picker closes without a pick (to undo it). */
void picker_set_preview(App *t, PickerChoose preview, PickerCancel cancel);

/* Adds a result row: label, a dimmed detail on the right (may be NULL), and the payload choose
 * gets. Up to 200 rows. */
void picker_add(App *t, guint generation, const char *label, const char *detail, const char *payload);

/* A line of text instead of results ("Searching…", "No results"); NULL hides it. */
void picker_message(App *t, guint generation, const char *text);

/* New data for query: fill the list again, if the picker still shows that query's results. */
void picker_requery(App *t, PickerQuery query);

/* How well query matches text, higher is better; -1 when the query letters do not all appear
 * in order. */
int fuzzy_score(const char *query, const char *text);
