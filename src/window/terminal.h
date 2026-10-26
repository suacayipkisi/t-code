/*
 * Terminals (VteTerminal): their look, starting programs in them, and the shell pane, which is
 * always there.
 */
#pragma once

#include "app.h"

#define DEFAULT_FONT "Cascadia Code 11"

/* A new terminal in the editor font in the color theme's colors. */
VteTerminal *new_terminal(App *t);

/* Gives the terminals, the editors and the Files panel the fonts the settings choose for them. */
void apply_fonts(App *t);

/* Creates t->shell.term (it restarts when the shell exits) ... */
void create_shell_pane(App *t);
/* ... starts the user's shell in it ... */
void start_shell_pane(App *t);
/* ... and makes it start again in the current project (after a project switch). */
void restart_shell_pane(App *t);

/* Types command into the shell pane and runs it. A program that runs in the shell gets Ctrl+C
 * first; when it does not give the prompt back within 2 seconds, a "terminal is busy" message
 * shows instead. */
void run_in_shell(App *t, const char *command);

/* cd into dir in the shell pane (kept out of the shell's history). */
void cd_in_shell(App *t, const char *dir);

/* The color theme changed: the shell and the Debug panel's terminal take its colors. */
void terminal_apply_colors(App *t);
