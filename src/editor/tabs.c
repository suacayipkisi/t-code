/*
 * The tabs and the two tab groups (see tabs.h). What an editor tab does is in editor.c; its page
 * is built in editorview.c.
 */
#include "editor/tabs.h"

#include "editor/gutter/diffgutter.h"
#include "editor/editor.h"
#include "editor/editorview.h"
#include "editor/preview/preview.h"
#include "editor/tabmenu.h"
#include "files/files.h"
#include "files/opentabs.h"
#include "git/compare.h"
#include "popups/contextmenu.h"
#include "window/statusbar.h"
#include "lsp/problems.h"
#include "lsp/symbols/outline.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <string.h>

/* The page went (its tab closed, or t-code quits): frees everything the tab holds. */
static void tab_free(Tab *tab)
{
  if (tab->preview)
    preview_free(tab->preview);
  editor_view_free(tab->view);
  g_clear_object(&tab->item);
  g_free(tab->compare.with);
  g_free(tab->compare.title);
  g_free(tab->path);
  g_free(tab);
#ifdef __GLIBC__
  malloc_trim(0); /* glibc keeps what the tab freed: give it back to the system */
#endif
}

/* add_tab() hangs the Tab on its page. */
Tab *tab_of(GtkWidget *page)
{
  return g_object_get_data(G_OBJECT(page), "t-code-tab");
}

/* The tabs in a group; 0 for the second window's while there is none. */
static int pages(App *t, int group)
{
  return t->editors.groups[group] ? gtk_notebook_get_n_pages(t->editors.groups[group]) : 0;
}

int tab_count(App *t)
{
  int n = 0;

  for (int g = 0; g < GROUPS; g++)
    n += pages(t, g);
  return n;
}

Tab *tab_nth(App *t, int index)
{
  for (int g = 0; g < GROUPS; index -= pages(t, g), g++)
    if (index < pages(t, g))
      return tab_of(gtk_notebook_get_nth_page(t->editors.groups[g], index));
  return NULL;
}

/* The tab's notebook page: its own, or the comparison holding both sides. */
static GtkWidget *frame_of(Tab *tab)
{
  return tab->frame ? tab->frame : tab->page;
}

int tab_group(Tab *tab)
{
  for (int g = 1; g < GROUPS; g++)
    if (tab->app->editors.groups[g] && gtk_notebook_page_num(tab->app->editors.groups[g], frame_of(tab)) >= 0)
      return g;
  return 0;
}

GtkWindow *tab_window(Tab *tab)
{
  return tab_group(tab) == WINDOW_GROUP ? tab->app->editors.second : tab->app->window;
}

/* The group's current tab, or NULL. */
static Tab *group_current(App *t, int g)
{
  int current = t->editors.groups[g] ? gtk_notebook_get_current_page(t->editors.groups[g]) : -1;

  return current >= 0 ? tab_of(gtk_notebook_get_nth_page(t->editors.groups[g], current)) : NULL;
}

Tab *current_tab(App *t)
{
  Tab *tab = group_current(t, t->editors.active);

  for (int g = 0; g < GROUPS && !tab; g++)
    tab = group_current(t, g);
  return tab;
}

Tab *find_tab(App *t, TabKind kind, const char *path)
{
  int n = tab_count(t);

  for (int i = 0; i < n; i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == kind && g_strcmp0(tab->path, path) == 0)
      return tab;
  }
  return NULL;
}

gboolean any_unsaved(App *t)
{
  int n = tab_count(t);

  for (int i = 0; i < n; i++)
    if (tab_nth(t, i)->modified)
      return TRUE;
  return FALSE;
}

/* The widget that takes the keyboard in a tab: an editor's text, not the box around it. */
static GtkWidget *focus_widget(Tab *tab)
{
  return tab->source ? GTK_WIDGET(tab->source) : tab->page;
}

void focus_current_tab(App *t)
{
  Tab *tab = current_tab(t);

  if (tab)
    gtk_widget_grab_focus(focus_widget(tab));
  else
    files_focus(t);
}

void focus_group(App *t, int group)
{
  if (group == 1 && !gtk_widget_get_visible(GTK_WIDGET(t->editors.groups[1])))
    return;
  if (group == WINDOW_GROUP && !t->editors.second)
    return;
  t->editors.active = group;
  focus_current_tab(t);
}

