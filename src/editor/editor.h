/*
 * Editors: a file in a GtkSourceView tab. Opening loads it in the background; saving writes it
 * back (also by itself a second after the last change, with autosave on); a file changed on disk
 * (git checkout, another program) is loaded again while the editor has nothing unsaved.
 */
#pragma once

#include "app.h"

/* Opening ------------------------------------------------------------------------------------ */

/* Opens path in a new tab of the active group. line > 0 puts the cursor there (column 1-based). */
Tab *open_editor(App *t, const char *path, int line, int column, gboolean readonly);

/* Opens path in an editor, or switches to the tab that has it already (and keeps it, if temporary). */
void open_file(App *t, const char *path);

/* A click in the Files panel: opens path in a temporary tab of the active group, in place of the
 * group's temporary tab, or switches to the tab that has it already. */
void open_file_temporary(App *t, const char *path);

/* The same, and puts the cursor at line and column (1-based; line 0: where it is). */
void open_file_at(App *t, const char *path, int line, int column);

/* Opens the file in the second group, splitting first. An already open file moves there, so the
 * same file is never in two tabs at once. */
void open_to_side(App *t, const char *path);

/* Moves the cursor to line and column (1-based) and scrolls there. */
void editor_goto(Tab *tab, int line, int column);

/* Loads the file again from disk (git restored it), dropping what is not saved. */
void editor_reload(Tab *tab);

/* Closes an editor; with unsaved changes it asks first (Save, Don't Save, Cancel). */
void quit_editor(Tab *tab);

/* Saving ------------------------------------------------------------------------------------- */

typedef void (*SavedFunc)(App *t, gpointer data);

/* Saves every changed editor, then calls then(t, data) (also when a save fails, and while the app
 * closes: then checks t->closing itself). */
void save_all(App *t, SavedFunc then, gpointer data);

/* Saves one editor (Ctrl+S). A file changed on disk since it was loaded or saved is not written
 * over: a bar over the editor offers Compare, Reload and Overwrite. */
void save_editor(Tab *tab);

/* Whether that bar is showing. */
gboolean editor_in_conflict(Tab *tab);

/* Turns code coloring off or on in every open editor (F7); new ones start the same way. */
void toggle_syntax(App *t);

/* Relative line numbers on or off in every editor, saved (Ctrl+Alt+L). */
void toggle_relative_numbers(App *t);
