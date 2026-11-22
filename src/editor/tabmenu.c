/*
 * A tab's right-click menu (see tabmenu.h). The items run once the menu has closed: each looks
 * the tab up among the open ones first, in case it went meanwhile.
 */
#include "editor/tabmenu.h"

#include "dialogs/templates.h"
#include "editor/editor.h"
#include "editor/preview/preview.h"
#include "editor/tabs.h"
#include "files/files.h"
#include "popups/contextmenu.h"
#include "window/secondwindow.h"
#include "window/window.h"

#include <string.h>

/* Whether tab is still open. */
static gboolean is_open(App *t, Tab *tab)
{
  for (int i = 0; i < tab_count(t); i++)
    if (tab_nth(t, i) == tab)
      return TRUE;
  return FALSE;
}

/* Closes a tab the way its × does: an editor asks about unsaved changes. */
static void close_one(Tab *tab)
{
  if (tab->kind == TAB_EDITOR) {
    select_tab(tab);
    quit_editor(tab);
  } else {
    remove_tab(tab);
  }
}

/* The menu's tab and its App (the tab may be gone when an item runs). */
typedef struct {
  App *app;
  Tab *tab;
} Target;

static Tab *target_tab(gpointer data)
{
  Target *target = data;

  return is_open(target->app, target->tab) ? target->tab : NULL;
}

/* Closes the tabs of tab's group that keep() picks (tab itself never). */
static void close_others(Tab *tab, gboolean (*keep)(Tab *tab, Tab *other))
{
  App *t = tab->app;
  GPtrArray *closing = g_ptr_array_new();

  for (int i = 0; i < tab_count(t); i++) {
    Tab *other = tab_nth(t, i);
    if (other != tab && tab_group(other) == tab_group(tab) && !keep(tab, other))
      g_ptr_array_add(closing, other);
  }
  for (guint i = 0; i < closing->len; i++)
    if (is_open(t, closing->pdata[i]))
      close_one(closing->pdata[i]);
  g_ptr_array_free(closing, TRUE);
}

static gboolean keep_none(Tab *tab G_GNUC_UNUSED, Tab *other G_GNUC_UNUSED)
{
  return FALSE;
}

static gboolean keep_left(Tab *tab, Tab *other)
{
  return tab_position(other) < tab_position(tab);
}

static void on_close(gpointer data)
{
  Tab *tab = target_tab(data);

  if (tab)
    close_one(tab);
}

static void on_close_others(gpointer data)
{
  Tab *tab = target_tab(data);

  if (tab)
    close_others(tab, keep_none);
}

static void on_close_right(gpointer data)
{
  Tab *tab = target_tab(data);

  if (tab)
    close_others(tab, keep_left);
}

static void on_close_all(gpointer data)
{
  Tab *tab = target_tab(data);

  if (tab) {
    close_others(tab, keep_none);
    if (is_open(tab->app, tab))
      close_one(tab);
  }
}

static void on_keep(gpointer data)
{
  Tab *tab = target_tab(data);

  if (tab)
    keep_tab(tab);
}

/* To the group beside its own (splitting when there is none), or from the second window back. */
static void on_other_group(gpointer data)
{
  Tab *tab = target_tab(data);
  App *t;

  if (!tab)
    return;
  t = tab->app;
  keep_tab(tab);
  if (group_window(t, tab_group(tab)) == 0) {
    t->editors.active = tab_group(tab);
    move_tab(tab, side_group(t));
  } else {
    move_tab(tab, window_first_group(t, 0));
  }
  select_tab(tab);
}

/* Into a new group beside its own. */
static void split_off(gpointer data, SplitSide side)
{
  Tab *tab = target_tab(data);

  if (!tab)
    return;
  keep_tab(tab);
  move_tab(tab, split_group(tab_group(tab), side));
  select_tab(tab);
}

static void on_split_right(gpointer data)
{
  split_off(data, SPLIT_RIGHT);
}

static void on_split_down(gpointer data)
{
  split_off(data, SPLIT_DOWN);
}

static void on_new_window(gpointer data)
{
  Tab *tab = target_tab(data);

  if (tab) {
    select_tab(tab);
    move_to_new_window(tab->app);
  }
}

static void on_stop_comparing(gpointer data)
{
  Tab *tab = target_tab(data);

  if (tab && tab->compare.old_side)
    remove_tab(tab); /* old versions: nothing to save */
}

static void on_preview(gpointer data)
{
  Tab *tab = target_tab(data);

  if (tab)
    open_preview(tab->app, tab->path);
}

static void on_copy_path(gpointer data)
{
  Tab *tab = target_tab(data);

  if (tab)
    copy_to_clipboard(tab->path);
}

/* The path from the project's folder (the whole path outside it). */
static void on_copy_relative(gpointer data)
{
  Tab *tab = target_tab(data);
  const char *root = tab ? tab->app->project.root : NULL;
  size_t len = root ? strlen(root) : 0;

  if (tab)
    copy_to_clipboard(strncmp(tab->path, root, len) == 0 && tab->path[len] == '/' ? tab->path + len + 1 : tab->path);
}

static void on_reveal(gpointer data)
{
  Tab *tab = target_tab(data);

  if (tab) {
    show_sidebar_page(tab->app, "files");
    files_reveal(tab->app, tab->path);
  }
}

static void on_save_template(gpointer data)
{
  Tab *tab = target_tab(data);

  if (tab)
    show_save_template(tab->app, tab);
}

void tab_menu(Tab *tab, GtkWidget *widget, double x, double y)
{
  MenuItem items[18];
  Target *target = g_new(Target, 1);
  GtkNotebook *group = tab_group(tab);
  int n = 0, window = group_window(tab->app, group);

  target->app = tab->app;
  target->tab = tab;
  items[n++] = (MenuItem){ "Close", on_close };
  items[n++] = (MenuItem){ "Close Others", on_close_others };
  items[n++] = (MenuItem){ "Close to the Right", on_close_right };
  items[n++] = (MenuItem){ "Close All", on_close_all };
  items[n++] = (MenuItem){ NULL, NULL };
  if (tab->temporary)
    items[n++] = (MenuItem){ "Keep Open", on_keep };
  if (tab->kind == TAB_EDITOR && has_preview(tab->path) && !tab->compare.with)
    items[n++] = (MenuItem){ "Open Preview", on_preview };
  items[n++] = (MenuItem){ window == 0 ? "Move to Other Group" : "Move to Main Window", on_other_group };
  if (gtk_notebook_get_n_pages(group) > 1) {
    items[n++] = (MenuItem){ "Split Right", on_split_right };
    items[n++] = (MenuItem){ "Split Down", on_split_down };
  }
  if (window == 0)
    items[n++] = (MenuItem){ "Move into New Window", on_new_window };
  if (tab->compare.old_side)
    items[n++] = (MenuItem){ "Stop Comparing", on_stop_comparing };
  if (tab->kind != TAB_VIEW) { /* a plugin's view has no file */
    items[n++] = (MenuItem){ NULL, NULL };
    items[n++] = (MenuItem){ "Copy Path", on_copy_path };
    items[n++] = (MenuItem){ "Copy Relative Path", on_copy_relative };
  }
  if (g_file_test(tab->path, G_FILE_TEST_EXISTS) && !tab->compare.with)
    items[n++] = (MenuItem){ "Reveal in Files", on_reveal };
  if (tab->kind == TAB_EDITOR && !tab->compare.with)
    items[n++] = (MenuItem){ "Save as Template…", on_save_template };
  context_menu(widget, x, y, items, n, target, g_free);
}