void select_tab(Tab *tab)
{
  App *t = tab->app;
  int group = tab_group(tab);

  gtk_notebook_set_current_page(t->editors.groups[group],
                                gtk_notebook_page_num(t->editors.groups[group], frame_of(tab)));
  t->editors.active = group;
  if (!gtk_window_is_active(tab_window(tab)))
    gtk_window_present(tab_window(tab));
  gtk_widget_grab_focus(focus_widget(tab));
}

void step_tab(App *t, int step)
{
  Tab *tab = current_tab(t);

  if (!tab)
    return;
  GtkNotebook *group = t->editors.groups[tab_group(tab)];
  int n = gtk_notebook_get_n_pages(group);
  gtk_notebook_set_current_page(group, (gtk_notebook_get_current_page(group) + step + n) % n);
  focus_current_tab(t);
}

/* A comparison is taken apart before its sides go: neither may reach the other once freed. */
static void unpair(Tab *tab)
{
  if (!tab->compare.old_side)
    return;
  unsync_scroll(tab);
  diff_gutter_pair(tab, NULL);
}

void remove_tab(Tab *tab)
{
  App *t = tab->app;
  GtkNotebook *group = t->editors.groups[tab_group(tab)];

  unpair(tab);
  gtk_notebook_remove_page(group, gtk_notebook_page_num(group, frame_of(tab))); /* both sides of a comparison */
  problems_update(t);
  focus_current_tab(t);
}

/* Editors ask about unsaved changes first. */
static void close_tab(Tab *tab)
{
  if (tab->kind != TAB_EDITOR) {
    remove_tab(tab);
    return;
  }
  select_tab(tab);
  quit_editor(tab);
}

void close_all_tabs(App *t)
{
  gboolean closing = t->closing;

  t->closing = TRUE; /* quiet while the tabs go */
  for (int g = 0; g < GROUPS; g++)
    while (pages(t, g) > 0) {
      unpair(tab_of(gtk_notebook_get_nth_page(t->editors.groups[g], 0)));
      gtk_notebook_remove_page(t->editors.groups[g], 0);
    }
  t->closing = closing;
  open_tabs_changed(t);              /* it was quiet meanwhile */
  if (!closing && t->editors.second) /* empty now */
    gtk_window_close(t->editors.second);
}

/* Labels ------------------------------------------------------------------------------------- */

void update_tab_label(Tab *tab)
{
  char *name;

  if (!tab->label) /* a comparison's right side: the left one's label is the page's */
    return;
  name = tab->compare.title ? g_strdup(tab->compare.title) : g_path_get_basename(tab->path);
  if (tab->compare.old_side) { /* "editor.c ↔ a86a89b" */
    const char *other = tab->compare.old_side->compare.title ? tab->compare.old_side->compare.title : "";
    const char *at = strstr(other, " @ ");
    char *both = g_strdup_printf("%s ↔ %s", name, at ? at + 3 : other);
    g_free(name);
    name = both;
  }
  char *text = tab->kind == TAB_PREVIEW ? g_strdup_printf("Preview %s", name)
                                        : g_strdup_printf("%s%s", tab->modified ? "● " : "", name);

  gtk_label_set_text(tab->label, text);
  if (tab->temporary)
    gtk_widget_add_css_class(GTK_WIDGET(tab->label), "temporary"); /* in italics */
  else
    gtk_widget_remove_css_class(GTK_WIDGET(tab->label), "temporary");
  gtk_widget_set_tooltip_text(gtk_widget_get_parent(GTK_WIDGET(tab->label)), tab->path);
  open_tabs_changed(tab->app); /* "●", italics */
  g_free(text);
  g_free(name);
}

/* A middle click on a tab's label closes it, like in a browser. */
static void on_tab_middle_click(GtkGestureClick *gesture G_GNUC_UNUSED, int n_press G_GNUC_UNUSED,
                                double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, Tab *tab)
{
  close_tab(tab);
}

