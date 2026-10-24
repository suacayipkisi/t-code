/*
 * The Files panel: the project's folder tree (a GtkListView over folder.c's lists), with file icons
 * and git colors, a right-click menu (open, open to the side, preview, compare, history,
 * new file / folder, cut / copy / paste, copy path, rename, delete, open in terminal) and drag and
 * drop. Only open folders are read and watched; a closed folder's rows are freed.
 */
#pragma once

#include "app.h"

/* The color theme changed: the rows are painted again. */
void files_restyle(App *t);

/* Builds the panel for t->project.root (t->files). */
GtkWidget *files_panel(App *t);

/* Shows the tree of t->project.root after a project switch. */
void files_set_root(App *t);

/* An open file's errors or warnings changed (or it closed): the names are colored again. */
void files_problems_changed(App *t);

/* git status changed (git/gitstatus.c): the git colors are read again. */
void files_git_changed(App *t);

/* Gives the keyboard focus to the tree. */
void files_focus(App *t);

/* The panel's font: t->settings.files_font at the window's text size. */
void files_apply_font(App *t);

/* Stops a git status still running (at shutdown). */
void files_stop(App *t);

/* Opens the folders on the way to path and selects it ("Reveal in Files"). */
void files_reveal(App *t, const char *path);
