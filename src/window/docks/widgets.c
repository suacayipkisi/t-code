/*
 * The docks' widgets (see docks.h): a tab bar per dock (its panels' icons, collapse and hide
 * buttons), dragging a panel's icon to another dock or the editor area's edges, and the icons'
 * right-click menu.
 */
#include "window/docks/docks.h"

#include "debug/debugger.h"
#include "debug/debugview.h"
#include "editor/editor.h"
#include "files/files.h"
#include "git/scm/scm.h"
#include "lsp/hierarchy.h"
#include "lsp/outline.h"
#include "lsp/problems.h"
#include "search/search.h"
#include "window/palette.h"
#include "window/theme.h"
#include "window/window.h"

#include <string.h>

/* Holds child without asking for its minimum width or height so panes can shrink freely. */
static GtkWidget *clip_box(GtkWidget *child, gboolean clip_height)
{
  GtkWidget *scrolled = gtk_scrolled_window_new();

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_EXTERNAL,
                                 clip_height ? GTK_POLICY_EXTERNAL : GTK_POLICY_NEVER);
  gtk_widget_set_hexpand(scrolled, TRUE);
  if (clip_height)
    gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), child);
  return scrolled;
}

/* The page name of the button under (x, y) in a stack switcher. */
static const char *page_at(GtkWidget *switcher, GtkStack *stack, double x, double y)
{
  GtkSelectionModel *pages = gtk_stack_get_pages(stack);
  GtkWidget *btn = gtk_widget_get_first_child(switcher);
  const char *found = NULL;

  for (guint i = 0; btn && !found; btn = gtk_widget_get_next_sibling(btn), i++) {
    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds(btn, switcher, &bounds) && x >= bounds.origin.x &&
        x <= bounds.origin.x + bounds.size.width && y >= bounds.origin.y && y <= bounds.origin.y + bounds.size.height) {
      GtkStackPage *page = g_list_model_get_item(G_LIST_MODEL(pages), i);
      if (page) {
        found = gtk_stack_page_get_name(page);
        g_object_unref(page);
      }
    }
  }
  g_object_unref(pages);
  return found ? found : gtk_stack_get_visible_child_name(stack);
}

/* The 0-based tab index of `page` inside its dock stack, or -1. */
static int panel_tab_index(App *t, const char *page)
{
  int d = panel_dock(t, page);
  if (d < 0)
    return -1;
  GtkSelectionModel *pages = gtk_stack_get_pages(t->dock.stacks[d]);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(pages));
  int found = -1;
  for (guint i = 0; i < n && found < 0; i++) {
    GtkStackPage *item = g_list_model_get_item(G_LIST_MODEL(pages), i);
    if (item && g_strcmp0(gtk_stack_page_get_name(item), page) == 0)
      found = (int)i;
    if (item)
      g_object_unref(item);
  }
  g_object_unref(pages);
  return found;
}

/* Computes the horizontal tab insertion index when dropping at `x` on dock `to`'s header bar. */
static int header_drop_pos(App *t, int to, const char *page, double x)
{
  GtkWidget *switcher = g_object_get_data(G_OBJECT(t->dock.stacks[to]), "tcode-switcher");
  if (!switcher)
    return -1;
  int orig_idx = (panel_dock(t, page) == to) ? panel_tab_index(t, page) : -1;
  GtkWidget *btn = gtk_widget_get_first_child(switcher);
  int pos = 0, rem_idx = 0;

  for (int i = 0; btn; btn = gtk_widget_get_next_sibling(btn), i++) {
    if (i == orig_idx)
      continue;
    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds(btn, t->dock.cards[to], &bounds)) {
      double left = bounds.origin.x, right = left + bounds.size.width, mid = left + bounds.size.width / 2.0;
      if (orig_idx >= 0 && x >= left && x <= right) {
        if (orig_idx < i)
          pos = rem_idx + 1;
      } else if (x > mid) {
        pos = rem_idx + 1;
      }
    }
    rem_idx++;
  }
  return pos;
}