/* A click on a tab shows it; a double click on a temporary one keeps it. */
static void on_tab_click(GtkGestureClick *gesture G_GNUC_UNUSED, int n_press, double x G_GNUC_UNUSED,
                         double y G_GNUC_UNUSED, Tab *tab)
{
  if (n_press == 1)
    select_tab(tab);
  else if (n_press == 2)
    keep_tab(tab);
}

/* A tab is dragged: what is carried is the Tab itself (drops look it up among the open tabs). */
static GdkContentProvider *on_tab_drag(GtkDragSource *source, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, Tab *tab)
{
  GdkPaintable *icon = gtk_widget_paintable_new(tab->item);

  gtk_drag_source_set_icon(source, icon, 0, 0);
  g_object_unref(icon);
  return gdk_content_provider_new_typed(G_TYPE_POINTER, tab);
}

/* A right click on a tab: its menu (tabmenu.c). */
static void on_tab_menu(GtkWidget *widget, double x, double y, gpointer tab)
{
  tab_menu(tab, widget, x, y);
}

/* A tab in the strip: its name (update_tab_label fills it) and a close button. */
static GtkWidget *tab_label(Tab *tab)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
  GtkGesture *middle = gtk_gesture_click_new(), *click = gtk_gesture_click_new();
  GtkDragSource *drag = gtk_drag_source_new();

  gtk_widget_add_css_class(box, "tcode-tab");
  tab->label = GTK_LABEL(gtk_label_new(NULL));
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->label));
  gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
  gtk_widget_set_focusable(close, FALSE);
  gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(close_tab), tab);
  gtk_box_append(GTK_BOX(box), close);

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), GDK_BUTTON_MIDDLE);
  g_signal_connect(middle, "pressed", G_CALLBACK(on_tab_middle_click), tab);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(middle));
  g_signal_connect(click, "pressed", G_CALLBACK(on_tab_click), tab);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
  g_signal_connect(drag, "prepare", G_CALLBACK(on_tab_drag), tab);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag));
  context_menu_attach(box, on_tab_menu, tab);
  return box;
}

/* Adding and moving -------------------------------------------------------------------------- */

/* Puts a page into a group at position (-1: the end) and shows it; its tab goes into the group's
 * strip (sync_strip, from page-added). */
static void insert_page(GtkNotebook *group, GtkWidget *page, int position)
{
  gtk_notebook_set_current_page(group, gtk_notebook_insert_page(group, page, NULL, position));
}

/* Adds page as a tab of the active group, right after its current tab, and switches to it. */
Tab *add_tab(App *t, TabKind kind, GtkWidget *page, const char *path)
{
  GtkNotebook *group = t->editors.groups[t->editors.active];
  Tab *tab = g_new0(Tab, 1);

  tab->app = t;
  tab->kind = kind;
  tab->page = page;
  tab->path = g_strdup(path);
  g_object_set_data_full(G_OBJECT(page), "t-code-tab", tab, (GDestroyNotify)tab_free);
  tab->item = g_object_ref_sink(tab_label(tab)); /* the tab's: it moves between strips */
  update_tab_label(tab);
  insert_page(group, page, gtk_notebook_get_current_page(group) + 1);
  return tab;
}

Tab *temporary_tab(App *t, int group)
{
  for (int i = 0; i < pages(t, group); i++) {
    Tab *tab = tab_of(gtk_notebook_get_nth_page(t->editors.groups[group], i));
    if (tab->temporary)
      return tab;
  }
  return NULL;
}

void keep_tab(Tab *tab)
{
  if (!tab->temporary)
    return;
  tab->temporary = FALSE;
  update_tab_label(tab);
}

int tab_position(Tab *tab)
{
  return gtk_notebook_page_num(tab->app->editors.groups[tab_group(tab)], frame_of(tab));
}

void set_tab_position(Tab *tab, int position)
{
  gtk_notebook_reorder_child(tab->app->editors.groups[tab_group(tab)], frame_of(tab), position);
}

/* The two sides share the width until the user moves the handle. */
static void on_max_position(GtkPaned *paned, GParamSpec *pspec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  int max = 0;

  g_object_get(paned, "max-position", &max, NULL);
  if (max <= 0)
    return;
  gtk_paned_set_position(paned, max / 2);
  g_signal_handlers_disconnect_by_func(paned, on_max_position, NULL);
}

