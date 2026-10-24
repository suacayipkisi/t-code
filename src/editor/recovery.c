/*
 * Crash Recovery: journals unsaved editor buffers and recovers them after a crash.
 */
#include "editor/recovery.h"

#include "editor/editor.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "window/statusbar.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define RECOVERY_DEBOUNCE_MS 1500

static char *recovery_dir(void)
{
  char *dir = g_build_filename(g_get_user_cache_dir(), "t-code", "recovery", NULL);
  g_mkdir_with_parents(dir, 0700);
  return dir;
}

static char *journal_path_for(const char *path)
{
  if (!path || !*path)
    return NULL;
  char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path, -1);
  char *dir = recovery_dir();
  char *journal = g_build_filename(dir, hash, NULL);
  g_free(dir);
  g_free(hash);
  return journal;
}

void recovery_discard_snapshot(const char *path)
{
  char *journal = journal_path_for(path);
  if (journal) {
    if (g_file_test(journal, G_FILE_TEST_EXISTS))
      g_unlink(journal);
    g_free(journal);
  }
}

void recovery_save_snapshot(Tab *tab)
{
  if (!tab || tab->kind != TAB_EDITOR || !tab->path || !tab->buffer || !tab->modified)
    return;
  if (!tab->view || tab->view->readonly || tab->view->loading)
    return;

  char *journal = journal_path_for(tab->path);
  if (!journal)
    return;

  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  char *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
  if (!text) {
    g_free(journal);
    return;
  }

  /* Header: TCODE_RECOVERY_V1 <timestamp> <path>\n */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  char *content = g_strdup_printf("TCODE_RECOVERY_V1 %" G_GINT64_FORMAT " %s\n%s", now, tab->path, text);

  if (g_file_set_contents(journal, content, -1, NULL))
    tab->view->journaled = TRUE;

  g_free(content);
  g_free(text);
  g_free(journal);
}

static gboolean on_snapshot_timer(gpointer data)
{
  Tab *tab = data;
  if (!tab || !tab->view)
    return G_SOURCE_REMOVE;
  tab->view->recovery_id = 0;
  if (tab->modified)
    recovery_save_snapshot(tab);
  return G_SOURCE_REMOVE;
}

void recovery_schedule_snapshot(Tab *tab)
{
  if (!tab || !tab->view || !tab->modified)
    return;
  if (tab->view->recovery_id)
    g_source_remove(tab->view->recovery_id);
  tab->view->recovery_id = g_timeout_add(RECOVERY_DEBOUNCE_MS, on_snapshot_timer, tab);
}

void recovery_forget(Tab *tab)
{
  if (!tab || tab->kind != TAB_EDITOR || !tab->view)
    return;
  if (tab->view->recovery_id) {
    g_source_remove(tab->view->recovery_id);
    tab->view->recovery_id = 0;
  }
  if (tab->view->journaled) {
    recovery_discard_snapshot(tab->path);
    tab->view->journaled = FALSE;
  }
}

void recovery_forget_unsaved(App *t)
{
  int n = tab_count(t);

  for (int i = 0; i < n; i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR && tab->modified)
      recovery_forget(tab);
  }
}

static char *read_journal_content(const char *journal, gint64 *out_time)
{
  char *raw = NULL;
  if (!g_file_get_contents(journal, &raw, NULL, NULL) || !raw)
    return NULL;

  if (!g_str_has_prefix(raw, "TCODE_RECOVERY_V1 ")) {
    g_free(raw);
    return NULL;
  }

  char *line_end = strchr(raw, '\n');
  if (!line_end) {
    g_free(raw);
    return NULL;
  }

  if (out_time) {
    char *p = raw + strlen("TCODE_RECOVERY_V1 ");
    *out_time = g_ascii_strtoll(p, NULL, 10);
  }

  char *body = g_strdup(line_end + 1);
  g_free(raw);
  return body;
}

static void on_restore(GtkButton *btn G_GNUC_UNUSED, gpointer data)
{
  Tab *tab = data;
  char *journal = journal_path_for(tab->path);
  if (!journal)
    return;

  gint64 stamp = 0;
  char *body = read_journal_content(journal, &stamp);
  g_free(journal);

  if (body) {
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(tab->buffer), body, -1);
    gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(tab->buffer), TRUE);
    tab->view->journaled = TRUE; /* the journal now stands for this buffer's changes */
    g_free(body);
    status_say(tab->app, "Unsaved changes restored from recovery journal");
  }

  if (tab->view->recovery_bar) {
    gtk_widget_set_visible(tab->view->recovery_bar, FALSE);
  }
}

static void on_discard(GtkButton *btn G_GNUC_UNUSED, gpointer data)
{
  Tab *tab = data;
  recovery_discard_snapshot(tab->path);
  if (tab->view->recovery_bar) {
    gtk_widget_set_visible(tab->view->recovery_bar, FALSE);
  }
}

static void add_bar_button(GtkWidget *bar, const char *label, GCallback clicked, gpointer data)
{
  GtkWidget *b = gtk_button_new_with_label(label);
  gtk_widget_set_focusable(b, FALSE);
  g_signal_connect(b, "clicked", clicked, data);
  gtk_box_append(GTK_BOX(bar), b);
}

void recovery_check_and_prompt(Tab *tab)
{
  if (!tab || tab->kind != TAB_EDITOR || !tab->path || !tab->view)
    return;

  char *journal = journal_path_for(tab->path);
  if (!journal || !g_file_test(journal, G_FILE_TEST_IS_REGULAR)) {
    g_free(journal);
    return;
  }

  gint64 journal_time = 0;
  char *recovered_text = read_journal_content(journal, &journal_time);
  g_free(journal);
  if (!recovered_text)
    return;

  /* Check disk file mtime: if disk is already newer than the recovery file, discard stale journal */
  GFile *gf = g_file_new_for_path(tab->path);
  GFileInfo *info = g_file_query_info(gf, G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (info) {
    GDateTime *dt = g_file_info_get_modification_date_time(info);
    if (dt) {
      gint64 disk_time = g_date_time_to_unix(dt);
      g_date_time_unref(dt);
      if (disk_time > journal_time) {
        g_object_unref(info);
        g_object_unref(gf);
        g_free(recovered_text);
        recovery_discard_snapshot(tab->path);
        return;
      }
    }
    g_object_unref(info);
  }
  g_object_unref(gf);

  /* Saved within the journal's second, or changes typed and undone: nothing to recover. */
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  char *loaded = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
  gboolean same = g_strcmp0(loaded, recovered_text) == 0;
  g_free(loaded);
  g_free(recovered_text);
  if (same) {
    recovery_discard_snapshot(tab->path);
    return;
  }

  /* Build recovery banner over editor if not already built */
  if (!tab->view->recovery_bar) {
    char *name = g_path_get_basename(tab->path);
    char *msg = g_strdup_printf("Unsaved changes for %s recovered from previous session.", name);
    GtkWidget *label = gtk_label_new(msg);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(bar, "tcode-conflict");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(bar), label);

    add_bar_button(bar, "Restore", G_CALLBACK(on_restore), tab);
    add_bar_button(bar, "Discard", G_CALLBACK(on_discard), tab);

    tab->view->recovery_bar = bar;
    gtk_box_prepend(GTK_BOX(tab->view->column), bar);
    g_free(msg);
    g_free(name);
  }
  gtk_widget_set_visible(tab->view->recovery_bar, TRUE);
}
