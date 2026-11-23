/*
 * The docks' widgets (see docks.h): a tab bar per dock (its panels' icons, collapse and hide
 * buttons) with its right-click menu, and the drop markers a panel icon dragged over the window
 * shows: over a dock the cross of editor/dropzone.h (the middle square: into that dock; the others:
 * a new dock on that side of it), over its tab bar a line where among the tabs it lands, over the
 * editor area squares along its left, bottom and right edges (a new dock at the end of that side).
 */
#include "window/docks/docks.h"

#include "debug/debugger.h"
#include "debug/view/debugview.h"
#include "editor/dropzone.h"
#include "editor/editor.h"
#include "files/files.h"
#include "git/scm/scm.h"
#include "lsp/symbols/hierarchy.h"
#include "plugins/manager.h"
#include "testing/testing.h"
#include "lsp/symbols/outline.h"
#include "lsp/problems.h"
#include "search/search.h"
#include "window/palette.h"
#include "window/theme.h"
#include "window/window.h"

#include <string.h>

/* The panels' names, for dragging them by number (panel_id: index + 1). */
static GPtrArray *panel_ids;

int panel_id(const char *page)
{
  if (!page)
    return 0;
  if (!panel_ids)
    panel_ids = g_ptr_array_new();
  for (guint i = 0; i < panel_ids->len; i++)
    if (strcmp(g_ptr_array_index(panel_ids, i), page) == 0)
      return (int)i + 1;
  g_ptr_array_add(panel_ids, g_strdup(page)); /* kept for the run: an id stays the same */
  return (int)panel_ids->len;
}

const char *panel_from_id(int id)
{
  return panel_ids && id >= 1 && id <= (int)panel_ids->len ? g_ptr_array_index(panel_ids, id - 1) : NULL;
}

const char *panel_icon(const char *page)
{
  static const char *const icons[][2] = {
    { "files", "tcode-files-symbolic" },     { "scm", "tcode-source-control-symbolic" },
    { "search", "tcode-search-symbolic" },   { "debug", "tcode-debug-symbolic" },
    { "run", "tcode-run-symbolic" },         { "problems", "tcode-warning-symbolic" },
    { "outline", "tcode-outline-symbolic" }, { "hierarchy", "tcode-hierarchy-symbolic" },
    { "testing", "tcode-testing-symbolic" }, { "plugins", "tcode-plugins-symbolic" },
  };

  for (size_t i = 0; i < G_N_ELEMENTS(icons); i++)
    if (strcmp(page, icons[i][0]) == 0)
      return icons[i][1];
  return "tcode-terminal-symbolic";
}

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
  Dock *dock = panel_dock(t, page);
  int found = -1;

  if (!dock)
    return -1;
  GtkSelectionModel *pages = gtk_stack_get_pages(dock->stack);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(pages));
  for (guint i = 0; i < n && found < 0; i++) {
    GtkStackPage *item = g_list_model_get_item(G_LIST_MODEL(pages), i);
    if (item && g_strcmp0(gtk_stack_page_get_name(item), page) == 0)
      found = (int)i;
    g_clear_object(&item);
  }
  g_object_unref(pages);
  return found;
}

/* Where among a dock's tabs a panel dropped at x (in the switcher's coordinates) goes, its own place
 * left out; line_x: the gap it goes into. */
