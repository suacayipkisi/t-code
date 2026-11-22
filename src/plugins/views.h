/*
 * Plugins' views (tapi_1's view-open and the rest): a read-only text a plugin fills, as a panel in a
 * dock on the left, bottom or right (dragged anywhere like t-code's own) or as a tab in an editor
 * group. Each is the plugin's own, by an id it chooses; a double click or Enter on a line sends the
 * view_activated event, and a view tab the user closes sends view_closed.
 */
#pragma once

#include "app.h"

/* Where a view goes ("left", "bottom", "right", "editor": the active group, "side": the group beside
 * it). */
typedef enum { VIEW_LEFT, VIEW_BOTTOM, VIEW_RIGHT, VIEW_EDITOR, VIEW_SIDE } ViewPlace;

/* The place a word names; FALSE when it names none. */
gboolean view_place(const char *word, ViewPlace *place);

/* Opens owner's view id with title and text (NULL: none), or, when it is open already, gives it the
 * title and text and shows it. icon: an icon name for a dock's tab (NULL: a default one). */
void view_open(App *t, const char *owner, const char *id, const char *title, ViewPlace place, const char *icon,
               const char *text);

/* Replaces (append: adds to) the text of owner's view; FALSE when it has no such view. */
gboolean view_set_text(App *t, const char *owner, const char *id, const char *text, gboolean append);

/* Shows owner's view (its dock or tab, switched to); closes it. FALSE when it has no such view. */
gboolean view_show(App *t, const char *owner, const char *id);
gboolean view_close(App *t, const char *owner, const char *id);

/* A view tab's title (the label of its tab), or NULL for another tab. */
const char *view_title(Tab *tab);
