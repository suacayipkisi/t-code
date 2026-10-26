/*
 * Line blame: who last changed the cursor's line and in which commit, dim after the line's end
 * ("Ada, 3 days ago · Fix the parser"). Asked of git blame once the cursor rests, with the editor's
 * text as it is (lines not saved or not committed say so); only for files in the project's
 * repository. Off with the "line-blame" setting.
 */
#pragma once

#include "app.h"

typedef struct Blame Blame;

/* Starts following a new editor's cursor; the state goes to tab->view->blame. */
void blame_attach(Tab *tab);

/* Stops, dropping an answer still to come (NULL is fine). */
void blame_free(Blame *blame);

/* The setting changed: the line's blame comes or goes. */
void blame_apply_setting(Tab *tab);

/* Draws it after the cursor's line (above the text). */
void blame_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot);

/* What is shown ("Ada, 3 days ago · Fix the parser"), or NULL (for the tests). */
const char *blame_text(Tab *tab);