void join_tabs(Tab *left, Tab *right)
{
  App *t = left->app;
  GtkNotebook *group = t->editors.groups[tab_group(left)], *right_group = t->editors.groups[tab_group(right)];
  int position = tab_position(left);
  GtkPaned *paned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));

  g_object_ref(left->page);
  g_object_ref(right->page);
  gtk_notebook_remove_page(right_group, gtk_notebook_page_num(right_group, right->page));
  right->label = NULL; /* went with its notebook page */
  gtk_notebook_remove_page(group, gtk_notebook_page_num(group, left->page));
  gtk_paned_set_start_child(paned, left->page);
  gtk_paned_set_end_child(paned, right->page);
  gtk_paned_set_shrink_start_child(paned, FALSE);
  gtk_paned_set_shrink_end_child(paned, FALSE);
  g_signal_connect(paned, "notify::max-position", G_CALLBACK(on_max_position), NULL);
  g_object_set_data(G_OBJECT(paned), "t-code-tab", left); /* tab_of(): the left side */
  left->frame = GTK_WIDGET(paned);
  left->compare.old_side = right;
  insert_page(group, left->frame, position);
  update_tab_label(left);
  g_object_unref(right->page);
  g_object_unref(left->page);
}

void part_tabs(Tab *left)
{
  App *t = left->app;
  GtkNotebook *group;
  int position;

  if (!left->compare.old_side)
    return;
  group = t->editors.groups[tab_group(left)];
  position = tab_position(left);
  unpair(left);
  g_object_ref(left->page);
  gtk_paned_set_start_child(GTK_PANED(left->frame), NULL);
  gtk_notebook_remove_page(group, gtk_notebook_page_num(group, left->frame)); /* the right side goes with it */
  left->frame = NULL;
  left->compare.old_side = NULL;
  insert_page(group, left->page, position);
  update_tab_label(left);
  g_object_unref(left->page);
}

/* Moves a tab (with its editor) to the end of another group. */
void move_tab(Tab *tab, int to)
{
  App *t = tab->app;
  int from = tab_group(tab);
  GtkWidget *frame = frame_of(tab);

  if (from == to)
    return;
  g_object_ref(frame);
  gtk_notebook_remove_page(t->editors.groups[from], gtk_notebook_page_num(t->editors.groups[from], frame));
  insert_page(t->editors.groups[to], frame, -1);
  g_object_unref(frame);
}

/* Groups ------------------------------------------------------------------------------------- */

void update_editor_area(App *t)
{
  gboolean split = gtk_widget_get_visible(GTK_WIDGET(t->editors.groups[1]));

  gtk_stack_set_visible_child_name(t->editors.area, pages(t, 0) > 0 || split ? "tabs" : "empty");
}

void set_split(App *t, gboolean split)
{
  GtkWidget *side = GTK_WIDGET(t->editors.groups[1]);

  if (!split) {
    while (gtk_notebook_get_n_pages(t->editors.groups[1]) > 0)
      move_tab(tab_of(gtk_notebook_get_nth_page(t->editors.groups[1], 0)), 0);
    t->editors.active = 0;
  }
  if (split != gtk_widget_get_visible(side)) {
    gtk_widget_set_visible(side, split);
    int width = gtk_widget_get_width(GTK_WIDGET(t->editors.split));
    if (split && width > 0)
      gtk_paned_set_position(t->editors.split, width / 2);
    if (split)
      t->editors.active = 1;
  }
  if (t->editors.split_button && gtk_toggle_button_get_active(t->editors.split_button) != split)
    gtk_toggle_button_set_active(t->editors.split_button, split);
  update_editor_area(t);
}

/* The second group goes away when its last tab closes. */
static gboolean unsplit_if_empty(gpointer data)
{
  App *t = data;

  if (!t->closing && gtk_widget_get_visible(GTK_WIDGET(t->editors.groups[1])) &&
      gtk_notebook_get_n_pages(t->editors.groups[1]) == 0)
    set_split(t, FALSE);
  return G_SOURCE_REMOVE;
}

