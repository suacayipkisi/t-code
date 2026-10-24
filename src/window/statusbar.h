/*
 * The status bar at the bottom:
 *   left   git branch ("*" when there are changes), clangd errors and warnings in the open files,
 *          Build and Run buttons and what they would build
 *   right  the current editor's indentation ("Spaces: 4", a click chooses the project's), cursor
 *          line and column, and the Keybinds button
 */
#pragma once

#include "app.h"

/* Creates the bar; its labels go to t->status. */
GtkWidget *status_bar(App *t);

/* Starts git status (git/gitstatus.c), whose answers fill the branch label, and fills the bar. */
void start_status_updates(App *t);

/* Fills the bar again from the tabs: problems, what Build would build, the cursor position. */
void update_status(App *t);

/* The current editor's indentation, "Spaces: 4" or "Tab Size: 4" (hidden without an editor). */
void status_show_indent(App *t);

/* The picker of indentations for the current project (the indentation label's click). */
void status_pick_indent(App *t);

/* Only the cursor position: it changes with every key, the rest of the bar does not. */
void status_show_position(App *t);

/* The branch label: "⎇ name", with "*" when the work tree has changes; branch NULL hides it
 * (not a repository). */
void status_show_branch(App *t, const char *branch, gboolean dirty);
/* Shows a short message in the status bar in the Build / Run label's place (socket "say", plugins,
 * crash recovery) for a few seconds; status updates meanwhile leave it there. */
void status_say(App *t, const char *message);
