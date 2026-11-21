/*
 * The second window (see secondwindow.h): a title bar over a tree of tab groups of its own, with the
 * main window's shortcuts.
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
  GtkNotebook *main_group = window_first_group(t, 0);

  for (int g = 0; !t->closing && g < group_count(t); g++) {
    GtkNotebook *group = group_nth(t, g);
    if (group_window(t, group) != 1)
      continue;
    while (gtk_notebook_get_n_pages(group) > 0)
      move_tab(tab_of(gtk_notebook_get_nth_page(group, 0)), main_group);
  }
  return FALSE;
}

static void on_destroy(GtkWidget *window G_GNUC_UNUSED, App *t)
{
  t->editors.second = NULL;
  forget_window_groups(t);
  if (!t->closing)
    focus_current_tab(t);
}

void show_second_window(App *t)
{
  GtkWindow *window;
  GtkWidget *view;

  if (t->editors.second)
    return;
  window = GTK_WINDOW(gtk_application_window_new(gtk_window_get_application(t->window)));
  t->editors.second = window;
  view = create_window_groups(t);
  gtk_window_set_title(window, gtk_window_get_title(t->window));
  gtk_window_set_titlebar(window, gtk_header_bar_new());
  gtk_window_set_default_size(window, 1000, 750);
  make_card(view);
  gtk_widget_add_css_class(view, "tcode-workspace");
  gtk_window_set_child(window, view);
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
    move_tab(existing, window_first_group(t, 1));
    select_tab(existing);
    return;
  }
  t->editors.active = window_first_group(t, 1);
  open_file(t, path);
}

void move_to_new_window(App *t)
{
  Tab *tab = current_tab(t);

  if (!tab || group_window(t, tab_group(tab)) == 1)
    return;
  show_second_window(t);
  keep_tab(tab);
  move_tab(tab, window_first_group(t, 1));
  select_tab(tab);
}
