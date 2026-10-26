/*
 * Showing, hiding, collapsing and moving the panels in the docks (see docks.h and window.h).
 */
#include "window/docks/docks.h"

#include "editor/tabs.h"
#include "files/files.h"
#include "lsp/problems.h"
#include "search/search.h"
#include "window/window.h"

#include <string.h>

/* Panes -------------------------------------------------------------------------------------- */

static void queue_panel_checks(App *t);

const char *const panel_names[PANELS] = { "files", "scm",      "search",  "shell",    "debug",
                                          "run",   "problems", "outline", "hierarchy" };
const int panel_default_docks[PANELS] = { 0, 0, 0, 4, 4, 0, 4, 0, 0 };

const char *panel_icon(const char *page)
{
  if (strcmp(page, "files") == 0)
    return "folder-symbolic";
  if (strcmp(page, "scm") == 0)
    return "send-to-symbolic";
  if (strcmp(page, "search") == 0)
    return "system-search-symbolic";
  if (strcmp(page, "debug") == 0)
    return "system-run-symbolic";
  if (strcmp(page, "run") == 0)
    return "media-playback-start-symbolic";
  if (strcmp(page, "problems") == 0)
    return "dialog-warning-symbolic";
  if (strcmp(page, "outline") == 0)
    return "view-list-symbolic";
  if (strcmp(page, "hierarchy") == 0)
    return "format-indent-more-symbolic";
  return "utilities-terminal-symbolic";
}

int panel_id(const char *page)
{
  for (int i = 0; i < PANELS; i++)
    if (g_strcmp0(page, panel_names[i]) == 0)
      return i + 1;
  return 0;
}

const char *panel_from_id(int id)
{
  return (id >= 1 && id <= PANELS) ? panel_names[id - 1] : NULL;
}

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
    gtk_widget_grab_focus(GTK_WIDGET(t->shell.term));
  else if (strcmp(page, "problems") == 0)
    problems_focus(t);
}

int panel_dock(App *t, const char *page)
{
  for (int d = 0; d < DOCKS; d++)
    if (t->dock.stacks[d] && gtk_stack_get_child_by_name(t->dock.stacks[d], page))
      return d;
  return -1;
}

/* Whether dock d holds any panel. */
static gboolean dock_has_pages(App *t, int d)
{
  return t->dock.stacks[d] && gtk_widget_get_first_child(GTK_WIDGET(t->dock.stacks[d])) != NULL;
}

gboolean side_has_pages(App *t, int side)
{
  for (int i = 0; i < 4; i++)
    if (dock_has_pages(t, side * 4 + i))
      return TRUE;
  return FALSE;
}

static gboolean side_any_visible(App *t, int side)
{
  for (int i = 0; i < 4; i++) {
    int d = side * 4 + i;
    if (dock_has_pages(t, d) && gtk_widget_get_visible(t->dock.cards[d]))
      return TRUE;
  }
  return FALSE;
}

GtkWidget *side_widget(App *t, int side)
{
  return side == 0 ? t->layout.left : (side == 1 ? t->layout.bottom : t->layout.right);
}

static void sync_sidebar_visible(App *t)
{
  if (t->layout.left)
    gtk_widget_set_visible(t->layout.left, side_any_visible(t, 0));
  if (t->layout.bottom)
    gtk_widget_set_visible(t->layout.bottom, side_any_visible(t, 1));
  if (t->layout.right)
    gtk_widget_set_visible(t->layout.right, side_any_visible(t, 2));
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
  gboolean expanded = side != 1;

  for (int i = 0; i < 4 && !expanded; i++)
    expanded = gtk_widget_get_visible(t->dock.cards[4 + i]) && !t->dock.collapsed[4 + i];
  return expanded && size > 0 && gtk_paned_get_position(paned) > size - 48;
}

/* Adjusts the paned positions on a side so collapsed docks shrink to their header bar and
 * expanded docks share the space. */
