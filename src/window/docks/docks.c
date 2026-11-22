/*
 * Showing, hiding, collapsing and moving the panels in the docks, and the docks' trees (see docks.h
 * and window.h).
 */
#include "window/docks/docks.h"

#include "editor/tabs.h"
#include "files/files.h"
#include "lsp/problems.h"
#include "search/search.h"
#include "window/terminal.h"
#include "window/window.h"

#include <string.h>

static void queue_panel_checks(App *t);

/* clang-format off */
const char *const panel_names[PANELS] = { "files",    "scm",     "search",    "shell",   "debug",  "run",
                                          "problems", "outline", "hierarchy", "testing", "plugins" };
const int panel_default_sides[PANELS] = { 0, 0, 0, 1, 1, 0, 1, 2, 2, 2, 0 };
static const char *const side_names[3] = { "left", "bottom", "right" };
/* clang-format on */

/* A collapsed dock's tab bar: its height on the sides, its width (its icons) side by side. */
#define COLLAPSED_HEIGHT 32
#define COLLAPSED_WIDTH 96

/* Finding docks ---------------------------------------------------------------------------------- */

Dock *dock_of(GtkWidget *widget)
{
  return widget ? g_object_get_data(G_OBJECT(widget), "t-code-dock") : NULL;
}

static gboolean is_dock(GtkWidget *widget)
{
  return dock_of(widget) != NULL;
}

GtkWidget *side_widget(App *t, int side)
{
  return side == 0 ? t->layout.left : (side == 1 ? t->layout.bottom : t->layout.right);
}

GtkWidget *side_root(App *t, int side)
{
  return side_widget(t, side) ? g_object_get_data(G_OBJECT(side_widget(t, side)), "t-code-dock-root") : NULL;
}

GPtrArray *side_docks(App *t, int side)
{
  GPtrArray *cards = g_ptr_array_new(), *docks = g_ptr_array_new();

  if (side_root(t, side))
    split_collect(side_root(t, side), is_dock, cards);
  for (guint i = 0; i < cards->len; i++)
    g_ptr_array_add(docks, dock_of(g_ptr_array_index(cards, i)));
  g_ptr_array_unref(cards);
  return docks;
}

Dock *side_first_dock(App *t, int side)
{
  GPtrArray *docks = side_docks(t, side);
  Dock *dock = docks->len ? g_ptr_array_index(docks, 0) : NULL;

  g_ptr_array_unref(docks);
  return dock;
}

static gboolean dock_has_pages(Dock *dock)
{
  return gtk_widget_get_first_child(GTK_WIDGET(dock->stack)) != NULL;
}

Dock *panel_dock(App *t, const char *page)
{
  for (guint i = 0; page && t->dock.list && i < t->dock.list->len; i++) {
    Dock *dock = g_ptr_array_index(t->dock.list, i);
    if (gtk_stack_get_child_by_name(dock->stack, page))
      return dock;
  }
  return NULL;
}

int panel_side(App *t, const char *page)
{
  Dock *dock = panel_dock(t, page);

  return dock ? dock->side : -1;
}

GtkStack *panel_stack(App *t, const char *page)
{
  Dock *dock = panel_dock(t, page);

  return dock ? dock->stack : NULL;
}

gboolean side_has_pages(App *t, int side)
{
  GPtrArray *docks = side_docks(t, side);
  gboolean any = FALSE;

  for (guint i = 0; i < docks->len && !any; i++)
    any = dock_has_pages(g_ptr_array_index(docks, i));
  g_ptr_array_unref(docks);
  return any;
}

/* Whether a side has a shown dock (expanded: an open one). */
static gboolean side_shows(App *t, int side, gboolean expanded)
{
  GPtrArray *docks = side_docks(t, side);
  gboolean any = FALSE;

  for (guint i = 0; i < docks->len && !any; i++) {
    Dock *dock = g_ptr_array_index(docks, i);
    any = dock_has_pages(dock) && gtk_widget_get_visible(dock->card) && (!expanded || !dock->collapsed);
  }
  g_ptr_array_unref(docks);
  return any;
}

gboolean side_has_expanded(App *t, int side)
{
  return side_shows(t, side, TRUE);
}

/* Showing the sides ------------------------------------------------------------------------------ */

static void focus_panel(App *t, const char *page)
{
  if (!page)
    return;
  if (strcmp(page, "search") == 0)
    search_focus();
  else if (strcmp(page, "files") == 0)
    files_focus(t);
  else if (strcmp(page, "scm") == 0)
    gtk_widget_grab_focus(t->scm);
  else if (strcmp(page, "shell") == 0)
    gtk_widget_grab_focus(GTK_WIDGET(current_shell(t)));
  else if (strcmp(page, "problems") == 0)
    problems_focus(t);
}

