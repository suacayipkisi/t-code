/* Tests for the About window (src/dialogs/about.c): the license text, the versions of the programs t-code
 * runs filled in once they answer, and closing the window before they do. Needs a display;
 * skipped without one. */
#include "dialogs/about.c"

static App app;

static GtkAboutDialog *open_about(void)
{
  GList *before = gtk_window_list_toplevels(), *after;
  GtkAboutDialog *dialog = NULL;

  show_about(&app);
  after = gtk_window_list_toplevels();
  for (GList *l = after; l; l = l->next)
    if (GTK_IS_ABOUT_DIALOG(l->data) && !g_list_find(before, l->data))
      dialog = l->data;
  g_list_free(before);
  g_list_free(after);
  return dialog;
}

static gboolean answered(GtkAboutDialog *dialog)
{
  return !strstr(gtk_about_dialog_get_system_information(dialog), "…");
}

static void test_contents(void)
{
  GtkAboutDialog *dialog = open_about();
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  const char *info;

  g_assert_nonnull(dialog);
  g_assert_cmpstr(gtk_about_dialog_get_version(dialog), ==, TCODE_VERSION);
  int width, height;
  gtk_window_get_default_size(GTK_WINDOW(dialog), &width, &height);
  g_assert_cmpint(width, ==, ABOUT_WIDTH); /* not GTK's small default */
  g_assert_cmpint(height, ==, ABOUT_HEIGHT);
  g_assert_nonnull(strstr(gtk_about_dialog_get_license(dialog), "MIT License"));
  g_assert_nonnull(strstr(gtk_about_dialog_get_license(dialog), "Permission is hereby granted"));

  /* Shown again while open: the same window. */
  show_about(&app);
  g_assert_true(open_about() == NULL);

  while (!answered(dialog) && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, TRUE);
  info = gtk_about_dialog_get_system_information(dialog);
  g_assert_true(answered(dialog));
  g_test_message("%s", info);
  g_assert_nonnull(strstr(info, "t-code " TCODE_VERSION));
  g_assert_nonnull(strstr(info, "/test/styles"));
  /* git is installed wherever t-code is built; its line is "git      2.x", not the program name. */
  g_assert_nonnull(strstr(info, "  git      2."));
  gtk_window_destroy(GTK_WINDOW(dialog));
}

/* Closed before the programs answer: their answers arrive later and must not touch the window. */
static void test_close_early(void)
{
  GtkAboutDialog *dialog = open_about();
  gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

  g_assert_nonnull(dialog);
  gtk_window_destroy(GTK_WINDOW(dialog));
  while (g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  /* And it opens again as a new window. */
  dialog = open_about();
  g_assert_nonnull(dialog);
  gtk_window_destroy(GTK_WINDOW(dialog));
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  if (!gtk_init_check()) {
    g_test_skip("no display");
    return g_test_run();
  }
  app.window = GTK_WINDOW(gtk_window_new());
  app.paths.styles = "/test/styles";
  g_test_add_func("/about/contents", test_contents);
  g_test_add_func("/about/close-early", test_close_early);
  return g_test_run();
}
