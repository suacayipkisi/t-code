/*
 * The main window and its layout:
 *
 *   +-------------+-------------------------------+
 *   |             | [main.c x] [util.c x]         |  editor tabs (GtkSourceView);
 *   |    files    |                               |  split into two groups side by side
 *   |             +-------------------------------+
 *   |             | shell                         |
 *   +-------------+-------------------------------+
 *   | branch  problems  Build  Run      Ln, Col  Keybinds |  status bar
 *
 * The sidebar on the left is a stack of three pages: the file manager ("files"), Source Control
 * ("scm") and Search ("search").
 */
#pragma once

#include "app.h"

/* The panels that go into the docks around the editors, by name, and the dock each is in at first. */
extern const char *const panel_names[PANELS];
extern const int panel_default_docks[PANELS];

/* Creates the window with everything in it, starts the shell and shows it. Called once, from
 * the application's "activate" or "open" signal. */
void build_window(App *t, GtkApplication *app);

/* Text size of the shell, editors and the Files panel (Ctrl+= / Ctrl+-), 0.5 to 3. */
void set_font_scale(App *t, double scale);

/* Text size of the Markdown previews (Ctrl+= / Ctrl+- in a preview tab), 0.5 to 3. */
void set_preview_zoom_scale(App *t, double scale);

/* Shows or hides a pane (the sidebar or the shell); a pane that had the keyboard gives it to the
 * current tab. */
void set_pane_visible(App *t, GtkWidget *pane, gboolean visible);

/* Hides a shown pane, or shows a hidden one and gives it the keyboard. */
void toggle_pane(App *t, GtkWidget *pane);

/* Shows the dock holding page ("files", "scm", "search" or "shell"), switches to it and gives it
 * the keyboard. */
void show_sidebar_page(App *t, const char *page);

/* Which dock holds page (0..3 left, 4..7 bottom, 8..11 right; see DOCKS in app.h), or -1. */
int panel_dock(App *t, const char *page);

/* Moves a panel (one of panel_names) to dock (0..DOCKS-1). */
void move_panel(App *t, const char *page, int dock);

/* Whether page shows: its dock and side are shown, not collapsed or squeezed to nothing, and it is
 * the dock's current tab. */
gboolean panel_shown(App *t, const char *page);

/* Hides page's dock when page shows, else shows it wherever it is (the title bar's panel buttons). */
void toggle_panel(App *t, const char *page);

/* Puts every panel back in its first dock, every dock open, the sides at their first sizes
 * ("View: Reset Panel Layout"). */
void reset_panel_layout(App *t);