static int header_drop_pos(Dock *to, const char *page, double x, double *line_x)
{
  int orig_idx = panel_dock(to->t, page) == to ? panel_tab_index(to->t, page) : -1;
  int pos = 0, i = 0;
  double line = 0;

  for (GtkWidget *btn = gtk_widget_get_first_child(to->switcher); btn; btn = gtk_widget_get_next_sibling(btn), i++) {
    graphene_rect_t bounds;
    if (i == orig_idx || !gtk_widget_compute_bounds(btn, to->switcher, &bounds))
      continue; /* its own icon */
    if (x > bounds.origin.x + bounds.size.width / 2) {
      pos++;
      line = bounds.origin.x + bounds.size.width;
    } else {
      line = bounds.origin.x;
      break;
    }
  }
  if (line_x)
    *line_x = line;
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
  Dock *cur = page ? panel_dock(t, page) : NULL;
  int idx = page ? panel_tab_index(t, page) : -1;

  if (!cur)
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
    case DOCK_TO_LEFT: move_panel_to_side(t, page, 0); break;
    case DOCK_STACK_LEFT: move_panel_edge(t, page, 0); break;
    case DOCK_TO_BOTTOM: move_panel_to_side(t, page, 1); break;
    case DOCK_SPLIT_BOTTOM: move_panel_edge(t, page, 1); break;
    case DOCK_TO_RIGHT: move_panel_to_side(t, page, 2); break;
    case DOCK_STACK_RIGHT: move_panel_edge(t, page, 2); break;
    case DOCK_COLLAPSE: set_dock_collapsed(cur, !cur->collapsed); break;
    case DOCK_HIDE: hide_dock(cur); break;
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
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED); /* not the tab bar's menu too */
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

/* A file dropped on the editor area opens. */
static gboolean on_editor_drop(GtkDropTarget *target G_GNUC_UNUSED, const GValue *value, double x G_GNUC_UNUSED,
                               double y G_GNUC_UNUSED, App *t)
{
  char *path = g_file_get_path(g_value_get_object(value));
  gboolean opened = path && g_file_test(path, G_FILE_TEST_IS_REGULAR);

  if (opened)
    open_file(t, path);
  g_free(path);
  return opened;
}

static void on_close_dock_clicked(GtkButton *btn G_GNUC_UNUSED, Dock *dock)
{
  hide_dock(dock);
}

static void on_collapse_dock_clicked(GtkButton *btn G_GNUC_UNUSED, Dock *dock)
{
  set_dock_collapsed(dock, !dock->collapsed);
}

/* dock.collapse and dock.hide: the tab bar's right-click menu on its own dock. */
static void on_dock_collapse_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer dock)
{
  set_dock_collapsed(dock, !((Dock *)dock)->collapsed);
}

static void on_dock_hide_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer dock)
{
  hide_dock(dock);
}

/* Right-click on the tab bar beside the icons (or its buttons): the Panels menu (panels and the
 * window's parts on and off) and this dock's collapse, hide and the layout's reset. */
static void on_header_right_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x, double y,
                                  gpointer data G_GNUC_UNUSED)
{
  GtkWidget *header = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  GtkPopover *pop = g_object_get_data(G_OBJECT(header), "tcode-panels-pop");

  if (!pop) {
    GMenu *menu = panels_menu(), *section = g_menu_new();
    g_menu_append(section, "Collapse / Expand Dock", "dock.collapse");
    g_menu_append(section, "Hide Dock", "dock.hide");
    g_menu_append(section, "Reset Panel Layout", "win.reset-panels");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    pop = GTK_POPOVER(gtk_popover_menu_new_from_model(G_MENU_MODEL(menu)));
    g_object_unref(menu);
    gtk_popover_set_has_arrow(pop, FALSE);
    gtk_widget_set_halign(GTK_WIDGET(pop), GTK_ALIGN_START);
    gtk_widget_set_parent(GTK_WIDGET(pop), header);
    g_object_set_data(G_OBJECT(header), "tcode-panels-pop", pop);
  }
  gtk_popover_set_pointing_to(pop, &(const GdkRectangle){ (int)x, (int)y, 1, 1 });
  gtk_popover_popup(pop);
}

static void on_stack_switched(GtkStack *stack G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, Dock *dock)
{
  if (dock->collapsed)
    set_dock_collapsed(dock, FALSE);
}

/* A dock's icons that do not fit its tab bar go into its "more" menu (fit_icons), like a narrow
 * side's tabs would rather do than show half an icon. */
static void on_more_chosen(GtkButton *button, GtkStack *stack)
{
  GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);

  gtk_stack_set_visible_child_name(stack, g_object_get_data(G_OBJECT(button), "tcode-page"));
  if (popover)
    gtk_popover_popdown(GTK_POPOVER(popover));
}

