/*
 * Open Tabs (see opentabs.h): a heading over a list box made again (in an idle) whenever the tabs
 * change; a handful of rows, so nothing is kept between.
 */
#include "files/opentabs.h"

#include "editor/editor.h"
#include "editor/tabmenu.h"
#include "editor/tabs.h"
#include "files/icons.h"
#include "popups/contextmenu.h"
#include "window/palette.h"

#include <string.h>

#define MAX_HEIGHT 200 /* pixels: about nine tabs; more scroll */

static struct {
  App *app;
  GtkWidget *section, *chevron, *scrolled;
  GtkListBox *list;
  guint refill_id;
} open_tabs;

static const char *const css =
    ".tcode-opentabs .section { font-size: 0.8em; font-weight: bold; color: var(--tc-fg); padding: 4px 10px 4px 2px; }"
    ".tcode-opentabs .section .chevron { -gtk-icon-size: 12px; margin-right: 4px; }"
    ".tcode-opentabs .group { font-size: 0.75em; font-weight: bold; color: var(--tc-fg-faint); padding: 4px 0 0 20px; }"
    ".tcode-opentabs list { background: transparent; }"
    ".tcode-opentabs list row { min-height: 22px; padding: 0 6px 0 20px; color: var(--tc-fg); }"
    ".tcode-opentabs list row:hover { background: var(--tc-hover); }"
    ".tcode-opentabs list row:selected { background: var(--tc-inactive-selection); }"
    ".tcode-opentabs .dir { color: var(--tc-fg-faint); font-size: 0.9em; margin-left: 6px; }"
    ".tcode-opentabs label.temporary { font-style: italic; }"
    ".tcode-opentabs .close { min-height: 0; min-width: 0; padding: 0 4px; background: none; box-shadow: none;"
    "  opacity: 0; }"
    ".tcode-opentabs row:hover .close, .tcode-opentabs .close.modified { opacity: 1; }"
    ".tcode-opentabs .close:hover { background: color-mix(in srgb, var(--tc-fg) 12%, transparent); }"
    ".tcode-opentabs { border-bottom: 1px solid var(--tc-border); padding-bottom: 2px; }";

/* Whether tab is still open (a click may come before the list follows a close). */
static gboolean is_open(Tab *tab)
{
  for (int i = 0; i < tab_count(open_tabs.app); i++)
    if (tab_nth(open_tabs.app, i) == tab)
      return TRUE;
  return FALSE;
}

/* Closes a tab the way its × in the tab strip does: an editor asks about unsaved changes. */
static void close_one(Tab *tab)
{
  if (!is_open(tab))
    return;
  if (tab->kind == TAB_EDITOR)
    quit_editor(tab);
  else
    remove_tab(tab);
}

static void on_row_activated(GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  Tab *tab = g_object_get_data(G_OBJECT(row), "t-code-tab");

  if (tab && is_open(tab))
    select_tab(tab);
}

static void on_middle_click(GtkGestureClick *click, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED,
                            gpointer data G_GNUC_UNUSED)
{
  GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click));

  close_one(g_object_get_data(G_OBJECT(row), "t-code-tab"));
}

static void on_close_clicked(GtkButton *button, gpointer data G_GNUC_UNUSED)
{
  close_one(g_object_get_data(G_OBJECT(button), "t-code-tab"));
}

/* A right click on a tab's row: the tab's menu, as in the tab strip. */
static void on_row_menu(GtkWidget *row, double x, double y, gpointer data G_GNUC_UNUSED)
{
  Tab *tab = g_object_get_data(G_OBJECT(row), "t-code-tab");

  if (tab && is_open(tab))
    tab_menu(tab, row, x, y);
}

/* A label with text in color (Pango markup). */
static GtkWidget *colored(const char *text, const char *color)
{
  GtkWidget *label = gtk_label_new(NULL);
  char *markup = g_markup_printf_escaped("<span foreground=\"%s\">%s</span>", color, text);

  gtk_label_set_markup(GTK_LABEL(label), markup);
  g_free(markup);
  return label;
}

/* A tab's row: its file's icon, its name as its tab shows it, its folder (relative to the project),
 * and ● (unsaved) or × to close. */
static GtkWidget *tab_row(Tab *tab)
{
  App *t = open_tabs.app;
  GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  char *base = g_path_get_basename(tab->path),
       *dir = g_path_get_dirname(tab->compare.with ? tab->compare.with : tab->path);
  const char *shown = tab->label ? gtk_label_get_text(tab->label) : base;
  const FileIcon *icon = file_icon(base, FALSE, FALSE);
  GtkWidget *name = gtk_label_new(g_str_has_prefix(shown, "● ") ? shown + strlen("● ") : shown);
  GtkWidget *where, *close = gtk_button_new_with_label(tab->modified ? "●" : "×");
  GtkGesture *middle = gtk_gesture_click_new();
  const char *relative = dir;
  size_t root = strlen(t->project.root);

  if (strncmp(dir, t->project.root, root) == 0 && (dir[root] == '/' || !dir[root]))
    relative = dir[root] ? dir + root + 1 : "";
  where = gtk_label_new(relative);
  gtk_box_append(GTK_BOX(box), colored(icon->glyph, icon->color));
  gtk_label_set_xalign(GTK_LABEL(name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  if (tab->temporary)
    gtk_widget_add_css_class(name, "temporary");
  gtk_box_append(GTK_BOX(box), name);
  gtk_widget_add_css_class(where, "dir");
  gtk_label_set_xalign(GTK_LABEL(where), 0);
  gtk_label_set_ellipsize(GTK_LABEL(where), PANGO_ELLIPSIZE_START);
  gtk_widget_set_hexpand(where, TRUE);
  gtk_box_append(GTK_BOX(box), where);
  gtk_widget_add_css_class(close, "close");
  if (tab->modified)
    gtk_widget_add_css_class(close, "modified");
  gtk_widget_set_focusable(close, FALSE);
  gtk_widget_set_tooltip_text(close, "Close");
  g_object_set_data(G_OBJECT(close), "t-code-tab", tab);
  g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), NULL);
  gtk_box_append(GTK_BOX(box), close);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_widget_set_tooltip_text(row, tab->path);
  g_object_set_data(G_OBJECT(row), "t-code-tab", tab);
  context_menu_attach(row, on_row_menu, NULL);
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), GDK_BUTTON_MIDDLE);
  g_signal_connect(middle, "released", G_CALLBACK(on_middle_click), NULL);
  gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(middle));
  g_free(dir);
  g_free(base);
  return row;
}