static void sync_side_paneds(App *t, int side)
{
  int base = side * 4;
  int collapsed_size = (side == 1) ? 96 : 32;

  for (int i = 2; i >= 1; i--) {
    if (t->dock.paneds[side][i]) {
      GtkWidget *end = (i == 2) ? t->dock.cards[base + 3] : GTK_WIDGET(t->dock.paneds[side][i + 1]);
      gtk_widget_set_visible(GTK_WIDGET(t->dock.paneds[side][i]),
                             gtk_widget_get_visible(t->dock.cards[base + i]) || gtk_widget_get_visible(end));
    }
  }
  for (int i = 0; i < 3; i++) {
    GtkPaned *p = t->dock.paneds[side][i];
    int d = base + i;
    if (!p || !gtk_widget_get_visible(t->dock.cards[d]))
      continue;
    gboolean any_after = FALSE, any_after_expanded = FALSE;
    int after_count = 0;
    for (int j = i + 1; j < 4; j++) {
      if (gtk_widget_get_visible(t->dock.cards[base + j])) {
        any_after = TRUE;
        after_count++;
        if (!t->dock.collapsed[base + j])
          any_after_expanded = TRUE;
      }
    }
    if (!any_after)
      continue;
    int total = (side == 1) ? gtk_widget_get_width(GTK_WIDGET(p)) : gtk_widget_get_height(GTK_WIDGET(p));
    int cur = gtk_paned_get_position(p);
    if (t->dock.collapsed[d]) {
      if (cur > collapsed_size + 16)
        t->dock.saved_pos[d] = cur;
      gtk_paned_set_position(p, collapsed_size);
    } else if (!any_after_expanded && total > collapsed_size * after_count + 40) {
      gtk_paned_set_position(p, total - collapsed_size * after_count);
    } else {
      int restore = t->dock.saved_pos[d] > collapsed_size + 16 ? t->dock.saved_pos[d] : (total > 120 ? total / 2 : 240);
      if (cur <= collapsed_size + 8 || (total > 120 && cur >= total - collapsed_size * after_count - 8))
        gtk_paned_set_position(p, restore);
    }
  }
  if (side == 1 && t->layout.vpaned && side_any_visible(t, 1)) {
    gboolean any_expanded = FALSE;
    for (int i = 0; i < 4; i++)
      if (gtk_widget_get_visible(t->dock.cards[4 + i]) && !t->dock.collapsed[4 + i])
        any_expanded = TRUE;
    int h = gtk_widget_get_height(GTK_WIDGET(t->layout.vpaned));
    int pos = gtk_paned_get_position(t->layout.vpaned);
    if (!any_expanded && h > 100) {
      if (pos < h - 48)
        t->dock.saved_pos[4] = pos;
      gtk_paned_set_position(t->layout.vpaned, h - 34);
    } else if (any_expanded && h > 200 && pos > h - 48) {
      gtk_paned_set_position(t->layout.vpaned, t->dock.saved_pos[4] > 100 && t->dock.saved_pos[4] < h - 60
                                                   ? t->dock.saved_pos[4]
                                                   : h - 240);
    }
  }
}

void set_dock_collapsed(App *t, int d, gboolean collapsed)
{
  if (d < 0 || d >= DOCKS)
    return;
  t->dock.collapsed[d] = collapsed;
  gtk_widget_set_visible(GTK_WIDGET(t->dock.stacks[d]), !collapsed);
  if (d / 4 != 1)
    gtk_widget_set_valign(t->dock.cards[d], collapsed ? GTK_ALIGN_START : GTK_ALIGN_FILL);
  GtkWidget *btn = g_object_get_data(G_OBJECT(t->dock.cards[d]), "tcode-collapse-btn");
  if (btn)
    gtk_button_set_icon_name(GTK_BUTTON(btn), collapsed ? "pan-end-symbolic" : "pan-down-symbolic");
  sync_side_paneds(t, d / 4);
  queue_panel_checks(t);
}