static void sync_sidebar_visible(App *t)
{
  for (int side = 0; side < 3; side++)
    if (side_widget(t, side))
      gtk_widget_set_visible(side_widget(t, side), side_shows(t, side, FALSE));
}

/* Hides a side left without a shown dock. Never shows one: a side hidden with its docks still
 * there (Ctrl+B) stays hidden when a dock on another side changes. */
static void hide_side_if_empty(App *t, int side)
{
  GtkWidget *w = side_widget(t, side);

  if (w && !side_shows(t, side, FALSE))
    gtk_widget_set_visible(w, FALSE);
}

/* Gives a side that is being shown room when its paned has squeezed it to (almost) nothing. */
static void make_room(App *t, int side)
{
  if (side == 0 && gtk_paned_get_position(t->layout.hpaned) < 48) {
    gtk_paned_set_position(t->layout.hpaned, 280);
  } else if (side == 1 && t->layout.vpaned) {
    int h = gtk_widget_get_height(GTK_WIDGET(t->layout.vpaned));
    if (h > 200 && gtk_paned_get_position(t->layout.vpaned) > h - 60)
      gtk_paned_set_position(t->layout.vpaned, h - 240);
  } else if (side == 2 && t->layout.rpaned) {
    int w = gtk_widget_get_width(GTK_WIDGET(t->layout.rpaned));
    int pos = gtk_paned_get_position(t->layout.rpaned);
    if (w > 320 && (pos <= 0 || pos > w - 60))
      gtk_paned_set_position(t->layout.rpaned, w - 280);
    else if (w <= 320 && pos <= 0)
      gtk_paned_set_position(t->layout.rpaned, 840);
  }
}

/* Whether a shown side has been squeezed to (almost) nothing by its paned, as make_room() puts it
 * right: a size saved on a bigger screen, a paned dragged to the edge. The bottom with every dock
 * collapsed is small on purpose. */
static gboolean side_squeezed(App *t, int side)
{
  if (side == 0)
    return t->layout.hpaned && gtk_paned_get_position(t->layout.hpaned) < 48;
  GtkPaned *paned = side == 1 ? t->layout.vpaned : t->layout.rpaned;
  int size = !paned      ? 0
             : side == 1 ? gtk_widget_get_height(GTK_WIDGET(paned))
                         : gtk_widget_get_width(GTK_WIDGET(paned));
  gboolean expanded = side != 1 || side_has_expanded(t, 1);

  return expanded && size > 0 && gtk_paned_get_position(paned) > size - 48;
}

/* A collapsed dock keeps only its tab bar in its split: the split's handle goes next to the bar, and
 * back where it was once the dock opens. */
static void fit_dock(Dock *dock)
{
  GtkPaned *split = split_parent(dock->card);

  if (!split || !gtk_widget_get_visible(split_other(dock->card)))
    return;
  gboolean vertical = gtk_orientable_get_orientation(GTK_ORIENTABLE(split)) == GTK_ORIENTATION_VERTICAL;
  gboolean start = gtk_paned_get_start_child(split) == dock->card;
  int total = vertical ? gtk_widget_get_height(GTK_WIDGET(split)) : gtk_widget_get_width(GTK_WIDGET(split));
  int bar = vertical ? COLLAPSED_HEIGHT : COLLAPSED_WIDTH, pos = gtk_paned_get_position(split);
  int collapsed_pos = start ? bar : total - bar;

  if (total <= 2 * bar)
    return;
  if (dock->collapsed) {
    if (ABS(pos - collapsed_pos) > 16)
      dock->saved_pos = pos;
    gtk_paned_set_position(split, collapsed_pos);
  } else if (ABS(pos - collapsed_pos) <= 16) {
    gtk_paned_set_position(split, dock->saved_pos > bar + 16 && dock->saved_pos < total - bar - 16 ? dock->saved_pos
                                                                                                   : total / 2);
  }
}

/* The bottom with every dock collapsed keeps only their tab bars; opening one gives the side its
 * height back. */
static void fit_bottom(App *t)
{
  static int saved;
  int h, pos;

  if (!t->layout.vpaned || !side_shows(t, 1, FALSE))
    return;
  h = gtk_widget_get_height(GTK_WIDGET(t->layout.vpaned));
  pos = gtk_paned_get_position(t->layout.vpaned);
  if (!side_has_expanded(t, 1) && h > 100) {
    if (pos < h - 48)
      saved = pos;
    gtk_paned_set_position(t->layout.vpaned, h - 34);
  } else if (side_has_expanded(t, 1) && h > 200 && pos > h - 48) {
    gtk_paned_set_position(t->layout.vpaned, saved > 100 && saved < h - 60 ? saved : h - 240);
  }
}