/* The dock icon menu: what it does to menu_target_page, the panel it was opened on. */
typedef enum {
  DOCK_TAB_LEFT,
  DOCK_TAB_RIGHT,
  DOCK_TO_LEFT,
  DOCK_STACK_LEFT,
  DOCK_TO_BOTTOM,
  DOCK_SPLIT_BOTTOM,
  DOCK_TO_RIGHT,
  DOCK_STACK_RIGHT,
  DOCK_COLLAPSE,
  DOCK_HIDE
} DockAction;

static const char *const dock_action_labels[] = {
  [DOCK_TAB_LEFT] = "Move Tab Left",           [DOCK_TAB_RIGHT] = "Move Tab Right",
  [DOCK_TO_LEFT] = "Move to Left (Tab)",       [DOCK_STACK_LEFT] = "Stack on Left (Above/Below)",
  [DOCK_TO_BOTTOM] = "Move to Bottom (Tab)",   [DOCK_SPLIT_BOTTOM] = "Split on Bottom (Side by Side)",
  [DOCK_TO_RIGHT] = "Move to Right (Tab)",     [DOCK_STACK_RIGHT] = "Stack on Right (Above/Below)",
  [DOCK_COLLAPSE] = "Collapse / Expand Panel", [DOCK_HIDE] = "Hide Panel",
};

static char *menu_target_page;
static App *menu_app;

/* Runs in an idle: the popover is gone first. */
static gboolean run_dock_action(gpointer data)
{
  DockAction action = GPOINTER_TO_INT(data);
  App *t = menu_app;
  const char *page = menu_target_page;
  int cur = page ? panel_dock(t, page) : -1, idx = page ? panel_tab_index(t, page) : -1;

  if (cur < 0)
    return G_SOURCE_REMOVE;
  switch (action) {
    case DOCK_TAB_LEFT:
      if (idx > 0)
        move_panel_at(t, page, cur, idx - 1);
      break;
    case DOCK_TAB_RIGHT:
      if (idx >= 0)
        move_panel_at(t, page, cur, idx + 1);
      break;
    case DOCK_TO_LEFT: move_panel(t, page, first_tab_dock(t, 0)); break;
    case DOCK_STACK_LEFT: move_panel(t, page, new_split_dock(t, 0, page)); break;
    case DOCK_TO_BOTTOM: move_panel(t, page, first_tab_dock(t, 1)); break;
    case DOCK_SPLIT_BOTTOM: move_panel(t, page, new_split_dock(t, 1, page)); break;
    case DOCK_TO_RIGHT: move_panel(t, page, first_tab_dock(t, 2)); break;
    case DOCK_STACK_RIGHT: move_panel(t, page, new_split_dock(t, 2, page)); break;
    case DOCK_COLLAPSE: set_dock_collapsed(t, cur, !t->dock.collapsed[cur]); break;
    case DOCK_HIDE: hide_dock(t, cur); break;
  }
  return G_SOURCE_REMOVE;
}

static void on_dock_action_clicked(GtkButton *btn, gpointer action)
{
  GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);

  if (pop)
    gtk_popover_popdown(GTK_POPOVER(pop));
  g_idle_add(run_dock_action, action);
}

/* Right-click on a dock tab icon: menu to reorder/move/split/stack/collapse/hide that panel. */
static void on_switcher_right_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x, double y, App *t)
{
  GtkWidget *switcher = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  GtkStack *stack = GTK_STACK(gtk_stack_switcher_get_stack(GTK_STACK_SWITCHER(switcher)));
  GtkWidget *header = gtk_widget_get_ancestor(switcher, GTK_TYPE_BOX);
  const char *page = page_at(switcher, stack, x, y);
  graphene_point_t pt = { (float)x, (float)y };
  GtkPopover *pop;

  if (!page || !header)
    return;
  g_set_str(&menu_target_page, page);
  menu_app = t;
  pop = g_object_get_data(G_OBJECT(header), "tcode-pop");
  if (!pop) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    pop = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_has_arrow(pop, FALSE);
    for (DockAction i = 0; i < G_N_ELEMENTS(dock_action_labels); i++) {
      GtkWidget *btn = gtk_button_new_with_label(dock_action_labels[i]);
      gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
      gtk_widget_set_halign(btn, GTK_ALIGN_FILL);
      g_signal_connect(btn, "clicked", G_CALLBACK(on_dock_action_clicked), GINT_TO_POINTER(i));
      gtk_box_append(GTK_BOX(box), btn);
    }
    gtk_popover_set_child(pop, box);
    gtk_widget_set_parent(GTK_WIDGET(pop), header);
    g_object_set_data(G_OBJECT(header), "tcode-pop", pop);
  }
  if (!gtk_widget_compute_point(switcher, header, &pt, &pt))
    return;
  gtk_popover_set_pointing_to(pop, &(const GdkRectangle){ (int)pt.x, (int)pt.y, 1, 1 });
  gtk_popover_popup(pop);
}

