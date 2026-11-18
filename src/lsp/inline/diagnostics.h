/*
 * The language server's diagnostics in an editor: errors underlined with a red wave, warnings
 * yellow, notes blue; their counts in the status bar, their messages when the
 * pointer rests on them (lsp/inline/hover.c).
 */
#pragma once

#include "app.h"

#include <json-glib/json-glib.h>

/* Shows a publishDiagnostics' list (replacing the server's last one; plugins' stay). */
void diagnostics_show(Tab *tab, JsonNode *diagnostics);

/* A plugin's diagnostics for path (socket "diagnostics-set"), in place of its last ones there: a JSON
 * list of {"range":[line,col,line,col],"severity":"error|warning|info|hint","message":..,"code":..,
 * "source":..}, 1-based. Kept per path: an editor opened later shows them too. NULL or [] drops them. */
void diagnostics_set_owned(App *t, const char *path, const char *owner, JsonNode *items);
/* Drops a plugin's diagnostics for path (NULL: every file). */
void diagnostics_clear_owned(App *t, const char *path, const char *owner);
/* An editor loaded its file: the plugins' diagnostics for it show. */
void diagnostics_restore(Tab *tab);

/* The messages of the diagnostics at iter, one per line ("error: ..."), or NULL. markup gets Pango
 * markup with each severity in its color. */
char *diagnostics_at(Tab *tab, const GtkTextIter *iter, gboolean markup);

/* The lines with errors (and with warnings), 0-based, for the minimap's marks. */
void diagnostics_lines(Tab *tab, GArray *errors, GArray *warnings);

/* Draws the wavy underlines for diagnostics on the view's text. */
void diagnostics_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot);

/* Forgets them all (the editor goes). */
void diagnostics_clear(Tab *tab);

/* The diagnostics touching lines first to last (0-based), as a JSON list the server sent them in. */
char *diagnostics_json(Tab *tab, int first, int last);

/* The diagnostics at iter as a JSON list the server sent them in, with start and end set to the range
 * they cover together; NULL (start and end unset) when there are none. */
char *diagnostics_json_at(Tab *tab, const GtkTextIter *iter, GtkTextIter *start, GtkTextIter *end);

typedef struct Diagnostic Diagnostic;

/* The diagnostics list for a tab, or NULL. */
GPtrArray *diagnostics_of(Tab *tab);

/* Gets line and column (1-based) of diagnostic d. */
void diagnostic_position(Tab *tab, const Diagnostic *d, int *line, int *column);

/* Gets diagnostic severity (1 = Error, 2 = Warning, 3 = Info, 4 = Hint). */
int diagnostic_severity(const Diagnostic *d);

/* Gets diagnostic message. */
const char *diagnostic_message(const Diagnostic *d);