static void fit_side(App *t, int side)
{
  GPtrArray *docks = side_docks(t, side);

  for (guint i = 0; i < docks->len; i++)
    fit_dock(g_ptr_array_index(docks, i));
  g_ptr_array_unref(docks);
  if (side == 1)
    fit_bottom(t);
}

void set_dock_collapsed(Dock *dock, gboolean collapsed)
{
  GtkWidget *btn = g_object_get_data(G_OBJECT(dock->card), "tcode-collapse-btn");

  dock->collapsed = collapsed;
  gtk_widget_set_visible(GTK_WIDGET(dock->stack), !collapsed);
  if (dock->side != 1)
    gtk_widget_set_valign(dock->card, collapsed ? GTK_ALIGN_START : GTK_ALIGN_FILL);
  if (btn)
    gtk_button_set_icon_name(GTK_BUTTON(btn),
                             collapsed ? "tcode-chevron-right-symbolic" : "tcode-chevron-down-symbolic");
  fit_side(dock->t, dock->side);
  queue_panel_checks(dock->t);
}

void update_docks(App *t)
{
  sync_sidebar_visible(t);
  for (int s = 0; s < 3; s++)
    fit_side(t, s);
}

void hide_dock(Dock *dock)
{
  if (!dock)
    return;
  if (gtk_widget_has_focus(dock->card))
    focus_current_tab(dock->t);
  gtk_widget_set_visible(dock->card, FALSE);
  hide_side_if_empty(dock->t, dock->side);
  fit_side(dock->t, dock->side);
}

static void show_side(App *t, int side, gboolean visible)
{
  GtkWidget *w = side_widget(t, side);

  if (!w)
    return;
  if (!side_has_pages(t, side)) {
    if (side != 2) {
      const char *fallback = (side == 0) ? "files" : "shell";
      if (visible)
        show_sidebar_page(t, fallback);
      else
        hide_dock(panel_dock(t, fallback));
    }
    return;
  }
  if (!visible && gtk_widget_has_focus(w))
    focus_current_tab(t);
  gtk_widget_set_visible(w, visible);
  if (visible) {
    GPtrArray *docks = side_docks(t, side);
    for (guint i = 0; i < docks->len; i++)
      gtk_widget_set_visible(((Dock *)g_ptr_array_index(docks, i))->card, TRUE);
    g_ptr_array_unref(docks);
    make_room(t, side);
    fit_side(t, side);
  }
}

void set_pane_visible(App *t, GtkWidget *pane, gboolean visible)
{
  if (pane == t->shell.panel) {
    if (visible)
      show_sidebar_page(t, "shell");
    else
      hide_dock(panel_dock(t, "shell"));
    return;
  }
  for (int side = 0; side < 3; side++)
    if (pane == side_widget(t, side)) {
      show_side(t, side, visible);
      return;
    }
  if (!visible && gtk_widget_has_focus(pane))
    focus_current_tab(t);
  gtk_widget_set_visible(pane, visible);
}

void show_sidebar_page(App *t, const char *page)
{
  Dock *dock = panel_dock(t, page);

  if (!dock)
    return;
  gtk_widget_set_visible(dock->card, TRUE);
  if (dock->collapsed)
    set_dock_collapsed(dock, FALSE);
  gtk_widget_set_visible(side_widget(t, dock->side), TRUE);
  make_room(t, dock->side);
  fit_side(t, dock->side);
  gtk_stack_set_visible_child_name(dock->stack, page);
  focus_panel(t, page);
}

gboolean panel_shown(App *t, const char *page)
{
  Dock *dock = panel_dock(t, page);

  return dock && gtk_widget_get_visible(dock->card) && !dock->collapsed &&
         gtk_widget_get_visible(side_widget(t, dock->side)) && !side_squeezed(t, dock->side) &&
         g_strcmp0(gtk_stack_get_visible_child_name(dock->stack), page) == 0;
}

void toggle_pane(App *t, GtkWidget *pane)
{
  if (pane == t->shell.panel || (pane == t->layout.left && !side_has_pages(t, 0))) {
    const char *name = (pane == t->shell.panel) ? "shell" : "files";
    Dock *dock = panel_dock(t, name);
    if (!dock)
      return;
    if (panel_shown(t, name))
      hide_dock(dock);
    else
      show_sidebar_page(t, name);
    return;
  }
  int side = pane == t->layout.left ? 0 : pane == t->layout.bottom ? 1 : pane == t->layout.right ? 2 : -1;
  /* a side squeezed to nothing looks hidden: it is shown (with room) rather than hidden */
  gboolean show = !gtk_widget_get_visible(pane) || (side >= 0 && side_squeezed(t, side));

  set_pane_visible(t, pane, show);
  if (show)
    gtk_widget_grab_focus(pane);
}