/* The second window goes when its last tab closes (or moves away). */
static gboolean close_window_if_empty(gpointer data)
{
  App *t = data;

  if (!t->closing && t->editors.second && pages(t, WINDOW_GROUP) == 0)
    gtk_window_close(t->editors.second);
  return G_SOURCE_REMOVE;
}

/* Which group a notebook is. */
static int group_index(App *t, GtkWidget *notebook)
{
  for (int g = 1; g < GROUPS; g++)
    if (notebook == GTK_WIDGET(t->editors.groups[g]))
      return g;
  return 0;
}

/* A tab was added to or removed from a group. */
static void on_tabs_changed(GtkNotebook *group, GtkWidget *child G_GNUC_UNUSED, guint index G_GNUC_UNUSED, App *t)
{
  if (t->closing)
    return;
  update_editor_area(t);
  update_status(t);
  outline_show_tab(t);
  if (group == t->editors.groups[1] && gtk_notebook_get_n_pages(group) == 0)
    g_idle_add(unsplit_if_empty, t);
  if (group == t->editors.groups[WINDOW_GROUP] && gtk_notebook_get_n_pages(group) == 0)
    g_idle_add(close_window_if_empty, t);
}

/* Another tab became the current one of a group: that group is the active one now. */
static void on_tab_switched(GtkNotebook *group, GtkWidget *page G_GNUC_UNUSED, guint index G_GNUC_UNUSED, App *t)
{
  if (t->closing) /* the groups give up their pages as the window goes: the panels may be gone */
    return;
  t->editors.active = group_index(t, GTK_WIDGET(group));
  update_status(t);
  outline_show_tab(t);
}

/* A click into a group makes it the active one (new tabs go there). */
static void on_group_focus(GtkEventControllerFocus *focus, App *t)
{
  t->editors.active = group_index(t, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(focus)));
  update_status(t);
}

/* The tab strip ------------------------------------------------------------------------------ */

/* GtkNotebook's own tabs scroll with arrows only: each group shows its notebook's pages in a strip of
 * its own instead, a box in a scrolled window (a thin scrollbar, the wheel scrolls it sideways). The
 * notebook keeps the pages and their order; the strip follows it. */

static GtkWidget *strip_of(GtkNotebook *group)
{
  return g_object_get_data(G_OBJECT(group), "t-code-strip");
}

/* Scrolls the strip to show the current tab (once the strip is laid out). */
static gboolean reveal_current(gpointer data)
{
  GtkNotebook *group = data;
  GtkWidget *strip = strip_of(group);
  int current = gtk_notebook_get_current_page(group);
  Tab *tab = current >= 0 ? tab_of(gtk_notebook_get_nth_page(group, current)) : NULL;
  graphene_rect_t bounds;

  g_object_set_data(G_OBJECT(group), "t-code-reveal", NULL);
  if (strip && tab && gtk_widget_compute_bounds(tab->item, strip, &bounds)) {
    GtkWidget *scrolled = gtk_widget_get_ancestor(strip, GTK_TYPE_SCROLLED_WINDOW);
    GtkAdjustment *sideways = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scrolled));
    double value = gtk_adjustment_get_value(sideways), page = gtk_adjustment_get_page_size(sideways);
    if (bounds.origin.x < value)
      gtk_adjustment_set_value(sideways, bounds.origin.x);
    else if (bounds.origin.x + bounds.size.width > value + page)
      gtk_adjustment_set_value(sideways, bounds.origin.x + bounds.size.width - page);
  }
  g_object_unref(group);
  return G_SOURCE_REMOVE;
}

