/*
 * The Search panel, the third sidebar page (Ctrl+Shift+F): text to find (Match
 * Case, Whole Word, Regular Expression), files to include and exclude, and the results grouped by
 * file. ripgrep (or grep) does the work; searchmatch.h holds the part that needs no GTK.
 */
#pragma once

#include "app.h"

/* Creates the panel. */
GtkWidget *search_panel(App *t);

/* Gives the keyboard to the search text, all of it selected to type over. */
void search_focus(void);

/* Runs the same search again (another project was opened). */
void search_reset(void);

/* Replace All without asking (what its question's Replace runs): every match of the search in the
 * project gets the replacement, in open files' editors or on disk; then the search runs again. */
void search_replace_run(void);
