/*
 * What t-code remembers between runs, in ~/.config/t-code/session.ini:
 *   [window]            size, pane sizes, which panes are shown, text size
 *   [recent]            the last opened project folders
 *   [project <sha1>]    per project folder: the open tabs of both groups and the active ones, the
 *                       indentation chosen for it in the status bar, its breakpoints and the
 *                       program it debugs
 * and the settings, in ~/.config/t-code/settings.ini.
 */
#pragma once

#include "app.h"

/* ~/.config/t-code/settings.ini (free it). */
char *settings_file(void);

/* Reads settings.ini into t->settings; missing values get their defaults. Without a
 * settings.ini yet, the settings session.ini kept before 0.3 move into one. */
void session_load_settings(App *t);

/* Writes t->settings to settings.ini, each with a comment saying what it does (keeping what was
 * added by hand), and the text size to session.ini. */
void session_save_settings(App *t);

/* Gives the window its saved size, pane sizes, visible panes and text size. */
void session_apply_window(App *t);

/* Reads what the current project keeps besides its tabs: the indentation chosen for it into
 * t->project.indent (none: width 0), its breakpoints into t->debug.breakpoints, its watch expressions
 * into the Run and Debug panel. */
void session_load_project(App *t);

/* Writes t->project.indent for the current project (width 0 forgets it). */
void session_save_project_indent(App *t);

/* A value kept for the current project ("debug-program"), or NULL (free it). */
char *session_project_value(App *t, const char *key);

/* Keeps value for the current project (NULL forgets it). */
void session_set_project_value(App *t, const char *key, const char *value);

/* Opens the tabs saved for the current project; returns whether any opened. */
gboolean session_restore_tabs(App *t);

/* Saves the window and the current project's tabs. */
void session_save(App *t);

/* Puts the current project at the top of the recent folders. */
void session_add_recent(App *t);

/* The recent folders, most recent first (free with g_strfreev); NULL when there are none. */
char **session_recent(void);