/* Fills the "more" menu with the panels whose icons are hidden, as it opens. */
static void fill_more(GtkMenuButton *more, gpointer data G_GNUC_UNUSED)
{
  GtkStack *stack = g_object_get_data(G_OBJECT(more), "tcode-stack");
  GtkWidget *switcher = g_object_get_data(G_OBJECT(stack), "tcode-switcher");
  GtkSelectionModel *pages = gtk_stack_get_pages(stack);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *popover = gtk_popover_new();
  guint i = 0;

  for (GtkWidget *icon = gtk_widget_get_first_child(switcher); icon; icon = gtk_widget_get_next_sibling(icon), i++) {
    GtkStackPage *page = g_list_model_get_item(G_LIST_MODEL(pages), i);
    if (page && gtk_widget_get_visible(icon) && gtk_widget_get_opacity(icon) == 0) {
      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget *button = gtk_button_new();
      gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name(gtk_stack_page_get_icon_name(page)));
      gtk_box_append(GTK_BOX(row), gtk_label_new(gtk_stack_page_get_title(page)));
      gtk_button_set_child(GTK_BUTTON(button), row);
      gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
      g_object_set_data_full(G_OBJECT(button), "tcode-page", g_strdup(gtk_stack_page_get_name(page)), g_free);
      g_signal_connect(button, "clicked", G_CALLBACK(on_more_chosen), stack);
      gtk_box_append(GTK_BOX(box), button);
    }
    g_clear_object(&page);
  }
  g_object_unref(pages);
  gtk_popover_set_child(GTK_POPOVER(popover), box);
  gtk_menu_button_set_popover(more, popover);
}

/* Hides (keeps their place, draws nothing) the icons past the tab bar's end, and shows "more" for
 * them; run after the tab bar's width changed. */
static gboolean fit_icons(gpointer data)
{
  GtkWidget *more = data;

  g_object_set_data(G_OBJECT(more), "tcode-fit", NULL);
  if (!gtk_widget_get_root(more)) /* its dock went meanwhile, and the stack with it (we hold only more) */
    return G_SOURCE_REMOVE;
  GtkStack *stack = g_object_get_data(G_OBJECT(more), "tcode-stack");
  GtkWidget *switcher = g_object_get_data(G_OBJECT(stack), "tcode-switcher");
  GtkWidget *bar = gtk_widget_get_ancestor(switcher, GTK_TYPE_SCROLLED_WINDOW);
  int width = gtk_widget_get_width(bar);
  gboolean overflow = FALSE;

  if (gtk_widget_get_visible(more)) /* without "more" the bar would be this much wider */
    width += gtk_widget_get_width(more);
  for (int pass = 0; pass < 2; pass++) {
    for (GtkWidget *icon = gtk_widget_get_first_child(switcher); icon; icon = gtk_widget_get_next_sibling(icon)) {
      graphene_rect_t bounds;
      gboolean fits =
          !gtk_widget_compute_bounds(icon, switcher, &bounds) || bounds.origin.x + bounds.size.width <= width;
      if (pass == 0)
        overflow |= !fits;
      else {
        gtk_widget_set_opacity(icon, fits ? 1 : 0);
        gtk_widget_set_can_target(icon, fits);
      }
    }
    if (pass == 0 && overflow) {
      int more_width = 0;
      gtk_widget_measure(more, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &more_width, NULL, NULL);
      width -= more_width;
    }
  }
  gtk_widget_set_visible(more, overflow);
  return G_SOURCE_REMOVE;
}

static void queue_fit_icons(GtkAdjustment *adjustment G_GNUC_UNUSED, GtkWidget *more)
{
  if (!g_object_get_data(G_OBJECT(more), "tcode-fit"))
    g_object_set_data(
        G_OBJECT(more), "tcode-fit",
        GUINT_TO_POINTER(g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, fit_icons, g_object_ref(more), g_object_unref)));
}

/* The card is going: its dock leaves the list. */
static void on_card_destroy(GtkWidget *card G_GNUC_UNUSED, Dock *dock)
{
  if (dock->t->dock.list)
    g_ptr_array_remove(dock->t->dock.list, dock);
}

