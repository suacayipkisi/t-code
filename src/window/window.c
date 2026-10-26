/*
 * The main window: building it, its title bar and sidebar, and showing or hiding its panes (see
 * window.h for the layout).
 */
#include "window/window.h"

#include "debug/debugger.h"
#include "debug/debugview.h"
#include "dialogs/settings.h"
#include "editor/editor.h"
#include "editor/editorview.h"
#include "editor/preview.h"
#include "editor/snippets.h"
#include "editor/tabs.h"
#include "files/files.h"
#include "git/scm.h"
#include "ipc/socket.h"
#include "popups/picker.h"
#include "project/project.h"
#include "project/session.h"
#include "project/userfiles.h"
#include "lsp/hierarchy.h"
#include "lsp/outline.h"
#include "lsp/problems.h"
#include "search/search.h"
#include "update/update.h"
#include "window/commands.h"
#include "window/paths.h"
#include "window/statusbar.h"
#include "window/terminal.h"
#include "window/theme.h"
#include "plugins/plugins.h"

#include <string.h>

/* Panes -------------------------------------------------------------------------------------- */

const char *const panel_names[PANELS] = { "files", "scm",      "search",  "shell",    "debug",
                                          "run",   "problems", "outline", "hierarchy" };
const int panel_default_docks[PANELS] = { 0, 0, 0, 4, 4, 0, 4, 0, 0 };

static void queue_panel_checks(App *t);

static const char *panel_icon(const char *page)
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

static int panel_id(const char *page)
{
  for (int i = 0; i < PANELS; i++)
    if (g_strcmp0(page, panel_names[i]) == 0)
      return i + 1;
  return 0;
}

static const char *panel_from_id(int id)
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

static gboolean dock_has_pages(App *t, int d)
{
  return t->dock.stacks[d] && gtk_widget_get_first_child(GTK_WIDGET(t->dock.stacks[d])) != NULL;
}

static gboolean side_has_pages(App *t, int side)
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

static GtkWidget *side_widget(App *t, int side)
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

static void set_dock_collapsed(App *t, int d, gboolean collapsed)
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

static void update_docks(App *t)
{
  for (int d = 0; d < DOCKS; d++)
    gtk_widget_set_visible(t->dock.cards[d], dock_has_pages(t, d));
  sync_sidebar_visible(t);
  for (int s = 0; s < 3; s++)
    sync_side_paneds(t, s);
}

static void hide_dock(App *t, int d)
{
  if (d < 0 || d >= DOCKS)
    return;
  if (gtk_widget_has_focus(t->dock.cards[d]))
    focus_current_tab(t);
  gtk_widget_set_visible(t->dock.cards[d], FALSE);
  sync_sidebar_visible(t);
  sync_side_paneds(t, d / 4);
}

void set_font_scale(App *t, double scale)
{
  t->settings.font_scale = CLAMP(scale, 0.5, 3.0);
  vte_terminal_set_font_scale(t->shell.term, t->settings.font_scale);
  if (t->debug.term)
    vte_terminal_set_font_scale(t->debug.term, t->settings.font_scale);
  editor_view_set_font(t);
  files_apply_font(t);
}