void toggle_panel(App *t, const char *page)
{
  if (panel_shown(t, page))
    hide_dock(panel_dock(t, page));
  else
    show_sidebar_page(t, page);
  queue_panel_checks(t);
}

/* The Panels menu checks the panels that show (their win.panel-<name> actions' state). */
static gboolean sync_panel_checks(gpointer data)
{
  App *t = data;

  t->dock.checks_id = 0;
  for (int i = 0; i < PANELS && !t->closing; i++) {
    char *name = g_strconcat("panel-", panel_names[i], NULL);
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(t->window), name);

    if (action)
      g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(panel_shown(t, panel_names[i])));
    g_free(name);
  }
  if (!t->closing)
    update_edges(t);
  return G_SOURCE_REMOVE;
}

/* Something about the panels changed: the menu's checks follow in an idle, once for many changes. */
static void queue_panel_checks(App *t)
{
  if (!t->dock.checks_id)
    t->dock.checks_id = g_idle_add(sync_panel_checks, t);
}

void on_layout_changed(GObject *object G_GNUC_UNUSED, GParamSpec *spec G_GNUC_UNUSED, App *t)
{
  queue_panel_checks(t);
}

void update_scm_badge(GtkStackPage *sp, GParamSpec *pspec G_GNUC_UNUSED, App *t)
{
  Dock *dock = panel_dock(t, "scm");

  if (!dock || !sp)
    return;
  GtkSelectionModel *pages = gtk_stack_get_pages(dock->stack);
  GtkWidget *btn = gtk_widget_get_first_child(dock->switcher);
  const char *title = gtk_stack_page_get_title(sp);
  const char *count =
      (title && g_str_has_prefix(title, "SOURCE CONTROL  ")) ? title + strlen("SOURCE CONTROL  ") : NULL;

  for (guint i = 0; btn; btn = gtk_widget_get_next_sibling(btn), i++) {
    GtkStackPage *item = g_list_model_get_item(G_LIST_MODEL(pages), i);
    gboolean match = (item == sp);
    if (item)
      g_object_unref(item);
    if (match) {
      if (count && *count) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *lbl = gtk_label_new(count);
        gtk_widget_add_css_class(lbl, "tcode-scm-badge");
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("tcode-source-control-symbolic"));
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_button_set_child(GTK_BUTTON(btn), box);
      } else {
        gtk_button_set_icon_name(GTK_BUTTON(btn), "tcode-source-control-symbolic");
      }
      break;
    }
  }
  g_object_unref(pages);
}

/* Moving panels ---------------------------------------------------------------------------------- */

void add_dock_page(Dock *dock, GtkWidget *child, const char *name, const char *title, const char *icon)
{
  GtkStackPage *page = gtk_stack_add_titled(dock->stack, child, name, title);

  gtk_stack_page_set_icon_name(page, icon);
  if (strcmp(name, "scm") == 0) {
    dock->t->dock.scm_stack = dock->stack;
    g_signal_connect_after(page, "notify::title", G_CALLBACK(update_scm_badge), dock->t);
  }
}

/* Moves a page from one stack to (the end of) another, with its title and icon. */
static void move_stack_page(Dock *from, Dock *to, const char *page)
{
  GtkWidget *child = gtk_stack_get_child_by_name(from->stack, page);
  GtkStackPage *sp = gtk_stack_get_page(from->stack, child);
  char *title = g_strdup(gtk_stack_page_get_title(sp)), *icon = g_strdup(gtk_stack_page_get_icon_name(sp));

  g_object_ref(child);
  gtk_stack_remove(from->stack, child);
  add_dock_page(to, child, page, title, icon);
  g_object_unref(child);
  g_free(title);
  g_free(icon);
}

/* The names of a dock's pages, in order (a new array of copies). */
static GPtrArray *page_names(Dock *dock)
{
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  GtkSelectionModel *pages = gtk_stack_get_pages(dock->stack);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(pages));

  for (guint i = 0; i < n; i++) {
    GtkStackPage *item = g_list_model_get_item(G_LIST_MODEL(pages), i);
    if (item && gtk_stack_page_get_name(item))
      g_ptr_array_add(names, g_strdup(gtk_stack_page_get_name(item)));
    g_clear_object(&item);
  }
  g_object_unref(pages);
  return names;
}

static gboolean prune_later(gpointer data)
{
  App *t = data;

  g_object_set_data(G_OBJECT(t->window), "t-code-prune", NULL);
  if (!t->closing)
    prune_docks(t);
  return G_SOURCE_REMOVE;
}

/* Docks left without panels go once the change is over (the drag that moved the last one may still
 * hold its icon). */
