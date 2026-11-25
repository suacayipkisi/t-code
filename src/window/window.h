/*
 * The main window and its layout:
 *
 *   +-------------+-------------------------------+
 *   |             | [main.c x] [util.c x]         |  editor tabs (GtkSourceView), in any
 *   |    files    |                               |  number of groups
 *   |             +-------------------------------+
 *   |             | shell                         |
 *   +-------------+-------------------------------+
 *   | branch  problems  Build  Run      Ln, Col  Keybinds |  status bar
 *
 * The panels (panel_names, and the plugins' views) sit in docks on the left, bottom and right, any
 * number of them on each side, as tabs or split; they can be dragged between them. window.c builds the window, titlebar.c its title bar, docks/ the
 * docks.
 */
#pragma once

#include "app.h"

/* t-code's own panels that go into the docks around the editors, by name, and the side each is on at
 * first (0 left, 1 bottom, 2 right). */
extern const char *const panel_names[PANELS];
extern const int panel_default_sides[PANELS];

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

/* The mouse button that dragged a paned is up: the sides it squeezed to nothing (smaller than they
 * can show their panels) hide, to open again at their last size. */
void end_pane_drag(App *t);

/* The strips on the window's edges show where a hidden side is (dragging one opens it), none while
 * a mouse button is down. */
void update_edges(App *t);

/* Hides a shown pane, or shows a hidden one and gives it the keyboard. */
void toggle_pane(App *t, GtkWidget *pane);

/* Shows the dock holding page ("files", "scm", "search" or "shell"), switches to it and gives it
 * the keyboard. */
void show_sidebar_page(App *t, const char *page);

/* The side page is on (0 left, 1 bottom, 2 right), or -1; the stack of its dock, or NULL. */
int panel_side(App *t, const char *page);
GtkStack *panel_stack(App *t, const char *page);

/* Moves a panel into the first dock of a side (a new one when the side has none). */
void move_panel_to_side(App *t, const char *page, int side);

/* A panel of someone else's (a plugin's view) into the first dock of a side, shown (show: its side and
 * dock open and it switched to; else only its tab added, the side and the page shown left as they are,
 * unless the side has no dock yet); and taking it away. */
void add_panel(App *t, const char *name, const char *title, const char *icon, GtkWidget *child, int side,
               gboolean show);
void remove_panel(App *t, const char *name);

/* The docks in session.ini's [window]: each side's tree and each dock's panels, collapsed or hidden. An
 * older session.ini's twelve numbered docks are read too. */
void docks_save(App *t, GKeyFile *kf);
void docks_restore(App *t, GKeyFile *kf);

/* Whether page shows: its dock and side are shown, not collapsed or squeezed to nothing, and it is
 * the dock's current tab. */
gboolean panel_shown(App *t, const char *page);

/* Hides page's dock when page shows, else shows it wherever it is (the title bar's panel buttons). */
void toggle_panel(App *t, const char *page);

/* Puts every panel back in its first dock, every dock open, the sides at their first sizes
 * ("View: Reset Panel Layout"). */
void reset_panel_layout(App *t);

/* Whether t-code's own panel page is turned on (settings.panels_off; any other panel is). One turned
 * off opens nowhere: its icon is gone from its dock, which keeps its place for when it comes back. */
gboolean panel_enabled(App *t, const char *page);

/* Turns page on (and shows it) or off, saving the setting (the Panels menu, the Settings window). */
void set_panel_enabled(App *t, const char *page, gboolean on);

/* Puts the interface settings into effect: the panels turned off, the menu bar or t-code's icon as
 * its button, the status bar, its Build and Run buttons and the title bar's side buttons. */
void apply_interface(App *t);
