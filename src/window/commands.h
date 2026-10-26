/*
 * Every command of the window in one table (commands.c): the keyboard shortcuts, the command
 * palette (Ctrl+Shift+P) and the Keybinds list are all made from it. A new command is one line
 * in that table.
 */
#pragma once

#include "app.h"

/* Adds the table's shortcuts to the window. */
void add_shortcuts(App *t);

/* The same for another window (the second window): commands that are not about the focused editor
 * bring the main window up first. */
void add_shortcuts_to(App *t, GtkWindow *window);
/* Rebuilds built-in and plugin shortcuts after a plugin reload. */
void refresh_shortcuts(App *t);

/* Runs the command of the table named name (as the command palette lists it), on the current tab.
 * The editor's right-click menu uses it. */
void run_command(App *t, const char *name);

/* A command and its shortcut, for the Keybinds window. */
typedef struct {
  const char *name;
  const char *keys; /* as shown; NULL when there is no shortcut */
} KeyHelp;

/* The window's commands; *n is set to how many. */
const KeyHelp *window_keys(int *n);
