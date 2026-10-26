/*
 * Snippets: t-code's own (the .snippets files in its snippets folder: C, C++, Python, Go, Rust,
 * JavaScript / TypeScript, shell) and the user's (any .snippets file in ~/.config/t-code/snippets,
 * the same GtkSourceView format).
 * Typing a snippet's trigger and Tab puts it in, its fields reached with Tab / Shift+Tab; the
 * completion list offers them too. GtkSourceView reads the files once: the user's changes apply at
 * the next start.
 */
#pragma once

#include "app.h"

/* Where snippets are read from: the user's folder, then t-code's (t->paths.snippets). Before the
 * first editor. */
void snippets_init(App *t);

/* Snippets in a new editor: Tab after a trigger, and the completion list. */
void snippets_attach(Tab *tab);

/* ~/.config/t-code/snippets/user.snippets (free it). */
char *snippets_user_file(void);

/* What user.snippets starts with: how it works and one example. */
extern const char *const snippets_template;