/* Makes the strip hold the notebook's tabs in its order, the current one checked. */
static void sync_strip(GtkNotebook *group)
{
  GtkWidget *strip = strip_of(group), *previous = NULL;
  int n = gtk_notebook_get_n_pages(group), current = gtk_notebook_get_current_page(group);
  GHashTable *items = g_hash_table_new(NULL, NULL);

  if (!strip)
    return;
  for (int i = 0; i < n; i++)
    g_hash_table_add(items, tab_of(gtk_notebook_get_nth_page(group, i))->item);
  for (GtkWidget *w = gtk_widget_get_first_child(strip), *next; w; w = next) {
    next = gtk_widget_get_next_sibling(w);
    if (!g_hash_table_contains(items, w)) /* closed, or moved to another group */
      gtk_box_remove(GTK_BOX(strip), w);
  }
  for (int i = 0; i < n; i++) {
    GtkWidget *item = tab_of(gtk_notebook_get_nth_page(group, i))->item;
    if (gtk_widget_get_parent(item) != strip) {
      if (gtk_widget_get_parent(item)) /* still in the group it comes from */
        gtk_box_remove(GTK_BOX(gtk_widget_get_parent(item)), item);
      gtk_box_append(GTK_BOX(strip), item);
    }
    gtk_box_reorder_child_after(GTK_BOX(strip), item, previous);
    if (i == current)
      gtk_widget_set_state_flags(item, GTK_STATE_FLAG_CHECKED, FALSE);
    else
      gtk_widget_unset_state_flags(item, GTK_STATE_FLAG_CHECKED);
    previous = item;
  }
  g_hash_table_destroy(items);
  open_tabs_changed(g_object_get_data(G_OBJECT(group), "t-code-app"));
  if (!g_object_get_data(G_OBJECT(group), "t-code-reveal")) {
    g_object_set_data(G_OBJECT(group), "t-code-reveal", GINT_TO_POINTER(1));
    g_idle_add(reveal_current, g_object_ref(group));
  }
}

static void on_strip_laid_out(GtkNotebook *group)
{
  if (!g_object_get_data(G_OBJECT(group), "t-code-reveal")) {
    g_object_set_data(G_OBJECT(group), "t-code-reveal", GINT_TO_POINTER(1));
    g_idle_add(reveal_current, g_object_ref(group));
  }
}

static void on_pages_changed(GtkNotebook *group)
{
  sync_strip(group);
}

/* The current page changes once the signal is done: the strip follows after it. */
static void on_page_switched(GtkNotebook *group)
{
  sync_strip(group);
}

/* The wheel scrolls the strip sideways (it has no vertical scrolling for the wheel to do). */
static gboolean on_strip_wheel(GtkEventControllerScroll *wheel, double dx, double dy, gpointer data G_GNUC_UNUSED)
{
  GtkWidget *scrolled = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(wheel));
  GtkAdjustment *sideways = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scrolled));
  double step = gtk_event_controller_scroll_get_unit(wheel) == GDK_SCROLL_UNIT_WHEEL ? 40 : 1;

  gtk_adjustment_set_value(sideways, gtk_adjustment_get_value(sideways) + (dy + dx) * step);
  return TRUE;
}

/* A tab dropped on a strip: it goes to that group, where it was dropped among the tabs. */
static gboolean on_tab_drop(GtkDropTarget *target, const GValue *value, double x, double y G_GNUC_UNUSED,
                            GtkNotebook *group)
{
  App *t = g_object_get_data(G_OBJECT(group), "t-code-app");
  GtkWidget *scrolled = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)), *strip = strip_of(group);
  Tab *tab = g_value_get_pointer(value), *found = NULL;
  int to = 0, position = 0, n = tab_count(t);
  graphene_point_t at;

  for (int i = 0; i < n && !found; i++) /* only a tab that is still open */
    if (tab_nth(t, i) == tab)
      found = tab;
  for (int g = 0; g < GROUPS; g++)
    if (t->editors.groups[g] == group)
      to = g;
  if (!found || !gtk_widget_compute_point(scrolled, strip, &GRAPHENE_POINT_INIT(x, 0), &at))
    return FALSE;
  /* Before the first tab whose middle is right of the drop. */
  for (GtkWidget *w = gtk_widget_get_first_child(strip); w; w = gtk_widget_get_next_sibling(w), position++) {
    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds(w, strip, &bounds) && at.x < bounds.origin.x + bounds.size.width / 2)
      break;
  }
  if (tab_group(tab) != to) {
    move_tab(tab, to);
  } else if (tab_position(tab) < position) {
    position--; /* it leaves its place first */
  }
  set_tab_position(tab, position);
  select_tab(tab);
  return TRUE;
}

/* The strip goes before its notebook when a window closes: the notebook's last page-removed signals
 * must not reach it. */
static void forget_strip(GtkNotebook *group)
{
  g_object_set_data(G_OBJECT(group), "t-code-strip", NULL);
}

