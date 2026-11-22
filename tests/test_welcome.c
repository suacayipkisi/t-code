/* Tests for the welcome window (src/dialogs/welcome.c): what it shows with and without recent folders,
 * and that picking a folder hands it over once and closes the window, while closing it picks
 * nothing. Needs a display; skipped without one. */
#include "dialogs/welcome.c"

#include "util.h"
#include "widgets.h"

static char *picked;
static int picks;

static void on_chosen(const char *folder, gpointer data)
{
  g_assert_true(data == &picks);
  g_set_str(&picked, folder);
  picks++;
}

static void reset(void)
{
  g_clear_pointer(&picked, g_free);
  picks = 0;
}

static GtkWidget *child(GtkWindow *window)
{
  return gtk_window_get_child(window);
}

/* The first button labelled text under root. */
static GtkWidget *find_button(GtkWidget *root, const char *text)
{
  if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), text) == 0)
    return root;
  for (GtkWidget *c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_button(c, text);
    if (found)
      return found;
  }
  return NULL;
}

/* First start: no recent folders, a line instead of the list; Open Folder is the default; the home
 * folder button picks the home folder and closes the window. */
static void test_first_start(void)
{
  GtkWindow *window = show_welcome(NULL, NULL, on_chosen, &picks);
  GtkWidget *home;

  reset();
  g_object_ref(window);
  g_assert_null(find_widget(child(window), GTK_TYPE_LIST_BOX, NULL, NULL));
  g_assert_true(gtk_window_get_default_widget(window) == find_button(child(window), "_Open Folder…"));
  home = find_button(child(window), "Start in _Home Folder");
  g_assert_nonnull(home);
  g_signal_emit_by_name(home, "clicked");
  g_assert_cmpint(picks, ==, 1);
  g_assert_cmpstr(picked, ==, g_get_home_dir());
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window))); /* destroyed */
  g_signal_emit_by_name(home, "clicked");                     /* once only */
  g_assert_cmpint(picks, ==, 1);
  g_object_unref(window);
}

/* Recent folders: only the ones that still exist are listed, in order; a click picks one. */
static void test_recent(void)
{
  char *dir = g_dir_make_tmp("t-code-welcome-XXXXXX", NULL);
  char *a = g_build_filename(dir, "alpha", NULL), *b = g_build_filename(dir, "beta", NULL);
  char *gone = g_build_filename(dir, "gone", NULL);
  const char *const recent[] = { a, gone, b, NULL };
  GtkWindow *window;
  GtkListBox *list;

  reset();
  g_mkdir(a, 0700);
  g_mkdir(b, 0700);
  window = show_welcome(NULL, recent, on_chosen, &picks);
  list = GTK_LIST_BOX(find_widget(child(window), GTK_TYPE_LIST_BOX, NULL, NULL));
  g_assert_nonnull(list);
  g_assert_cmpstr(gtk_widget_get_tooltip_text(GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0))), ==, a);
  g_assert_cmpstr(gtk_widget_get_tooltip_text(GTK_WIDGET(gtk_list_box_get_row_at_index(list, 1))), ==, b);
  g_assert_null(gtk_list_box_get_row_at_index(list, 2));

  g_signal_emit_by_name(list, "row-activated", gtk_list_box_get_row_at_index(list, 1));
  g_assert_cmpint(picks, ==, 1);
  g_assert_cmpstr(picked, ==, b);

  g_rmdir(a);
  g_rmdir(b);
  g_rmdir(dir);
  g_free(a);
  g_free(b);
  g_free(gone);
  g_free(dir);
}

/* All the recent folders gone: the line again, not an empty list. Closed: nothing picked. */
static void test_close(void)
{
  const char *const recent[] = { "/nonexistent/t-code-welcome", NULL };
  GtkWindow *window;

  reset();
  window = show_welcome(NULL, recent, on_chosen, &picks);
  g_assert_null(find_widget(child(window), GTK_TYPE_LIST_BOX, NULL, NULL));
  gtk_window_close(window);
  run_for(50);
  g_assert_cmpint(picks, ==, 0);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  ignore_gtk_noise();
  if (!gtk_init_check()) {
    g_test_skip("no display");
    return g_test_run();
  }
  g_test_add_func("/welcome/first-start", test_first_start);
  g_test_add_func("/welcome/recent", test_recent);
  g_test_add_func("/welcome/close", test_close);
  return g_test_run();
}
