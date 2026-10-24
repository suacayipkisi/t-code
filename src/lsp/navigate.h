/*
 * Moving around the code with the language server: Go to Definition (F12,
 * Ctrl+click) and back (Alt+Left), Find All References (Shift+F12, in the popup list), Format
 * Document (Shift+Alt+F) and Rename Symbol (F2, in every file it is used in).
 */
#pragma once

#include "app.h"

#include <json-glib/json-glib.h>

/* Adds Ctrl+click (Go to Definition) to a new editor. */
void navigate_attach(Tab *tab);

/* Jumps to the definition of the symbol at the cursor (in another tab when it is elsewhere). */
void go_to_definition(Tab *tab);

/* Back to where the last Go to Definition (or reference) was taken from. */
void go_back(App *t);

/* Lists the places the symbol at the cursor is used, to pick one. */
void find_references(Tab *tab);

/* Formats the file with the language server (clangd: the project's .clang-format). */
void format_document(Tab *tab);

/* Ctrl+S: saves the file, formatting it first when the "Format on save" setting is on. */
void save_with_format(Tab *tab);

/* Opens the header of a source file or the source of a header (clangd, else a file beside it
 * with the same name). */
void switch_source_header(Tab *tab);

/* Lists the file's functions, types and the like in the popup list; one goes to it. */
void go_to_symbol(Tab *tab);
/* Lists symbols across the project through the language server; the chosen result opens its file. */
void go_to_workspace_symbol(Tab *tab);

/* Asks for a new name for the symbol at the cursor and renames it everywhere. */
void rename_symbol(Tab *tab);

/* Lists the server's fixes and refactorings for the selection, or the cursor's line, and the
 * problems on it; the one picked is applied. */
void quick_fix(Tab *tab);

/* Applies a CodeAction or Command of tab's server: its edit goes in; a command is run by the server,
 * which sends the edit back (workspace/applyEdit). */
void navigate_apply_action(Tab *tab, JsonNode *action, gboolean utf16);

/* Applies a WorkspaceEdit to the files it names (the open ones saved when save). */
void navigate_apply_edit(App *t, JsonNode *edit, gboolean save, gboolean utf16);
