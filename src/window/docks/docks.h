/*
 * The docks around the editors, shared by window/window.c, window/titlebar.c and the docks' own
 * files: docks.c shows, hides, collapses and moves the panels and keeps the docks' trees (window.h has
 * what others call), widgets.c builds a dock with its tab bar and right-click menu, and the drop
 * markers a dragged panel icon shows. Each side (0 left, 1 bottom, 2 right) is a tree of splits
 * (window/splits.h) of any number of docks; a dock is a card with its panels' icons over a stack of
 * them, and goes once its last panel leaves.
 */
#pragma once

#include "app.h"
#include "window/splits.h"

typedef struct Dock {
  App *t;
  int side;            /* 0 left, 1 bottom, 2 right */
  GtkWidget *card;     /* the tree's leaf: the tab bar over the stack */
  GtkStack *stack;     /* the panels, by name */
  GtkWidget *switcher; /* their icons */
  gboolean collapsed;  /* only the tab bar shows */
  int saved_pos;       /* its split's position before it collapsed */
} Dock;

/* widgets.c ----------------------------------------------------------------------------------------- */

/* Creates the sides (t->layout.left, bottom, right) with a dock each holding t-code's panels, and the
 * editor area's drop target for files. */
void docks_create(App *t);

/* A new dock for side, in no tree yet. */
Dock *dock_new(App *t, int side);

/* content (the window's panes) under the drop markers a dragged panel icon shows. */
GtkWidget *docks_drop_view(App *t, GtkWidget *content);

/* t-code's own panels' icons (others: a terminal's). */
const char *panel_icon(const char *page);

/* A panel's number for dragging it (from 1; 0 for none), and back. */
int panel_id(const char *page);
const char *panel_from_id(int id);

/* What a panel icon dragged to (x, y) in docks_drop_view's coordinates would do; shows it. Dropping it
 * there: FALSE when nothing happens. Leaving: the markers go. */
int docks_drop_hover(App *t, const char *page, double x, double y);
gboolean docks_drop(App *t, const char *page, double x, double y);
void docks_drop_leave(App *t);

/* The middle of a marker (dropzone.h's DropZone) as the last hover placed it; FALSE when not shown. */
gboolean docks_drop_marker(App *t, int zone, double *x, double *y);

/* docks.c --------------------------------------------------------------------------------------------- */

/* The dock of a card (NULL for any other widget). */
Dock *dock_of(GtkWidget *widget);

/* The dock holding page, or NULL. */
Dock *panel_dock(App *t, const char *page);

/* A side's docks in order (a new array); its first one (NULL when it has none). */
GPtrArray *side_docks(App *t, int side);
Dock *side_first_dock(App *t, int side);

/* Whether a side holds any panel turned on; whether one of its shown docks is open (not collapsed); a
 * side's widget. */
gboolean side_has_pages(App *t, int side);
gboolean side_has_expanded(App *t, int side);
GtkWidget *side_widget(App *t, int side);

/* The root of a side's tree of docks (window/splits.h), inside its widget. */
GtkWidget *side_root(App *t, int side);

/* Collapses a dock to its tab bar, or opens it; hides it; shows the sides that have shown docks. */
void set_dock_collapsed(Dock *dock, gboolean collapsed);
void hide_dock(Dock *dock);
void update_docks(App *t);

/* Adds a page to a dock (shown) with its title and icon. */
void add_dock_page(Dock *dock, GtkWidget *child, const char *name, const char *title, const char *icon);

/* Moves page into dock `to` with its tab at insert_pos (past the end: last); into a new dock beside
 * another; into a new dock at the end of a side. A dock left without panels goes. */
void move_panel_at(App *t, const char *page, Dock *to, int insert_pos);
void move_panel_beside(App *t, const char *page, Dock *beside, SplitSide side);
void move_panel_edge(App *t, const char *page, int side);

/* Takes away the docks without panels (moving the last one out queues it too). */
void prune_docks(App *t);

/* Source Control's tab shows its count of changes (its page's title, "SOURCE CONTROL  3"). */
void update_scm_badge(GtkStackPage *sp, GParamSpec *pspec, App *t);

/* A "notify" handler: something about the panels changed, the Panels menu's checks follow (in an
 * idle). */
void on_layout_changed(GObject *object, GParamSpec *spec, App *t);

/* The panels turned off hide (their docks too when nothing else is left in them), the others show
 * (window.h's apply_interface). */
void apply_panels(App *t);

/* titlebar.c: the menu bar on the left; the sides' toggles on the right. */
GtkWidget *title_bar(App *t);
/* The menu bar or t-code's icon as its button, the side buttons, the Panels menu's checks. */
void title_bar_apply(App *t);
/* The Panels menu: a section turning t-code's panels on and off, one for the window's parts (win.
 * actions; also the docks' right-click menu). A new menu. */
GMenu *panels_menu(void);
/* A gtk-decoration-layout without "icon" (the title bar shows its own logo); free with g_free. */
char *title_bar_layout_without_icon(const char *layout);