void update_docks(App *t)
{
  for (int d = 0; d < DOCKS; d++)
    gtk_widget_set_visible(t->dock.cards[d], dock_has_pages(t, d));
  sync_sidebar_visible(t);
  for (int s = 0; s < 3; s++)
    sync_side_paneds(t, s);
}

void hide_dock(App *t, int d)
{
  if (d < 0 || d >= DOCKS)
    return;
  if (gtk_widget_has_focus(t->dock.cards[d]))
    focus_current_tab(t);
  gtk_widget_set_visible(t->dock.cards[d], FALSE);
  sync_sidebar_visible(t);
  sync_side_paneds(t, d / 4);
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
        hide_dock(t, panel_dock(t, fallback));
    }
    return;
  }
  if (!visible && gtk_widget_has_focus(w))
    focus_current_tab(t);
  gtk_widget_set_visible(w, visible);
  if (visible) {
    for (int i = 0; i < 4; i++)
      gtk_widget_set_visible(t->dock.cards[side * 4 + i], dock_has_pages(t, side * 4 + i));
    make_room(t, side);
    sync_side_paneds(t, side);
  }
}

void set_pane_visible(App *t, GtkWidget *pane, gboolean visible)
{
  if (pane == GTK_WIDGET(t->shell.term)) {
    if (visible)
      show_sidebar_page(t, "shell");
    else
      hide_dock(t, panel_dock(t, "shell"));
    return;
  }
  if (pane == t->layout.left) {
    show_side(t, 0, visible);
    return;
  }
  if (pane == t->layout.bottom) {
    show_side(t, 1, visible);
    return;
  }
  if (pane == t->layout.right) {
    show_side(t, 2, visible);
    return;
  }
  if (!visible && gtk_widget_has_focus(pane))
    focus_current_tab(t);
  gtk_widget_set_visible(pane, visible);
}

void show_sidebar_page(App *t, const char *page)
{
  int d = panel_dock(t, page);

  if (d < 0)
    return;
  int side = d / 4;
  gtk_widget_set_visible(t->dock.cards[d], TRUE);
  if (t->dock.collapsed[d])
    set_dock_collapsed(t, d, FALSE);
  gtk_widget_set_visible(side_widget(t, side), TRUE);
  make_room(t, side);
  sync_side_paneds(t, side);
  gtk_stack_set_visible_child_name(t->dock.stacks[d], page);
  focus_panel(t, page);
}