static GdkContentProvider *on_drag_prepare(GtkDragSource *source, double x, double y, GtkStack *stack)
{
  GtkWidget *switcher = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
  int id = panel_id(page_at(switcher, stack, x, y));

  if (!id)
    return NULL;
  return gdk_content_provider_new_typed(G_TYPE_INT, id);
}

static gboolean on_dock_drop(GtkDropTarget *target, const GValue *value, double x, double y, App *t)
{
  GtkWidget *box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
  const char *page = panel_from_id(g_value_get_int(value));
  int to = 0, rel = 0;

  if (!page)
    return FALSE;
  for (int d = 0; d < DOCKS; d++)
    if (t->dock.cards[d] == box)
      to = d;
  /* Dropping on the header bar (y < 36) inserts or reorders the tab left/right among header icons. */
  if (y < 36) {
    move_panel_at(t, page, to, header_drop_pos(t, to, page, x));
    return TRUE;
  }
  int w = MAX(gtk_widget_get_width(box), 1), h = MAX(gtk_widget_get_height(box), 1);
  int side = to / 4;

  /* Dropping near top/bottom (on Left/Right) or left/right (on Bottom) creates a split; center tabs. */
  if (side == 1) {
    if (x < w * 0.28)
      rel = -1;
    else if (x > w * 0.72)
      rel = 1;
  } else {
    if (y < h * 0.30)
      rel = -1;
    else if (y > h * 0.70)
      rel = 1;
  }
  move_panel_relative(t, page, to, rel);
  return TRUE;
}

/* A file dropped on the editor area opens; a panel dropped near its left, right or bottom edge moves or
 * splits there. */
static gboolean on_editor_drop(GtkDropTarget *target, const GValue *value, double x, double y, App *t)
{
  GtkWidget *area = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
  int w = MAX(gtk_widget_get_width(area), 1), h = MAX(gtk_widget_get_height(area), 1);
  const char *page;
  int side, to;

  if (G_VALUE_HOLDS(value, G_TYPE_FILE)) { /* a file: opened */
    char *path = g_file_get_path(g_value_get_object(value));
    gboolean opened = path && g_file_test(path, G_FILE_TEST_IS_REGULAR);
    if (opened)
      open_file(t, path);
    g_free(path);
    return opened;
  }
  page = panel_from_id(g_value_get_int(value));
  if (!page)
    return FALSE;
  if (x > w * 0.70)
    side = 2;
  else if (x < w * 0.30)
    side = 0;
  else
    side = 1;
  if (side == 1)
    to = (x > w * 0.5 && side_has_pages(t, 1)) ? new_split_dock(t, 1, page) : first_tab_dock(t, 1);
  else
    to = (y > h * 0.5 && side_has_pages(t, side)) ? new_split_dock(t, side, page) : first_tab_dock(t, side);
  move_panel(t, page, to);
  return TRUE;
}

static void on_close_dock_clicked(GtkButton *btn, App *t)
{
  int d = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "tcode-dock"));

  hide_dock(t, d);
}

static void on_collapse_dock_clicked(GtkButton *btn, App *t)
{
  int d = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "tcode-dock"));

  set_dock_collapsed(t, d, !t->dock.collapsed[d]);
}

static void on_stack_switched(GtkStack *stack, GParamSpec *pspec G_GNUC_UNUSED, App *t)
{
  for (int d = 0; d < DOCKS; d++)
    if (t->dock.stacks[d] == stack && t->dock.collapsed[d])
      set_dock_collapsed(t, d, FALSE);
}