static void queue_prune(App *t)
{
  if (!g_object_get_data(G_OBJECT(t->window), "t-code-prune")) {
    g_object_set_data(G_OBJECT(t->window), "t-code-prune", GINT_TO_POINTER(1));
    g_idle_add(prune_later, t);
  }
}

void prune_docks(App *t)
{
  GPtrArray *empty = g_ptr_array_new();

  for (guint i = 0; i < t->dock.list->len; i++)
    if (!dock_has_pages(g_ptr_array_index(t->dock.list, i)))
      g_ptr_array_add(empty, g_ptr_array_index(t->dock.list, i));
  for (guint i = 0; i < empty->len; i++) {
    Dock *dock = g_ptr_array_index(empty, i);
    int side = dock->side;
    split_remove(dock->card); /* the card goes: its dock leaves t->dock.list */
    hide_side_if_empty(t, side);
    fit_side(t, side);
  }
  g_ptr_array_unref(empty);
}

/* After a panel came in or left: both docks show, the sides follow, the panel is the current one. */
static void panel_moved(App *t, const char *page, Dock *from, Dock *to)
{
  if (t->dock.scm_stack && t->scm && gtk_stack_get_child_by_name(t->dock.scm_stack, "scm"))
    update_scm_badge(gtk_stack_get_page(t->dock.scm_stack, t->scm), NULL, t);
  gtk_widget_set_visible(to->card, TRUE);
  if (to->collapsed)
    set_dock_collapsed(to, FALSE);
  if (from && !dock_has_pages(from)) {
    gtk_widget_set_visible(from->card, FALSE);
    queue_prune(t);
  }
  if (from)
    hide_side_if_empty(t, from->side);
  gtk_widget_set_visible(side_widget(t, to->side), TRUE);
  make_room(t, to->side);
  if (from)
    fit_side(t, from->side);
  fit_side(t, to->side);
  gtk_stack_set_visible_child_name(to->stack, page);
  queue_panel_checks(t);
}

void move_panel_at(App *t, const char *page, Dock *to, int insert_pos)
{
  Dock *from = panel_dock(t, page);
  GPtrArray *names;
  int at = 0;

  if (!from || !to)
    return;
  names = page_names(to);
  for (guint i = 0; i < names->len; i++) /* where it is among the others, if it is in `to` already */
    if (strcmp(g_ptr_array_index(names, i), page) == 0) {
      g_ptr_array_remove_index(names, i);
      if ((int)i == CLAMP(insert_pos, 0, (int)names->len)) {
        g_ptr_array_unref(names);
        return; /* already there */
      }
      break;
    }
  move_stack_page(from, to, page);
  for (guint i = 0; i < names->len; i++) /* the pages after its place go after it again */
    if (at++ >= insert_pos)
      move_stack_page(to, to, g_ptr_array_index(names, i));
  g_ptr_array_unref(names);
  panel_moved(t, page, from, to);
}

/* Where a new dock goes on a side: under the last one (on the left and right), right of it (at the
 * bottom). */
static SplitSide along(int side)
{
  return side == 1 ? SPLIT_RIGHT : SPLIT_DOWN;
}

void move_panel_beside(App *t, const char *page, Dock *beside, SplitSide where)
{
  Dock *from = panel_dock(t, page), *dock;

  if (!from || !beside ||
      (from == beside &&
       gtk_widget_get_first_child(GTK_WIDGET(from->stack)) == gtk_widget_get_last_child(GTK_WIDGET(from->stack))))
    return; /* its dock's only panel: nothing to split off */
  dock = dock_new(t, beside->side);
  split_beside(side_root(t, beside->side), beside->card, dock->card, where, 0);
  move_panel_at(t, page, dock, 0);
}

/* A new dock at the end of a side (its first one when it has none). */
static Dock *append_dock(App *t, int side)
{
  GPtrArray *docks = side_docks(t, side);
  int n = (int)docks->len;
  Dock *dock = dock_new(t, side);

  g_ptr_array_unref(docks);
  split_beside(side_root(t, side), split_root_child(side_root(t, side)), dock->card, along(side),
               n ? 1.0 / (n + 1) : 0);
  return dock;
}

void move_panel_edge(App *t, const char *page, int side)
{
  Dock *from = panel_dock(t, page);

  if (!from)
    return;
  if (from->side == side && !split_parent(from->card) && !dock_has_pages(from))
    return;
  move_panel_at(t, page, append_dock(t, side), 0);
}

void move_panel_to_side(App *t, const char *page, int side)
{
  Dock *to = side_first_dock(t, side);

  if (!to)
    to = append_dock(t, side);
  move_panel_at(t, page, to, G_MAXINT);
}