void toggle_pane(App *t, GtkWidget *pane)
{
  if (pane == GTK_WIDGET(t->shell.term) || (pane == t->layout.left && !side_has_pages(t, 0))) {
    const char *name = (pane == GTK_WIDGET(t->shell.term)) ? "shell" : "files";
    int d = panel_dock(t, name);
    if (d < 0)
      return;
    int side = d / 4;
    gboolean shown = gtk_widget_get_visible(t->dock.cards[d]) && !t->dock.collapsed[d] &&
                     gtk_widget_get_visible(side_widget(t, side)) && !side_squeezed(t, side);
    if (shown && g_strcmp0(gtk_stack_get_visible_child_name(t->dock.stacks[d]), name) == 0)
      hide_dock(t, d);
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

gboolean panel_shown(App *t, const char *page)
{
  int d = panel_dock(t, page);

  return d >= 0 && gtk_widget_get_visible(t->dock.cards[d]) && !t->dock.collapsed[d] &&
         gtk_widget_get_visible(side_widget(t, d / 4)) && !side_squeezed(t, d / 4) &&
         g_strcmp0(gtk_stack_get_visible_child_name(t->dock.stacks[d]), page) == 0;
}

void toggle_panel(App *t, const char *page)
{
  if (panel_shown(t, page))
    hide_dock(t, panel_dock(t, page));
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

void reset_panel_layout(App *t)
{
  static const int first_split[3] = { 320, 480, 320 }; /* build_side_chain()'s */
  int h = gtk_widget_get_height(GTK_WIDGET(t->layout.vpaned)), w = gtk_widget_get_width(GTK_WIDGET(t->layout.rpaned));

  for (int i = 0; i < PANELS; i++) /* in panel_names' order: their tabs too */
    move_panel(t, panel_names[i], panel_default_docks[i]);
  for (int d = 0; d < DOCKS; d++) {
    t->dock.saved_pos[d] = 0;
    if (t->dock.collapsed[d])
      set_dock_collapsed(t, d, FALSE);
  }
  for (int side = 0; side < 3; side++)
    for (int i = 0; i < 3; i++)
      gtk_paned_set_position(t->dock.paneds[side][i], first_split[side]);
  update_docks(t);
  gtk_paned_set_position(t->layout.hpaned, 280);
  gtk_paned_set_position(t->layout.vpaned, h > 400 ? h - 240 : 620);
  gtk_paned_set_position(t->layout.rpaned, w > 600 ? w - 280 : 840);
  show_side(t, 0, TRUE);
  show_side(t, 1, TRUE);
  gtk_stack_set_visible_child_name(t->dock.stacks[panel_dock(t, "shell")], "shell");
  show_sidebar_page(t, "files");
  queue_panel_checks(t);
}

void update_scm_badge(GtkStackPage *sp, GParamSpec *pspec G_GNUC_UNUSED, App *t)
{
  int d = panel_dock(t, "scm");
  if (d < 0 || !sp)
    return;
  GtkWidget *switcher = g_object_get_data(G_OBJECT(t->dock.stacks[d]), "tcode-switcher");
  if (!switcher)
    return;
  GtkSelectionModel *pages = gtk_stack_get_pages(t->dock.stacks[d]);
  GtkWidget *btn = gtk_widget_get_first_child(switcher);
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
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("send-to-symbolic"));
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_button_set_child(GTK_BUTTON(btn), box);
      } else {
        gtk_button_set_icon_name(GTK_BUTTON(btn), "send-to-symbolic");
      }
      break;
    }
  }
  g_object_unref(pages);
}

static void move_stack_page(App *t, GtkStack *from_stack, GtkStack *to_stack, const char *page)
{
  GtkWidget *child = gtk_stack_get_child_by_name(from_stack, page);
  GtkStackPage *sp = gtk_stack_get_page(from_stack, child);
  char *title = g_strdup(gtk_stack_page_get_title(sp));

  g_object_ref(child);
  gtk_stack_remove(from_stack, child);
  sp = gtk_stack_add_titled(to_stack, child, page, title);
  gtk_stack_page_set_icon_name(sp, panel_icon(page));
  g_object_unref(child);
  g_free(title);
  if (strcmp(page, "scm") == 0) {
    t->dock.scm_stack = to_stack;
    g_signal_connect_after(sp, "notify::title", G_CALLBACK(update_scm_badge), t);
  }
}

