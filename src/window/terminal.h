/*
 * Terminals (VteTerminal): their look, starting programs in them, and the Terminal panel: the
 * shells as tabs, the first one always there.
 */
#pragma once

#include "app.h"

#define DEFAULT_FONT "Cascadia Code 11"

/* A new terminal in the editor font in the color theme's colors. */
VteTerminal *new_terminal(App *t);

/* Gives the terminals, the editors and the Files panel the fonts the settings choose for them. */
void apply_fonts(App *t);

/* Creates the Terminal panel (t->shell.panel): its tabs (t->shell.tabs) with the first terminal,
 * t->shell.term, which is never closed and starts again when its shell exits ... */
void create_shell_pane(App *t);
/* ... starts the user's shell in it ... */
void start_shell_pane(App *t);
/* ... and, after a project switch, closes the other tabs and starts it again in the new project. */
void restart_shell_pane(App *t);

/* A new terminal tab, its shell started in dir (NULL: the project root), shown and focused. Its
 * tab closes with its × or when its shell exits. */
VteTerminal *open_shell(App *t, const char *dir);

/* A new terminal tab named name running argv (NULL: the user's shell) in the project root, shown but
 * not focused (a plugin's: socket "terminal-open"). It closes when its program exits and is not kept
 * in session.ini. */
VteTerminal *open_program(App *t, const char *name, char **argv);

/* The i-th terminal tab (the first one is 0), or NULL. */
VteTerminal *shell_at(App *t, int i);

/* Closes a terminal tab (not the first one): what runs in it gets SIGHUP. */
void close_shell_tab(App *t, VteTerminal *term);

/* Closes the terminal tab showing now (not the first one). */
void close_current_shell(App *t);

/* Asks for a new name for the terminal tab showing now. */
void rename_current_shell(App *t);

/* The terminal tab showing now. */
VteTerminal *current_shell(App *t);

/* How many terminal tabs there are, the first one included. */
int shell_count(App *t);

/* A terminal tab's name ("Terminal", "Terminal 2", or what it was renamed to) ... */
const char *shell_name(VteTerminal *term);
/* ... and renaming it (a blank name changes nothing). */
void shell_rename(VteTerminal *term, const char *name);

/* The folders the tabs after the first one are in, and their names in *names, for session.ini
 * (free both with g_strfreev) ... */
char **shell_folders(App *t, char ***names);
/* ... and opening them again behind the current one (a folder gone: the project root). */
void shell_restore(App *t, char **folders, char **names);

/* Types command into the first terminal and runs it (Build, Run). A program that runs in the shell
 * gets Ctrl+C first; when it does not give the prompt back within 2 seconds, a "terminal is busy"
 * message shows instead. */
void run_in_shell(App *t, const char *command);

/* cd into dir in the terminal tab showing now (kept out of the shell's history). */
void cd_in_shell(App *t, const char *dir);

/* The color theme changed: the terminal tabs and the Debug panel's terminal take its colors. */
void terminal_apply_colors(App *t);