Dock *dock_new(App *t, int side)
{
  Dock *dock = g_new0(Dock, 1);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *collapse_btn = gtk_button_new_from_icon_name("tcode-chevron-down-symbolic");
  GtkWidget *switcher = gtk_stack_switcher_new();
  GtkWidget *close_btn = gtk_button_new_from_icon_name("tcode-close-symbolic");
  GtkStack *stack = GTK_STACK(gtk_stack_new());
  GtkGesture *right = gtk_gesture_click_new();
  GtkDragSource *drag = gtk_drag_source_new();

  dock->t = t;
  dock->side = side;
  dock->card = box;
  dock->stack = stack;
  dock->switcher = switcher;
  g_object_set_data_full(G_OBJECT(box), "t-code-dock", dock, g_free);
  g_signal_connect(box, "destroy", G_CALLBACK(on_card_destroy), dock);
  g_ptr_array_add(t->dock.list, dock);

  gtk_widget_set_vexpand(GTK_WIDGET(stack), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(stack), TRUE);
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), stack);
  gtk_widget_set_focusable(switcher, FALSE);
  gtk_widget_set_halign(switcher, GTK_ALIGN_START);
  g_signal_connect(stack, "notify::visible-child", G_CALLBACK(on_stack_switched), dock);

  gtk_button_set_has_frame(GTK_BUTTON(collapse_btn), FALSE);
  gtk_widget_set_focusable(collapse_btn, FALSE);
  gtk_widget_set_tooltip_text(collapse_btn, "Expand / Collapse Panel");
  gtk_widget_add_css_class(collapse_btn, "tcode-dock-close");
  g_signal_connect(collapse_btn, "clicked", G_CALLBACK(on_collapse_dock_clicked), dock);

  gtk_button_set_has_frame(GTK_BUTTON(close_btn), FALSE);
  gtk_widget_set_focusable(close_btn, FALSE);
  gtk_widget_set_tooltip_text(close_btn, "Hide Panel");
  gtk_widget_add_css_class(close_btn, "tcode-dock-close");
  g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_dock_clicked), dock);

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), GDK_BUTTON_SECONDARY);
  g_signal_connect(right, "pressed", G_CALLBACK(on_switcher_right_click), t);
  gtk_widget_add_controller(switcher, GTK_EVENT_CONTROLLER(right));

  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
  gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
  g_signal_connect(drag, "prepare", G_CALLBACK(on_drag_prepare), stack);
  gtk_widget_add_controller(switcher, GTK_EVENT_CONTROLLER(drag));

  gtk_widget_add_css_class(header, "tcode-dock-header");
  const GActionEntry dock_actions[] = {
    { .name = "collapse", .activate = on_dock_collapse_action },
    { .name = "hide", .activate = on_dock_hide_action },
  };
  GSimpleActionGroup *group = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(group), dock_actions, G_N_ELEMENTS(dock_actions), dock);
  gtk_widget_insert_action_group(header, "dock", G_ACTION_GROUP(group));
  g_object_unref(group);
  GtkGesture *header_right = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(header_right), GDK_BUTTON_SECONDARY);
  g_signal_connect(header_right, "pressed", G_CALLBACK(on_header_right_click), NULL);
  gtk_widget_add_controller(header, GTK_EVENT_CONTROLLER(header_right));
  gtk_box_append(GTK_BOX(header), collapse_btn);
  GtkWidget *bar = clip_box(switcher, FALSE);
  g_object_set_data(G_OBJECT(bar), "t-code-dock-bar", GINT_TO_POINTER(1));
  GtkWidget *more = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(more), "tcode-more-symbolic");
  gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(more), FALSE);
  gtk_widget_set_tooltip_text(more, "More Panels");
  gtk_widget_add_css_class(more, "tcode-dock-close");
  gtk_widget_set_focusable(more, FALSE);
  gtk_widget_set_visible(more, FALSE);
  g_object_set_data(G_OBJECT(more), "tcode-stack", stack);
  gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(more), fill_more, NULL, NULL);
  g_signal_connect_object(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(bar)), "changed",
                          G_CALLBACK(queue_fit_icons), more, 0);
  gtk_box_append(GTK_BOX(header), bar);
  gtk_box_append(GTK_BOX(header), more);
  gtk_box_append(GTK_BOX(header), close_btn);

  gtk_widget_add_css_class(box, "tcode-sidebar");
  make_card(box);
  gtk_box_append(GTK_BOX(box), header);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(stack));
  g_object_set_data(G_OBJECT(box), "tcode-collapse-btn", collapse_btn);
  g_object_set_data(G_OBJECT(stack), "tcode-switcher", switcher);
  /* the Panels menu's checks follow whatever shows or hides a panel */
  g_signal_connect(box, "notify::visible", G_CALLBACK(on_layout_changed), t);
  g_signal_connect(stack, "notify::visible-child", G_CALLBACK(on_layout_changed), t);
  return dock;
}