/* Moves page to dock `to` and places its tab at `insert_pos` (or appends at end if insert_pos < 0). */
void move_panel_at(App *t, const char *page, int to, int insert_pos)
{
  int from = panel_dock(t, page);

  if (from < 0 || to < 0 || to >= DOCKS || (from == to && insert_pos < 0))
    return;
  const char *after[PANELS];
  int n_after = 0;
  if (insert_pos >= 0) {
    GtkSelectionModel *pages = gtk_stack_get_pages(t->dock.stacks[to]);
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(pages));
    int cur_pos = -1, idx = 0;
    for (guint i = 0; i < n_items; i++) {
      GtkStackPage *item = g_list_model_get_item(G_LIST_MODEL(pages), i);
      const char *nm = item ? panel_from_id(panel_id(gtk_stack_page_get_name(item))) : NULL;
      if (nm) {
        if (strcmp(nm, page) == 0) {
          cur_pos = idx;
        } else {
          if (idx >= insert_pos && n_after < PANELS)
            after[n_after++] = nm;
          idx++;
        }
      }
      if (item)
        g_object_unref(item);
    }
    g_object_unref(pages);
    if (from == to && cur_pos == CLAMP(insert_pos, 0, idx))
      return;
  }
  move_stack_page(t, t->dock.stacks[from], t->dock.stacks[to], page);
  for (int i = 0; i < n_after; i++)
    move_stack_page(t, t->dock.stacks[to], t->dock.stacks[to], after[i]);
  if (t->dock.scm_stack && t->scm)
    update_scm_badge(gtk_stack_get_page(t->dock.scm_stack, t->scm), NULL, t);
  gtk_widget_set_visible(t->dock.cards[from], dock_has_pages(t, from));
  gtk_widget_set_visible(t->dock.cards[to], TRUE);
  if (t->dock.collapsed[to])
    set_dock_collapsed(t, to, FALSE);
  sync_sidebar_visible(t);
  make_room(t, to / 4);
  sync_side_paneds(t, from / 4);
  sync_side_paneds(t, to / 4);
  gtk_stack_set_visible_child_name(t->dock.stacks[to], page);
}

void move_panel(App *t, const char *page, int to)
{
  move_panel_at(t, page, to, 99);
}

/* First occupied dock on side (for adding a tab), or side*4 if none. */
int first_tab_dock(App *t, int side)
{
  int base = side * 4;
  for (int i = 0; i < 4; i++)
    if (dock_has_pages(t, base + i))
      return base + i;
  return base;
}

/* A new split slot on side (after the last occupied slot, compacting empty slots if needed). */
int new_split_dock(App *t, int side, const char *moving_page)
{
  int base = side * 4;
  int from = panel_dock(t, moving_page);

  /* If moving_page is already the sole page of a dock on this side, keep it if at the end. */
  for (int i = 3; i >= 0; i--) {
    if (dock_has_pages(t, base + i)) {
      if (i < 3)
        return base + i + 1;
      break;
    }
  }
  for (int i = 0; i < 4; i++)
    if (!dock_has_pages(t, base + i) || base + i == from)
      return base + i;
  return base + 3;
}

/* Inserts page into a slot before (rel < 0) or after (rel > 0) target_dock on its side. */
void move_panel_relative(App *t, const char *page, int target_dock, int rel)
{
  if (rel == 0) {
    if (panel_dock(t, page) != target_dock)
      move_panel(t, page, target_dock);
    return;
  }
  int side = target_dock / 4, base = side * 4, idx = target_dock % 4;
  int want = (rel > 0) ? idx + 1 : idx;

  if (rel > 0 && idx + 1 < 4 && !dock_has_pages(t, base + idx + 1)) {
    move_panel(t, page, base + idx + 1);
    return;
  }
  if (rel < 0 && idx - 1 >= 0 && !dock_has_pages(t, base + idx - 1)) {
    move_panel(t, page, base + idx - 1);
    return;
  }
  /* Find an empty slot on this side and shift panels one step to open `base + want`. */
  int empty = -1;
  for (int i = 0; i < 4; i++)
    if (!dock_has_pages(t, base + i))
      empty = i;
  if (empty < 0) {
    move_panel(t, page, new_split_dock(t, side, page));
    return;
  }
  if (empty > want) {
    for (int i = empty; i > want; i--) {
      for (int p = 0; p < PANELS; p++)
        if (panel_dock(t, panel_names[p]) == base + i - 1)
          move_panel(t, panel_names[p], base + i);
    }
  } else if (empty < want) {
    if (rel > 0)
      want--;
    for (int i = empty; i < want; i++) {
      for (int p = 0; p < PANELS; p++)
        if (panel_dock(t, panel_names[p]) == base + i + 1)
          move_panel(t, panel_names[p], base + i);
    }
  }
  move_panel(t, page, base + CLAMP(want, 0, 3));
}