void add_panel(App *t, const char *name, const char *title, const char *icon, GtkWidget *child, int side)
{
  Dock *dock;

  if (panel_dock(t, name))
    return;
  side = CLAMP(side, 0, 2);
  dock = side_first_dock(t, side);
  if (!dock)
    dock = append_dock(t, side);
  add_dock_page(dock, child, name, title, icon);
  panel_id(name); /* it can be dragged */
  panel_moved(t, name, NULL, dock);
}

void remove_panel(App *t, const char *name)
{
  Dock *dock = panel_dock(t, name);

  if (!dock)
    return;
  gtk_stack_remove(dock->stack, gtk_stack_get_child_by_name(dock->stack, name));
  if (!dock_has_pages(dock)) {
    gtk_widget_set_visible(dock->card, FALSE);
    hide_side_if_empty(t, dock->side);
    queue_prune(t);
  }
  queue_panel_checks(t);
}

void reset_panel_layout(App *t)
{
  int h = gtk_widget_get_height(GTK_WIDGET(t->layout.vpaned)), w = gtk_widget_get_width(GTK_WIDGET(t->layout.rpaned));

  for (int i = 0; i < PANELS; i++) /* in panel_names' order: their tabs too */
    move_panel_to_side(t, panel_names[i], panel_default_sides[i]);
  for (int side = 0; side < 3; side++) { /* every other panel (a plugin's) into its side's first dock */
    GPtrArray *docks = side_docks(t, side);
    for (guint i = 1; i < docks->len; i++) {
      GPtrArray *names = page_names(g_ptr_array_index(docks, i));
      for (guint j = 0; j < names->len; j++)
        move_panel_to_side(t, g_ptr_array_index(names, j), side);
      g_ptr_array_unref(names);
    }
    g_ptr_array_unref(docks);
  }
  prune_docks(t);
  for (guint i = 0; i < t->dock.list->len; i++) {
    Dock *dock = g_ptr_array_index(t->dock.list, i);
    dock->saved_pos = 0;
    gtk_widget_set_visible(dock->card, TRUE);
    if (dock->collapsed)
      set_dock_collapsed(dock, FALSE);
  }
  update_docks(t);
  gtk_paned_set_position(t->layout.hpaned, 280);
  gtk_paned_set_position(t->layout.vpaned, h > 400 ? h - 240 : 620);
  gtk_paned_set_position(t->layout.rpaned, w > 600 ? w - 280 : 840);
  show_side(t, 0, TRUE);
  show_side(t, 1, TRUE);
  show_side(t, 2, TRUE);
  gtk_stack_set_visible_child_name(panel_stack(t, "outline"), "outline");
  gtk_stack_set_visible_child_name(panel_stack(t, "shell"), "shell");
  show_sidebar_page(t, "files");
  queue_panel_checks(t);
}

/* The session ------------------------------------------------------------------------------------ */

/* session.ini's dock-layout for this format (the twelve numbered docks wrote 12). */
#define TREE_LAYOUT 100

/* A panel the session keeps: t-code's own (a plugin's view comes back when the plugin opens it). */
static gboolean kept_panel(const char *name)
{
  for (int i = 0; i < PANELS; i++)
    if (strcmp(name, panel_names[i]) == 0)
      return TRUE;
  return FALSE;
}

void docks_save(App *t, GKeyFile *kf)
{
  char **keys = g_key_file_get_keys(kf, "window", NULL, NULL);

  for (int i = 0; keys && keys[i]; i++) /* the docks as they were last time go */
    if (g_str_has_prefix(keys[i], "dock-") || g_str_has_prefix(keys[i], "docks-") ||
        strcmp(keys[i], "panel-order") == 0 || strcmp(keys[i], "sidebar-split") == 0 ||
        strcmp(keys[i], "right-split") == 0)
      g_key_file_remove_key(kf, "window", keys[i], NULL);
  g_strfreev(keys);
  g_key_file_set_integer(kf, "window", "dock-layout", TREE_LAYOUT);
  for (int side = 0; side < 3; side++) {
    char *layout = split_write(side_root(t, side), is_dock, 'd'), key[64];
    GPtrArray *docks = side_docks(t, side);
    g_snprintf(key, sizeof(key), "docks-%s", side_names[side]);
    g_key_file_set_string(kf, "window", key, layout ? layout : "");
    for (guint i = 0; i < docks->len; i++) {
      Dock *dock = g_ptr_array_index(docks, i);
      GPtrArray *names = page_names(dock);
      const char *current = gtk_stack_get_visible_child_name(dock->stack);
      for (guint j = names->len; j-- > 0;)
        if (!kept_panel(g_ptr_array_index(names, j)))
          g_ptr_array_remove_index(names, j);
      g_snprintf(key, sizeof(key), "dock-%s-%u", side_names[side], i);
      g_key_file_set_string_list(kf, "window", key, (const char *const *)names->pdata, names->len);
      g_snprintf(key, sizeof(key), "dock-%s-%u-page", side_names[side], i);
      if (current && kept_panel(current))
        g_key_file_set_string(kf, "window", key, current);
      g_snprintf(key, sizeof(key), "dock-%s-%u-collapsed", side_names[side], i);
      g_key_file_set_boolean(kf, "window", key, dock->collapsed);
      g_snprintf(key, sizeof(key), "dock-%s-%u-hidden", side_names[side], i);
      g_key_file_set_boolean(kf, "window", key, !gtk_widget_get_visible(dock->card));
      g_ptr_array_unref(names);
    }
    g_ptr_array_unref(docks);
    g_free(layout);
  }
}