GtkWidget *tab_group_widget(GtkNotebook *group)
{
  return g_object_get_data(G_OBJECT(group), "t-code-group");
}

/* A tab group's notebook (its pages), under its own tab strip; tab_group_widget() holds both, shown
 * and hidden with the notebook. Tabs can be dragged between the groups' strips. */
GtkNotebook *new_tab_group(App *t)
{
  GtkNotebook *group = GTK_NOTEBOOK(gtk_notebook_new());
  GtkEventController *focus = gtk_event_controller_focus_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *scrolled = gtk_scrolled_window_new();
  GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_POINTER, GDK_ACTION_MOVE);

  gtk_notebook_set_show_tabs(group, FALSE);
  gtk_notebook_set_show_border(group, FALSE);
  gtk_widget_set_hexpand(GTK_WIDGET(group), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(group), TRUE);
  g_object_set_data(G_OBJECT(group), "t-code-app", t);
  g_object_set_data(G_OBJECT(group), "t-code-strip", strip);
  g_signal_connect_object(strip, "destroy", G_CALLBACK(forget_strip), group, G_CONNECT_SWAPPED);
  g_object_set_data(G_OBJECT(group), "t-code-group", box);

  gtk_widget_add_css_class(scrolled, "tcode-tabs");
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), strip);
  /* Laid out (tabs added, the window resized): the current tab stays in view. */
  g_signal_connect_object(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scrolled)), "changed",
                          G_CALLBACK(on_strip_laid_out), group, G_CONNECT_SWAPPED);
  gtk_event_controller_set_propagation_phase(wheel, GTK_PHASE_CAPTURE);
  g_signal_connect(wheel, "scroll", G_CALLBACK(on_strip_wheel), NULL);
  gtk_widget_add_controller(scrolled, wheel);
  g_signal_connect(drop, "drop", G_CALLBACK(on_tab_drop), group);
  gtk_widget_add_controller(scrolled, GTK_EVENT_CONTROLLER(drop));
  gtk_box_append(GTK_BOX(box), scrolled);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(group));
  gtk_widget_set_hexpand(box, TRUE);
  g_object_bind_property(group, "visible", box, "visible", G_BINDING_SYNC_CREATE);

  g_signal_connect(group, "page-added", G_CALLBACK(on_pages_changed), NULL);
  g_signal_connect(group, "page-removed", G_CALLBACK(on_pages_changed), NULL);
  g_signal_connect(group, "page-reordered", G_CALLBACK(on_pages_changed), NULL);
  g_signal_connect_after(group, "switch-page", G_CALLBACK(on_page_switched), NULL);
  g_signal_connect(group, "page-added", G_CALLBACK(on_tabs_changed), t);
  g_signal_connect(group, "page-removed", G_CALLBACK(on_tabs_changed), t);
  g_signal_connect(group, "switch-page", G_CALLBACK(on_tab_switched), t);
  g_signal_connect(focus, "enter", G_CALLBACK(on_group_focus), t);
  gtk_widget_add_controller(GTK_WIDGET(group), focus);
  return group;
}

/* The editor area: a hint while nothing is open, otherwise the two groups. */
void create_groups(App *t)
{
  GtkWidget *hint = gtk_label_new("Open a file from the file manager on the left, or press Ctrl+P");

  for (int g = 0; g < WINDOW_GROUP; g++) /* the second window's comes with it */
    t->editors.groups[g] = new_tab_group(t);
  t->editors.split = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_paned_set_start_child(t->editors.split, tab_group_widget(t->editors.groups[0]));
  gtk_paned_set_end_child(t->editors.split, tab_group_widget(t->editors.groups[1]));
  gtk_widget_set_visible(GTK_WIDGET(t->editors.groups[1]), FALSE);

  t->editors.area = GTK_STACK(gtk_stack_new());
  gtk_widget_add_css_class(hint, "dim-label");
  gtk_stack_add_named(t->editors.area, hint, "empty");
  gtk_stack_add_named(t->editors.area, GTK_WIDGET(t->editors.split), "tabs");
  gtk_stack_set_visible_child_name(t->editors.area, "empty");
}