void set_preview_zoom_scale(App *t, double scale)
{
  int n = tab_count(t);

  t->settings.preview_scale = CLAMP(scale, 0.5, 3.0);
  for (int i = 0; i < n; i++)
    if (tab_nth(t, i)->kind == TAB_PREVIEW)
      preview_set_zoom(tab_nth(t, i), t->settings.preview_scale);
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

static void on_layout_changed(GObject *object G_GNUC_UNUSED, GParamSpec *spec G_GNUC_UNUSED, App *t)
{
  queue_panel_checks(t);
}

/* The window goes: the menu's actions with it. */
static void on_window_destroy(GtkWidget *window G_GNUC_UNUSED, App *t)
{
  g_clear_handle_id(&t->dock.checks_id, g_source_remove);
  g_clear_handle_id(&t->status.say_id, g_source_remove); /* its label goes with the window */
  g_clear_handle_id(&t->status.progress_id, g_source_remove);
  plugins_stop(t);
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

static void update_scm_badge(GtkStackPage *sp, GParamSpec *pspec G_GNUC_UNUSED, App *t)
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
static void move_panel_at(App *t, const char *page, int to, int insert_pos)
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
static int first_tab_dock(App *t, int side)
{
  int base = side * 4;
  for (int i = 0; i < 4; i++)
    if (dock_has_pages(t, base + i))
      return base + i;
  return base;
}

/* A new split slot on side (after the last occupied slot, compacting empty slots if needed). */
static int new_split_dock(App *t, int side, const char *moving_page)
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
static void move_panel_relative(App *t, const char *page, int target_dock, int rel)
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

/* Closing ------------------------------------------------------------------------------------ */

/* The window's close button, once nothing unsaved is left (or the user said to drop it): the
 * session is saved, then the window goes; its tabs stop their editors on the way. */
static void close_window(App *t, const char *arg G_GNUC_UNUSED)
{
  session_save(t);
  t->closing = TRUE;
  if (t->editors.second) /* its tabs go with it, as the main window's do */
    gtk_window_destroy(t->editors.second);
  gtk_window_destroy(t->window);
}

/* The window manager asks to close: TRUE keeps the window until unless_unsaved() has asked about
 * unsaved changes. (Autosave leaves little: a second's worth of typing.) */
static gboolean on_close_request(GtkWindow *window G_GNUC_UNUSED, App *t)
{
  if (t->closing)
    return FALSE;
  unless_unsaved(t, "Close t-code?", "Close", close_window, NULL);
  return TRUE;
}

/* Title bar and docks ------------------------------------------------------------------------ */

/* The Split Editor button in the title bar. */
static void on_split_toggled(GtkToggleButton *button, App *t)
{
  set_split(t, gtk_toggle_button_get_active(button));
  focus_current_tab(t);
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

/* When the user drags a paned handle all the way to the edge (< 24px), collapse that pane cleanly. */
static void on_hpaned_position(GtkPaned *paned, GParamSpec *spec G_GNUC_UNUSED, App *t)
{
  int pos = gtk_paned_get_position(paned);

  if (pos > 0 && pos < 24 && gtk_widget_get_visible(t->layout.left)) {
    gtk_paned_set_position(paned, 280);
    set_pane_visible(t, t->layout.left, FALSE);
  }
}

static void on_rpaned_position(GtkPaned *paned, GParamSpec *spec G_GNUC_UNUSED, App *t)
{
  int w = gtk_widget_get_width(GTK_WIDGET(paned));
  int pos = gtk_paned_get_position(paned);

  if (w > 320 && pos > w - 24 && gtk_widget_get_visible(t->layout.right)) {
    gtk_paned_set_position(paned, w - 280);
    set_pane_visible(t, t->layout.right, FALSE);
  }
}

static void on_vpaned_position(GtkPaned *paned, GParamSpec *spec G_GNUC_UNUSED, App *t)
{
  int h = gtk_widget_get_height(GTK_WIDGET(paned));
  int pos = gtk_paned_get_position(paned);

  if (h > 200 && pos > h - 24 && gtk_widget_get_visible(t->layout.bottom)) {
    gtk_paned_set_position(paned, h - 240);
    set_pane_visible(t, t->layout.bottom, FALSE);
  }
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
static void create_docks(App *t)
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
  GtkCssProvider *provider = gtk_css_provider_new();
  GtkDropTarget *ed_drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_MOVE | GDK_ACTION_COPY);
  GType dropped[] = { G_TYPE_INT, G_TYPE_FILE }; /* a panel's icon, a file from the Files panel */

  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

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

static void on_toggle_left(App *t)
{
  toggle_pane(t, t->layout.left);
}

static void on_toggle_right(App *t)
{
  if (side_has_pages(t, 2))
    toggle_pane(t, t->layout.right);
}

static void on_toggle_bottom(App *t)
{
  toggle_pane(t, side_has_pages(t, 1) ? t->layout.bottom : GTK_WIDGET(t->shell.term));
}

/* win.panel-<name> (the Panels menu): shows or hides that panel. */
static void on_panel_action(GSimpleAction *action, GVariant *param G_GNUC_UNUSED, gpointer t)
{
  toggle_panel(t, g_action_get_name(G_ACTION(action)) + strlen("panel-"));
}

/* win.command: runs commands[]'s command of that name (window/commands.c), for the menu bar. */
static void on_command_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param, gpointer t)
{
  run_command(t, g_variant_get_string(param, NULL));
}

/* win.quit: closes the window, asking about unsaved files as its close button does. */
static void on_quit_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer t)
{
  gtk_window_close(((App *)t)->window);
}

/* The menu bar's actions: a checked one per panel, win.command and win.quit. */
static void add_menu_actions(App *t)
{
  static const GActionEntry actions[] = {
    { .name = "command", .activate = on_command_action, .parameter_type = "s" },
    { .name = "quit", .activate = on_quit_action },
  };

  g_action_map_add_action_entries(G_ACTION_MAP(t->window), actions, G_N_ELEMENTS(actions), t);
  for (int i = 0; i < PANELS; i++) {
    char *name = g_strconcat("panel-", panel_names[i], NULL);
    GSimpleAction *action = g_simple_action_new_stateful(name, NULL, g_variant_new_boolean(FALSE));

    g_signal_connect(action, "activate", G_CALLBACK(on_panel_action), t);
    g_action_map_add_action(G_ACTION_MAP(t->window), G_ACTION(action));
    g_object_unref(action);
    g_free(name);
  }
}

/* An item running action (with target, a string, when not NULL), its shortcut accel shown. */
static void add_item(GMenu *menu, const char *label, const char *action, const char *target, const char *accel)
{
  GMenuItem *item = g_menu_item_new(label, NULL);

  g_menu_item_set_action_and_target_value(item, action, target ? g_variant_new_string(target) : NULL);
  if (accel)
    g_menu_item_set_attribute(item, "accel", "s", accel);
  g_menu_append_item(menu, item);
  g_object_unref(item);
}

/* An item running commands[]'s command of that name. */
static void add_command(GMenu *menu, const char *label, const char *command, const char *accel)
{
  add_item(menu, label, "win.command", command, accel);
}

static void add_submenu(GMenu *bar, const char *label, GMenu *menu)
{
  g_menu_append_submenu(bar, label, G_MENU_MODEL(menu));
  g_object_unref(menu);
}

/* Files, T-Code, Settings, Panels (checked while they show), About and Help. */
static GMenu *menu_bar_model(App *t)
{
  static const char *const labels[PANELS] = { "Files",         "Source Control", "Search",  "Terminal", "Debug",
                                              "Run and Debug", "Problems",       "Outline", "Hierarchy" };
  static const char *const accels[PANELS] = { "<Alt>1", "<Control><Shift>g", "<Control><Shift>f", "<Control>j",
                                              NULL,     "<Control><Shift>d", "<Control><Shift>m", NULL,
                                              NULL };
  GMenu *bar = g_menu_new(), *menu = g_menu_new(), *section = g_menu_new();

  add_command(menu, "Open Folder…", "Open Folder…", "<Control><Shift>o");
  g_menu_append_submenu(menu, "Recent Folders", G_MENU_MODEL(t->recent_menu));
  add_submenu(bar, "Files", menu);

  menu = g_menu_new();
  add_item(menu, "New Window", "app.new-window", NULL, NULL);
  add_command(menu, "Command Palette…", "Command Palette…", "<Control><Shift>p");
  add_command(menu, "Check for Updates Now", "Check for Updates Now", NULL);
  add_item(section, "Quit", "win.quit", NULL, NULL);
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  add_submenu(bar, "T-Code", menu);

  menu = g_menu_new();
  section = g_menu_new();
  add_command(menu, "Settings…", "Settings…", "<Control>comma");
  add_command(menu, "Color Theme…", "Preferences: Color Theme…", NULL);
  add_command(section, "Open Settings File", "Preferences: Open Settings File", NULL);
  add_command(section, "Open Theme CSS", "Preferences: Open Theme CSS", NULL);
  add_command(section, "Open Editor Colors", "Preferences: Open Editor Colors", NULL);
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  add_submenu(bar, "Settings", menu);

  menu = g_menu_new();
  for (int i = 0; i < PANELS; i++) {
    char *action = g_strconcat("win.panel-", panel_names[i], NULL);

    add_item(menu, labels[i], action, NULL, accels[i]);
    g_free(action);
  }
  add_submenu(bar, "Panels", menu);

  menu = g_menu_new();
  add_item(menu, "About t-code", "win.about", NULL, NULL);
  add_submenu(bar, "About", menu);

  menu = g_menu_new();
  add_item(menu, "Keyboard Shortcuts", "win.keybinds", NULL, NULL);
  add_item(menu, "Reset Panel Layout", "win.reset-panels", NULL, NULL);
  add_submenu(bar, "Help", menu);
  return bar;
}

/* The menu bar on the left; the sides' toggles and Split Editor on the right. */
static GtkWidget *header_bar(App *t)
{
  GtkWidget *header = gtk_header_bar_new();
  GMenu *model = menu_bar_model(t);
  GtkWidget *menu_bar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(model));
  GtkWidget *left_btn = gtk_button_new_from_icon_name("sidebar-show-symbolic");
  GtkWidget *right_btn = gtk_button_new_from_icon_name("sidebar-show-right-symbolic");
  GtkWidget *bottom_btn = gtk_button_new_from_icon_name("go-bottom-symbolic");
  GtkWidget *split = gtk_toggle_button_new();

  add_menu_actions(t);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), menu_bar);
  g_object_unref(model);

  gtk_button_set_icon_name(GTK_BUTTON(split), "view-dual-symbolic");
  gtk_widget_set_tooltip_text(split, "Split Editor (Ctrl+\\)");
  t->editors.split_button = GTK_TOGGLE_BUTTON(split);
  g_signal_connect(split, "toggled", G_CALLBACK(on_split_toggled), t);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), split);

  gtk_widget_set_tooltip_text(right_btn, "Toggle Right Sidebar");
  g_signal_connect_swapped(right_btn, "clicked", G_CALLBACK(on_toggle_right), t);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), right_btn);

  gtk_widget_set_tooltip_text(bottom_btn, "Toggle Bottom Panel (Ctrl+J)");
  g_signal_connect_swapped(bottom_btn, "clicked", G_CALLBACK(on_toggle_bottom), t);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), bottom_btn);

  gtk_widget_set_tooltip_text(left_btn, "Toggle Left Sidebar (Ctrl+B)");
  g_signal_connect_swapped(left_btn, "clicked", G_CALLBACK(on_toggle_left), t);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), left_btn);
  return header;
}

