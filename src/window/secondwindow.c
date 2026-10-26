/*
 * The second window (see secondwindow.h): a title bar over one tab group, with the main window's
 * shortcuts.
 */
#include "window/secondwindow.h"

#include "editor/editor.h"
#include "editor/tabs.h"
#include "window/commands.h"
#include "window/theme.h"

/* Closed by the user: its tabs go back to the main window (nothing is lost or asked). When t-code
 * quits, they go with it. */
static gboolean on_close_request(GtkWindow *window G_GNUC_UNUSED, App *t)
{
  GtkNotebook *group = t->editors.groups[WINDOW_GROUP];

  while (!t->closing && group && gtk_notebook_get_n_pages(group) > 0)
    move_tab(tab_of(gtk_notebook_get_nth_page(group, 0)), 0);
  return FALSE;
}

static void on_destroy(GtkWidget *window G_GNUC_UNUSED, App *t)
{
  t->editors.second = NULL;
  t->editors.groups[WINDOW_GROUP] = NULL;
  if (t->editors.active == WINDOW_GROUP)
    t->editors.active = 0;
  if (!t->closing)
    focus_current_tab(t);
}

void show_second_window(App *t)
{
  GtkWindow *window;
  GtkWidget *group;

  if (t->editors.second)
    return;
  window = GTK_WINDOW(gtk_application_window_new(gtk_window_get_application(t->window)));
  group = GTK_WIDGET(new_tab_group(t));
  t->editors.second = window;
  t->editors.groups[WINDOW_GROUP] = GTK_NOTEBOOK(group);
  gtk_window_set_title(window, gtk_window_get_title(t->window));
  gtk_window_set_titlebar(window, gtk_header_bar_new());
  gtk_window_set_default_size(window, 1000, 750);
  gtk_widget_set_vexpand(tab_group_widget(GTK_NOTEBOOK(group)), TRUE);
  make_card(tab_group_widget(GTK_NOTEBOOK(group)));
  gtk_widget_add_css_class(tab_group_widget(GTK_NOTEBOOK(group)), "tcode-workspace");
  gtk_window_set_child(window, tab_group_widget(GTK_NOTEBOOK(group)));
  add_shortcuts_to(t, window);
  g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), t);
  g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), t);
  gtk_window_present(window);
}

void open_in_new_window(App *t, const char *path)
{
  Tab *existing = find_tab(t, TAB_EDITOR, path);

  show_second_window(t);
  if (existing) {
    keep_tab(existing);
    move_tab(existing, WINDOW_GROUP);
    select_tab(existing);
    return;
  }
  t->editors.active = WINDOW_GROUP;
  open_file(t, path);
}

void move_to_new_window(App *t)
{
  Tab *tab = current_tab(t);

  if (!tab || tab_group(tab) == WINDOW_GROUP)
    return;
  show_second_window(t);
  keep_tab(tab);
  move_tab(tab, WINDOW_GROUP);
  select_tab(tab);
}