/* A group's heading, when more than one group has tabs. */
static GtkWidget *group_row(int group)
{
  static const char *const names[GROUPS] = { "GROUP 1", "GROUP 2", "NEW WINDOW" };
  GtkWidget *row = gtk_list_box_row_new(), *label = gtk_label_new(names[group]);

  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_add_css_class(label, "group");
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
  return row;
}

/* Makes the list again from the tabs, group by group in their order. */
static gboolean refill(gpointer data G_GNUC_UNUSED)
{
  App *t = open_tabs.app;

  open_tabs.refill_id = 0;
  if (t->closing) /* asked for before the window started closing: its groups may be gone */
    return G_SOURCE_REMOVE;

  Tab *current = current_tab(t);
  int used = 0, n = tab_count(t);

  gtk_list_box_remove_all(open_tabs.list);
  for (int g = 0; g < GROUPS; g++)
    for (int i = 0; i < n; i++)
      if (tab_group(tab_nth(t, i)) == g) {
        used++;
        break;
      }
  for (int g = 0; g < GROUPS; g++) {
    gboolean heading = FALSE;
    for (int i = 0; i < n; i++) {
      Tab *tab = tab_nth(t, i);
      if (tab_group(tab) != g)
        continue;
      if (used > 1 && !heading) {
        gtk_list_box_append(open_tabs.list, group_row(g));
        heading = TRUE;
      }
      GtkWidget *row = tab_row(tab);
      gtk_list_box_append(open_tabs.list, row);
      if (tab == current)
        gtk_list_box_select_row(open_tabs.list, GTK_LIST_BOX_ROW(row));
    }
  }
  gtk_widget_set_visible(open_tabs.scrolled, n > 0 && open_tabs_shown());
  return G_SOURCE_REMOVE;
}

void open_tabs_changed(App *t)
{
  if (open_tabs.list && !t->closing && !open_tabs.refill_id)
    open_tabs.refill_id = g_idle_add(refill, NULL);
}

gboolean open_tabs_shown(void)
{
  return !open_tabs.chevron || g_object_get_data(G_OBJECT(open_tabs.chevron), "t-code-folded") == NULL;
}

void open_tabs_show(gboolean shown)
{
  if (!open_tabs.chevron)
    return;
  g_object_set_data(G_OBJECT(open_tabs.chevron), "t-code-folded", shown ? NULL : GINT_TO_POINTER(1));
  gtk_image_set_from_icon_name(GTK_IMAGE(open_tabs.chevron), shown ? "pan-down-symbolic" : "pan-end-symbolic");
  gtk_widget_set_visible(open_tabs.scrolled, shown && tab_count(open_tabs.app) > 0);
}

/* A click on the heading opens or folds the list. */
static void on_heading_click(GtkGestureClick *click, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                             double y G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  open_tabs_show(!open_tabs_shown());
}

GtkWidget *open_tabs_new(App *t)
{
  GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), *title = gtk_label_new("OPEN TABS");
  GtkGesture *click = gtk_gesture_click_new();

  palette_add_css(css);

  open_tabs.app = t;
  open_tabs.section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(open_tabs.section, "tcode-opentabs");
  open_tabs.chevron = gtk_image_new_from_icon_name("pan-down-symbolic");
  gtk_widget_add_css_class(open_tabs.chevron, "chevron");
  gtk_widget_add_css_class(head, "section");
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  gtk_box_append(GTK_BOX(head), open_tabs.chevron);
  gtk_box_append(GTK_BOX(head), title);
  g_signal_connect(click, "released", G_CALLBACK(on_heading_click), NULL);
  gtk_widget_add_controller(head, GTK_EVENT_CONTROLLER(click));

  open_tabs.list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(open_tabs.list, GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(open_tabs.list, TRUE);
  g_signal_connect(open_tabs.list, "row-activated", G_CALLBACK(on_row_activated), NULL);
  open_tabs.scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(open_tabs.scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(open_tabs.scrolled), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(open_tabs.scrolled), MAX_HEIGHT);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(open_tabs.scrolled), GTK_WIDGET(open_tabs.list));
  gtk_widget_set_visible(open_tabs.scrolled, FALSE); /* no tabs yet */

  gtk_box_append(GTK_BOX(open_tabs.section), head);
  gtk_box_append(GTK_BOX(open_tabs.section), open_tabs.scrolled);
  return open_tabs.section;
}