static GtkWidget *new_dock(App *t, int index)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *collapse_btn = gtk_button_new_from_icon_name("pan-down-symbolic");
  GtkWidget *switcher = gtk_stack_switcher_new();
  GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
  GtkStack *stack = GTK_STACK(gtk_stack_new());
  GtkGesture *right = gtk_gesture_click_new();
  GtkDragSource *drag = gtk_drag_source_new();
  GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE);

  gtk_widget_set_vexpand(GTK_WIDGET(stack), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(stack), TRUE);
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), stack);
  gtk_widget_set_focusable(switcher, FALSE);
  gtk_widget_set_halign(switcher, GTK_ALIGN_START);
  g_signal_connect(stack, "notify::visible-child", G_CALLBACK(on_stack_switched), t);

  gtk_button_set_has_frame(GTK_BUTTON(collapse_btn), FALSE);
  gtk_widget_set_focusable(collapse_btn, FALSE);
  gtk_widget_set_tooltip_text(collapse_btn, "Expand / Collapse Panel");
  gtk_widget_add_css_class(collapse_btn, "tcode-dock-close");
  g_object_set_data(G_OBJECT(collapse_btn), "tcode-dock", GINT_TO_POINTER(index));
  g_signal_connect(collapse_btn, "clicked", G_CALLBACK(on_collapse_dock_clicked), t);

  gtk_button_set_has_frame(GTK_BUTTON(close_btn), FALSE);
  gtk_widget_set_focusable(close_btn, FALSE);
  gtk_widget_set_tooltip_text(close_btn, "Hide Panel");
  gtk_widget_add_css_class(close_btn, "tcode-dock-close");
  g_object_set_data(G_OBJECT(close_btn), "tcode-dock", GINT_TO_POINTER(index));
  g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_dock_clicked), t);

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), GDK_BUTTON_SECONDARY);
  g_signal_connect(right, "pressed", G_CALLBACK(on_switcher_right_click), t);
  gtk_widget_add_controller(switcher, GTK_EVENT_CONTROLLER(right));

  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
  gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
  g_signal_connect(drag, "prepare", G_CALLBACK(on_drag_prepare), stack);
  gtk_widget_add_controller(switcher, GTK_EVENT_CONTROLLER(drag));

  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drop), GTK_PHASE_CAPTURE);
  g_signal_connect(drop, "drop", G_CALLBACK(on_dock_drop), t);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drop));

  gtk_widget_add_css_class(header, "tcode-dock-header");
  gtk_box_append(GTK_BOX(header), collapse_btn);
  gtk_box_append(GTK_BOX(header), clip_box(switcher, FALSE));
  gtk_box_append(GTK_BOX(header), close_btn);

  gtk_widget_add_css_class(box, "tcode-sidebar");
  make_card(box);
  gtk_box_append(GTK_BOX(box), header);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(stack));
  t->dock.cards[index] = box;
  t->dock.stacks[index] = stack;
  g_object_set_data(G_OBJECT(box), "tcode-collapse-btn", collapse_btn);
  g_object_set_data(G_OBJECT(stack), "tcode-switcher", switcher);
  return box;
}

static void add_dock_panel(App *t, int dock, GtkWidget *child, const char *name, const char *title)
{
  GtkStackPage *page = gtk_stack_add_titled(t->dock.stacks[dock], child, name, title);

  gtk_stack_page_set_icon_name(page, panel_icon(name));
  if (strcmp(name, "scm") == 0)
    g_signal_connect_after(page, "notify::title", G_CALLBACK(update_scm_badge), t);
}

/* Builds a chain of 3 paneds holding the 4 docks of one side (vertical for Left/Right, horizontal for Bottom). */
static GtkWidget *build_side_chain(App *t, int side, GtkOrientation orient, int default_pos)
{
  int base = side * 4;

  for (int i = 2; i >= 0; i--) {
    GtkPaned *p = GTK_PANED(gtk_paned_new(orient));
    gtk_paned_set_start_child(p, t->dock.cards[base + i]);
    gtk_paned_set_end_child(p, i == 2 ? t->dock.cards[base + 3] : GTK_WIDGET(t->dock.paneds[side][i + 1]));
    gtk_paned_set_shrink_start_child(p, TRUE);
    gtk_paned_set_shrink_end_child(p, TRUE);
    gtk_paned_set_position(p, default_pos);
    t->dock.paneds[side][i] = p;
  }
  return GTK_WIDGET(t->dock.paneds[side][0]);
}