/* Dragging a panel ------------------------------------------------------------------------------- */

static struct {
  GtkWidget *view, *hint;
  DropMarkers markers;
  DropZone zone;
  graphene_rect_t preview;
  Dock *dock;   /* under the pointer */
  int position; /* DROP_STRIP: where among its tabs */
} drop;

static void draw_hint(GtkDrawingArea *area, cairo_t *cr, int width G_GNUC_UNUSED, int height G_GNUC_UNUSED,
                      gpointer data G_GNUC_UNUSED)
{
  drop_markers_draw(GTK_WIDGET(area), cr, &drop.markers, drop.zone, &drop.preview);
}

int docks_drop_hover(App *t, const char *page, double x, double y)
{
  GtkWidget *w = gtk_widget_pick(drop.view, x, y, GTK_PICK_DEFAULT);
  /* the editor area as it shows: its clip (window.c), which may hold a bigger view */
  GtkWidget *editors = gtk_widget_get_ancestor(t->editors.view, GTK_TYPE_SCROLLED_WINDOW);
  gboolean on_bar = FALSE, in_editors = FALSE;
  graphene_rect_t room;

  if (!editors)
    editors = t->editors.view;

  drop.zone = DROP_NONE;
  drop.dock = NULL;
  drop.markers = (DropMarkers){ 0 };
  for (; w && w != drop.view && !drop.dock; w = gtk_widget_get_parent(w)) {
    on_bar |= g_object_get_data(G_OBJECT(w), "t-code-dock-bar") != NULL;
    in_editors |= w == editors;
    drop.dock = dock_of(w);
  }
  if (drop.dock && gtk_widget_compute_bounds(drop.dock->card, drop.view, &room)) {
    Dock *from = panel_dock(t, page);
    gboolean alone = from == drop.dock && gtk_widget_get_first_child(GTK_WIDGET(from->stack)) ==
                                              gtk_widget_get_last_child(GTK_WIDGET(from->stack));
    drop_markers_place(&drop.markers, &room, &room, !alone && !drop.dock->collapsed, FALSE);
    drop.zone = drop_marker_at(&drop.markers, x, y);
    if (drop.zone != DROP_NONE) {
      drop_zone_preview(drop.zone, &room, &room, &drop.preview);
    } else if (on_bar) {
      graphene_point_t at;
      graphene_rect_t bar;
      double line = 0;
      if (gtk_widget_compute_point(drop.view, drop.dock->switcher, &GRAPHENE_POINT_INIT(x, y), &at) &&
          gtk_widget_compute_bounds(drop.dock->switcher, drop.view, &bar)) {
        drop.position = header_drop_pos(drop.dock, page, at.x, &line);
        graphene_rect_init(&drop.preview, bar.origin.x + line - 1, bar.origin.y, 3, bar.size.height);
        drop.zone = DROP_STRIP;
      }
    }
  } else if (in_editors && gtk_widget_compute_bounds(editors, drop.view, &room)) {
    drop_markers_place(&drop.markers, &room, &room, FALSE, TRUE);
    drop.markers.shown[DROP_CENTER] = FALSE;
    drop.markers.shown[DROP_EDGE_TOP] = FALSE; /* no dock above the editors */
    drop.zone = drop_marker_at(&drop.markers, x, y);
    if (drop.zone != DROP_NONE)
      drop_zone_preview(drop.zone, &room, &room, &drop.preview);
  }
  gtk_widget_set_visible(drop.hint, TRUE);
  gtk_widget_queue_draw(drop.hint);
  return drop.zone;
}