/* The window --------------------------------------------------------------------------------- */

void build_window(App *t, GtkApplication *app)
{
  GtkWidget *overlay = gtk_overlay_new();                        /* the picker (Ctrl+P) floats over the content */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); /* the panes, the status bar */

  /* Before the window: the theme, where the helpers are, and the sockets they connect to. */
  load_theme(t);
  watch_user_files(t); /* settings.ini, theme.css and the editor colors, edited by hand */
  gtk_source_init();
  find_files(t);
  snippets_init(t);
  start_socket(t);
  plugins_init(t);
  gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()), t->paths.icons);
  gtk_window_set_default_icon_name("io.github.tcode");

  t->window = GTK_WINDOW(gtk_application_window_new(app));
  gtk_window_set_default_size(t->window, 1400, 900);
  add_project_actions(t);
  gtk_window_set_titlebar(t->window, header_bar(t));
  g_signal_connect(t->window, "close-request", G_CALLBACK(on_close_request), t);
  g_signal_connect(t->window, "destroy", G_CALLBACK(on_window_destroy), t);
  GSimpleAction *reset = g_simple_action_new("reset-panels", NULL);
  g_signal_connect_swapped(reset, "activate", G_CALLBACK(reset_panel_layout), t);
  g_action_map_add_action(G_ACTION_MAP(t->window), G_ACTION(reset));
  g_object_unref(reset);
  update_title(t);

  create_shell_pane(t);
  create_groups(t);
  create_docks(t);

  /* The editors above the bottom dock bar. */
  make_card(GTK_WIDGET(t->editors.area));
  t->layout.vpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_VERTICAL));
  gtk_paned_set_start_child(t->layout.vpaned, GTK_WIDGET(t->editors.area));
  gtk_paned_set_end_child(t->layout.vpaned, t->layout.bottom);
  gtk_paned_set_resize_end_child(t->layout.vpaned, FALSE);
  gtk_paned_set_shrink_start_child(t->layout.vpaned, TRUE);
  gtk_paned_set_shrink_end_child(t->layout.vpaned, TRUE);
  g_signal_connect(t->layout.vpaned, "notify::position", G_CALLBACK(on_vpaned_position), t);
  gtk_paned_set_position(t->layout.vpaned, 620);

  /* The right sidebar right of the center area. */
  t->layout.rpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_paned_set_start_child(t->layout.rpaned, GTK_WIDGET(t->layout.vpaned));
  gtk_paned_set_end_child(t->layout.rpaned, t->layout.right);
  gtk_paned_set_resize_end_child(t->layout.rpaned, FALSE);
  gtk_paned_set_shrink_start_child(t->layout.rpaned, TRUE);
  gtk_paned_set_shrink_end_child(t->layout.rpaned, TRUE);
  g_signal_connect(t->layout.rpaned, "notify::position", G_CALLBACK(on_rpaned_position), t);
  gtk_paned_set_position(t->layout.rpaned, 840);

  /* The left sidebar left of them. */
  t->layout.hpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_paned_set_start_child(t->layout.hpaned, t->layout.left);
  gtk_paned_set_end_child(t->layout.hpaned, GTK_WIDGET(t->layout.rpaned));
  gtk_paned_set_resize_start_child(t->layout.hpaned, FALSE);
  gtk_paned_set_shrink_start_child(t->layout.hpaned, TRUE);
  gtk_paned_set_shrink_end_child(t->layout.hpaned, TRUE);
  g_signal_connect(t->layout.hpaned, "notify::position", G_CALLBACK(on_hpaned_position), t);
  gtk_paned_set_position(t->layout.hpaned, 280);
  gtk_widget_set_vexpand(GTK_WIDGET(t->layout.hpaned), TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(t->layout.hpaned), "tcode-workspace");
  g_signal_connect(t->layout.hpaned, "notify::position", G_CALLBACK(on_layout_changed), t);
  g_signal_connect(t->layout.vpaned, "notify::position", G_CALLBACK(on_layout_changed), t);
  g_signal_connect(t->layout.rpaned, "notify::position", G_CALLBACK(on_layout_changed), t);

  gtk_box_append(GTK_BOX(content), GTK_WIDGET(t->layout.hpaned));
  gtk_box_append(GTK_BOX(content), status_bar(t));
  gtk_overlay_set_child(GTK_OVERLAY(overlay), content);
  picker_init(t, GTK_OVERLAY(overlay));
  gtk_window_set_child(t->window, overlay);

  /* The last session's sizes, then start the programs and show it. */
  add_shortcuts(t);
  session_apply_window(t);
  session_load_project(t);
  if (strcmp(t->project.root, g_get_home_dir()) != 0) /* home means no folder (see reopen_last_project) */
    session_add_recent(t);
  rebuild_recent_menu(t);
  start_shell_pane(t);
  start_status_updates(t);
  gtk_window_present(t->window);
  files_focus(t);
  updates_start(t); /* the release check (Settings' "Check for updates") starts from here */
}
