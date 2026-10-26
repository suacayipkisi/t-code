/*
 * The docks around the editors, shared by window/window.c, window/titlebar.c and the docks' own
 * files: docks.c shows, hides, collapses and moves the panels (window.h has what others call),
 * widgets.c builds the docks with their tab bars, dragging and right-click menu. Twelve docks: 0..3
 * left (stacked), 4..7 bottom (side by side), 8..11 right (stacked); each side is a chain of paneds.
 */
#pragma once

#include "app.h"

/* widgets.c: creates the docks with every panel in its first one, the sides (t->layout.left, bottom,
 * right) and the editor area's drop target. */
void docks_create(App *t);

/* A panel's icon, and its number in panel_names (from 1; 0 for none) for dragging it, and back. */
const char *panel_icon(const char *page);
int panel_id(const char *page);
const char *panel_from_id(int id);

/* Whether a side (0 left, 1 bottom, 2 right) holds any panel; a side's widget. */
gboolean side_has_pages(App *t, int side);
GtkWidget *side_widget(App *t, int side);

/* Collapses dock d to its tab bar, or opens it; hides it; shows each dock that holds a panel. */
void set_dock_collapsed(App *t, int d, gboolean collapsed);
void hide_dock(App *t, int d);
void update_docks(App *t);

/* Moves page to dock `to` with its tab at insert_pos (< 0: where it is; past the end: last); to a
 * new dock before (rel < 0) or after (rel > 0) target_dock, or into it (rel 0). */
void move_panel_at(App *t, const char *page, int to, int insert_pos);
void move_panel_relative(App *t, const char *page, int target_dock, int rel);

/* A side's first dock holding panels (side * 4 when none), and a new dock after its last one. */
int first_tab_dock(App *t, int side);
int new_split_dock(App *t, int side, const char *moving_page);

/* Source Control's tab shows its count of changes (its page's title, "SOURCE CONTROL  3"). */
void update_scm_badge(GtkStackPage *sp, GParamSpec *pspec, App *t);

/* A "notify" handler: something about the panels changed, the Panels menu's checks follow (in an
 * idle). */
void on_layout_changed(GObject *object, GParamSpec *spec, App *t);

/* titlebar.c: the menu bar on the left; the sides' toggles and Split Editor on the right. */
GtkWidget *title_bar(App *t);
