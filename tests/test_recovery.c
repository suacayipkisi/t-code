/* Tests for crash recovery journaling in src/editor/recovery.c */
#include "editor/recovery.h"
#include "editor/editorview.h"
#include "editor/tabs.h"

#include <glib.h>
#include <glib/gstdio.h>

/* clang-format off */
void status_say(App *t, const char *message) { (void)t; (void)message; }
/* clang-format on */

/* The App's editors, for recovery_forget_unsaved(). */
static Tab *tabs[4];
static int n_tabs;

int tab_count(App *t)
{
  (void)t;
  return n_tabs;
}

Tab *tab_nth(App *t, int i)
{
  (void)t;
  return tabs[i];
}

static App app;
static char *folder; /* a file per test goes here */

/* An editor showing path with text in it, modified or not. */
static Tab *editor(const char *name, const char *text, gboolean modified)
{
  Tab *tab = g_new0(Tab, 1);

  tab->app = &app;
  tab->kind = TAB_EDITOR;
  tab->path = g_build_filename(folder, name, NULL);
  tab->buffer = gtk_source_buffer_new(NULL);
  tab->view = g_new0(EditorView, 1);
  tab->view->tab = tab;
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(tab->buffer), text, -1);
  gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(tab->buffer), modified);
  tab->modified = modified;
  return tab;
}

static void editor_free(Tab *tab)
{
  if (tab->view->recovery_id)
    g_source_remove(tab->view->recovery_id);
  recovery_discard_snapshot(tab->path);
  g_unlink(tab->path);
  g_object_unref(tab->buffer);
  g_free(tab->view);
  g_free(tab->path);
  g_free(tab);
}

static char *journal_of(const char *path)
{
  char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path, -1);
  char *journal = g_build_filename(g_get_user_cache_dir(), "t-code", "recovery", hash, NULL);

  g_free(hash);
  return journal;
}

/* A journal as an earlier session left it, stamped at time with text. */
static void write_journal(const char *path, gint64 time, const char *text)
{
  char *journal = journal_of(path), *dir = g_path_get_dirname(journal);
  char *content = g_strdup_printf("TCODE_RECOVERY_V1 %" G_GINT64_FORMAT " %s\n%s", time, path, text);

  g_mkdir_with_parents(dir, 0700);
  g_assert_true(g_file_set_contents(journal, content, -1, NULL));
  g_free(content);
  g_free(dir);
  g_free(journal);
}

static gboolean has_journal(const char *path)
{
  char *journal = journal_of(path);
  gboolean found = g_file_test(journal, G_FILE_TEST_EXISTS);

  g_free(journal);
  return found;
}

static void test_lifecycle(void)
{
  Tab *tab = editor("lifecycle.c", "int x;\n", FALSE);

  recovery_discard_snapshot(tab->path); /* no journal yet: nothing happens */
  g_assert_false(has_journal(tab->path));
  write_journal(tab->path, g_get_real_time() / G_USEC_PER_SEC + 10, "int recovered = 42;\n");
  g_assert_true(has_journal(tab->path));
  recovery_discard_snapshot(tab->path);
  g_assert_false(has_journal(tab->path));
  editor_free(tab);
}

/* An unmodified editor is not journaled; a modified one is, and the journal holds its text. */
static void test_snapshot(void)
{
  Tab *clean = editor("clean.c", "int a;\n", FALSE), *dirty = editor("dirty.c", "int b;\n", TRUE);
  char *journal = journal_of(dirty->path), *content = NULL;

  recovery_save_snapshot(clean);
  g_assert_false(has_journal(clean->path));
  g_assert_false(clean->view->journaled);
  recovery_save_snapshot(dirty);
  g_assert_true(dirty->view->journaled);
  g_assert_true(g_file_get_contents(journal, &content, NULL, NULL));
  g_assert_true(g_str_has_prefix(content, "TCODE_RECOVERY_V1 "));
  g_assert_true(g_str_has_suffix(content, "\nint b;\n"));
  g_free(content);
  g_free(journal);
  editor_free(clean);
  editor_free(dirty);
}