/* Creates the 12 docks (4 Left vertical, 4 Bottom horizontal, 4 Right vertical) and puts the
 * four panels in their default slots. */
void docks_create(App *t)
{
  static const char *const css =
      ".tcode-dock-header { border-bottom: 1px solid var(--tc-border); padding: 0 2px; }"
      ".tcode-sidebar stackswitcher { padding: 4px 2px; border-spacing: 4px; }"
      ".tcode-sidebar stackswitcher button, .tcode-dock-close { min-height: 24px; min-width: 24px;"
      "  padding: 2px 5px; border-radius: 7px; background: none; box-shadow: none; border: none; color: var(--tc-fg-dim); }"
      ".tcode-sidebar stackswitcher button:checked { color: var(--tc-fg-bright); background: color-mix(in srgb, var(--tc-fg) 10%, transparent); }"
      ".tcode-sidebar stackswitcher button:hover, .tcode-dock-close:hover {"
      "  color: var(--tc-fg-bright); background: color-mix(in srgb, var(--tc-fg) 5%, transparent); }"
      ".tcode-scm-badge { font-size: 0.78em; font-weight: bold; }";
  GtkDropTarget *ed_drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_MOVE | GDK_ACTION_COPY);
  GType dropped[] = { G_TYPE_INT, G_TYPE_FILE }; /* a panel's icon, a file from the Files panel */

  palette_add_css(css);

  for (int d = 0; d < DOCKS; d++)
    new_dock(t, d);

  t->scm = clip_box(scm_panel(t), TRUE);
  t->dock.scm_stack = t->dock.stacks[0];
  gtk_widget_add_css_class(GTK_WIDGET(t->shell.term), "tcode-shell");
  vte_terminal_set_clear_background(t->shell.term, FALSE);

  add_dock_panel(t, 0, clip_box(files_panel(t), TRUE), "files", "Files (Alt+1)");
  add_dock_panel(t, 0, t->scm, "scm", "SOURCE CONTROL");
  add_dock_panel(t, 0, clip_box(search_panel(t), TRUE), "search", "Search (Ctrl+Shift+F)");
  add_dock_panel(t, 4, GTK_WIDGET(t->shell.term), "shell", "Terminal (Alt+3 / Ctrl+J)");
  add_dock_panel(t, 4, debug_panel(t), "debug", "Debug (F5)");
  add_dock_panel(t, 0, clip_box(debug_view(t), TRUE), "run", "Run and Debug (Ctrl+Shift+D)");
  t->problems = problems_panel(t);
  add_dock_panel(t, 4, clip_box(t->problems, TRUE), "problems", "Problems (Ctrl+Shift+M)");
  add_dock_panel(t, 0, clip_box(outline_panel(t), TRUE), "outline", "Outline");
  add_dock_panel(t, 0, clip_box(hierarchy_panel(t), TRUE), "hierarchy", "Hierarchy (Shift+Alt+H)");
  t->layout.left = build_side_chain(t, 0, GTK_ORIENTATION_VERTICAL, 320);
  t->layout.bottom = build_side_chain(t, 1, GTK_ORIENTATION_HORIZONTAL, 480);
  t->layout.right = build_side_chain(t, 2, GTK_ORIENTATION_VERTICAL, 320);
  update_docks(t);
  /* the Panels menu's checks follow whatever shows or hides a panel */
  for (int d = 0; d < DOCKS; d++) {
    g_signal_connect(t->dock.cards[d], "notify::visible", G_CALLBACK(on_layout_changed), t);
    g_signal_connect(t->dock.stacks[d], "notify::visible-child", G_CALLBACK(on_layout_changed), t);
  }
  for (int side = 0; side < 3; side++)
    g_signal_connect(side_widget(t, side), "notify::visible", G_CALLBACK(on_layout_changed), t);

  gtk_drop_target_set_gtypes(ed_drop, dropped, G_N_ELEMENTS(dropped));
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(ed_drop), GTK_PHASE_CAPTURE);
  g_signal_connect(ed_drop, "drop", G_CALLBACK(on_editor_drop), t);
  gtk_widget_add_controller(GTK_WIDGET(t->editors.area), GTK_EVENT_CONTROLLER(ed_drop));
}