static GtkWidget *new_dock_card(gpointer data)
{
  Dock **next = data; /* the side's App and side, as a template */
  Dock *dock = dock_new((*next)->t, (*next)->side);

  return dock->card;
}

/* What one side should hold: its tree, and each dock's panels in order. */
typedef struct {
  char *layout;     /* NULL: the side as it is */
  GPtrArray *docks; /* of GStrv */
} SideLayout;

/* The older session.ini: twelve numbered docks (0..3 left, 4..7 bottom, 8..11 right) in a chain on
 * each side, each panel's number in dock-<name>, the tabs' order in panel-order; before that, fewer
 * numbers. */
static void read_numbered(GKeyFile *kf, int version, SideLayout sides[3])
{
  char **order = g_key_file_get_string_list(kf, "window", "panel-order", NULL, NULL);
  GPtrArray *slots[12];

  for (int d = 0; d < 12; d++)
    slots[d] = g_ptr_array_new();
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; pass == 0 ? (order && order[i]) : i < PANELS; i++) {
      const char *name = pass == 0 ? order[i] : panel_names[i];
      char key[32];
      gboolean seen = FALSE;
      for (int d = 0; d < 12 && !seen; d++)
        for (guint j = 0; j < slots[d]->len && !seen; j++)
          seen = strcmp(g_ptr_array_index(slots[d], j), name) == 0;
      g_snprintf(key, sizeof(key), "dock-%s", name);
      if (seen || !kept_panel(name) || !g_key_file_has_key(kf, "window", key, NULL))
        continue;
      int d = g_key_file_get_integer(kf, "window", key, NULL);
      if (version != 12) /* before the twelve docks */
        d = d == 2 ? 4 : d == 3 ? 8 : d == 4 ? 9 : d;
      if (d >= 0 && d < 12)
        g_ptr_array_add(slots[d], (gpointer)name);
    }
  for (int side = 0; side < 3; side++) {
    GString *layout = g_string_new(NULL);
    int used = 0;
    for (int i = 0; i < 4; i++)
      used += slots[side * 4 + i]->len > 0;
    for (int i = 0, n = 0; i < 4; i++) {
      GPtrArray *slot = slots[side * 4 + i];
      if (!slot->len)
        continue;
      if (n < used - 1) /* a chain: each split's first side one dock, the rest after it */
        g_string_append_printf(layout, "%c(%d,d,", side == 1 ? 'h' : 'v', 1000 / (used - n));
      else
        g_string_append_c(layout, 'd');
      n++;
      g_ptr_array_add(slot, NULL);
      g_ptr_array_add(sides[side].docks, g_strdupv((char **)slot->pdata));
    }
    for (int i = 1; i < used; i++)
      g_string_append_c(layout, ')');
    sides[side].layout = used ? g_string_free(layout, FALSE) : (g_string_free(layout, TRUE), NULL);
  }
  for (int d = 0; d < 12; d++)
    g_ptr_array_unref(slots[d]);
  g_strfreev(order);
}