void docks_drop_leave(App *t G_GNUC_UNUSED)
{
  drop.zone = DROP_NONE;
  drop.markers = (DropMarkers){ 0 };
  gtk_widget_set_visible(drop.hint, FALSE);
}

gboolean docks_drop(App *t, const char *page, double x, double y)
{
  DropZone zone = page && panel_dock(t, page) ? docks_drop_hover(t, page, x, y) : DROP_NONE;
  Dock *dock = drop.dock;
  int position = drop.position;

  docks_drop_leave(t);
  switch (zone) {
    case DROP_NONE: return FALSE;
    case DROP_CENTER: move_panel_at(t, page, dock, G_MAXINT); break;
    case DROP_STRIP: move_panel_at(t, page, dock, position); break;
    case DROP_LEFT: move_panel_beside(t, page, dock, SPLIT_LEFT); break;
    case DROP_RIGHT: move_panel_beside(t, page, dock, SPLIT_RIGHT); break;
    case DROP_TOP: move_panel_beside(t, page, dock, SPLIT_UP); break;
    case DROP_BOTTOM: move_panel_beside(t, page, dock, SPLIT_DOWN); break;
    case DROP_EDGE_LEFT: move_panel_edge(t, page, 0); break;
    case DROP_EDGE_BOTTOM: move_panel_edge(t, page, 1); break;
    case DROP_EDGE_RIGHT: move_panel_edge(t, page, 2); break;
    default: return FALSE;
  }
  return TRUE;
}

gboolean docks_drop_marker(App *t G_GNUC_UNUSED, int zone, double *x, double *y)
{
  if (zone < DROP_CENTER || zone >= DROP_MARKERS || !drop.markers.shown[zone])
    return FALSE;
  *x = drop.markers.rect[zone].origin.x + drop.markers.rect[zone].size.width / 2;
  *y = drop.markers.rect[zone].origin.y + drop.markers.rect[zone].size.height / 2;
  return TRUE;
}

static GdkDragAction on_panel_motion(GtkDropTarget *target, double x, double y, App *t)
{
  const GValue *value = gtk_drop_target_get_value(target);
  const char *page = value && G_VALUE_HOLDS_INT(value) ? panel_from_id(g_value_get_int(value)) : NULL;

  return docks_drop_hover(t, page, x, y) != DROP_NONE ? GDK_ACTION_MOVE : 0;
}

static void on_panel_leave(GtkDropTarget *target G_GNUC_UNUSED, App *t)
{
  docks_drop_leave(t);
}

static gboolean on_panel_drop(GtkDropTarget *target G_GNUC_UNUSED, const GValue *value, double x, double y, App *t)
{
  return docks_drop(t, panel_from_id(g_value_get_int(value)), x, y);
}

GtkWidget *docks_drop_view(App *t, GtkWidget *content)
{
  GtkWidget *view = gtk_overlay_new();
  GtkDropTarget *target = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE); /* a panel's icon */

  drop.view = view;
  drop.hint = gtk_drawing_area_new();
  gtk_overlay_set_child(GTK_OVERLAY(view), content);
  gtk_widget_add_css_class(drop.hint, "tcode-drop-hint");
  gtk_widget_set_can_target(drop.hint, FALSE);
  gtk_widget_set_visible(drop.hint, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drop.hint), draw_hint, NULL, NULL);
  gtk_overlay_add_overlay(GTK_OVERLAY(view), drop.hint);
  gtk_drop_target_set_preload(target, TRUE); /* the panel is known while it moves: its own dock's sides */
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(target), GTK_PHASE_CAPTURE);
  g_signal_connect(target, "enter", G_CALLBACK(on_panel_motion), t);
  g_signal_connect(target, "motion", G_CALLBACK(on_panel_motion), t);
  g_signal_connect(target, "leave", G_CALLBACK(on_panel_leave), t);
  g_signal_connect(target, "drop", G_CALLBACK(on_panel_drop), t);
  gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(target));
  return view;
}

