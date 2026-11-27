/*
 * Trusted folders. A project's own programs run only in a folder the user trusted: its venv's
 * Python and pylsp, its node_modules TypeScript, Rust build scripts and proc macros, and the
 * Testing panel's listing on its own (pytest, cargo and go test run the project's code). Elsewhere
 * the language servers use programs from PATH and a bar above the editors offers to trust the
 * folder. Build, Run, tasks and a Refresh click are the user's own request and are not asked about.
 *
 * trust.c (GLib-only): the list, one folder per line in ~/.config/t-code/trusted; a folder inside
 * a trusted one is trusted too. trustbar.c: the bar.
 */
#pragma once

#include <glib.h>

typedef struct App App;
typedef struct _GtkWidget GtkWidget; /* GTK's own name, so the GLib-only half needs no GTK */

/* Whether folder is trusted (it or a folder above it is in the list). FALSE for NULL. */
gboolean trust_has(const char *folder);

/* Adds folder to the list (nothing when it is trusted already). */
gboolean trust_add(const char *folder, GError **error);

/* trustbar.c ------------------------------------------------------------------------------------ */

/* The bar (hidden), put above the editors by the window. */
GtkWidget *trust_bar_new(App *t);

/* Whether the project's own programs may run. When not, what ("its Python environment") is added
 * to the bar, which shows unless the user answered "Not Now" for this project. FALSE without a
 * project. */
gboolean trust_project(App *t, const char *what);

/* The project changed: the bar hides and forgets what it listed. */
void trust_reset(App *t);