void docks_restore(App *t, GKeyFile *kf)
{
  int version = g_key_file_has_key(kf, "window", "dock-layout", NULL)
                    ? g_key_file_get_integer(kf, "window", "dock-layout", NULL)
                    : 0;
  SideLayout sides[3];
  GtkWidget *roots[3];

  for (int side = 0; side < 3; side++)
    sides[side] = (SideLayout){ NULL, g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev) };
  if (version == TREE_LAYOUT) {
    for (int side = 0; side < 3; side++) {
      char key[64];
      g_snprintf(key, sizeof(key), "docks-%s", side_names[side]);
      sides[side].layout = g_key_file_get_string(kf, "window", key, NULL);
      for (guint i = 0;; i++) {
        g_snprintf(key, sizeof(key), "dock-%s-%u", side_names[side], i);
        if (!g_key_file_has_key(kf, "window", key, NULL))
          break;
        g_ptr_array_add(sides[side].docks, g_key_file_get_string_list(kf, "window", key, NULL, NULL));
      }
    }
  } else {
    read_numbered(kf, version, sides);
  }
  /* Each side's new tree is built off the window, the panels moved into it, and then it takes the old
   * tree's place (whatever is left in the old one goes to its side's first new dock). */
  for (int side = 0; side < 3; side++) {
    Dock template = { .t = t, .side = side }, *next = &template;
    GPtrArray *docks;
    roots[side] = NULL;
    if (!sides[side].layout)
      continue;
    roots[side] = g_object_ref_sink(split_root_new());
    if (*sides[side].layout && !split_read(roots[side], sides[side].layout, 'd', new_dock_card, &next)) {
      g_clear_object(&roots[side]);
      continue;
    }
    GPtrArray *cards = g_ptr_array_new();
    split_collect(roots[side], is_dock, cards);
    docks = sides[side].docks;
    for (guint i = 0; i < cards->len && i < docks->len; i++) {
      Dock *dock = dock_of(g_ptr_array_index(cards, i));
      char **names = g_ptr_array_index(docks, i);
      for (int j = 0; names && names[j]; j++) {
        Dock *from = panel_dock(t, names[j]);
        if (from && from != dock)
          move_stack_page(from, dock, names[j]);
      }
    }
    g_ptr_array_unref(cards);
  }
  for (int side = 0; side < 3; side++) {
    if (!roots[side])
      continue;
    GtkWidget *root = side_root(t, side), *old = split_root_child(root), *tree = split_root_child(roots[side]);
    GPtrArray *left = side_docks(t, side);
    for (guint i = 0; i < left->len; i++) { /* panels the session did not place stay on their side */
      GPtrArray *names = page_names(g_ptr_array_index(left, i));
      Dock *first = NULL;
      if (tree) {
        GPtrArray *cards = g_ptr_array_new();
        split_collect(tree, is_dock, cards);
        first = dock_of(g_ptr_array_index(cards, 0));
        g_ptr_array_unref(cards);
      }
      for (guint j = 0; j < names->len; j++) {
        if (!first) { /* a side saved empty: a new dock for them */
          first = dock_new(t, side);
          split_beside(roots[side], NULL, first->card, along(side), 0);
          tree = first->card;
        }
        move_stack_page(g_ptr_array_index(left, i), first, g_ptr_array_index(names, j));
      }
      g_ptr_array_unref(names);
    }
    g_ptr_array_unref(left);
    if (old) { /* its docks are empty now: they leave the list here, not when the last ref goes (a queued prune
                * would find a card outside the window) */
      GPtrArray *cards = g_ptr_array_new();
      split_collect(old, is_dock, cards);
      for (guint i = 0; i < cards->len; i++)
        g_ptr_array_remove(t->dock.list, dock_of(g_ptr_array_index(cards, i)));
      g_ptr_array_unref(cards);
      gtk_box_remove(GTK_BOX(root), old);
    }
    if (tree) {
      g_object_ref(tree);
      gtk_box_remove(GTK_BOX(roots[side]), tree);
      gtk_box_append(GTK_BOX(root), tree);
      g_object_unref(tree);
    }
    g_object_unref(roots[side]);
  }
  for (int side = 0; side < 3; side++) { /* each dock's current tab, collapsed or hidden */
    GPtrArray *docks = side_docks(t, side);
    for (guint i = 0; version == TREE_LAYOUT && i < docks->len; i++) {
      Dock *dock = g_ptr_array_index(docks, i);
      char key[64], *page;
      g_snprintf(key, sizeof(key), "dock-%s-%u-page", side_names[side], i);
      page = g_key_file_get_string(kf, "window", key, NULL);
      if (page && gtk_stack_get_child_by_name(dock->stack, page))
        gtk_stack_set_visible_child_name(dock->stack, page);
      g_free(page);
      g_snprintf(key, sizeof(key), "dock-%s-%u-collapsed", side_names[side], i);
      if (g_key_file_get_boolean(kf, "window", key, NULL))
        set_dock_collapsed(dock, TRUE);
      g_snprintf(key, sizeof(key), "dock-%s-%u-hidden", side_names[side], i);
      gtk_widget_set_visible(dock->card, !g_key_file_get_boolean(kf, "window", key, NULL));
    }
    g_ptr_array_unref(docks);
    g_free(sides[side].layout);
    g_ptr_array_unref(sides[side].docks);
  }
  prune_docks(t);
  if (t->dock.scm_stack && t->scm && gtk_stack_get_child_by_name(t->dock.scm_stack, "scm"))
    update_scm_badge(gtk_stack_get_page(t->dock.scm_stack, t->scm), NULL, t);
  update_docks(t);
}