/* Creates the sides with a dock each, and t-code's panels in them. */
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
  GtkDropTarget *ed_drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_MOVE | GDK_ACTION_COPY); /* from Files */
  Dock *docks[3];
  /* clang-format off */
  static const struct { int side; const char *name, *title; } order[] = {
    { 0, "files", "Files (Alt+1)" },      { 0, "scm", "SOURCE CONTROL" },
    { 0, "search", "Search (Ctrl+Shift+F)" },
    { 1, "shell", "Terminal (Alt+3 / Ctrl+J)" }, { 1, "debug", "Debug (F5)" },
    { 0, "run", "Run and Debug (Ctrl+Shift+D)" }, { 1, "problems", "Problems (Ctrl+Shift+M)" },
    { 2, "outline", "Outline" },          { 2, "hierarchy", "Hierarchy (Shift+Alt+H)" },
    { 2, "testing", "Testing" },          { 0, "plugins", "Plugins" },
  };
  /* clang-format on */

  palette_add_css(css);
  t->dock.list = g_ptr_array_new();
  for (int side = 0; side < 3; side++) {
    /* Held without asking for the docks' smallest size, as the paneds of a split would: the window's
     * paneds squeeze a side instead of measuring it below its minimum. */
    GtkWidget *root = split_root_new(), *clip = clip_box(root, TRUE);
    gtk_viewport_set_scroll_to_focus(GTK_VIEWPORT(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(clip))), FALSE);
    /* Its natural size is the docks'. And never nothing: a GtkPaned that gives a side no room before
     * its position is set marks it not child-visible, and it stays unmapped once it has room. */
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(clip), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(clip), TRUE);
    gtk_widget_set_size_request(clip, 1, 1);
    g_object_set_data(G_OBJECT(clip), "t-code-dock-root", root);
    *(side == 0 ? &t->layout.left : side == 1 ? &t->layout.bottom : &t->layout.right) = clip;
    docks[side] = dock_new(t, side);
    gtk_box_append(GTK_BOX(root), docks[side]->card);
    g_signal_connect(side_widget(t, side), "notify::visible", G_CALLBACK(on_layout_changed), t);
  }
  t->scm = clip_box(scm_panel(t), TRUE);
  t->problems = problems_panel(t);
  for (size_t i = 0; i < G_N_ELEMENTS(order); i++) {
    const char *name = order[i].name;
    GtkWidget *child = strcmp(name, "files") == 0       ? clip_box(files_panel(t), TRUE)
                       : strcmp(name, "scm") == 0       ? t->scm
                       : strcmp(name, "search") == 0    ? clip_box(search_panel(t), TRUE)
                       : strcmp(name, "shell") == 0     ? t->shell.panel
                       : strcmp(name, "debug") == 0     ? debug_panel(t)
                       : strcmp(name, "run") == 0       ? clip_box(debug_view(t), TRUE)
                       : strcmp(name, "problems") == 0  ? clip_box(t->problems, TRUE)
                       : strcmp(name, "outline") == 0   ? clip_box(outline_panel(t), TRUE)
                       : strcmp(name, "hierarchy") == 0 ? clip_box(hierarchy_panel(t), TRUE)
                       : strcmp(name, "testing") == 0   ? clip_box(testing_panel(t), TRUE)
                                                        : clip_box(plugins_panel(t), TRUE);
    add_dock_page(docks[order[i].side], child, name, order[i].title, panel_icon(name));
    panel_id(name);
  }
  update_docks(t);

  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(ed_drop), GTK_PHASE_CAPTURE);
  g_signal_connect(ed_drop, "drop", G_CALLBACK(on_editor_drop), t);
  gtk_widget_add_controller(GTK_WIDGET(t->editors.area), GTK_EVENT_CONTROLLER(ed_drop));
}