/* Saved or "Don't Save": this session's journal goes, and a snapshot still waiting is cancelled. */
static void test_forget(void)
{
  Tab *tab = editor("forget.c", "int c;\n", TRUE);

  recovery_save_snapshot(tab);
  g_assert_true(has_journal(tab->path));
  recovery_schedule_snapshot(tab);
  g_assert_cmpuint(tab->view->recovery_id, !=, 0);
  recovery_forget(tab);
  g_assert_false(has_journal(tab->path));
  g_assert_false(tab->view->journaled);
  g_assert_cmpuint(tab->view->recovery_id, ==, 0);
  editor_free(tab);
}

/* A journal an earlier session left (a crash) is not this session's to delete: it waits for the
 * bar's Restore or Discard. */
static void test_forget_keeps_earlier_session(void)
{
  Tab *tab = editor("earlier.c", "int d;\n", FALSE);

  write_journal(tab->path, g_get_real_time() / G_USEC_PER_SEC + 10, "int lost;\n");
  recovery_forget(tab);
  g_assert_true(has_journal(tab->path));
  editor_free(tab);
}

/* "Close t-code?" / "Open another folder?" answered yes: every modified editor's journal goes. */
static void test_forget_unsaved(void)
{
  Tab *dirty = editor("unsaved.c", "int e;\n", TRUE), *clean = editor("kept.c", "int f;\n", FALSE);

  recovery_save_snapshot(dirty);
  write_journal(clean->path, g_get_real_time() / G_USEC_PER_SEC + 10, "int crash;\n");
  tabs[0] = dirty;
  tabs[1] = clean;
  n_tabs = 2;
  recovery_forget_unsaved(&app);
  n_tabs = 0;
  g_assert_false(has_journal(dirty->path));
  g_assert_true(has_journal(clean->path));
  editor_free(dirty);
  editor_free(clean);
}

/* A journal with the text the file has now (saved within the journal's second, changes undone),
 * or older than the file, is dropped without a bar. */
static void test_nothing_to_recover(void)
{
  Tab *same = editor("same.c", "int g;\n", FALSE), *stale = editor("stale.c", "int h;\n", FALSE);

  g_assert_true(g_file_set_contents(same->path, "int g;\n", -1, NULL));
  write_journal(same->path, g_get_real_time() / G_USEC_PER_SEC + 10, "int g;\n");
  recovery_check_and_prompt(same);
  g_assert_false(has_journal(same->path));
  g_assert_null(same->view->recovery_bar);

  g_assert_true(g_file_set_contents(stale->path, "int h;\n", -1, NULL));
  write_journal(stale->path, 1, "int old;\n");
  recovery_check_and_prompt(stale);
  g_assert_false(has_journal(stale->path));
  g_assert_null(stale->view->recovery_bar);
  editor_free(same);
  editor_free(stale);
}

int main(int argc, char **argv)
{
  char *home = g_dir_make_tmp("t-code-recovery-XXXXXX", NULL);
  char *cache = g_build_filename(home, "cache", NULL);
  int status;

  g_setenv("XDG_CACHE_HOME", cache, TRUE); /* not the real ~/.cache */
  folder = g_build_filename(home, "project", NULL);
  g_mkdir_with_parents(folder, 0700);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/recovery/lifecycle", test_lifecycle);
  g_test_add_func("/recovery/snapshot", test_snapshot);
  g_test_add_func("/recovery/forget", test_forget);
  g_test_add_func("/recovery/forget-keeps-earlier-session", test_forget_keeps_earlier_session);
  g_test_add_func("/recovery/forget-unsaved", test_forget_unsaved);
  g_test_add_func("/recovery/nothing-to-recover", test_nothing_to_recover);
  status = g_test_run();

  char *recovery = g_build_filename(cache, "t-code", "recovery", NULL), *tcode = g_path_get_dirname(recovery);
  g_rmdir(recovery);
  g_rmdir(tcode);
  g_rmdir(cache);
  g_rmdir(folder);
  g_rmdir(home);
  g_free(tcode);
  g_free(recovery);
  g_free(folder);
  g_free(cache);
  g_free(home);
  return status;
}
