#define _DEFAULT_SOURCE /* kill() under -std=c23 with musl */
/*
 * Tests for how tabs open and close, in a whole t-code window (every module but main.c) on a
 * temporary project with a temporary config: temporary tabs (a click in the Files panel),
 * comparisons in one tab, the first run's layout and fonts, the files edited by
 * hand, debugging with gdb. Needs a display; skipped without one.
 */
#include "editor/editor.h"
#include "debug/debugger.h"
#include "debug/view/debugview.h"
#include "debug/disassembly.h"
#include "application/processes.h"
#include "dialogs/settings.h"
#include "editor/gutter/breakpoints.h"
#include "editor/gutter/diffgutter.h"
#include "editor/editorview.h"
#include "editor/preview/preview.h"
#include "editor/recovery.h"
#include "editor/tabmenu.h"
#include "editor/tabs.h"
#include "files/fileops.h"
#include "files/opentabs.h"
#include "git/compare.h"
#include "git/scm/scm.h"
#include "ipc/socket.h"
#include "lsp/inline/diagnostics.h"
#include "lsp/servers.h"
#include "lsp/symbols/navigate.h"
#include "lsp/symbols/outline.h"
#include "lsp/symbols/symbols.h"
#include "plugins/plugins.h"
#include "plugins/registry.h"
#include "popups/contextmenu.h"
#include "project/run/run.h"
#include "project/session.h"
#include "project/toolchain.h"
#include "project/userfiles.h"
#include "update/update.h"
#include "window/commands.h"
#include "window/secondwindow.h"
#include "window/statusbar.h"
#include "window/terminal.h"
#include "window/palette.h"
#include "window/docks/docks.h"
#include "window/theme.h"
#include "window/window.h"
#include "broadway.h"
#include "util.h"
#include "widgets.h"

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <signal.h>

static App app;

static char *file(const char *name)
{
  char *path = g_build_filename(app.project.root, name, NULL);

  if (!g_file_test(path, G_FILE_TEST_EXISTS))
    g_assert_true(g_file_set_contents(path, "one\ntwo\n", -1, NULL));
  return path;
}

typedef struct {
  char *text;
  gsize length;
  char delimiter;
  GError *error;
  gboolean done;
} SocketResponse;

static void socket_response_read(GObject *source, GAsyncResult *result, gpointer data)
{
  SocketResponse *response = data;

  response->text =
      g_data_input_stream_read_upto_finish(G_DATA_INPUT_STREAM(source), result, &response->length, &response->error);
  if (!response->error) {
    int delimiter = g_data_input_stream_read_byte(G_DATA_INPUT_STREAM(source), NULL, &response->error);
    if (!response->error && delimiter != response->delimiter)
      g_set_error(&response->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unexpected socket response delimiter");
  }
  if (!response->text && response->length == 0 && !response->error)
    response->text = g_strdup("");
  response->done = TRUE;
}

static gboolean socket_response_done(gconstpointer data)
{
  return ((const SocketResponse *)data)->done;
}

static char *read_socket_response(GSocketConnection *conn, const char *stop, gsize stop_length)
{
  GDataInputStream *input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
  SocketResponse response = { .delimiter = stop[0] };
  g_data_input_stream_read_upto_async(input, stop, stop_length, G_PRIORITY_DEFAULT, NULL, socket_response_read,
                                      &response);
  run_until(socket_response_done, &response);
  g_assert_true(response.done);
  g_assert_no_error(response.error);
  g_object_unref(input);
  return response.text;
}

static char *read_socket_line(GDataInputStream *input)
{
  SocketResponse response = { .delimiter = '\n' };
  g_data_input_stream_read_upto_async(input, "\n", 1, G_PRIORITY_DEFAULT, NULL, socket_response_read, &response);
  run_until(socket_response_done, &response);
  g_assert_true(response.done);
  g_assert_no_error(response.error);
  return response.text;
}

/* The open tabs' file names in order, "*" after temporary ones: "a.txt* b.txt". */
static char *tabs(void)
{
  GString *text = g_string_new(NULL);

  for (int i = 0; i < tab_count(&app); i++) {
    Tab *tab = tab_nth(&app, i);
    char *name = g_path_get_basename(tab->path);
    g_string_append_printf(text, "%s%s%s", i ? " " : "", name, tab->temporary ? "*" : "");
    g_free(name);
  }
  return g_string_free(text, FALSE);
}

static gboolean tabs_are(gconstpointer want)
{
  char *now = tabs();
  gboolean same = g_strcmp0(now, want) == 0;

  g_free(now);
  return same;
}

/* The tabs are want, once what closes or opens them has run (the main loop may be slow under load). */
static void assert_tabs(const char *want)
{
  char *now;

  run_until(tabs_are, want);
  now = tabs();
  g_assert_cmpstr(now, ==, want);
  g_free(now);
}

static void close_everything(void)
{
  close_all_tabs(&app);
  set_split(&app, FALSE);
  run_for(50);
}

static void open_temporary(const char *name)
{
  char *path = file(name);

  open_file_temporary(&app, path);
  g_free(path);
  run_for(20);
}

static void open_kept(const char *name)
{
  char *path = file(name);

  open_file(&app, path);
  g_free(path);
  run_for(20);
}

/* A click replaces the last click's tab, in its place; opening a file for good, or editing it, keeps
 * its tab. */
static void test_temporary(void)
{
  open_temporary("a.txt");
  assert_tabs("a.txt*");
  open_temporary("b.txt");
  assert_tabs("b.txt*");
  open_kept("b.txt"); /* a double click */
  assert_tabs("b.txt");
  open_temporary("c.txt");
  assert_tabs("b.txt c.txt*");

  open_kept("d.txt");
  select_tab(tab_nth(&app, 1)); /* c.txt; d.txt after it */
  open_temporary("e.txt");
  assert_tabs("b.txt e.txt* d.txt"); /* where c.txt was */
  open_temporary("b.txt");           /* already open: shown, nothing replaced */
  assert_tabs("b.txt e.txt* d.txt");

  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab_nth(&app, 1)->buffer), "x", -1); /* edited */
  assert_tabs("b.txt e.txt d.txt");
  gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(tab_nth(&app, 1)->buffer), FALSE); /* nothing to ask */
  open_temporary("f.txt"); /* after the current tab, b.txt (shown just before) */
  assert_tabs("b.txt f.txt* e.txt d.txt");
  keep_tab(tab_nth(&app, 1)); /* a double click on its tab */
  assert_tabs("b.txt f.txt e.txt d.txt");
  close_everything();
}

/* Each group has its own temporary tab. */
static void test_temporary_per_group(void)
{
  open_temporary("a.txt");
  set_split(&app, TRUE); /* the right group is active now */
  open_temporary("b.txt");
  assert_tabs("a.txt* b.txt*");
  open_temporary("c.txt");
  assert_tabs("a.txt* c.txt*");
  g_assert_cmpint(tab_group(tab_nth(&app, 1)), ==, 1);
  close_everything();
}

static gboolean git(const char *const *args)
{
  GPtrArray *argv = g_ptr_array_new();
  gboolean ok;
  int status = 0;

  g_ptr_array_add(argv, "git");
  for (int i = 0; args[i]; i++)
    g_ptr_array_add(argv, (gpointer)args[i]);
  g_ptr_array_add(argv, NULL);
  ok = g_spawn_sync(app.project.root, (char **)argv->pdata, NULL,
                    G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL,
                    NULL, &status, NULL) &&
       g_spawn_check_wait_status(status, NULL);
  g_ptr_array_free(argv, TRUE);
  return ok;
}

/* The old version shown next to path in its comparison tab, or NULL. */
static Tab *old_version_of(const char *path)
{
  for (int i = 0; i < tab_count(&app); i++) {
    Tab *tab = tab_nth(&app, i);
    if (g_strcmp0(tab->path, path) == 0 && tab->compare.old_side)
      return tab->compare.old_side;
  }
  return NULL;
}

static gboolean compared(gconstpointer path)
{
  return old_version_of(path) != NULL;
}

/* A comparison is one tab with both sides ("tracked.txt ↔ <commit>"), on the left the working file:
 * Stop Comparing closes the old version and keeps the file; closing the tab closes both. */
static void test_comparison_one_tab(void)
{
  char *path = file("tracked.txt");
  Tab *tab;

  if (!git((const char *[]){ "init", "-q", NULL }) || !git((const char *[]){ "add", "tracked.txt", NULL }) ||
      !git((const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "one", NULL })) {
    g_test_skip("no git");
    g_free(path);
    return;
  }
  g_assert_true(g_file_set_contents(path, "one\nTWO\n", -1, NULL));
  app.project.repo_top = g_canonicalize_filename(app.project.root, NULL);
  g_set_str(&app.project.scm_top, app.project.repo_top);

  open_temporary("tracked.txt");
  compare_with_head(&app, path);
  run_until(compared, path);  /* git answers in the background */
  assert_tabs("tracked.txt"); /* one tab, kept (no longer temporary) */
  tab = tab_nth(&app, 0);
  g_assert_nonnull(tab->compare.old_side);
  g_assert_cmpstr(tab->compare.old_side->compare.with, ==, path);
  g_assert_true(g_str_has_prefix(gtk_label_get_text(tab->label), "tracked.txt ↔ "));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(app.editors.groups[1]))); /* no second group */

  stop_comparing(&app); /* the file stays */
  run_for(50);
  assert_tabs("tracked.txt");
  g_assert_null(tab_nth(&app, 0)->compare.old_side);
  g_assert_cmpstr(gtk_label_get_text(tab_nth(&app, 0)->label), ==, "tracked.txt");

  compare_with_head(&app, path);
  run_until(compared, path);
  stop_comparing(&app);
  compare_with_head(&app, path);
  run_until(compared, path);
  compare_with_head(&app, path); /* again: still one comparison */
  run_for(300);
  assert_tabs("tracked.txt");
  quit_editor(tab_nth(&app, 0)); /* closes both */
  run_for(50);
  assert_tabs("");
  g_free(path);
}

static gboolean second_line_marked(gconstpointer tab)
{
  PalColor color;
  return diff_gutter_mark((Tab *)tab, 1, &color);
}

/* A file in a Mercurial repository: its gutter marks what changed since the last commit (hg cat), and
 * Compare with HEAD shows that version next to it, named by its short hash. */
static void test_hg_comparison(void)
{
  char *hg = g_find_program_in_path("hg");
  char *dir = g_build_filename(app.project.root, "hg-project", NULL), *path = g_build_filename(dir, "h.txt", NULL);
  int status = 0;

  if (!hg) {
    g_test_skip("no hg");
    g_free(path);
    g_free(dir);
    return;
  }
  g_mkdir_with_parents(dir, 0755);
  g_assert_true(g_file_set_contents(path, "one\ntwo\n", -1, NULL));
  const char *init[] = { hg, "init", NULL }, *commit[] = { hg, "commit", "-A", "-m", "one", "-u", "t", NULL };
  g_assert_true(g_spawn_sync(dir, (char **)init, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL,
                             NULL, NULL, NULL, &status, NULL));
  g_assert_true(g_spawn_sync(dir, (char **)commit, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL,
                             NULL, NULL, NULL, &status, NULL));
  g_assert_true(g_spawn_check_wait_status(status, NULL));
  g_assert_true(g_file_set_contents(path, "one\nTWO\n", -1, NULL));

  open_file(&app, path);
  Tab *tab = tab_nth(&app, 0);
  run_until(second_line_marked, tab);
  g_assert_true(second_line_marked(tab));

  compare_with_head(&app, path);
  run_until(compared, path);
  Tab *old = old_version_of(path);
  g_assert_nonnull(old);
  char *text = NULL;
  g_assert_true(g_file_get_contents(old->path, &text, NULL, NULL));
  g_assert_cmpstr(text, ==, "one\ntwo\n");
  const char *label = gtk_label_get_text(tab_nth(&app, 0)->label);
  g_assert_true(g_str_has_prefix(label, "h.txt ↔ "));
  g_assert_cmpuint(strlen(label + strlen("h.txt ↔ ")), ==, 12);
  quit_editor(tab_nth(&app, 0));
  run_for(50);
  assert_tabs("");
  g_free(text);
  g_free(hg);
  g_free(path);
  g_free(dir);
}

/* Clicking changed files one after the other in Source Control: each one opens with its old version
 * in its tab, every time. */
static void test_comparisons_in_a_row(void)
{
  const char *names[] = { "r1.txt", "r2.txt", "r3.txt", "r4.txt", "r5.txt", "r6.txt" };
  char *paths[G_N_ELEMENTS(names)];

  for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
    paths[i] = file(names[i]);
    if (!git((const char *[]){ "add", names[i], NULL })) {
      g_test_skip("no git");
      return;
    }
  }
  if (!git((const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "rows", NULL })) {
    g_test_skip("no git");
    return;
  }
  g_set_str(&app.project.repo_top, NULL);
  app.project.repo_top = g_canonicalize_filename(app.project.root, NULL);
  g_set_str(&app.project.scm_top, app.project.repo_top);
  for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
    g_assert_true(g_file_set_contents(paths[i], "one\nchanged\n", -1, NULL));

  for (int round = 0; round < 2; round++)
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
      compare_with_head(&app, paths[i]);
      run_until(compared, paths[i]);
      if (!old_version_of(paths[i])) {
        char *now = tabs();
        g_test_message("round %d, %s: tabs %s", round, names[i], now);
        g_free(now);
      }
      g_assert_nonnull(old_version_of(paths[i]));
    }
  close_everything();
  for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
    g_free(paths[i]);
}

static gboolean loaded(gconstpointer tab)
{
  return !((const Tab *)tab)->view->loading;
}

static gboolean in_conflict(gconstpointer tab)
{
  return editor_in_conflict((Tab *)tab);
}

static gboolean saved(gconstpointer tab)
{
  return !((const Tab *)tab)->modified;
}

/* A file changed on disk under unsaved changes is not written over by a save: the bar asks, and
 * Overwrite writes the editor's text; Reload takes the file's. */
static void test_save_conflict(void)
{
  char *path = g_build_filename(app.project.root, "conflict.txt", NULL), *text = NULL;
  Tab *tab;

  g_assert_true(g_file_set_contents(path, "mine\n", -1, NULL));
  open_file(&app, path);
  tab = find_tab(&app, TAB_EDITOR, path);
  run_until(loaded, tab);
  g_usleep(10 * 1000);                                                     /* another mtime */
  g_assert_true(g_file_set_contents(path, "theirs\n", -1, NULL));          /* another program */
  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "x", -1); /* unsaved */
  save_editor(tab);
  run_until(in_conflict, tab);
  g_assert_true(editor_in_conflict(tab));
  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  g_assert_cmpstr(text, ==, "theirs\n"); /* not written over */
  g_free(text);
  g_assert_true(tab->modified);

  tab->view->overwrite = TRUE; /* the bar's Overwrite */
  save_editor(tab);
  run_until(saved, tab);
  g_assert_false(editor_in_conflict(tab));
  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  g_assert_cmpstr(text, ==, "xmine\n");
  g_free(text);

  /* Changed on disk again while unsaved: the bar shows before any save; Reload takes the disk's. */
  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "y", -1);
  g_usleep(10 * 1000);
  g_assert_true(g_file_set_contents(path, "again\n", -1, NULL));
  run_until(in_conflict, tab);
  g_assert_true(editor_in_conflict(tab));
  editor_reload(tab);
  run_until(loaded, tab);
  g_assert_false(editor_in_conflict(tab));
  g_assert_false(tab->modified);
  close_everything();
  g_free(path);
}

/* An edit is journaled for crash recovery; saving it (or undoing it back to the saved text)
 * deletes the journal, so it is not offered again the next time the file opens. */
static void test_recovery_journal(void)
{
  char *path = g_build_filename(app.project.root, "journaled.txt", NULL);
  char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path, -1);
  char *journal = g_build_filename(g_get_user_cache_dir(), "t-code", "recovery", hash, NULL);
  Tab *tab;

  g_assert_true(g_file_set_contents(path, "saved\n", -1, NULL));
  open_file(&app, path);
  tab = find_tab(&app, TAB_EDITOR, path);
  run_until(loaded, tab);
  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "x", -1);
  recovery_save_snapshot(tab); /* what the timer does a moment after the edit */
  g_assert_true(g_file_test(journal, G_FILE_TEST_EXISTS));
  save_editor(tab);
  run_until(saved, tab);
  g_assert_false(g_file_test(journal, G_FILE_TEST_EXISTS));

  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "y", -1);
  recovery_save_snapshot(tab);
  g_assert_true(g_file_test(journal, G_FILE_TEST_EXISTS));
  gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(tab->buffer), FALSE); /* back to the saved text */
  g_assert_false(g_file_test(journal, G_FILE_TEST_EXISTS));
  close_everything();
  g_free(journal);
  g_free(hash);
  g_free(path);
}

static gboolean has_tab(gconstpointer path)
{
  return find_tab(&app, TAB_EDITOR, path) != NULL;
}

/* A request read while the window closes is dropped: its groups and panels may be gone. */
static void test_socket_while_closing(void)
{
  char *path = file("late.txt"), *request = g_strdup_printf("open %s\n", path);
  GSocketClient *client = g_socket_client_new();
  GSocketAddress *address = g_unix_socket_address_new(app.paths.socket);
  GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);

  g_assert_nonnull(conn);
  app.closing = TRUE;
  g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), request, strlen(request),
                                          NULL, NULL, NULL));
  run_for(200);
  app.closing = FALSE;
  g_assert_false(has_tab(path));
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);
  g_object_unref(address);
  g_object_unref(client);
  g_free(request);
  g_free(path);
}

/* Requests on the control socket may end with NUL bytes instead of line breaks: a path may hold a
 * line break. */
static void test_socket_requests(void)
{
  char *odd = g_build_filename(app.project.root, "two\nlines.txt", NULL);
  char *plain = file("plain.txt");
  char *request = g_strdup_printf("open %s%copen %s", plain, '\0', odd);
  GSocketClient *client = g_socket_client_new();
  GSocketAddress *address = g_unix_socket_address_new(app.paths.socket);
  GSocketConnection *conn;

  g_assert_true(g_file_set_contents(odd, "x\n", -1, NULL));
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_nonnull(conn);
  /* "open <plain>\0open <odd>\0": the string's own NUL ends the second one */
  g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), request,
                                          strlen(plain) + strlen(odd) + 12, NULL, NULL, NULL));
  run_until(has_tab, odd);
  g_assert_true(has_tab(plain));
  g_assert_true(has_tab(odd));
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);

  char *lined = file("lined.txt"), *line = g_strdup_printf("open %s\n", lined); /* tools/headless.sh's way */
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), line, strlen(line), NULL,
                                          NULL, NULL));
  run_until(has_tab, lined);
  g_assert_true(has_tab(lined));
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);
  g_free(line);
  /* Test say */
  const char *say_cmd = "say Testing status notification\n";
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_nonnull(conn);
  g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), say_cmd, strlen(say_cmd),
                                          NULL, NULL, NULL));
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);
  run_for(50);
  g_assert_cmpstr(gtk_label_get_text(app.status.build), ==, "Testing status notification");

  /* Test get-file */
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_true(
      g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), "get-file\n", 9, NULL, NULL, NULL));
  char *got_file = read_socket_response(conn, "\n", 1);
  g_assert_cmpstr(got_file, ==, lined);
  g_free(got_file);
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);

  /* Test get-project */
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), "get-project\n", 12, NULL,
                                          NULL, NULL));
  char *got_proj = read_socket_response(conn, "\n", 1);
  g_assert_cmpstr(got_proj, ==, app.project.root);
  g_free(got_proj);
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);

  /* Test replace-all with multiline content over NUL-delimited requests. */
  const char *expected_text = "Hello World\nSecond line";
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  char *replace = g_strdup_printf("replace-all %s%c", expected_text, '\0');
  g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), replace,
                                          strlen(replace) + 1, NULL, NULL, NULL));
  g_free(replace);
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);
  run_for(50);

  /* Test get-text returns the entire buffer followed by a NUL terminator. */
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_true(
      g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), "get-text\n", 9, NULL, NULL, NULL));
  char *got_text = read_socket_response(conn, "", 1);
  g_assert_cmpstr(got_text, ==, expected_text);
  g_free(got_text);
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), "replace-all \0", 13, NULL,
                                          NULL, NULL));
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);
  run_for(50);

  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_true(
      g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), "get-text\n", 9, NULL, NULL, NULL));
  got_text = read_socket_response(conn, "", 1);
  g_assert_cmpstr(got_text, ==, "");
  g_free(got_text);
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);

  /* Test append */
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), "append  Extra\n", 13, NULL,
                                          NULL, NULL));
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);
  run_for(50);

  /* Test insert */
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_true(
      g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), "insert !\n", 9, NULL, NULL, NULL));
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);
  run_for(50);

  /* Test save */
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_true(
      g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), "save\n", 5, NULL, NULL, NULL));
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);
  run_for(50);

  g_free(lined);
  g_object_unref(address);
  g_object_unref(client);
  close_everything();
  g_free(request);
  g_free(plain);
  g_free(odd);
}

/* A request on a tapi_1 connection, and its answer line. */
static char *tapi_ask(GSocketConnection *conn, GDataInputStream *input, const char *request)
{
  g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), request, strlen(request),
                                          NULL, NULL, NULL));
  return read_socket_line(input);
}

/* A connection that says "hello tapi_1" gets an answer to every request: "ok", "ok <value>" or
 * "error <code> <message>"; get-text's "ok" is followed by the text and a NUL. */
static void test_socket_tapi(void)
{
  GSocketClient *client = g_socket_client_new();
  GSocketAddress *address = g_unix_socket_address_new(app.paths.socket);
  GSocketConnection *conn;
  GDataInputStream *input;
  char *answer, *expected;

  close_everything();
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_nonnull(conn);
  input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));

  answer = tapi_ask(conn, input, "hello tapi_7\n");
  g_assert_true(g_str_has_prefix(answer, "error unsupported-api "));
  g_free(answer);
  answer = tapi_ask(conn, input, "version\n"); /* answers before hello too */
  g_assert_true(g_str_has_prefix(answer, "ok tapi_1 t-code "));
  g_free(answer);
  answer = tapi_ask(conn, input, "hello tapi_1\n");
  g_assert_true(g_str_has_prefix(answer, "ok tapi_1 t-code "));
  g_free(answer);

  answer = tapi_ask(conn, input, "say From a plugin\n");
  g_assert_cmpstr(answer, ==, "ok");
  g_free(answer);
  g_assert_cmpstr(gtk_label_get_text(app.status.build), ==, "From a plugin");

  answer = tapi_ask(conn, input, "insert x\n"); /* no editor open */
  g_assert_true(g_str_has_prefix(answer, "error no-editor "));
  g_free(answer);
  answer = tapi_ask(conn, input, "open relative.txt\n");
  g_assert_true(g_str_has_prefix(answer, "error bad-argument "));
  g_free(answer);
  answer = tapi_ask(conn, input, "frobnicate now\n");
  g_assert_cmpstr(answer, ==, "error unknown-command never heard of frobnicate");
  g_free(answer);
  answer = tapi_ask(conn, input, "debug /bin/true\n"); /* internal: only "_debug", or a legacy connection's "debug" */
  g_assert_cmpstr(answer, ==, "error unknown-command never heard of debug");
  g_free(answer);

  answer = tapi_ask(conn, input, "get-project\n");
  expected = g_strconcat("ok ", app.project.root, NULL);
  g_assert_cmpstr(answer, ==, expected);
  g_free(expected);
  g_free(answer);

  char *path = file("tapi.txt"), *open = g_strdup_printf("open %s\n", path);
  g_assert_true(g_file_set_contents(path, "one\ntwo\n", -1, NULL));
  answer = tapi_ask(conn, input, open);
  g_assert_cmpstr(answer, ==, "ok");
  g_free(answer);
  run_until(has_tab, path);
  run_until(loaded, find_tab(&app, TAB_EDITOR, path));
  answer = tapi_ask(conn, input, "get-file\n");
  expected = g_strconcat("ok ", path, NULL);
  g_assert_cmpstr(answer, ==, expected);
  g_free(expected);
  g_free(answer);
  answer = tapi_ask(conn, input, "get-text\n");
  g_assert_cmpstr(answer, ==, "ok");
  g_free(answer);
  SocketResponse text = { .delimiter = '\0' };
  g_data_input_stream_read_upto_async(input, "", 1, G_PRIORITY_DEFAULT, NULL, socket_response_read, &text);
  run_until(socket_response_done, &text);
  g_assert_no_error(text.error);
  g_assert_cmpstr(text.text, ==, "one\ntwo\n");
  g_free(text.text);

  answer = tapi_ask(conn, input, "subscribe\n");
  g_assert_cmpstr(answer, ==, "ok");
  g_free(answer);

  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(input);
  g_object_unref(conn);
  g_object_unref(address);
  g_object_unref(client);
  close_everything();
  g_free(open);
  g_free(path);
}

/* A tapi_1 connection (said hello: hello may be "hello tapi_1 <token>") and its answers. */
typedef struct {
  GSocketClient *client;
  GSocketConnection *conn;
  GDataInputStream *input;
} TapiConn;

static TapiConn tapi_connect(const char *hello)
{
  GSocketAddress *address = g_unix_socket_address_new(app.paths.socket);
  TapiConn c = { g_socket_client_new(), NULL, NULL };
  char *request = g_strdup_printf("%s\n", hello), *answer;

  c.conn = g_socket_client_connect(c.client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_nonnull(c.conn);
  c.input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(c.conn)));
  answer = tapi_ask(c.conn, c.input, request);
  g_assert_true(g_str_has_prefix(answer, "ok tapi_1 "));
  g_free(answer);
  g_free(request);
  g_object_unref(address);
  return c;
}

/* request's answer line (request without its line break). */
static char *tapi(TapiConn *c, const char *request)
{
  char *line = g_strconcat(request, "\n", NULL), *answer = tapi_ask(c->conn, c->input, line);

  g_free(line);
  return answer;
}

/* request must answer expected. */
static void tapi_expect(TapiConn *c, const char *request, const char *expected)
{
  char *answer = tapi(c, request);

  g_assert_cmpstr(answer, ==, expected);
  g_free(answer);
}

/* The text after a "ok" line: up to the NUL. */
static char *tapi_text(TapiConn *c)
{
  SocketResponse text = { .delimiter = '\0' };

  g_data_input_stream_read_upto_async(c->input, "", 1, G_PRIORITY_DEFAULT, NULL, socket_response_read, &text);
  run_until(socket_response_done, &text);
  g_assert_no_error(text.error);
  return text.text;
}

static void tapi_close(TapiConn *c)
{
  g_io_stream_close(G_IO_STREAM(c->conn), NULL, NULL);
  g_object_unref(c->input);
  g_object_unref(c->conn);
  g_object_unref(c->client);
}

/* The active editor through tapi_1: open at a place, the cursor, the selection, edits as one undo
 * step, a lasting status item, the commit message box. */
static void test_socket_tapi_editing(void)
{
  char *path = file("tapi-edit.txt"), *request, *text;
  TapiConn c;
  Tab *tab;

  g_assert_true(g_file_set_contents(path, "alpha\nbeta\ngamma\n", -1, NULL));
  close_everything();
  c = tapi_connect("hello tapi_1");
  tapi_expect(&c, "get-cursor", "error no-editor no editor here, open a file first");
  tapi_expect(&c, "get-selection-range", "error no-editor no editor here, open a file first");
  request = g_strdup_printf("open %s:2:3", path);
  tapi_expect(&c, request, "ok");
  g_free(request);
  run_until(has_tab, path);
  tab = find_tab(&app, TAB_EDITOR, path);
  run_until(loaded, tab);
  tapi_expect(&c, "get-cursor", "ok 2 3");

  tapi_expect(&c, "set-selection 1 2 2 3", "ok");
  tapi_expect(&c, "get-selection", "ok");
  text = tapi_text(&c);
  g_assert_cmpstr(text, ==, "lpha\nbe");
  g_free(text);
  tapi_expect(&c, "get-cursor", "ok 2 3"); /* the cursor at the second place */
  tapi_expect(&c, "get-selection-range", "ok 1 2 2 3");
  tapi_expect(&c, "set-selection 2 3 1 2", "ok"); /* backwards: the range still starts first */
  tapi_expect(&c, "get-selection-range", "ok 1 2 2 3");

  tapi_expect(&c, "set-selection 3 1 3 1", "ok");
  tapi_expect(&c, "get-selection-range", "ok 3 1 3 1"); /* nothing selected: the cursor twice */
  tapi_expect(&c, "edit [{\"range\":[1,1,1,6],\"text\":\"ALPHA\"},{\"range\":[2,5,2,5],\"text\":\"!\"}]", "ok");
  tapi_expect(&c, "get-text", "ok");
  text = tapi_text(&c);
  g_assert_cmpstr(text, ==, "ALPHA\nbeta!\ngamma\n");
  g_free(text);
  tapi_expect(&c, "get-cursor", "ok 3 1"); /* stays */
  g_assert_true(gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(tab->buffer)));
  gtk_text_buffer_undo(GTK_TEXT_BUFFER(tab->buffer)); /* one step for both */
  tapi_expect(&c, "get-text", "ok");
  text = tapi_text(&c);
  g_assert_cmpstr(text, ==, "alpha\nbeta\ngamma\n");
  g_free(text);
  text = tapi(&c, "edit {\"not\":\"a list\"}");
  g_assert_true(g_str_has_prefix(text, "error bad-argument "));
  g_free(text);

  tapi_expect(&c, "status-set count 3 words", "ok");
  g_assert_cmpstr(status_plugin_text(&app, "socket/count"), ==, "3 words");
  tapi_expect(&c, "status-set count 4 words", "ok");
  g_assert_cmpstr(status_plugin_text(&app, "socket/count"), ==, "4 words");
  tapi_expect(&c, "status-clear count", "ok");
  g_assert_null(status_plugin_text(&app, "socket/count"));

  tapi_expect(&c, "scm-message feat: from a plugin", "ok");
  char *message = scm_message();
  g_assert_cmpstr(message, ==, "feat: from a plugin");
  g_free(message);
  scm_set_message(NULL);

  tapi_expect(&c, "lsp-refresh", "ok"); /* a plugin installed its language server */

  tapi_close(&c);
  close_everything();
  g_free(path);
}

static GtkWidget *focused_entry(void);
static gboolean entry_focused(gconstpointer data);

/* pick and ask show the picker; the connection's answer waits for the user, and its next requests
 * wait behind it. Escape answers "error canceled". */
static void test_socket_tapi_pick(void)
{
  TapiConn c = tapi_connect("hello tapi_1");
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(c.conn));
  const char *pick =
      "pick {\"title\":\"Board\",\"items\":[\"uno\",{\"label\":\"mega\",\"detail\":\"2560\"}]}\nversion\n";
  GtkWidget *entry;
  char *answer;

  close_everything();
  g_assert_true(g_output_stream_write_all(out, pick, strlen(pick), NULL, NULL, NULL));
  run_until(entry_focused, NULL);
  entry = focused_entry();
  gtk_editable_set_text(GTK_EDITABLE(entry), "meg");
  run_for(400); /* the search entry waits a moment before it searches */
  g_signal_emit_by_name(entry, "activate");
  answer = read_socket_line(c.input);
  g_assert_cmpstr(answer, ==, "ok 1 mega");
  g_free(answer);
  answer = read_socket_line(c.input); /* "version" was answered after the pick, not before */
  g_assert_true(g_str_has_prefix(answer, "ok tapi_1 t-code "));
  g_free(answer);

  g_assert_true(g_output_stream_write_all(out, "ask Branch name\n", 16, NULL, NULL, NULL));
  run_until(entry_focused, NULL);
  entry = focused_entry();
  gtk_editable_set_text(GTK_EDITABLE(entry), "feature/x");
  run_for(400);
  g_signal_emit_by_name(entry, "activate");
  answer = read_socket_line(c.input);
  g_assert_cmpstr(answer, ==, "ok feature/x");
  g_free(answer);

  g_assert_true(g_output_stream_write_all(out, "ask Anything\n", 13, NULL, NULL, NULL));
  run_until(entry_focused, NULL);
  g_signal_emit_by_name(focused_entry(), "stop-search"); /* Escape */
  answer = read_socket_line(c.input);
  g_assert_cmpstr(answer, ==, "error canceled the picker closed and nobody picked anything");
  g_free(answer);
  tapi_close(&c);
}

/* A plugin's diagnostics: kept per file (shown once it opens), replaced only by its own next list,
 * left alone by the language server's, cleared by diagnostics-clear. */
static void test_socket_tapi_diagnostics(void)
{
  char *path = file("tapi-diag.sh"), *request;
  TapiConn c = tapi_connect("hello tapi_1");
  GPtrArray *list;
  Tab *tab;

  close_everything();
  g_assert_true(g_file_set_contents(path, "echo $1\nls\n", -1, NULL));
  request = g_strdup_printf("diagnostics-set {\"path\":\"%s\",\"items\":[{\"range\":[1,6,1,8],\"severity\":\"warning\","
                            "\"message\":\"Double quote to prevent globbing\",\"code\":\"SC2086\","
                            "\"source\":\"shellcheck\"}]}",
                            path);
  tapi_expect(&c, request, "ok"); /* before the file is open */
  g_free(request);
  open_file(&app, path);
  run_until(has_tab, path);
  tab = find_tab(&app, TAB_EDITOR, path);
  run_until(loaded, tab);
  list = diagnostics_of(tab);
  g_assert_nonnull(list);
  g_assert_cmpuint(list->len, ==, 1);
  g_assert_cmpstr(diagnostic_message(g_ptr_array_index(list, 0)), ==,
                  "Double quote to prevent globbing (shellcheck SC2086)");
  g_assert_cmpint(diagnostic_severity(g_ptr_array_index(list, 0)), ==, 2);
  g_assert_cmpint(tab->problems.warnings, ==, 1);
  int line, column;
  diagnostic_position(tab, g_ptr_array_index(list, 0), &line, &column);
  g_assert_cmpint(line, ==, 1);
  g_assert_cmpint(column, ==, 6);

  diagnostics_show(tab, NULL); /* the language server's empty list leaves the plugin's */
  g_assert_cmpuint(diagnostics_of(tab)->len, ==, 1);

  request = g_strdup_printf("diagnostics-set {\"path\":\"%s\",\"items\":[{\"range\":[2,1,2,3],\"severity\":\"error\","
                            "\"message\":\"no\"},{\"range\":[1,1],\"message\":\"start\"}]}",
                            path);
  tapi_expect(&c, request, "ok"); /* replaces its own */
  g_free(request);
  g_assert_cmpuint(diagnostics_of(tab)->len, ==, 2);
  g_assert_cmpint(tab->problems.errors, ==, 2);
  g_assert_cmpint(tab->problems.warnings, ==, 0);

  tapi_expect(&c, "diagnostics-clear", "ok");
  g_assert_cmpuint(diagnostics_of(tab)->len, ==, 0);
  g_assert_null(app.plugin_diagnostics ? g_hash_table_lookup(app.plugin_diagnostics, path) : NULL);
  request = g_strdup("diagnostics-set {\"path\":\"relative.sh\",\"items\":[]}");
  char *answer = tapi(&c, request);
  g_assert_true(g_str_has_prefix(answer, "error bad-argument "));
  g_free(answer);
  g_free(request);
  tapi_close(&c);
  close_everything();
  g_free(path);
}

static gboolean terminal_shows(gconstpointer data)
{
  const char *const *want = data;
  VteTerminal *term = shell_at(&app, atoi(want[0]));
  char *text = term ? vte_terminal_get_text_format(term, VTE_FORMAT_TEXT) : NULL;
  gboolean found = text && strstr(text, want[1]);

  g_free(text);
  return found;
}

static gboolean shells_are(gconstpointer count);

/* A plugin's terminal tab runs its program; it types into it, interrupts it and closes it, and only
 * it may. Plugins need the "terminal" permission to open one, and "edit" to change the text. */
static void test_socket_tapi_terminals(void)
{
  char *dir = g_build_filename(app.project.root, "tapi-plugins", NULL);
  char *manifest = g_build_filename(dir, "plain", "plugin.json", NULL);
  char *run = g_build_filename(dir, "plain", "run.sh", NULL);
  TapiConn c = tapi_connect("hello tapi_1"), *other;
  int before = shell_count(&app);
  char *answer, *id, *request, *hello;

  answer = tapi(&c, "terminal-open Echo -- cat");
  g_assert_true(g_str_has_prefix(answer, "ok "));
  id = g_strdup(answer + 3);
  g_free(answer);
  g_assert_cmpint(shell_count(&app), ==, before + 1);
  g_assert_cmpstr(shell_name(shell_at(&app, before)), ==, "Echo");

  request = g_strdup_printf("terminal-send %s marco", id);
  tapi_expect(&c, request, "ok");
  g_free(request);
  char *index = g_strdup_printf("%d", before);
  const char *want[] = { index, "marco" };
  run_until(terminal_shows, want);

  /* a plugin (its token) may not touch the socket's tab, nor open one without "terminal" */
  g_mkdir_with_parents(g_path_get_dirname(manifest), 0755);
  g_assert_true(g_file_set_contents(manifest,
                                    "{\"api\":\"tapi_1\",\"name\":\"Plain\",\"executable\":\"run.sh\","
                                    "\"commands\":[{\"id\":\"go\",\"name\":\"Go\"}]}",
                                    -1, NULL));
  g_assert_true(g_file_set_contents(run, "#!/bin/sh\n", -1, NULL));
  g_chmod(run, 0755);
  plugins_scan_dir(&app, dir);
  Plugin *plain = plugin_find(&app, "plain");
  g_assert_nonnull(plain);
  hello = g_strdup_printf("hello tapi_1 %s", plain->token);
  TapiConn plugin = tapi_connect(hello);
  other = &plugin;
  request = g_strdup_printf("terminal-close %s", id);
  tapi_expect(other, request, "error bad-argument no terminal tab with that id is yours");
  answer = tapi(other, "terminal-open Mine -- cat");
  g_assert_cmpstr(answer, ==,
                  "error not-permitted plugin.json does not ask for \"terminal\" (add it to \"permissions\")");
  g_free(answer);
  open_file(&app, run);
  run_until(has_tab, run);
  run_until(loaded, find_tab(&app, TAB_EDITOR, run));
  tapi_expect(other, "insert x",
              "error not-permitted plugin.json does not ask for \"edit\" (add it to \"permissions\")");
  tapi_expect(other, "get-file", (answer = g_strconcat("ok ", run, NULL)));
  g_free(answer);
  plain->permissions = PLUGIN_EDIT;
  tapi_expect(other, "insert x", "ok");
  tapi_expect(&c, "hello tapi_1 not-a-token", "error bad-argument never seen that token, were the plugins reloaded?");

  request = g_strdup_printf("terminal-interrupt %s", id);
  tapi_expect(&c, request, "ok"); /* cat ends: its tab closes */
  g_free(request);
  run_until(shells_are, GINT_TO_POINTER(before));

  answer = tapi(&c, "terminal-open Shell");
  g_free(id);
  id = g_strdup(answer + 3);
  g_free(answer);
  g_assert_cmpint(shell_count(&app), ==, before + 1);
  request = g_strdup_printf("terminal-close %s", id);
  tapi_expect(&c, request, "ok");
  g_free(request);
  g_assert_cmpint(shell_count(&app), ==, before);

  g_ptr_array_remove(app.plugins, plain);
  tapi_close(other);
  tapi_close(&c);
  close_everything();
  g_free(hello);
  g_free(index);
  g_free(id);
  g_free(run);
  g_free(manifest);
  g_free(dir);
}

static gboolean programs_ended;

/* Whether pid runs: a zombie does not (in a container whose first process reaps nothing, an orphan
 * that ended stays one, and kill(pid, 0) still succeeds on it). */
static gboolean process_runs(int pid)
{
  char *path = g_strdup_printf("/proc/%d/stat", pid), *text = NULL;
  gboolean runs = g_file_get_contents(path, &text, NULL, NULL) && strrchr(text, ')') && strrchr(text, ')')[2] != 'Z';

  g_free(text);
  g_free(path);
  return runs;
}

static void on_programs_ended(App *t)
{
  programs_ended = TRUE;
  gtk_widget_set_visible(GTK_WIDGET(t->window), TRUE); /* the test goes on in this window */
}

static gboolean flag_set(gconstpointer flag)
{
  return *(const gboolean *)flag;
}

/* The file holds a whole line (a pid written with echo). */
static gboolean line_written(gconstpointer path)
{
  char *text = NULL;
  gboolean done = g_file_get_contents(path, &text, NULL, NULL) && strchr(text, '\n');

  g_free(text);
  return done;
}

/* t-code closes: a plugin's program with a close text gets it typed and ends by itself; one without
 * gets SIGTERM at once, and so does its helper in a new session; neither keeps t-code waiting. */
static void test_end_programs(void)
{
  char *out = g_build_filename(app.project.root, "close-text.out", NULL);
  char *pidfile = g_build_filename(app.project.root, "helper.pid", NULL);
  TapiConn c = tapi_connect("hello tapi_1");
  int before = shell_count(&app);
  char *answer, *polite, *rough, *request, *text = NULL;

  request = g_strdup_printf("terminal-open Polite -- sh -c 'read line; printf %%s \"$line\" > %s'", out);
  answer = tapi(&c, request);
  g_assert_true(g_str_has_prefix(answer, "ok "));
  polite = g_strdup(answer + 3);
  g_free(answer);
  g_free(request);
  request = g_strdup_printf("terminal-open Rough -- sh -c 'setsid sleep 300 & echo $! > %s; wait'", pidfile);
  answer = tapi(&c, request);
  rough = g_strdup(answer + 3);
  g_free(answer);
  g_free(request);
  request = g_strdup_printf("terminal-close-text %s quit please", polite);
  tapi_expect(&c, request, "ok");
  g_free(request);
  tapi_expect(&c, "terminal-close-text 9999 exit", "error bad-argument no terminal tab with that id is yours");
  run_until(line_written, pidfile);
  g_assert_true(g_file_get_contents(pidfile, &text, NULL, NULL));
  int helper = atoi(text);
  g_free(text);
  g_assert_cmpint(helper, >, 0);
  g_assert_true(process_runs(helper));
  run_for(300); /* the programs' sessions are known once their spawns are done */

  gint64 start = g_get_monotonic_time();
  programs_ended = FALSE;
  end_programs(&app, on_programs_ended);
  run_until(flag_set, &programs_ended);
  g_assert_true(programs_ended);
  g_assert_cmpint(g_get_monotonic_time() - start, <, 2 * G_USEC_PER_SEC);
  g_assert_true(g_file_get_contents(out, &text, NULL, NULL));
  g_assert_cmpstr(text, ==, "quit please");
  g_free(text);
  for (int i = 0; i < 100 && process_runs(helper); i++)
    run_for(20);
  g_assert_false(process_runs(helper));

  run_until(shells_are, GINT_TO_POINTER(before));
  g_assert_cmpint(shell_count(&app), ==, before);

  /* typed before the program started (no main loop turn in between): written once it has */
  char *cat[] = { "cat", NULL };
  VteTerminal *late = open_program(&app, "Late", cat);
  shell_type(late, "typed early\r", -1);
  char *late_index = g_strdup_printf("%d", before);
  run_until(terminal_shows, (const char *const[]){ late_index, "typed early" });
  g_assert_true(terminal_shows((const char *const[]){ late_index, "typed early" }));
  close_shell_tab(&app, late);
  g_free(late_index);

  tapi_close(&c);
  g_unlink(out);
  g_unlink(pidfile);
  g_free(polite);
  g_free(rough);
  g_free(pidfile);
  g_free(out);
}

static gboolean no_socket_event_clients(gconstpointer data G_GNUC_UNUSED);

/* The next event called name on a subscribed connection (the others before it are skipped). */
static char *next_event(GDataInputStream *input, const char *name)
{
  char *prefix = g_strdup_printf("{\"event\":\"%s\"", name);

  for (;;) {
    char *message = read_socket_line(input);
    if (g_str_has_prefix(message, prefix)) {
      g_free(prefix);
      return message;
    }
    g_free(message);
  }
}

/* An event's line for path, with "by":"plugin" when by_plugin. */
static char *event_line(const char *name, const char *path, gboolean by_plugin)
{
  return g_strdup_printf("{\"event\":\"%s\",\"path\":\"%s\"%s}", name, path, by_plugin ? ",\"by\":\"plugin\"" : "");
}

/* A subscriber hears a file become active, load, change (once typing pauses), save and close; what a
 * socket request caused is "by":"plugin". */
static void test_socket_events(void)
{
  char *path = file("socket-events.c");
  GSocketClient *client = g_socket_client_new();
  GSocketAddress *address = g_unix_socket_address_new(app.paths.socket);
  GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  GSocketConnection *plugin = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  GDataInputStream *input, *plugin_input;
  char *message, *expected;
  GOutputStream *out;

  g_assert_nonnull(conn);
  out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
  plugin_input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(plugin)));
  g_assert_true(g_output_stream_write_all(out, "subscribe\n", 10, NULL, NULL, NULL));
  message = read_socket_line(input);
  g_assert_cmpstr(message, ==, "{\"event\":\"subscribed\"}");
  g_free(message);

  open_file(&app, path);
  run_until(has_tab, path);
  Tab *tab = find_tab(&app, TAB_EDITOR, path);
  g_assert_nonnull(tab);
  run_until(loaded, tab);
  message = next_event(input, "active_changed");
  expected = event_line("active_changed", path, FALSE);
  g_assert_cmpstr(message, ==, expected);
  g_free(expected);
  g_free(message);
  message = next_event(input, "did_open");
  expected = event_line("did_open", path, FALSE);
  g_assert_cmpstr(message, ==, expected);
  g_free(expected);
  g_free(message);

  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "edited", -1); /* typed */
  message = next_event(input, "did_change");
  expected = event_line("did_change", path, FALSE);
  g_assert_cmpstr(message, ==, expected);
  g_free(expected);
  g_free(message);

  g_free(tapi_ask(plugin, plugin_input, "hello tapi_1\n"));
  message = tapi_ask(plugin, plugin_input, "insert !\n"); /* a plugin's edit, then its save */
  g_assert_cmpstr(message, ==, "ok");
  g_free(message);
  message = next_event(input, "did_change");
  expected = event_line("did_change", path, TRUE);
  g_assert_cmpstr(message, ==, expected);
  g_free(expected);
  g_free(message);
  message = tapi_ask(plugin, plugin_input, "save\n");
  g_assert_cmpstr(message, ==, "ok");
  g_free(message);
  message = next_event(input, "did_save");
  expected = event_line("did_save", path, TRUE);
  g_assert_cmpstr(message, ==, expected);
  g_free(expected);
  g_free(message);

  save_editor(tab); /* the user's own save */
  run_until(saved, tab);
  message = next_event(input, "did_save");
  expected = event_line("did_save", path, FALSE);
  g_assert_cmpstr(message, ==, expected);
  g_free(expected);
  g_free(message);

  close_everything();
  message = next_event(input, "did_close");
  expected = event_line("did_close", path, FALSE);
  g_assert_cmpstr(message, ==, expected);
  g_free(expected);
  g_free(message);

  g_io_stream_close(G_IO_STREAM(plugin), NULL, NULL);
  g_object_unref(plugin_input);
  g_object_unref(plugin);
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(input);
  g_object_unref(conn);
  g_object_unref(address);
  g_object_unref(client);
  run_until(no_socket_event_clients, NULL);
  close_everything();
  g_free(path);
}
static gboolean no_socket_event_clients(gconstpointer data G_GNUC_UNUSED)
{
  return !app.socket_events || app.socket_events->len == 0;
}

/* A plugin that subscribes and then never reads its events: t-code is not held up (the events used to
 * be written with blocking calls on the main loop, which froze it once the socket's buffer filled),
 * and once its unread output passes the limit it is dropped and its connection closed. */
static void test_socket_stalled_subscriber(void)
{
  GSocketClient *client = g_socket_client_new();
  GSocketAddress *address = g_unix_socket_address_new(app.paths.socket);
  GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  GInputStream *input;
  char *path = g_strdup_printf("%s/%0200d.c", app.project.root, 0), buffer[4096], *line;
  gsize received = 0;
  gssize got;
  int sent = 0;

  g_assert_nonnull(conn);
  g_assert_true(
      g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), "subscribe\n", 10, NULL, NULL, NULL));
  GDataInputStream *lines = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
  g_filter_input_stream_set_close_base_stream(G_FILTER_INPUT_STREAM(lines), FALSE);
  line = read_socket_line(lines);
  g_assert_cmpstr(line, ==, "{\"event\":\"subscribed\"}");
  g_free(line);
  g_object_unref(lines);
  g_assert_cmpuint(app.socket_events->len, ==, 1);

  /* About 2 MB of events, more than the socket's buffer and the limit together. */
  for (; sent < 8000 && app.socket_events->len > 0; sent++) {
    socket_did_save(&app, path);
    if (sent % 100 == 0)
      run_for(1); /* the writes in flight go on */
  }
  g_assert_cmpuint(app.socket_events->len, ==, 0); /* dropped */
  g_assert_cmpint(sent, <, 8000);

  run_for(100); /* its connection is closed */
  input = g_io_stream_get_input_stream(G_IO_STREAM(conn));
  while ((got = g_input_stream_read(input, buffer, sizeof buffer, NULL, NULL)) > 0)
    received += got;
  g_assert_cmpint(got, ==, 0); /* the end, not an error */
  g_assert_cmpuint(received, <, (gsize)sent * (strlen(path) + 32));

  g_object_unref(conn);
  g_object_unref(address);
  g_object_unref(client);
  g_free(path);
}

static gboolean no_second_window(gconstpointer data G_GNUC_UNUSED)
{
  return app.editors.second == NULL;
}

/* "Open in New Window": the file's tab goes to a second window (one tab per file still); closing that
 * window brings its tabs back, and the session remembers them. */
static void test_second_window(void)
{
  char *a = file("w1.txt"), *b = file("w2.txt");
  Tab *tab;

  open_kept("w1.txt");
  open_in_new_window(&app, a); /* already open: its tab moves */
  run_for(50);
  g_assert_nonnull(app.editors.second);
  g_assert_cmpint(tab_count(&app), ==, 1);
  tab = find_tab(&app, TAB_EDITOR, a);
  g_assert_cmpint(tab_group(tab), ==, WINDOW_GROUP);
  g_assert_true(tab_window(tab) == app.editors.second);
  open_file(&app, a); /* still one tab */
  g_assert_cmpint(tab_count(&app), ==, 1);

  open_kept("w2.txt");
  move_to_new_window(&app); /* the current tab: w2.txt, opened in the active group (the window's) */
  g_assert_cmpint(tab_group(find_tab(&app, TAB_EDITOR, b)), ==, WINDOW_GROUP);

  session_save(&app);
  gtk_window_close(app.editors.second); /* the tabs come back */
  run_until(no_second_window, NULL);
  g_assert_null(app.editors.second);
  g_assert_cmpint(tab_count(&app), ==, 2);
  g_assert_cmpint(tab_group(find_tab(&app, TAB_EDITOR, a)), ==, 0);
  g_assert_cmpint(tab_group(find_tab(&app, TAB_EDITOR, b)), ==, 0);

  close_everything();
  session_restore_tabs(&app); /* as saved: both in the second window */
  run_for(100);
  g_assert_nonnull(app.editors.second);
  g_assert_cmpint(tab_group(find_tab(&app, TAB_EDITOR, a)), ==, WINDOW_GROUP);
  g_assert_cmpint(tab_group(find_tab(&app, TAB_EDITOR, b)), ==, WINDOW_GROUP);

  remove_tab(find_tab(&app, TAB_EDITOR, a));
  remove_tab(find_tab(&app, TAB_EDITOR, b)); /* the last one: the window goes */
  run_until(no_second_window, NULL);
  g_assert_null(app.editors.second);
  g_assert_cmpint(app.editors.active, ==, 0);
  close_everything();
  session_save(&app);
  g_free(a);
  g_free(b);
}

/* The tab names in a group's strip, in order: "a.txt b.txt". */
static char *strip_names(int group)
{
  GtkWidget *strip = gtk_widget_get_parent(tab_nth(&app, 0)->item);
  GString *text = g_string_new(NULL);

  for (int i = 0; i < tab_count(&app); i++)
    if (tab_group(tab_nth(&app, i)) == group)
      strip = gtk_widget_get_parent(tab_nth(&app, i)->item);
  for (GtkWidget *w = gtk_widget_get_first_child(strip); w; w = gtk_widget_get_next_sibling(w)) {
    Tab *tab = NULL;
    for (int i = 0; i < tab_count(&app); i++)
      if (tab_nth(&app, i)->item == w)
        tab = tab_nth(&app, i);
    char *name = g_path_get_basename(tab->path);
    g_string_append_printf(text, "%s%s%s", text->len ? " " : "", name,
                           gtk_widget_get_state_flags(w) & GTK_STATE_FLAG_CHECKED ? "!" : "");
    g_free(name);
  }
  return g_string_free(text, FALSE);
}

typedef struct {
  int group;
  const char *want;
} Strip;

static gboolean strip_is(gconstpointer data)
{
  const Strip *strip = data;
  char *now = strip_names(strip->group);
  gboolean same = g_strcmp0(now, strip->want) == 0;

  g_free(now);
  return same;
}

/* The strip shows want, once its updates have run (the main loop may be slow under load). */
static void assert_strip(int group, const char *want)
{
  Strip strip = { group, want };
  char *now;

  run_until(strip_is, &strip);
  now = strip_names(group);
  g_assert_cmpstr(now, ==, want);
  g_free(now);
}

static gboolean laid_out(gconstpointer item)
{
  return gtk_widget_get_width((GtkWidget *)item) > 30;
}

/* Each group's tab strip follows its tabs: their order, the current one ("!"), a tab moved to the
 * other group or dropped among the tabs, a comparison (one tab). */
static void test_tab_strip(void)
{
  open_kept("s1.txt");
  open_kept("s2.txt");
  open_kept("s3.txt");
  assert_strip(0, "s1.txt s2.txt s3.txt!");
  select_tab(tab_nth(&app, 0));
  run_for(20);
  assert_strip(0, "s1.txt! s2.txt s3.txt");

  /* s3 dropped before s1 in the same strip */
  GtkWidget *strip = gtk_widget_get_parent(tab_nth(&app, 0)->item);
  GtkWidget *scrolled = gtk_widget_get_ancestor(strip, GTK_TYPE_SCROLLED_WINDOW);
  GValue value = G_VALUE_INIT;
  g_value_init(&value, G_TYPE_POINTER);
  g_value_set_pointer(&value, tab_nth(&app, 2));
  GtkDropTarget *target = NULL;
  GListModel *controllers = gtk_widget_observe_controllers(scrolled);
  for (guint i = 0; i < g_list_model_get_n_items(controllers) && !target; i++) {
    GObject *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_DROP_TARGET(controller))
      target = GTK_DROP_TARGET(controller);
    g_object_unref(controller);
  }
  g_object_unref(controllers);
  g_assert_nonnull(target);
  for (GtkWidget *w = gtk_widget_get_first_child(strip); w; w = gtk_widget_get_next_sibling(w))
    run_until(laid_out, w); /* the drop is placed by the tabs' places */
  graphene_point_t at;      /* on s1's left edge, wherever the strip is scrolled to (s3, the last opened) */
  g_assert_true(gtk_widget_compute_point(gtk_widget_get_first_child(strip), scrolled, &GRAPHENE_POINT_INIT(1, 0), &at));
  gboolean dropped = FALSE;
  g_signal_emit_by_name(target, "drop", &value, (double)at.x, 0.0, &dropped);
  g_assert_true(dropped);
  run_for(20);
  assert_strip(0, "s3.txt! s1.txt s2.txt");
  assert_tabs("s3.txt s1.txt s2.txt");

  set_split(&app, TRUE);
  move_tab(tab_nth(&app, 1), 1); /* s1 to the right */
  run_for(20);
  assert_strip(0, "s3.txt! s2.txt");
  assert_strip(1, "s1.txt!");
  g_value_unset(&value);
  close_everything();
}

/* The right-click menu shown on widget. */
static GtkPopoverMenu *shown_menu(GtkWidget *widget)
{
  GtkWidget *popover = NULL;

  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (GTK_IS_POPOVER_MENU(child))
      popover = child;
  g_assert_nonnull(popover);
  return GTK_POPOVER_MENU(popover);
}

/* The action of the item labelled label in the menu shown on widget, or NULL (free it). */
static char *menu_action(GtkWidget *widget, const char *label)
{
  GMenuModel *model = gtk_popover_menu_get_menu_model(shown_menu(widget));
  char *action = NULL;

  for (int s = 0; s < g_menu_model_get_n_items(model) && !action; s++) {
    GMenuModel *section = g_menu_model_get_item_link(model, s, G_MENU_LINK_SECTION);
    for (int i = 0; i < g_menu_model_get_n_items(section) && !action; i++) {
      char *text = NULL;
      if (g_menu_model_get_item_attribute(section, i, G_MENU_ATTRIBUTE_LABEL, "s", &text) && !g_strcmp0(text, label))
        g_menu_model_get_item_attribute(section, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);
      g_free(text);
    }
    g_object_unref(section);
  }
  return action;
}

/* Whether widget holds no right-click menu any more (it went once it closed and its item ran). */
static gboolean menu_gone(gconstpointer widget)
{
  for (GtkWidget *child = gtk_widget_get_first_child((GtkWidget *)widget); child;
       child = gtk_widget_get_next_sibling(child))
    if (GTK_IS_POPOVER_MENU(child))
      return FALSE;
  return TRUE;
}

/* Chooses the item labelled label in the right-click menu shown on widget, as a click would. The item
 * runs once the menu has closed, in an idle: waited for (a fixed 20 ms was too short on a busy CI
 * machine). */
static void choose_menu_item(GtkWidget *widget, const char *label)
{
  char *action = menu_action(widget, label);

  g_assert_nonnull(action);
  g_assert_true(gtk_widget_activate_action(widget, action, NULL));
  g_free(action);
  gtk_popover_popdown(GTK_POPOVER(shown_menu(widget)));
  run_until(menu_gone, widget);
  g_assert_true(menu_gone(widget));
  run_for(20);
}

static void count_run(gpointer data)
{
  (*(int *)data)++;
}

static int menu_freed;

static void count_free(gpointer data G_GNUC_UNUSED)
{
  menu_freed++;
}

/* A right-click menu runs the chosen item once it closed and frees its data, chosen or not; a
 * tab's menu closes the tabs right of it, moves a tab to the other group, and previews a Markdown
 * file (only a Markdown file). */
static void test_context_menus(void)
{
  int ran = 0;
  MenuItem items[] = { { "First", count_run }, { NULL, NULL }, { "Second", count_run } };

  open_kept("m1.txt");
  open_kept("m2.txt");
  open_kept("m3.txt");
  run_for(20);
  GtkWidget *widget = tab_nth(&app, 0)->item;

  menu_freed = 0;
  context_menu(widget, 1, 1, items, G_N_ELEMENTS(items), &ran, count_free);
  choose_menu_item(widget, "Second");
  g_assert_cmpint(ran, ==, 1);
  g_assert_cmpint(menu_freed, ==, 1);
  context_menu(widget, 1, 1, items, G_N_ELEMENTS(items), &ran, count_free);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (GTK_IS_POPOVER_MENU(child))
      gtk_popover_popdown(GTK_POPOVER(child)); /* closed without a choice */
  run_until(menu_gone, widget);
  g_assert_cmpint(ran, ==, 1);
  g_assert_cmpint(menu_freed, ==, 2);

  /* a list whose menu was opened empties (its popover is a child too: remove_all never ended) */
  GtkWidget *window = gtk_window_new();
  GtkListBox *list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(list));
  gtk_list_box_append(list, gtk_label_new("row"));
  gtk_window_present(GTK_WINDOW(window));
  run_for(20);
  context_menu(GTK_WIDGET(list), 1, 1, items, G_N_ELEMENTS(items), &ran, count_free);
  context_menu_clear_list(list);
  g_assert_null(gtk_list_box_get_row_at_index(list, 0));
  gtk_popover_popdown(GTK_POPOVER(shown_menu(GTK_WIDGET(list))));
  run_until(menu_gone, list);
  gtk_window_destroy(GTK_WINDOW(window));

  tab_menu(tab_nth(&app, 0), widget, 1, 1);
  choose_menu_item(widget, "Close to the Right");
  assert_tabs("m1.txt");

  open_kept("m2.txt");
  run_for(20);
  widget = tab_nth(&app, 1)->item;
  tab_menu(tab_nth(&app, 1), widget, 1, 1);
  choose_menu_item(widget, "Move to Other Group");
  assert_strip(0, "m1.txt!");
  assert_strip(1, "m2.txt!");
  close_everything();

  /* "Open Preview" on a Markdown file's tab only */
  open_kept("m1.txt");
  open_kept("notes.md");
  run_for(20);
  widget = tab_nth(&app, 0)->item;
  tab_menu(tab_nth(&app, 0), widget, 1, 1);
  char *none = menu_action(widget, "Open Preview");
  g_assert_null(none);
  gtk_popover_popdown(GTK_POPOVER(shown_menu(widget)));
  run_for(20);
  widget = tab_nth(&app, 1)->item;
  tab_menu(tab_nth(&app, 1), widget, 1, 1);
  choose_menu_item(widget, "Open Preview");
  g_assert_cmpint(current_tab(&app)->kind, ==, TAB_PREVIEW);
  g_assert_true(g_str_has_suffix(current_tab(&app)->path, "notes.md"));
  close_everything();
}

static gboolean current_is(gconstpointer name)
{
  Tab *tab = current_tab(&app);

  return tab && g_str_has_suffix(tab->path, name);
}

/* Alt+O goes from a source file to its header and back (clangd, else the file beside it). */
static void test_switch_source_header(void)
{
  g_free(file("pair.h"));
  open_kept("pair.c");
  switch_source_header(current_tab(&app));
  run_until(current_is, "/pair.h");
  switch_source_header(current_tab(&app));
  run_until(current_is, "/pair.c");
  g_assert_true(current_is("/pair.c"));
  close_everything();
}

/* The Open Tabs list at the top of the Files panel (its first child). */
static GtkListBox *open_tabs_list(void)
{
  GtkWidget *section = gtk_widget_get_first_child(app.files);
  GtkWidget *scrolled = gtk_widget_get_last_child(section);

  return GTK_LIST_BOX(gtk_widget_get_first_child(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled))));
}

static int list_rows(GtkListBox *list)
{
  int n = 0;

  while (gtk_list_box_get_row_at_index(list, n))
    n++;
  return n;
}

/* Open Tabs lists every group's tabs (with group headings once two groups have tabs), the current
 * one selected; a click shows a tab; it folds, and the session keeps that. */
static void test_open_tabs(void)
{
  GtkListBox *list = open_tabs_list();
  GtkListBoxRow *row;

  open_kept("o1.txt");
  open_kept("o2.txt");
  run_for(50);
  g_assert_cmpint(list_rows(list), ==, 2);
  g_assert_true(gtk_list_box_get_selected_row(list) == gtk_list_box_get_row_at_index(list, 1)); /* o2 */

  row = gtk_list_box_get_row_at_index(list, 0);
  g_signal_emit_by_name(list, "row-activated", row);
  run_for(50);
  g_assert_true(current_tab(&app) == tab_nth(&app, 0)); /* o1 */

  set_split(&app, TRUE);
  move_tab(tab_nth(&app, 1), 1);
  run_for(50);
  g_assert_cmpint(list_rows(list), ==, 4); /* GROUP 1, o1, GROUP 2, o2 */

  open_tabs_show(FALSE);
  g_assert_false(open_tabs_shown());
  session_save(&app);
  open_tabs_show(TRUE);
  session_apply_window(&app); /* as saved: folded */
  g_assert_false(open_tabs_shown());
  open_tabs_show(TRUE);
  session_save(&app);
  close_everything();
  run_for(50);
  g_assert_cmpint(list_rows(list), ==, 0);
}

/* The picker's entry key controller (the arrows go through it). */
static GtkEventController *picker_keys(GtkWidget *entry)
{
  GListModel *controllers = gtk_widget_observe_controllers(entry);
  GtkEventController *keys = NULL;

  for (guint i = 0; i < g_list_model_get_n_items(controllers) && !keys; i++) {
    GObject *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_EVENT_CONTROLLER_KEY(controller))
      keys = GTK_EVENT_CONTROLLER(controller);
    g_object_unref(controller);
  }
  g_object_unref(controllers);
  return keys;
}

static GtkWidget *focused_entry(void)
{
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(app.window));

  return focus ? gtk_widget_get_ancestor(focus, GTK_TYPE_SEARCH_ENTRY) : NULL;
}

static gboolean entry_focused(gconstpointer data G_GNUC_UNUSED)
{
  return focused_entry() != NULL;
}

/* "Preferences: Color Theme": the arrows show each theme at once, unsaved; Escape goes back to the
 * saved one, Enter keeps the one shown. */
static void test_theme_preview(void)
{
  char *saved = g_strdup(app.settings.color_theme);
  gboolean handled = FALSE;
  GtkWidget *entry;

  color_theme_pick(&app);
  run_until(entry_focused, NULL); /* the first window takes a moment to have the focus */
  entry = focused_entry();
  g_assert_nonnull(entry);
  g_signal_emit_by_name(picker_keys(entry), "key-pressed", GDK_KEY_Down, 0, 0, &handled);
  run_for(50);
  g_assert_true(handled);
  g_assert_cmpstr(palette_id(), !=, saved);             /* the second theme, shown */
  g_assert_cmpstr(app.settings.color_theme, ==, saved); /* not saved */
  g_signal_emit_by_name(entry, "stop-search");          /* Escape */
  run_for(50);
  g_assert_cmpstr(palette_id(), ==, saved);

  color_theme_pick(&app);
  run_for(50);
  entry = focused_entry();
  g_signal_emit_by_name(picker_keys(entry), "key-pressed", GDK_KEY_Down, 0, 0, &handled);
  g_signal_emit_by_name(entry, "activate"); /* Enter */
  run_for(50);
  g_assert_cmpstr(app.settings.color_theme, ==, palette_id());
  g_assert_cmpstr(palette_id(), !=, saved);

  g_set_str(&app.settings.color_theme, saved); /* back, for the other tests */
  color_theme_apply(&app, saved);
  g_free(saved);
}

/* What a Build or Run command prints, run by sh as the shell would. */
static char *shell_output(const char *command)
{
  char *argv[] = { "sh", "-c", (char *)command, NULL }, *out = NULL;

  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
                             NULL, NULL));
  return out;
}

static char *command_for(const char *path, gboolean and_run)
{
  open_file(&app, path);
  run_for(200);
  return project_command(&app, and_run);
}

static char *test_command_for(const char *path)
{
  open_file(&app, path);
  run_for(200);
  return project_test_command(&app);
}

/* A script runs with its interpreter (a Python file in a folder with a Makefile too), Build checks
 * its syntax; an executable with "#!" runs itself; C still goes to make. */
static void test_run_scripts(void)
{
  char *dir = g_build_filename(app.project.root, "run scripts", NULL); /* a space: the paths are quoted */
  char *makefile = g_build_filename(dir, "Makefile", NULL), *py = g_build_filename(dir, "it's.py", NULL);
  char *bad = g_build_filename(dir, "bad.py", NULL), *tool = g_build_filename(dir, "tool", NULL);
  char *c = g_build_filename(dir, "main.c", NULL), *command, *out, *summary;

  g_mkdir_with_parents(dir, 0755);
  g_assert_true(g_file_set_contents(makefile, "all:\n\techo made\n", -1, NULL));
  g_assert_true(g_file_set_contents(py, "import os\nprint('ran in', os.path.basename(os.getcwd()))\n", -1, NULL));
  g_assert_true(g_file_set_contents(bad, "def f(:\n", -1, NULL));
  g_assert_true(g_file_set_contents(tool, "#!/bin/sh\necho tool ran\n", -1, NULL));
  g_chmod(tool, 0755);
  g_assert_true(g_file_set_contents(c, "int main(void) { return 0; }\n", -1, NULL));

  command = command_for(py, TRUE);
  g_assert_false(project_can_debug(&app));
  summary = project_summary(&app);
  g_assert_cmpstr(summary, ==, "python3: it's.py");
  g_free(summary);
  if (g_find_program_in_path("python3")) {
    out = shell_output(command);
    g_assert_cmpstr(out, ==, "ran in run scripts\n"); /* in the file's folder */
    g_free(out);
    g_free(command);
    command = project_command(&app, FALSE);
    out = shell_output(command);
    g_assert_cmpstr(out, ==, "t-code: no syntax errors\n");
    g_free(out);
    g_free(command);
    command = command_for(bad, FALSE);
    out = shell_output(command);
    g_assert_cmpstr(out, ==, ""); /* the error goes to stderr */
    g_free(out);
  }
  g_free(command);

  command = command_for(tool, TRUE);
  out = shell_output(command);
  g_assert_cmpstr(out, ==, "tool ran\n");
  g_free(out);
  g_free(command);

  command = command_for(c, TRUE);
  g_assert_true(project_can_debug(&app));
  g_assert_nonnull(strstr(command, "make"));
  g_free(command);

  close_all_tabs(&app);
  run_for(100);
  for (const char *const *f = (const char *const[]){ makefile, py, bad, tool, c, NULL }; *f; f++)
    g_unlink(*f);
  g_rmdir(dir);
  g_free(c);
  g_free(tool);
  g_free(bad);
  g_free(py);
  g_free(makefile);
  g_free(dir);
}

/* Writes text to root/name, making its folders (mode: its permissions). */
static char *put(const char *root, const char *name, const char *text, int mode)
{
  return write_at(root, name, text, -1, mode);
}

/* Configured and conventional project test commands run in the integrated shell's working tree. */
static void test_run_tests(void)
{
  char *root = g_build_filename(app.project.root, "run tests", NULL);
  char *task_root = g_build_filename(root, "task project", NULL);
  char *make_root = g_build_filename(root, "make project", NULL);
  char *cmake_root = g_build_filename(root, "cmake project", NULL);
  char *meson_root = g_build_filename(root, "meson project", NULL);
  char *node_root = g_build_filename(root, "node project", NULL);
  char *go_root = g_build_filename(root, "go project", NULL);
  char *cargo_root = g_build_filename(root, "cargo project", NULL);
  char *python_root = g_build_filename(root, "python project", NULL);
  char *maven_root = g_build_filename(root, "maven project", NULL);
  char *gradle_root = g_build_filename(root, "gradle project", NULL);
  char *node_empty_root = g_build_filename(root, "node without tests", NULL);
  char *fake_bin = g_build_filename(root, "bin", NULL);
  char *saved_path = g_strdup(g_getenv("PATH")), *path_env;
  char *path, *command, *output, *work_dir;

  work_dir = g_build_filename(task_root, "work", NULL);
  g_mkdir_with_parents(work_dir, 0755);
  g_free(work_dir);
  g_free(put(task_root, ".t-code/tasks.json",
             "{\"default\":\"tests\",\"profiles\":{\"tests\":{\"test\":\"pwd\",\"cwd\":\"work\"}}}", 0644));
  path = put(task_root, "src/main.c", "int main(void) { return 0; }\n", 0644);
  command = test_command_for(path);
  output = shell_output(command);
  char *expected = g_strdup_printf("%s/work\n", task_root);
  g_assert_cmpstr(output, ==, expected);
  g_free(expected);
  char *folder = project_run_folder(&app); /* F5's program starts where Run's does: the profile's cwd */
  expected = g_strdup_printf("%s/work", task_root);
  g_assert_cmpstr(folder, ==, expected);
  g_free(expected);
  g_free(folder);
  g_free(output);
  g_free(command);
  g_free(path);
  path = put(root, "single.c", "int main(void) { return 0; }\n", 0644);
  command = test_command_for(path);
  g_assert_nonnull(strstr(command, "no test runner detected for this file"));
  g_assert_nonnull(strstr(command, "; false"));
  g_free(command);
  g_free(path);

  g_free(put(make_root, "Makefile", "test:\n\t@echo make-test\n", 0644));
  path = put(make_root, "src/main.c", "int main(void) { return 0; }\n", 0644);
  command = test_command_for(path);
  output = shell_output(command);
  g_assert_cmpstr(output, ==, "make-test\n");
  g_free(output);
  g_free(command);
  g_free(path);
  g_free(put(fake_bin, "ctest", "#!/bin/sh\nexit 0\n", 0755));
  g_free(put(fake_bin, "meson", "#!/bin/sh\nexit 0\n", 0755));
  path_env = g_strdup_printf("%s:%s", fake_bin, saved_path ? saved_path : "");
  g_setenv("PATH", path_env, TRUE);

  g_free(put(cmake_root, "CMakeLists.txt", "cmake_minimum_required(VERSION 3.16)\nproject(test_runner NONE)\n", 0644));
  path = put(cmake_root, "src/main.c", "int main(void) { return 0; }\n", 0644);
  command = test_command_for(path);
  g_assert_nonnull(strstr(command, "cmake --build"));
  g_assert_nonnull(strstr(command, "ctest -N"));
  g_assert_nonnull(strstr(command, "ctest --output-on-failure"));
  g_assert_nonnull(strstr(command, "Total Tests: [1-9]"));
  g_free(command);
  g_free(path);
  g_free(put(meson_root, "meson.build", "project('demo', 'c')\nexecutable('demo', 'src/main.c')\n", 0644));
  path = put(meson_root, "src/main.c", "int main(void) { return 0; }\n", 0644);
  command = command_for(path, FALSE);
  g_assert_nonnull(strstr(command, "meson setup"));
  g_assert_nonnull(strstr(command, "meson compile -C"));
  g_assert_nonnull(strstr(command, "--buildtype=debug"));
  g_free(command);
  command = command_for(path, TRUE);
  g_assert_nonnull(strstr(command, "No executable in Meson build/"));
  g_assert_nonnull(strstr(command, "meson-private"));
  g_free(command);
  char *summary = project_summary(&app);
  g_assert_cmpstr(summary, ==, "Meson: meson project");
  g_free(summary);
  g_assert_true(project_can_debug(&app));
  command = project_test_command(&app);
  g_assert_nonnull(strstr(command, "meson test -C"));
  g_free(command);
  g_setenv("PATH", saved_path ? saved_path : "", TRUE);
  g_free(path_env);
  g_free(path);

  g_free(put(node_root, "package.json", "{\"scripts\":{\"test\":\"echo node-test\"}}\n", 0644));
  path = put(node_root, "src/index.js", "console.log('app')\n", 0644);
  command = test_command_for(path);
  g_assert_nonnull(strstr(command, "npm test"));
  g_free(command);
  g_free(path);
  g_free(put(node_empty_root, "package.json", "{\"name\":\"without-tests\"}\n", 0644));
  path = put(node_empty_root, "index.js", "console.log('app')\n", 0644);
  command = test_command_for(path);
  g_assert_nonnull(strstr(command, "no test script in package.json"));
  g_assert_nonnull(strstr(command, "; false"));
  g_free(command);
  g_free(path);

  g_free(put(go_root, "go.mod", "module example.test/demo\n\ngo 1.22\n", 0644));
  path = put(go_root, "main.go", "package main\nfunc main() {}\n", 0644);
  command = test_command_for(path);
  g_assert_nonnull(strstr(command, "go test ./..."));
  g_free(command);
  g_free(path);

  g_free(put(cargo_root, "Cargo.toml", "[package]\nname=\"demo\"\nversion=\"0.1.0\"\n", 0644));
  path = put(cargo_root, "src/main.rs", "fn main() {}\n", 0644);
  command = test_command_for(path);
  g_assert_nonnull(strstr(command, "cargo test"));
  g_free(command);
  g_free(path);

  g_free(put(python_root, "pyproject.toml", "[project]\nname = \"demo\"\n", 0644));
  path = put(python_root, "main.py", "print('app')\n", 0644);
  command = test_command_for(path);
  g_assert_nonnull(strstr(command, "-m pytest"));
  g_free(command);
  g_free(path);
  g_free(put(maven_root, "pom.xml", "<project><modelVersion>4.0.0</modelVersion></project>\n", 0644));
  path = put(maven_root, "src/main/java/App.java", "class App {}\n", 0644);
  command = test_command_for(path);
  g_assert_nonnull(strstr(command, "mvn test"));
  g_free(command);
  g_free(path);

  g_free(put(gradle_root, "build.gradle", "", 0644));
  path = put(gradle_root, "src/main/java/App.java", "class App {}\n", 0644);
  command = test_command_for(path);
  g_assert_nonnull(strstr(command, "gradle test"));
  g_free(command);
  g_free(path);

  close_all_tabs(&app);
  run_for(100);
  char *argv[] = { "rm", "-rf", root, NULL };
  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL));
  g_free(python_root);
  g_free(cargo_root);
  g_free(go_root);
  g_free(node_root);
  g_free(gradle_root);
  g_free(maven_root);
  g_free(node_empty_root);
  g_free(cmake_root);
  g_free(make_root);
  g_free(task_root);
  g_free(meson_root);
  g_free(saved_path);
  g_free(fake_bin);
  g_free(root);
}

/* Whether Run's command for path contains part (and, when program is installed, what running it
 * prints contains output). */
static void check_run(const char *path, const char *part, const char *program, const char *output)
{
  char *command = command_for(path, TRUE), *found = program ? g_find_program_in_path(program) : NULL;

  g_assert_false(project_can_debug(&app));
  if (!strstr(command, part))
    g_error("%s: no \"%s\" in: %s", path, part, command);
  if (found && output) {
    char *out = shell_output(command);
    if (!strstr(out, output))
      g_error("%s printed \"%s\", not \"%s\"", command, out, output);
    g_free(out);
  }
  g_free(found);
  g_free(command);
}

/* Other languages' files run in their own projects: Python from its root (its packages import, its
 * venv's python), Node's start script, Cargo's binary, Go's package, Maven's class, Gradle's wrapper. */
static void test_run_projects(void)
{
  char *root = g_build_filename(app.project.root, "run projects", NULL), *path, *summary;
  char *py = g_build_filename(root, "py", NULL), *node = g_build_filename(root, "node", NULL);
  char *cargo = g_build_filename(root, "cargo", NULL), *other = g_build_filename(root, "other", NULL);

  g_free(put(py, "pyproject.toml", "[project]\nname = \"demo\"\n", 0644));
  g_free(put(py, "mypkg/__init__.py", "", 0644));
  g_free(put(py, "mypkg/util.py", "NAME = 'util'\n", 0644));
  path = put(py, "scripts/main.py",
             "import os\nfrom mypkg import util\nprint(util.NAME, os.path.basename(os.getcwd()))\n", 0644);
  check_run(path, "PYTHONPATH=", "python3", "util py\n"); /* from the root, its package found */
  g_free(path);
  /* in a package: as a module, so its relative imports work */
  path = put(py, "mypkg/tool.py", "from .util import NAME\nprint('tool', NAME)\n", 0644);
  check_run(path, "-m 'mypkg.tool'", "python3", "tool util\n");
  g_free(path);
  path = g_build_filename(py, "scripts", "main.py", NULL);
  g_free(put(py, ".venv/pyvenv.cfg", "home = /usr/bin\n", 0644));
  g_free(put(py, ".venv/bin/python", "#!/bin/sh\necho venv python\n", 0755));
  check_run(path, ".venv/bin/python", "sh", "venv python\n");
  summary = project_summary(&app);
  g_assert_cmpstr(summary, ==, "python (.venv): main.py");
  g_free(summary);
  /* the interpreter picked for the project wins ("Python: Select Interpreter…") */
  g_free(put(py, "tools/pyvenv.cfg", "home = /usr/bin\n", 0644));
  char *picked = put(py, "tools/bin/python", "#!/bin/sh\necho picked python\n", 0755);
  toolchain_set(&app, "python", picked);
  check_run(path, "tools/bin/python", "sh", "picked python\n");
  summary = project_summary(&app);
  g_assert_cmpstr(summary, ==, "python (tools): main.py");
  g_free(summary);
  toolchain_set(&app, "python", NULL);
  g_free(picked);
  g_free(path);

  g_free(put(node, "package.json", "{ \"scripts\": { \"start\": \"node index.js\" } }\n", 0644));
  g_free(put(node, "index.js", "console.log('started')\n", 0644));
  path = put(node, "lib/util.js", "module.exports = 1\n", 0644); /* not the program: the start script runs */
  check_run(path, "npm run start", "npm", "started\n");
  g_free(put(node, "pnpm-lock.yaml", "", 0644));
  check_run(path, "pnpm run start", NULL, NULL);
  g_free(path);
  path = put(other, "plain/package.json", "{ \"name\": \"plain\" }\n", 0644);
  g_free(path);
  path = put(other, "plain/tool.js", "console.log('tool')\n", 0644); /* no start script: the file */
  check_run(path, "node ", "node", "tool\n");
  g_free(path);

  g_free(put(cargo, "Cargo.toml", "[package]\nname = \"demo\"\nversion = \"0.1.0\"\nedition = \"2021\"\n", 0644));
  g_free(put(cargo, "src/main.rs", "fn main() { println!(\"main ran\"); }\n", 0644));
  path = put(cargo, "src/bin/other.rs", "fn main() { println!(\"other ran\"); }\n", 0644);
  check_run(path, "cargo run --bin 'other'", "cargo", "other ran\n");
  /* the picked features and target ("Rust: Select Features…" / "Select Target…") */
  toolchain_set(&app, "rust-features", "json,tls");
  toolchain_set(&app, "rust-target", "wasm32-unknown-unknown");
  check_run(path, "cargo run --bin 'other' --features 'json,tls' --target 'wasm32-unknown-unknown'", NULL, NULL);
  toolchain_set(&app, "rust-features", "all");
  toolchain_set(&app, "rust-target", NULL);
  check_run(path, "cargo run --bin 'other' --all-features)", NULL, NULL);
  toolchain_set(&app, "rust-features", NULL);
  g_free(path);
  /* a library in a workspace: cargo runs the workspace's program */
  g_free(put(other, "ws/Cargo.toml", "[workspace]\nmembers = [\"app\", \"lib\"]\nresolver = \"2\"\n", 0644));
  g_free(put(other, "ws/app/Cargo.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\nedition = \"2021\"\n", 0644));
  g_free(put(other, "ws/app/src/main.rs", "fn main() { println!(\"app ran\"); }\n", 0644));
  g_free(put(other, "ws/lib/Cargo.toml", "[package]\nname = \"lib\"\nversion = \"0.1.0\"\nedition = \"2021\"\n", 0644));
  path = put(other, "ws/lib/src/lib.rs", "pub fn f() {}\n", 0644);
  check_run(path, "/ws' && cargo run", "cargo", "app ran\n");
  g_free(path);
  path = put(other, "alone.rs", "fn main() { println!(\"alone ran\"); }\n", 0644);
  check_run(path, "rustc", "rustc", "alone ran\n");
  g_free(path);

  g_free(put(other, "go/go.mod", "module demo\n\ngo 1.22\n", 0644));
  path = put(other, "go/cmd/demo/main.go", "package main\n\nfunc main() {}\n", 0644);
  check_run(path, "go run .", NULL, NULL);
  g_free(path);
  g_free(put(other, "maven/pom.xml", "<project/>\n", 0644));
  path = put(other, "maven/src/main/java/com/example/App.java", "package com.example;\n\npublic class App {}\n", 0644);
  check_run(path, "java -cp target/classes 'com.example.App'", NULL, NULL);
  g_free(path);
  g_free(put(other, "gradle/build.gradle", "", 0644));
  g_free(put(other, "gradle/gradlew", "#!/bin/sh\n", 0755));
  path = put(other, "gradle/src/main/java/App.java", "public class App {}\n", 0644);
  check_run(path, "./gradlew run", NULL, NULL);
  g_free(path);

  close_all_tabs(&app);
  run_for(100);
  char *argv[] = { "rm", "-rf", root, NULL };
  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL));
  g_free(other);
  g_free(cargo);
  g_free(node);
  g_free(py);
  g_free(root);
}

/* The text in the preview's buffer from where needle starts. */
static GtkTextIter preview_find(GtkTextBuffer *buffer, const char *needle)
{
  GtkTextIter start, at;

  gtk_text_buffer_get_start_iter(buffer, &start);
  g_assert_true(gtk_text_iter_forward_search(&start, needle, 0, &at, NULL, NULL));
  return at;
}

static gboolean has_tag(GtkTextIter *iter, const char *name)
{
  return gtk_text_iter_has_tag(
      iter, gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(gtk_text_iter_get_buffer(iter)), name));
}

/* The Markdown preview looks: quotes are not italic, <sub> text is smaller and lower, a link is
 * underlined only under the pointer, and nothing scrolls sideways. */
static void test_preview_look(void)
{
  char *path = g_build_filename(app.project.root, "look.md", NULL);
  GtkEventController *motion = NULL;
  GtkTextIter iter;
  GdkRectangle where;
  int x, y;

  g_assert_true(
      g_file_set_contents(path,
                          "# Title\n\n"
                          "> quoted **bold**\n\n"
                          "<sub>a caption</sub>\n\n"
                          "see [the link](https://example.com) here\n\n"
                          "```c\nint a_very_long_line_of_code_that_is_much_wider_than_the_view = 1; // and more\n```\n",
                          -1, NULL));
  open_preview(&app, path);
  run_for(300);
  Tab *tab = current_tab(&app);
  g_assert_true(tab->kind == TAB_PREVIEW);
  GtkTextView *view = GTK_TEXT_VIEW(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(tab->page)));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
  GtkPolicyType hpolicy;

  gtk_scrolled_window_get_policy(GTK_SCROLLED_WINDOW(tab->page), &hpolicy, NULL);
  g_assert_cmpint(hpolicy, ==, GTK_POLICY_EXTERNAL);

  iter = preview_find(buffer, "quoted");
  g_assert_true(has_tag(&iter, "quote"));
  GSList *tags = gtk_text_iter_get_tags(&iter);
  for (GSList *each = tags; each; each = each->next) {
    gboolean italic;
    g_object_get(each->data, "style-set", &italic, NULL);
    g_assert_false(italic);
  }
  g_slist_free(tags);
  iter = preview_find(buffer, "a caption");
  g_assert_true(has_tag(&iter, "sub")); /* smaller and lowered */

  iter = preview_find(buffer, "the link");
  g_assert_true(has_tag(&iter, "link"));
  g_assert_false(has_tag(&iter, "link-hover"));
  GListModel *controllers = gtk_widget_observe_controllers(GTK_WIDGET(view));
  for (guint i = 0; !motion && i < g_list_model_get_n_items(controllers); i++) {
    GtkEventController *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_EVENT_CONTROLLER_MOTION(controller))
      motion = controller;
    g_object_unref(controller);
  }
  g_object_unref(controllers);
  g_assert_nonnull(motion);
  gtk_text_iter_forward_chars(&iter, 2);
  /* Where the link is once the view is laid out (under load that takes a while): the point found
   * must lead back to the link. */
  gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  GtkTextIter back;
  do {
    run_for(20);
    gtk_text_view_get_iter_location(view, &iter, &where);
    gtk_text_view_get_iter_at_location(view, &back, where.x + 1, where.y + where.height / 2);
  } while ((where.height <= 0 || !has_tag(&back, "link")) && g_get_monotonic_time() < end);
  if (where.height > 0) {
    gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, where.x + 1, where.y + where.height / 2, &x,
                                          &y);
    g_signal_emit_by_name(motion, "motion", (double)x, (double)y);
    iter = preview_find(buffer, "the link");
    gtk_text_iter_forward_chars(&iter, 2);
    g_assert_true(has_tag(&iter, "link-hover"));
    g_signal_emit_by_name(motion, "leave");
    iter = preview_find(buffer, "the link");
    gtk_text_iter_forward_chars(&iter, 2);
    g_assert_false(has_tag(&iter, "link-hover"));
  }

  iter = preview_find(buffer, "int a_very_long");
  g_assert_true(has_tag(&iter, "pre"));
  GtkWrapMode wrap;
  g_object_get(gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), "pre"), "wrap-mode", &wrap, NULL);
  g_assert_cmpint(wrap, !=, GTK_WRAP_NONE); /* a long line wraps: the preview does not scroll sideways */

  /* Ctrl+= in a preview tab sizes the previews' text only, not the editors'; Ctrl+0 brings it back. */
  GtkTextTag *base = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), "base");
  double before, after;
  g_object_get(base, "size-points", &before, NULL);
  run_command(&app, "Zoom In");
  g_object_get(base, "size-points", &after, NULL);
  g_assert_cmpfloat_with_epsilon(app.settings.preview_scale, 1.1, 1e-9);
  g_assert_cmpfloat(app.settings.font_scale, ==, 1.0);
  g_assert_cmpfloat_with_epsilon(after, before * 1.1, 0.01); /* Pango keeps 1/1024 points */
  run_command(&app, "Reset Zoom");
  g_assert_cmpfloat(app.settings.preview_scale, ==, 1.0);
  g_assert_cmpfloat(app.settings.font_scale, ==, 1.0);

  remove_tab(tab);
  g_unlink(path);
  g_free(path);
}

/* The paintables (formulas) in the preview's buffer, in order; the caller frees the array. */
static GPtrArray *preview_formulas(GtkTextBuffer *buffer, GArray *offsets)
{
  GPtrArray *formulas = g_ptr_array_new();
  GtkTextIter iter;

  for (gtk_text_buffer_get_start_iter(buffer, &iter); !gtk_text_iter_is_end(&iter); gtk_text_iter_forward_char(&iter)) {
    GdkPaintable *paintable = gtk_text_iter_get_paintable(&iter);
    if (paintable) {
      int offset = gtk_text_iter_get_offset(&iter);
      g_ptr_array_add(formulas, paintable);
      g_array_append_val(offsets, offset);
    }
  }
  return formulas;
}

/* How far a tag at the iter lowers the text (a formula's depth), in Pango units. */
static int lowered(GtkTextIter *iter)
{
  GSList *tags = gtk_text_iter_get_tags(iter);
  int rise = 0;

  for (GSList *each = tags; each; each = each->next) {
    gboolean set;
    int value;
    g_object_get(each->data, "rise-set", &set, "rise", &value, NULL);
    if (set)
      rise = value;
  }
  g_slist_free(tags);
  return -rise;
}

/* TeX math in the preview: $...$ inline on the baseline (a fraction reaches below it), $$...$$ and
 * ```math centered on lines of their own, prices stay text, broken TeX still shows (too deep: nothing); a quote that
 * starts with [!WARNING] is an alert. Images and links with an empty target (half typed) show their text. */
static void test_preview_math(void)
{
  char *path = g_build_filename(app.project.root, "math.md", NULL);
  GString *deep = g_string_new("$");
  GArray *offsets = g_array_new(FALSE, FALSE, sizeof(int));
  GtkTextIter iter;

  for (int i = 0; i < 300; i++)
    g_string_append_c(deep, '{');
  g_string_append(deep, "x$\n\n");
  char *text =
      g_strconcat("Empty targets ![alt]() and [a link]().\n\n| h |\n|---|\n| ![c]() [d]() |\n\n"
                  "Inline $x^2$ and $\\frac{a}{b}$ here.\n\n"
                  "$$\\sum_{i=1}^n i = \\frac{n(n+1)}{2}$$\n\n"
                  "```math\n\\begin{aligned} a &= b \\\\ c &= \\left( \\sqrt[3]{d} \\right) \\end{aligned}\n```\n\n"
                  "Broken $\\frac{\\left( {x \\foo$ and ",
                  deep->str,
                  "> [!WARNING]\n> careful [link](#x)\n\n"
                  "Costs $5 and $10.\n",
                  NULL);
  g_assert_true(g_file_set_contents(path, text, -1, NULL));
  open_preview(&app, path);
  run_for(300);
  Tab *tab = current_tab(&app);
  g_assert_true(tab->kind == TAB_PREVIEW);
  GtkTextView *view = GTK_TEXT_VIEW(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(tab->page)));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);

  GPtrArray *formulas = preview_formulas(buffer, offsets);
  g_assert_cmpuint(formulas->len, ==, 5); /* the one 300 groups deep shows nothing, without crashing */
  GdkPaintable *square = g_ptr_array_index(formulas, 0), *fraction = g_ptr_array_index(formulas, 1);
  g_assert_cmpint(gdk_paintable_get_intrinsic_height(fraction), >, gdk_paintable_get_intrinsic_height(square));
  gtk_text_buffer_get_iter_at_offset(buffer, &iter, g_array_index(offsets, int, 1));
  g_assert_cmpint(lowered(&iter), >, 0); /* the denominator goes below the baseline */
  g_assert_false(has_tag(&iter, "math-display"));
  for (guint i = 2; i <= 3; i++) {
    gtk_text_buffer_get_iter_at_offset(buffer, &iter, g_array_index(offsets, int, i));
    g_assert_true(has_tag(&iter, "math-display"));
    g_assert_true(gtk_text_iter_starts_line(&iter));
  }
  GdkPaintable *aligned = g_ptr_array_index(formulas, 3);
  g_assert_cmpint(gdk_paintable_get_intrinsic_height(aligned), >, 2 * gdk_paintable_get_intrinsic_height(square));

  iter = preview_find(buffer, "Costs $5 and $10.");
  iter = preview_find(buffer, "Warning");
  g_assert_true(has_tag(&iter, "alert4"));
  g_assert_false(has_tag(&iter, "quote"));
  gtk_text_buffer_get_start_iter(buffer, &iter);
  g_assert_false(gtk_text_iter_forward_search(&iter, "[!WARNING]", 0, NULL, NULL, NULL));
  iter = preview_find(buffer, "link");
  g_assert_true(has_tag(&iter, "link"));

  g_ptr_array_free(formulas, TRUE);
  g_array_free(offsets, TRUE);
  remove_tab(tab);
  g_unlink(path);
  g_free(path);
  g_free(text);
  g_string_free(deep, TRUE);
}

/* The first label under root whose markup holds needle, or NULL. */
static GtkWidget *find_label_with(GtkWidget *root, const char *needle)
{
  if (GTK_IS_LABEL(root) && strstr(gtk_label_get_label(GTK_LABEL(root)), needle))
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_label_with(child, needle);
    if (found)
      return found;
  }
  return NULL;
}

/* Whether a preview shows a table's grid. */
static gboolean table_shown(gconstpointer view)
{
  return find_widget(GTK_WIDGET(view), GTK_TYPE_GRID, NULL, "tcode-preview-table") != NULL;
}

/* The preview shows what GitHub shows: every HTML5 entity, :emoji:, raw HTML (formatting, <sub> and <sup>
 * raised, <kbd>, links, anchors, images, comments over several lines left out, GFM's filtered tags as
 * text), footnotes, <details> that a click on their summary opens, math and images in table cells, and a
 * caption over a diagram it cannot draw. */
static void test_preview_github(void)
{
  char *path = g_build_filename(app.project.root, "github.md", NULL);
  char *svg = g_build_filename(app.project.root, "badge.svg", NULL);
  GtkTextIter iter;
  GdkRectangle where;
  int x, y, pictures = 0;

  g_assert_true(g_file_set_contents(svg,
                                    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"10\">"
                                    "<rect width=\"40\" height=\"10\" fill=\"#08c\"/></svg>",
                                    -1, NULL));
  g_assert_true(g_file_set_contents(
      path,
      "Entities &frac12; &NotEqualTilde; &#x1F600; &bogus; and :tada: but :no_such_emoji:\n\n"
      "Tags <ins>under</ins> H<sub>2</sub>O E=mc<sup>2</sup> <kbd>Ctrl</kbd> <a href=\"#end\">html link</a>\n\n"
      "<!-- a comment\nover > lines -->\n\n"
      "<p align=\"center\"><a href=\"https://example.com\"><img src=\"badge.svg\" alt=\"logo\"></a></p>\n\n"
      "A note[^n].\n\n"
      "<details>\n<summary>More</summary>\n\nsecret text\n\n</details>\n\n"
      "| a | b |\n|---|---|\n| $x^2$ | ![c](badge.svg) |\n\n"
      "```mermaid\ngraph TD; A-->B;\n```\n\n"
      "<script>alert(1)</script>\n\n"
      "<a name=\"end\"></a>The end.\n\n"
      "[^n]: The note's text.\n",
      -1, NULL));
  open_preview(&app, path);
  run_for(300);
  Tab *tab = current_tab(&app);
  g_assert_true(tab->kind == TAB_PREVIEW);
  GtkTextView *view = GTK_TEXT_VIEW(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(tab->page)));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);

  iter = preview_find(buffer, "Entities ½ ≂̸ 😀 &bogus; and 🎉 but :no_such_emoji:");
  iter = preview_find(buffer, "under");
  g_assert_true(has_tag(&iter, "underline"));
  iter = preview_find(buffer, "2O");
  g_assert_true(has_tag(&iter, "sub"));
  iter = preview_find(buffer, "2 Ctrl");
  g_assert_true(has_tag(&iter, "sup"));
  iter = preview_find(buffer, "Ctrl");
  g_assert_true(has_tag(&iter, "code"));
  iter = preview_find(buffer, "html link");
  g_assert_true(has_tag(&iter, "link"));
  gtk_text_buffer_get_start_iter(buffer, &iter);
  g_assert_false(gtk_text_iter_forward_search(&iter, "comment", 0, NULL, NULL, NULL));
  g_assert_false(gtk_text_iter_forward_search(&iter, "lines", 0, NULL, NULL, NULL));
  g_assert_false(gtk_text_iter_forward_search(&iter, "[^n]", 0, NULL, NULL, NULL));
  iter = preview_find(buffer, "<script>alert(1)</script>");
  iter = preview_find(buffer, "Mermaid diagram");
  g_assert_true(has_tag(&iter, "caption"));
  iter = preview_find(buffer, "The note's text. ↩");
  iter = preview_find(buffer, "↩");
  g_assert_true(has_tag(&iter, "link"));

  for (gtk_text_buffer_get_start_iter(buffer, &iter); !gtk_text_iter_is_end(&iter); gtk_text_iter_forward_char(&iter)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    guint n = 0;
    GtkWidget **widgets = anchor ? gtk_text_child_anchor_get_widgets(anchor, &n) : NULL;
    if (n && GTK_IS_PICTURE(widgets[0])) {
      pictures++;
      g_assert_true(has_tag(&iter, "center")); /* <p align="center"> */
    }
    g_free(widgets);
  }
  g_assert_cmpint(pictures, ==, 1); /* the SVG drawn (the one in the table is in its grid) */
  run_until(table_shown, view);     /* a table comes in once the view has its size */
  GtkWidget *grid = find_widget(GTK_WIDGET(view), GTK_TYPE_GRID, NULL, "tcode-preview-table");
  g_assert_nonnull(grid);
  g_assert_nonnull(find_label_with(grid, "<sup>2</sup>")); /* $x^2$ in a cell */
  g_assert_nonnull(find_widget(grid, GTK_TYPE_PICTURE, NULL, NULL));

  /* <details> is closed: its text is hidden until its summary is clicked. */
  iter = preview_find(buffer, "secret text");
  g_assert_true(has_tag(&iter, "hidden"));
  iter = preview_find(buffer, "▶  More");
  GtkGesture *click = NULL;
  GListModel *controllers = gtk_widget_observe_controllers(GTK_WIDGET(view));
  for (guint i = 0; !click && i < g_list_model_get_n_items(controllers); i++) {
    GtkEventController *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_GESTURE_CLICK(controller))
      click = GTK_GESTURE(controller);
    g_object_unref(controller);
  }
  g_object_unref(controllers);
  g_assert_nonnull(click);
  gtk_text_iter_forward_chars(&iter, 4);
  gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  do {
    run_for(20);
    gtk_text_view_get_iter_location(view, &iter, &where);
  } while (where.height <= 0 && g_get_monotonic_time() < end);
  gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, where.x + 1, where.y + where.height / 2, &x, &y);
  g_signal_emit_by_name(click, "released", 1, (double)x, (double)y);
  iter = preview_find(buffer, "secret text");
  g_assert_false(has_tag(&iter, "hidden"));
  iter = preview_find(buffer, "▼  More");

  remove_tab(tab);
  g_unlink(path);
  g_unlink(svg);
  g_free(svg);
  g_free(path);
}

/* The tables in a preview's text: how many there are, and how many show their grid. */
static void count_tables(GtkTextBuffer *buffer, int *tables, int *shown)
{
  GtkTextIter iter;

  *tables = *shown = 0;
  for (gtk_text_buffer_get_start_iter(buffer, &iter); !gtk_text_iter_is_end(&iter); gtk_text_iter_forward_char(&iter)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    guint n = 0;
    GtkWidget **widgets = anchor ? gtk_text_child_anchor_get_widgets(anchor, &n) : NULL;
    if (n && GTK_IS_BOX(widgets[0])) {
      GtkWidget *grid = gtk_widget_get_first_child(widgets[0]);
      int height;
      (*tables)++;
      gtk_widget_get_size_request(widgets[0], NULL, &height);
      if (grid)
        (*shown)++;
      else
        g_assert_cmpint(height, >, 20); /* as tall as its rows will be */
    }
    g_free(widgets);
  }
}

static gboolean picture_shown(gconstpointer view)
{
  return find_widget(GTK_WIDGET(view), GTK_TYPE_PICTURE, NULL, NULL) != NULL;
}

static gboolean last_table_shown(gconstpointer buffer)
{
  GtkTextIter iter;
  GtkTextChildAnchor *anchor = NULL;

  for (gtk_text_buffer_get_end_iter((GtkTextBuffer *)buffer, &iter); !anchor && gtk_text_iter_backward_char(&iter);)
    anchor = gtk_text_iter_get_child_anchor(&iter);
  guint n = 0;
  GtkWidget **widgets = anchor ? gtk_text_child_anchor_get_widgets(anchor, &n) : NULL;
  gboolean shown = n && gtk_widget_get_first_child(widgets[0]);
  g_free(widgets);
  return shown;
}

/* Whether the text at iter has a color of a code block's highlighting. */
static gboolean colored(const GtkTextIter *iter)
{
  GSList *tags = gtk_text_iter_get_tags(iter);
  gboolean found = FALSE;

  for (GSList *each = tags; each; each = each->next) {
    gboolean set;
    char *name;
    g_object_get(each->data, "foreground-set", &set, "name", &name, NULL);
    found |= set && !name; /* the copies of the style scheme's colors have no names */
    g_free(name);
  }
  g_slist_free(tags);
  return found;
}

/* A big file opens without the window stopping: its hundreds of tables come in as they are scrolled
 * near (each one waits as an empty box as tall as it will be), a local image is decoded in a thread
 * (its alt text shows meanwhile), and code blocks of several languages are still colored. */
static void test_preview_big(void)
{
  static const guchar pixel[] = {
    /* a 1×1 PNG */
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x00, 0x05, 0x00, 0x01, 0xff,
    0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
  };
  char *png = g_build_filename(app.project.root, "shot.png", NULL);
  char *path = g_build_filename(app.project.root, "big.md", NULL);
  GString *text = g_string_new("![a shot](shot.png)\n\n");
  int tables, shown;

  g_assert_true(g_file_set_contents(png, (const char *)pixel, sizeof pixel, NULL));
  for (int i = 0; i < 300; i++) {
    g_string_append_printf(text, "## Part %d\n\nSome `code` here.\n\n| Key | What |\n|---|---|\n", i);
    for (int row = 0; row < 6; row++)
      g_string_append_printf(text, "| `Ctrl+%d` | does thing %d with some longer words |\n", row, row);
    g_string_append(text, i % 2 ? "\n```python\nwhile True:\n    pass\n```\n\n" : "\n```c\nreturn 0;\n```\n\n");
  }
  g_assert_true(g_file_set_contents(path, text->str, -1, NULL));
  open_preview(&app, path);
  Tab *tab = current_tab(&app);
  g_assert_true(tab->kind == TAB_PREVIEW);
  GtkTextView *view = GTK_TEXT_VIEW(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(tab->page)));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
  g_assert_false(picture_shown(view)); /* being decoded */
  run_until(picture_shown, view);
  g_assert_true(picture_shown(view));

  run_until(table_shown, view);
  count_tables(buffer, &tables, &shown);
  g_assert_cmpint(tables, ==, 300);
  g_assert_cmpint(shown, >, 0);
  g_assert_cmpint(shown, <, 30); /* the ones near the top */
  g_assert_false(last_table_shown(buffer));

  GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view));
  gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment));
  run_until(last_table_shown, buffer);
  g_assert_true(last_table_shown(buffer));

  GtkTextIter iter = preview_find(buffer, "return 0;");
  g_assert_true(colored(&iter));
  iter = preview_find(buffer, "while True:");
  g_assert_true(colored(&iter));

  remove_tab(tab);
  g_unlink(path);
  g_unlink(png);
  g_string_free(text, TRUE);
  g_free(path);
  g_free(png);
}

/* Images open in a tab that shows them (temporary from a single click, like editors), not as text; an
 * SVG opens in an editor and its preview draws it; a PDF or an archive does not open as text. */
static void test_open_images(void)
{
  char *png = g_build_filename(app.project.root, "pixel.png", NULL);
  char *svg = g_build_filename(app.project.root, "shape.svg", NULL);
  char *pdf = g_build_filename(app.project.root, "missing.pdf", NULL); /* missing: no app starts */
  char *zip = g_build_filename(app.project.root, "missing.tar.gz", NULL);
  static const guchar pixel[] = {
    /* a 1×1 PNG */
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x00, 0x05, 0x00, 0x01, 0xff,
    0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
  };
  GtkTextIter iter;

  g_assert_true(g_file_set_contents(png, (const char *)pixel, sizeof pixel, NULL));
  g_assert_true(g_file_set_contents(
      svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"4\"><rect width=\"8\" height=\"4\"/></svg>",
      -1, NULL));

  open_file_temporary(&app, png);
  run_for(100);
  Tab *tab = current_tab(&app);
  g_assert_true(tab->kind == TAB_PREVIEW);
  g_assert_true(tab->temporary);
  g_assert_cmpstr(gtk_label_get_text(tab->label), ==, "pixel.png");
  GtkTextView *view = GTK_TEXT_VIEW(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(tab->page)));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
  gtk_text_buffer_get_start_iter(buffer, &iter);
  g_assert_nonnull(gtk_text_iter_get_child_anchor(&iter));
  iter = preview_find(buffer, "1 × 1 · ");
  open_file(&app, png); /* opened on purpose: kept */
  g_assert_true(current_tab(&app) == tab);
  g_assert_false(tab->temporary);
  remove_tab(tab);

  open_file(&app, svg);
  tab = current_tab(&app);
  g_assert_true(tab->kind == TAB_EDITOR); /* SVG is text */
  run_command(&app, "Markdown Preview");
  Tab *drawn = current_tab(&app);
  g_assert_true(drawn->kind == TAB_PREVIEW);
  view = GTK_TEXT_VIEW(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(drawn->page)));
  iter = preview_find(gtk_text_view_get_buffer(view), "8 × 4 · ");
  remove_tab(drawn);
  remove_tab(tab);

  int tabs = tab_count(&app);
  open_file(&app, pdf);
  open_file_temporary(&app, zip);
  g_assert_cmpint(tab_count(&app), ==, tabs);

  g_unlink(png);
  g_unlink(svg);
  g_free(zip);
  g_free(pdf);
  g_free(svg);
  g_free(png);
}

/* A renamed theme's old id (in a settings.ini from before) gives its new one. */
static void test_theme_renamed(void)
{
  g_assert_cmpstr(palette_known_id("barbie"), ==, "pink");
  g_assert_cmpstr(palette_known_id("windows-95"), ==, "retro-gray");
  g_assert_cmpstr(palette_known_id("hotdog-stand"), ==, "hot-red");
  g_assert_cmpstr(palette_known_id("game-boy"), ==, "handheld-green");
  g_assert_cmpstr(palette_known_id("commodore-64"), ==, "8-bit-blue");
  g_assert_cmpstr(palette_known_id("matrix"), ==, "green-rain");
  g_assert_cmpstr(palette_known_id("dark-modern"), ==, "dark-classic");
  g_assert_cmpstr(palette_known_id("dracula"), ==, "dracula");
  g_assert_cmpstr(palette_known_id("no-such-theme"), ==, "dark-classic");
}

/* Every theme's editor scheme gets the theme's current line color (its key was misspelled once, so
 * every theme but Dark Classic kept Dark Classic's). */
static void test_theme_current_line(void)
{
  char *saved = g_strdup(app.settings.color_theme);

  for (int n = 1; n < palette_count(); n++) {
    GtkSourceStyle *style;
    char *background = NULL;

    color_theme_apply(&app, palette_nth_id(n));
    style = gtk_source_style_scheme_get_style(palette_scheme(&app), "current-line");
    g_assert_nonnull(style);
    g_object_get(style, "background", &background, NULL);
    g_assert_cmpint(g_ascii_strcasecmp(background, palette_hex(PAL_CURRENT_LINE)), ==, 0);
    g_free(background);
  }
  color_theme_apply(&app, saved);
  g_free(saved);
}

/* A file opens at its left edge, also in a narrow view without word wrap (the cursor's reveal used to
 * scroll sideways by a few pixels, cutting off every line's first letters). */
static void test_opens_at_left_edge(void)
{
  char *path = g_build_filename(app.project.root, "long.txt", NULL);
  GString *text = g_string_new(NULL);
  gboolean wrap = app.settings.word_wrap;
  GtkAdjustment *sideways;

  for (int i = 0; i < 40; i++)
    g_string_append(text, "a line much longer than the view is wide, so that it could scroll sideways\n");
  g_assert_true(g_file_set_contents(path, text->str, -1, NULL));
  app.settings.word_wrap = FALSE;
  open_file(&app, path);
  run_for(300);
  sideways = gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(tab_nth(&app, 0)->source));
  g_assert_cmpfloat(gtk_adjustment_get_value(sideways), ==, 0);
  close_everything();
  app.settings.word_wrap = wrap;
  g_string_free(text, TRUE);
  g_free(path);
}

/* The first run (no session.ini yet): Files shows on the left, the shell below the editors, Outline,
 * Hierarchy and Testing on the right. */
static void test_first_run_layout(void)
{
  g_assert_cmpint(panel_dock(&app, "files"), ==, 0);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(app.dock.stacks[0]), ==, "files");
  g_assert_cmpint(panel_dock(&app, "shell"), ==, 4);
  g_assert_cmpint(panel_dock(&app, "outline"), ==, 8);
  g_assert_cmpint(panel_dock(&app, "hierarchy"), ==, 8);
  g_assert_cmpint(panel_dock(&app, "testing"), ==, 8);
  g_assert_true(gtk_widget_get_visible(app.layout.right));
}

/* A font that is not installed falls back to monospace, not to a proportional one. */
static void test_font_fallback(void)
{
  const PangoFontDescription *font = vte_terminal_get_font(app.shell.term);

  g_assert_cmpstr(pango_font_description_get_family(font), ==, "Cascadia Code,monospace");
}

/* A commit message typed but not committed is kept with the project: closing t-code (or opening
 * another folder) saves it, and the project brings it back; once empty, it is gone from the session. */
static void test_commit_message_kept(void)
{
  char *message;

  scm_set_message("Fix the parser\n\nIt dropped the last token.");
  session_save(&app);
  scm_set_message("");
  session_load_project(&app);
  message = scm_message();
  g_assert_cmpstr(message, ==, "Fix the parser\n\nIt dropped the last token.");
  g_free(message);

  scm_reset(); /* another project: the draft stays until that project's own replaces it */
  message = scm_message();
  g_assert_cmpstr(message, ==, "Fix the parser\n\nIt dropped the last token.");
  g_free(message);

  scm_set_message("  ");
  session_save(&app);
  scm_set_message("left over");
  session_load_project(&app);
  message = scm_message();
  g_assert_cmpstr(message, ==, "");
  g_free(message);
}
/* The settings session.ini kept before 0.3 moved into settings.ini, each with its comment. */
static void test_settings_moved(void)
{
  char *path = settings_file(), *text = NULL;
  char *session = g_build_filename(g_get_user_config_dir(), "t-code", "session.ini", NULL), *old = NULL;

  g_assert_false(app.settings.minimap); /* main() wrote it to session.ini first */
  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  g_assert_nonnull(strstr(text, "minimap=false"));
  g_assert_nonnull(strstr(text, "# A small picture of the file next to the scrollbar\nminimap=false"));
  g_assert_nonnull(strstr(text, "theme=rounded"));
  g_assert_true(g_file_get_contents(session, &old, NULL, NULL));
  g_assert_null(strstr(old, "[settings]"));
  g_free(old);
  g_free(session);
  g_free(text);
  g_free(path);
}

/* settings.ini edited by another program applies at once; t-code's own saves change nothing. */
static void test_settings_edited(void)
{
  char *path = settings_file(), *text = NULL;

  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  char **parts = g_strsplit(text, "theme=rounded", -1);
  char *square = g_strjoinv("theme=square", parts);
  g_assert_true(g_file_set_contents(path, square, -1, NULL));
  for (int i = 0; i < 100 && !app.settings.square; i++)
    run_for(20);
  g_assert_true(app.settings.square);
  g_assert_true(app.settings.minimap == FALSE); /* the rest stays */

  app.settings.square = FALSE;
  session_save_settings(&app);
  run_for(400);
  g_assert_false(app.settings.square);
  g_strfreev(parts);
  g_free(square);
  g_free(text);
  g_free(path);
}

/* The files to edit by hand open in tabs: theme.css made with its examples, a copy of the colors. */
static void test_open_user_files(void)
{
  char *theme = g_build_filename(g_get_user_config_dir(), "t-code", "theme.css", NULL), *text = NULL;
  char *colors = g_build_filename(g_get_user_config_dir(), "t-code", "styles", "dark-classic.xml", NULL);

  open_settings_file(&app);
  open_user_theme(&app);
  open_editor_colors(&app);
  run_for(100);
  assert_tabs("settings.ini theme.css dark-classic.xml");
  g_assert_true(g_file_get_contents(theme, &text, NULL, NULL));
  g_assert_nonnull(strstr(text, "Saving this file applies it at once"));
  g_free(text);
  g_assert_true(g_file_get_contents(colors, &text, NULL, NULL));
  g_assert_nonnull(strstr(text, "Your copy of t-code's editor colors"));
  g_assert_nonnull(strstr(text, "id=\"t-code-dark-classic\""));
  g_free(text);
  close_everything();
  g_free(colors);
  g_free(theme);
}

/* A comparison opens at its first change: the working file's cursor on that line, not on line 1. */
static void test_comparison_first_change(void)
{
  char *path = g_build_filename(app.project.root, "long.txt", NULL);
  GString *text = g_string_new(NULL);
  GtkTextIter cursor;
  Tab *tab;

  for (int i = 1; i <= 80; i++)
    g_string_append_printf(text, "line %d\n", i);
  g_assert_true(g_file_set_contents(path, text->str, -1, NULL));
  if (!git((const char *[]){ "init", "-q", NULL }) || !git((const char *[]){ "add", "long.txt", NULL }) ||
      !git((const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "long", NULL })) {
    g_test_skip("no git");
    g_string_free(text, TRUE);
    g_free(path);
    return;
  }
  app.project.repo_top = g_canonicalize_filename(app.project.root, NULL);
  g_set_str(&app.project.scm_top, app.project.repo_top);
  char *changed = g_strstr_len(text->str, -1, "line 60\n");
  memcpy(changed, "LINE", 4); /* line 60 differs */
  g_assert_true(g_file_set_contents(path, text->str, -1, NULL));

  compare_with_head(&app, path);
  run_until(compared, path);
  run_for(500); /* both sides load, their differences are worked out */
  tab = tab_nth(&app, 0);
  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &cursor,
                                   gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer)));
  g_assert_cmpint(gtk_text_iter_get_line(&cursor), ==, 59);
  quit_editor(tab);
  run_for(50);
  g_string_free(text, TRUE);
  g_free(path);
}

/* The Panels menu's action of page. */
static GAction *panel_action(const char *page)
{
  char *name = g_strconcat("panel-", page, NULL);
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(app.window), name);

  g_free(name);
  return action;
}

static void click_panel(const char *page)
{
  g_action_activate(panel_action(page), NULL);
  run_for(50);
}

static gboolean button_on(const char *page)
{
  GVariant *state = g_action_get_state(panel_action(page));
  gboolean on = g_variant_get_boolean(state);

  g_variant_unref(state);
  return on;
}

/* The interface uses t-code's own icons (assets/icons/ui, built in): the panels' tabs, the status
 * bar and the window controls GTK makes, whose theme icons titlebar.c swaps. */
static gboolean controls_owned(gconstpointer data G_GNUC_UNUSED)
{
  GtkWidget *close = find_widget(GTK_WIDGET(app.window), GTK_TYPE_BUTTON, NULL, "close");
  GtkWidget *image = close ? find_widget(close, GTK_TYPE_IMAGE, NULL, NULL) : NULL;

  return image && g_strcmp0(gtk_image_get_icon_name(GTK_IMAGE(image)), "tcode-window-close-symbolic") == 0;
}

static void test_own_icons(void)
{
  GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
  GtkWidget *problems = find_widget(GTK_WIDGET(app.window), GTK_TYPE_BOX, NULL, "problems");

  for (int i = 0; i < PANELS; i++)
    g_assert_true(gtk_icon_theme_has_icon(theme, panel_icon(panel_names[i])));
  g_assert_nonnull(problems);
  g_assert_cmpstr(gtk_image_get_icon_name(GTK_IMAGE(find_widget(problems, GTK_TYPE_IMAGE, NULL, NULL))), ==,
                  "tcode-error-symbolic");
  g_assert_true(gtk_icon_theme_has_icon(theme, "tcode-error-symbolic"));
  run_until(controls_owned, NULL);
  g_assert_true(controls_owned(NULL));
}

/* The first terminal tab's folder (shell_folders lists the others) is where dir is. */
static gboolean second_shell_in(gconstpointer dir)
{
  char **names = NULL, **folders = shell_folders(&app, &names);
  gboolean in = folders[0] && strcmp(folders[0], dir) == 0;

  g_strfreev(names);
  g_strfreev(folders);
  return in;
}

static gboolean shells_are(gconstpointer n)
{
  return shell_count(&app) == GPOINTER_TO_INT(n);
}

/* Terminal tabs: + opens a shell in a folder, named after the lowest free number; a tab renames
 * from its entry; × and exit close it but the first one stays; Build goes to the first, the
 * session keeps the others' folders and names, and a project switch closes them. */
static void test_terminal_tabs(void)
{
  char *made = g_dir_make_tmp("t-code-terminal-XXXXXX", NULL), *dir = realpath(made, NULL);
  char **names = NULL, **folders;
  VteTerminal *second, *third;
  GtkWidget *entry;

  g_assert_cmpint(shell_count(&app), ==, 1);
  g_assert_true(current_shell(&app) == app.shell.term);
  second = open_shell(&app, dir);
  g_assert_cmpint(shell_count(&app), ==, 2);
  g_assert_true(current_shell(&app) == second);
  g_assert_cmpstr(shell_name(second), ==, "Terminal 2");
  run_until(second_shell_in, dir);
  g_assert_true(second_shell_in(dir));

  rename_current_shell(&app);
  entry = find_widget(app.shell.panel, GTK_TYPE_ENTRY, NULL, NULL);
  g_assert_nonnull(entry);
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "Terminal 2");
  gtk_editable_set_text(GTK_EDITABLE(entry), "  Server ");
  g_signal_emit_by_name(entry, "activate");
  g_assert_cmpstr(shell_name(second), ==, "Server");
  shell_rename(second, "   "); /* blank: kept */
  g_assert_cmpstr(shell_name(second), ==, "Server");

  run_command(&app, "Terminal: New Terminal");
  third = current_shell(&app);
  g_assert_cmpint(shell_count(&app), ==, 3);
  g_assert_cmpstr(shell_name(third), ==, "Terminal 2"); /* free again since the rename */
  close_current_shell(&app);
  g_assert_cmpint(shell_count(&app), ==, 2);

  /* Build types into the first tab and shows it; the first one does not close. */
  run_in_shell(&app, " true");
  g_assert_true(current_shell(&app) == app.shell.term);
  close_current_shell(&app);
  g_assert_cmpint(shell_count(&app), ==, 2);

  folders = shell_folders(&app, &names);
  g_assert_cmpstr(folders[0], ==, dir);
  g_assert_cmpstr(names[0], ==, "Server");
  g_assert_null(folders[1]);

  /* exit in its shell closes the tab. */
  vte_terminal_feed_child(second, " exit\r", -1);
  run_until(shells_are, GINT_TO_POINTER(1));
  g_assert_cmpint(shell_count(&app), ==, 1);

  /* What the session kept comes back behind the current tab; a project switch closes it. */
  shell_restore(&app, folders, names);
  g_assert_cmpint(shell_count(&app), ==, 2);
  g_assert_true(current_shell(&app) == app.shell.term);
  g_assert_cmpstr(shell_name(VTE_TERMINAL(gtk_notebook_get_nth_page(app.shell.tabs, 1))), ==, "Server");
  run_until(second_shell_in, dir);
  restart_shell_pane(&app);
  g_assert_cmpint(shell_count(&app), ==, 1);

  g_strfreev(names);
  g_strfreev(folders);
  g_rmdir(made);
  free(dir);
  g_free(made);
}

/* Each panel has an item in the Panels menu, checked while the panel shows: choosing it hides a
 * shown panel and brings a hidden one back from wherever it is, also from a side squeezed to
 * nothing (a size saved on a bigger screen), which Ctrl+J then shows too instead of hiding it.
 * "View: Reset Panel Layout" puts everything back. */
static void test_panel_buttons(void)
{
  int h = gtk_widget_get_height(GTK_WIDGET(app.layout.vpaned));

  reset_panel_layout(&app);
  run_for(50);
  for (int i = 0; i < PANELS; i++)
    g_assert_nonnull(panel_action(panel_names[i]));
  g_assert_true(button_on("files"));
  g_assert_false(button_on("scm")); /* a tab behind Files */
  g_assert_true(button_on("shell"));

  click_panel("scm");
  g_assert_true(panel_shown(&app, "scm"));
  g_assert_true(button_on("scm"));
  g_assert_false(button_on("files"));
  click_panel("scm"); /* shown: hidden */
  g_assert_false(panel_shown(&app, "scm"));
  g_assert_false(gtk_widget_get_visible(app.layout.left));
  click_panel("files");
  g_assert_true(panel_shown(&app, "files"));
  g_assert_true(gtk_widget_get_visible(app.layout.left));

  g_assert_cmpint(h, >, 200);
  gtk_paned_set_position(app.layout.vpaned, h - 10); /* the bottom squeezed to nothing */
  run_for(50);
  g_assert_false(panel_shown(&app, "shell"));
  g_assert_false(button_on("shell"));
  click_panel("shell");
  g_assert_true(panel_shown(&app, "shell"));
  g_assert_cmpint(gtk_paned_get_position(app.layout.vpaned), <=, h - 48);
  gtk_paned_set_position(app.layout.vpaned, h - 10);
  toggle_pane(&app, app.layout.bottom); /* Ctrl+J's side toggle: shown, not hidden */
  g_assert_true(gtk_widget_get_visible(app.layout.bottom));
  g_assert_true(panel_shown(&app, "shell"));

  move_panel(&app, "files", 8); /* moved right, its dock hidden, the bottom collapsed */
  click_panel("files");
  g_assert_false(panel_shown(&app, "files"));
  click_panel("shell");
  reset_panel_layout(&app);
  run_for(50);
  for (int i = 0; i < PANELS; i++)
    g_assert_cmpint(panel_dock(&app, panel_names[i]), ==, panel_default_docks[i]);
  g_assert_true(panel_shown(&app, "files"));
  g_assert_true(panel_shown(&app, "shell"));
  g_assert_true(gtk_widget_get_visible(app.layout.right)); /* Outline, Hierarchy, Testing */
  g_assert_true(button_on("files"));
}

/* The label under widget that shows text, or NULL. */
static GtkWidget *label_under(GtkWidget *widget, const char *text)
{
  if (GTK_IS_LABEL(widget) && g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text) == 0)
    return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = label_under(child, text);
    if (found)
      return found;
  }
  return NULL;
}

/* Shows an edge strip being dragged inwards by (dx, dy). */
static void drag_edge(int side, double dx, double dy)
{
  GListModel *controllers = gtk_widget_observe_controllers(app.layout.edges[side]);
  GObject *drag = NULL;

  for (guint i = 0; i < g_list_model_get_n_items(controllers) && !drag; i++) {
    GObject *c = g_list_model_get_item(controllers, i);
    if (GTK_IS_GESTURE_DRAG(c))
      drag = c;
    else
      g_object_unref(c);
  }
  g_object_unref(controllers);
  g_assert_nonnull(drag);
  g_signal_emit_by_name(drag, "drag-update", dx, dy);
  g_object_unref(drag);
}

static gboolean more_shown(gconstpointer more)
{
  return gtk_widget_get_visible(GTK_WIDGET(more));
}

static gboolean more_hidden(gconstpointer more)
{
  return !gtk_widget_get_visible(GTK_WIDGET(more));
}

/* Resizing the sides: the panels on the sides fit in a side's smallest width (shorter buttons, no
 * cut-off rows); dragged smaller than that, a side squeezes to nothing and hides on release (to open
 * at its last size), dragged back out first it stays; a hidden side's edge strip opens it when
 * dragged inwards. A dock's icons that do not fit its tab bar go into its "more" menu. */
static void test_panel_resize(void)
{
  int w = gtk_widget_get_width(GTK_WIDGET(app.layout.rpaned));
  int max = 0;
  GtkWidget *more, *switcher;
  gboolean hidden_icon = FALSE;

  reset_panel_layout(&app);
  run_for(100);
  for (int i = 0; i < PANELS; i++) {
    int d = panel_dock(&app, panel_names[i]), min = 0;
    GtkWidget *child = gtk_stack_get_child_by_name(app.dock.stacks[d], panel_names[i]);

    if (d / 4 == 1) /* the bottom's are as wide as the window */
      continue;
    if (GTK_IS_SCROLLED_WINDOW(child))
      child = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(child));
    if (GTK_IS_VIEWPORT(child))
      child = gtk_viewport_get_child(GTK_VIEWPORT(child));
    gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, &min, NULL, NULL, NULL);
    if (min > 170) /* the smallest side */
      g_test_message("%s is at least %d wide", panel_names[i], min);
    g_assert_cmpint(min, <=, 170);
  }

  /* The left side: dragged below its smallest width, out again, below again, released. */
  app.layout.pressed = TRUE;
  gtk_paned_set_position(app.layout.hpaned, 240);
  gtk_paned_set_position(app.layout.hpaned, 120);
  g_assert_cmpint(gtk_paned_get_position(app.layout.hpaned), ==, 0);
  g_assert_true(gtk_widget_get_visible(app.layout.left));
  gtk_paned_set_position(app.layout.hpaned, 200);
  g_assert_cmpint(gtk_paned_get_position(app.layout.hpaned), ==, 200);
  gtk_paned_set_position(app.layout.hpaned, 60);
  g_assert_cmpint(gtk_paned_get_position(app.layout.hpaned), ==, 0);
  end_pane_drag(&app);
  run_for(50);
  g_assert_false(gtk_widget_get_visible(app.layout.left));
  g_assert_cmpint(gtk_paned_get_position(app.layout.hpaned), ==, 200);
  g_assert_true(gtk_widget_get_visible(app.layout.edges[0]));

  /* Its edge strip: a twitch does nothing, a drag opens it as wide as the pointer came. */
  app.layout.pressed = TRUE;
  drag_edge(0, 2, 0);
  g_assert_false(gtk_widget_get_visible(app.layout.left));
  drag_edge(0, 90, 0);
  g_assert_true(gtk_widget_get_visible(app.layout.left));
  g_assert_cmpint(gtk_paned_get_position(app.layout.hpaned), ==, 0); /* too narrow yet */
  drag_edge(0, 260, 0);
  g_assert_cmpint(gtk_paned_get_position(app.layout.hpaned), ==, 260);
  end_pane_drag(&app);
  run_for(50);
  g_assert_true(gtk_widget_get_visible(app.layout.left));
  g_assert_false(gtk_widget_get_visible(app.layout.edges[0]));

  /* The right side and the bottom the same way. */
  g_object_get(app.layout.rpaned, "max-position", &max, NULL);
  g_assert_cmpint(max, >, 600);
  app.layout.pressed = TRUE;
  gtk_paned_set_position(app.layout.rpaned, max - 100);
  g_assert_cmpint(gtk_paned_get_position(app.layout.rpaned), ==, max);
  end_pane_drag(&app);
  run_for(50);
  g_assert_false(gtk_widget_get_visible(app.layout.right));
  g_assert_true(gtk_widget_get_visible(app.layout.edges[2]));
  app.layout.pressed = TRUE;
  drag_edge(2, -300, 0);
  end_pane_drag(&app);
  run_for(50);
  g_assert_true(gtk_widget_get_visible(app.layout.right));
  run_for(1000);
  g_assert_cmpint(gtk_widget_get_width(app.layout.right), >=, 290);

  g_object_get(app.layout.vpaned, "max-position", &max, NULL);
  app.layout.pressed = TRUE;
  gtk_paned_set_position(app.layout.vpaned, max - 60);
  end_pane_drag(&app);
  run_for(50);
  g_assert_false(gtk_widget_get_visible(app.layout.bottom));
  g_assert_true(gtk_widget_get_visible(app.layout.edges[1]));
  app.layout.pressed = TRUE;
  drag_edge(1, 0, -200);
  end_pane_drag(&app);
  run_for(50);
  g_assert_true(gtk_widget_get_visible(app.layout.bottom));
  g_assert_false(gtk_widget_get_visible(app.layout.edges[1]));

  /* Seven icons in a narrow dock: some go into "more", which shows them. */
  for (int i = 0; i < PANELS; i++)
    if (panel_default_docks[i] == 8)
      move_panel(&app, panel_names[i], 0);
  gtk_paned_set_position(app.layout.hpaned, 180);
  more = find_widget(app.dock.cards[0], GTK_TYPE_MENU_BUTTON, "More Panels", NULL);
  g_assert_nonnull(more);
  run_until(more_shown, more);
  g_assert_true(gtk_widget_get_visible(more));
  switcher = g_object_get_data(G_OBJECT(app.dock.stacks[0]), "tcode-switcher");
  for (GtkWidget *icon = gtk_widget_get_first_child(switcher); icon; icon = gtk_widget_get_next_sibling(icon))
    hidden_icon |= gtk_widget_get_opacity(icon) == 0;
  g_assert_true(hidden_icon);
  gtk_menu_button_popup(GTK_MENU_BUTTON(more));
  run_for(50);
  g_assert_nonnull(label_under(GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(more))), "Testing"));
  gtk_menu_button_popdown(GTK_MENU_BUTTON(more));
  gtk_paned_set_position(app.layout.hpaned, MIN(w - 300, 700));
  run_until(more_hidden, more);
  g_assert_false(gtk_widget_get_visible(more));

  reset_panel_layout(&app);
  run_for(50);
}

/* The toplevel window showing a label with text (a dialog), or NULL. */
static GtkWindow *window_saying(gconstpointer text)
{
  GtkWindow *found = NULL;

  for (guint i = 0; i < g_list_model_get_n_items(gtk_window_get_toplevels()) && !found; i++) {
    GtkWindow *window = g_list_model_get_item(gtk_window_get_toplevels(), i);
    if (label_under(GTK_WIDGET(window), text))
      found = window;
    g_object_unref(window);
  }
  return found;
}

static gboolean shows(gconstpointer text)
{
  return window_saying(text) != NULL;
}

static gboolean build_label_is_target(gconstpointer data G_GNUC_UNUSED)
{
  char *target = project_summary(&app);
  gboolean same = g_strcmp0(gtk_label_get_text(app.status.build), target) == 0;

  g_free(target);
  return same;
}

/* A file whose language server is not on PATH shows "No LSP"; its click opens the window with the
 * commands that install it. Without a file there is no notice. */
static void test_no_lsp(void)
{
  char *saved_path = g_strdup(g_getenv("PATH")), *path = file("notes.yaml");
  GtkWindow *install = NULL;

  g_setenv("PATH", "/nonexistent", TRUE);
  open_file(&app, path);
  run_for(20);
  update_status(&app);
  g_assert_true(gtk_widget_get_visible(app.status.no_lsp));
  g_assert_nonnull(strstr(gtk_widget_get_tooltip_text(app.status.no_lsp), "yaml-language-server"));
  g_signal_emit_by_name(app.status.no_lsp, "clicked");
  GListModel *toplevels = gtk_window_get_toplevels();
  for (guint i = 0; i < g_list_model_get_n_items(toplevels) && !install; i++) {
    GtkWindow *each = g_list_model_get_item(toplevels, i);
    if (g_strcmp0(gtk_window_get_title(each), "Install yaml-language-server") == 0)
      install = each;
    g_object_unref(each);
  }
  g_assert_nonnull(install);
  gtk_window_close(install);

  g_setenv("PATH", saved_path ? saved_path : "", TRUE);
  close_all_tabs(&app); /* no file, no notice */
  update_status(&app);
  g_assert_false(gtk_widget_get_visible(app.status.no_lsp));
  g_free(path);
  g_free(saved_path);
}

static Sym *outline_sym(const char *name, int kind, int line, int end_line)
{
  Sym *sym = g_new0(Sym, 1);

  sym->name = g_strdup(name);
  sym->kind = kind;
  sym->line = sym->name_line = line;
  sym->end_line = end_line;
  sym->end_column = 50;
  sym->name_column = 2;
  sym->children = g_ptr_array_new();
  return sym;
}

static void outline_sym_free(Sym *sym)
{
  g_ptr_array_set_free_func(sym->children, (GDestroyNotify)outline_sym_free);
  g_ptr_array_unref(sym->children);
  g_free(sym->name);
  g_free(sym);
}

/* The Outline panel lists the current editor's symbols indented, selects the one around the cursor
 * and goes to one when clicked; with no editor it says so. */
static void test_outline(void)
{
  char *path = file("outline.c"), *text;
  Tab *tab;
  GPtrArray *roots = g_ptr_array_new_with_free_func((GDestroyNotify)outline_sym_free);
  Sym *shape = outline_sym("Shape", 23, 0, 3);
  GtkTextIter iter;

  g_assert_true(g_file_set_contents(path, "struct Shape {\n  int w;\n  int h;\n};\nint main(void) {}\n", -1, NULL));
  open_file(&app, path);
  run_for(50);
  tab = current_tab(&app);
  g_assert_nonnull(tab);
  g_ptr_array_add(shape->children, outline_sym("w", 8, 1, 1));
  g_ptr_array_add(shape->children, outline_sym("h", 8, 2, 2));
  g_ptr_array_add(roots, shape);
  g_ptr_array_add(roots, outline_sym("main", 12, 4, 4));
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &iter, 2, 4);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &iter);

  /* outline_fill is what the symbols' answer does; symbols_at reads the editor's own, so the
   * selection follows them: none here. */
  outline_fill(&app, tab, roots);
  text = outline_text(&app);
  g_assert_cmpstr(text, ==, "S Shape\n  · w\n  · h\nƒ main\n");
  g_free(text);
  GtkListBox *list = GTK_LIST_BOX(find_widget(find_widget(GTK_WIDGET(app.window), G_TYPE_NONE, NULL, "tcode-outline"),
                                              GTK_TYPE_LIST_BOX, NULL, NULL));
  g_signal_emit_by_name(list, "row-activated", gtk_list_box_get_row_at_index(list, 3)); /* main */
  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &iter,
                                   gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer)));
  g_assert_cmpint(gtk_text_iter_get_line(&iter), ==, 4);
  g_assert_cmpint(gtk_text_iter_get_line_offset(&iter), ==, 2);
  g_ptr_array_unref(roots);
  outline_fill(&app, NULL, NULL);
  text = outline_text(&app);
  g_assert_cmpstr(text, ==, "(Open a file to see its symbols.)\n");
  g_free(text);

  /* Switching tabs shows the new tab's outline, not the one it leaves (switch-page comes before the
   * notebook's current page changes). */
  char *md = file("outline.md");
  GtkNotebook *group = app.editors.groups[app.editors.active];
  g_assert_true(g_file_set_contents(md, "# Notes\n", -1, NULL));
  open_preview(&app, md);
  run_for(50);
  g_assert_cmpint(gtk_notebook_get_n_pages(group), ==, 2);
  gtk_notebook_set_current_page(group, 0); /* outline.c */
  g_assert_true(current_tab(&app) == tab);
  text = outline_text(&app);
  g_assert_cmpstr(text, !=, "(Open a file to see its symbols.)\n");
  g_free(text);
  gtk_notebook_set_current_page(group, 1); /* the preview */
  g_assert_cmpint(current_tab(&app)->kind, ==, TAB_PREVIEW);
  text = outline_text(&app);
  g_assert_cmpstr(text, ==, "(Open a file to see its symbols.)\n");
  g_free(text);
  close_all_tabs(&app);
  g_free(md);
  g_free(path);
}

/* A plugin's "say" stays in the status bar for a few seconds: status updates (diagnostics, tab
 * changes, many a second while typing) used to replace it at once. Then the Build target is back. */
static void test_status_say(void)
{
  status_say(&app, "Word Count: 12 words");
  update_status(&app);
  g_assert_cmpstr(gtk_label_get_text(app.status.build), ==, "Word Count: 12 words");
  run_for(500);
  update_status(&app);
  g_assert_cmpstr(gtk_label_get_text(app.status.build), ==, "Word Count: 12 words");
  status_say(&app, "Plugins reloaded"); /* a newer one starts its own time */
  g_assert_cmpstr(gtk_label_get_text(app.status.build), ==, "Plugins reloaded");
  run_until(build_label_is_target, NULL);
  g_assert_true(build_label_is_target(NULL));
  g_assert_cmpuint(app.status.say_id, ==, 0);
}

/* "Check for Updates Now" answers on screen: the latest release, a newer one's window, or no answer
 * (it used to print to stderr only). curl is a script that answers like GitHub's API. */
static void test_update_check_now(void)
{
  char *bin = g_build_filename(app.project.root, "update-bin", NULL);
  char *saved_path = g_strdup(g_getenv("PATH")), *path_env = g_strdup_printf("%s:%s", bin, saved_path);
  char *same = g_strdup_printf("#!/bin/sh\necho '{\"tag_name\":\"v%s\",\"html_url\":\"https://example.invalid\"}'\n",
                               TCODE_VERSION);
  char *latest = g_strdup_printf("You have the latest release, t-code %s.", TCODE_VERSION);
  const char *offline = "Could not check for updates: no answer from GitHub (offline?).";

  g_setenv("PATH", path_env, TRUE);
  g_free(put(bin, "curl", same, 0755));
  updates_check_now(&app);
  run_until(shows, latest);
  g_assert_true(shows(latest));
  gtk_window_destroy(window_saying(latest));

  g_free(put(bin, "curl", "#!/bin/sh\nexit 6\n", 0755)); /* could not resolve host */
  updates_check_now(&app);
  run_until(shows, offline);
  g_assert_true(shows(offline));
  gtk_window_destroy(window_saying(offline));

  g_free(
      put(bin, "curl", "#!/bin/sh\necho '{\"tag_name\":\"v99.0\",\"html_url\":\"https://example.invalid\"}'\n", 0755));
  updates_check_now(&app);
  run_until(shows, "t-code 99.0 is out");
  g_assert_true(shows("t-code 99.0 is out"));
  gtk_window_destroy(window_saying("t-code 99.0 is out"));
  run_for(50);

  g_setenv("PATH", saved_path, TRUE);
  g_free(latest);
  g_free(same);
  g_free(path_env);
  g_free(saved_path);
  g_free(bin);
}

/* The open Settings window, or NULL. */
static GtkWindow *settings_window(void)
{
  GtkWindow *settings = NULL;

  for (guint i = 0; i < g_list_model_get_n_items(gtk_window_get_toplevels()); i++) {
    GtkWindow *window = g_list_model_get_item(gtk_window_get_toplevels(), i);
    if (g_strcmp0(gtk_window_get_title(window), "Settings") == 0)
      settings = window;
    g_object_unref(window);
  }
  return settings;
}

/* The widgets of type under widget. */
static int count_of(GtkWidget *widget, GType type)
{
  int n = G_TYPE_CHECK_INSTANCE_TYPE(widget, type);

  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    n += count_of(child, type);
  return n;
}

static GtkStack *settings_stack(GtkWindow *settings)
{
  for (GtkWidget *child = gtk_widget_get_first_child(gtk_window_get_child(settings)); child;
       child = gtk_widget_get_next_sibling(child))
    if (GTK_IS_STACK(child))
      return GTK_STACK(child);
  return NULL;
}

/* The Settings window lists its pages on the left, each scrolling when the window is smaller; every
 * setting is still there, and it opens again on the page it closed on. */
static void test_settings_pages(void)
{
  static const char *const pages[] = {
    "general", "editor", "indentation", "appearance", "c", "servers", "files", NULL
  };
  GtkWindow *settings;
  GtkStack *stack;
  int switches = 0;

  show_settings(&app);
  run_for(50);
  settings = settings_window();
  g_assert_nonnull(settings);
  g_assert_true(gtk_window_get_resizable(settings));
  stack = settings_stack(settings);
  g_assert_nonnull(stack);
  g_assert_cmpint(count_of(gtk_window_get_child(settings), GTK_TYPE_STACK_SIDEBAR), ==, 1);
  for (int i = 0; pages[i]; i++) {
    GtkWidget *page = gtk_stack_get_child_by_name(stack, pages[i]);
    g_assert_nonnull(page);
    g_assert_true(GTK_IS_SCROLLED_WINDOW(page));
    switches += count_of(page, GTK_TYPE_SWITCH);
  }
  g_assert_cmpint(switches, ==, 15 + SERVER_NONE); /* none lost on the way; one per built-in server */
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "general");

  gtk_stack_set_visible_child_name(stack, "appearance");
  gtk_window_destroy(settings);
  run_for(50);
  show_settings(&app);
  run_for(50);
  settings = settings_window();
  g_assert_cmpstr(gtk_stack_get_visible_child_name(settings_stack(settings)), ==, "appearance");
  GtkSelectionModel *rows = gtk_stack_get_pages(settings_stack(settings)); /* what the list on the left shows */
  g_assert_true(gtk_selection_model_is_selected(rows, 3));
  g_assert_false(gtk_selection_model_is_selected(rows, 0));
  g_object_unref(rows);
  gtk_stack_set_visible_child_name(settings_stack(settings), "general"); /* as it was, for the other tests */
  gtk_window_destroy(settings);
  run_for(50);
}

static GtkWindow *window_titled(const char *title)
{
  GtkWindow *found = NULL;

  for (guint i = 0; i < g_list_model_get_n_items(gtk_window_get_toplevels()); i++) {
    GtkWindow *window = g_list_model_get_item(gtk_window_get_toplevels(), i);
    if (g_strcmp0(gtk_window_get_title(window), title) == 0)
      found = window;
    g_object_unref(window);
  }
  return found;
}

/* Whether a label under root says text. */
static gboolean says(GtkWidget *root, const char *text)
{
  if (GTK_IS_LABEL(root) && g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), text) == 0)
    return TRUE;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child))
    if (says(child, text))
      return TRUE;
  return FALSE;
}

/* The target of the menu item labeled label in model or its submenus and sections, or NULL. */
static char *menu_target(GMenuModel *model, const char *label)
{
  char *target = NULL;

  for (int i = 0; model && i < g_menu_model_get_n_items(model) && !target; i++) {
    char *text = NULL;
    if (g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &text) && !g_strcmp0(text, label))
      g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_TARGET, "s", &target);
    g_free(text);
    static const char *const links[] = { G_MENU_LINK_SUBMENU, G_MENU_LINK_SECTION };
    for (guint l = 0; !target && l < G_N_ELEMENTS(links); l++) {
      GMenuModel *inner = g_menu_model_get_item_link(model, i, links[l]);
      target = menu_target(inner, label);
      g_clear_object(&inner);
    }
  }
  return target;
}

/* "Plugins: Manage Plugins" (also Settings → Plugins… in the menu bar): the plugins on the left
 * (Word Count among them, installed in the user's folder), the first one's details on the right; its switch turns it off (its command leaves the palette) and on; a
 * reload keeps the window going; a second call brings the same window back. */
static void test_plugins_window(void)
{
  Plugin *p = plugin_find(&app, "word-count");
  GtkWindow *plugins;
  GtkWidget *list, *toggle;

  g_assert_nonnull(p);
  GtkWidget *bar = find_widget(GTK_WIDGET(app.window), GTK_TYPE_POPOVER_MENU_BAR, NULL, NULL);
  char *target = menu_target(gtk_popover_menu_bar_get_menu_model(GTK_POPOVER_MENU_BAR(bar)), "Plugins…");
  g_assert_cmpstr(target, ==, "Plugins: Manage Plugins"); /* also in the Settings menu */
  g_free(target);
  run_command(&app, "Plugins: Manage Plugins");
  run_for(50);
  plugins = window_titled("Plugins");
  g_assert_nonnull(plugins);
  list = find_widget(GTK_WIDGET(plugins), GTK_TYPE_LIST_BOX, NULL, NULL);
  g_assert_cmpint(count_of(list, GTK_TYPE_LIST_BOX_ROW), ==, (int)app.plugins->len);
  for (guint i = 0; i < app.plugins->len; i++)
    if (g_ptr_array_index(app.plugins, i) == p)
      gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), (int)i));
  run_for(20);
  g_assert_true(says(GTK_WIDGET(plugins), "Word Count"));
  g_assert_true(says(GTK_WIDGET(plugins), "tapi_1"));
  g_assert_true(says(GTK_WIDGET(plugins), "your plugins folder"));
  g_assert_true(says(GTK_WIDGET(plugins), "Word Count: Count Words in the Selection or Tab    Ctrl+Alt+W"));

  toggle = find_widget(gtk_window_get_child(plugins), GTK_TYPE_SWITCH, NULL, NULL);
  g_assert_true(gtk_switch_get_active(GTK_SWITCH(toggle)));
  gtk_switch_set_active(GTK_SWITCH(toggle), FALSE);
  g_assert_false(p->enabled);
  g_assert_true(says(GTK_WIDGET(plugins), "off"));
  gtk_switch_set_active(GTK_SWITCH(toggle), TRUE);
  g_assert_true(p->enabled);

  run_command(&app, "Plugins: Reload Installed Plugins"); /* the old Plugin pointers go */
  run_for(700);                                           /* the window looks again */
  g_assert_true(says(GTK_WIDGET(plugins), "Word Count"));
  run_command(&app, "Plugins: Manage Plugins");
  run_for(20);
  g_assert_true(window_titled("Plugins") == plugins);
  gtk_window_destroy(plugins);
  run_for(50);
  g_assert_null(window_titled("Plugins"));
}

/* The first-run question about the plugin called name, or NULL. */
static GtkWindow *question_of(const char *name)
{
  char *title = g_strdup_printf("Last heads-up before %s runs", name);
  GtkWindow *found = NULL;

  for (guint i = 0; i < g_list_model_get_n_items(gtk_window_get_toplevels()); i++) {
    GtkWindow *window = g_list_model_get_item(gtk_window_get_toplevels(), i);
    if (gtk_widget_get_visible(GTK_WIDGET(window)) && says(GTK_WIDGET(window), title))
      found = window;
    g_object_unref(window);
  }
  g_free(title);
  return found;
}

/* The first run's question goes over the main window when the Plugins window, still open, is behind it
 * (over that one it went unseen and the command seemed dead); asked again meanwhile, the status bar
 * says the question is open instead of nothing. */
static void test_plugins_run_question(void)
{
  Plugin quiet = { .id = "quiet", .name = "Quiet", .provider = "" };
  GtkWindow *plugins, *question = NULL;
  GtkWidget *not_now;

  run_command(&app, "Plugins: Manage Plugins");
  run_for(50);
  plugins = window_titled("Plugins");
  g_assert_nonnull(plugins);
  gtk_window_present(app.window);
  run_for(50);
  plugins_confirm_run(&app, &quiet, "go");
  run_for(50);
  question = question_of("Quiet");
  g_assert_nonnull(question);
  g_assert_true(gtk_window_get_transient_for(question) == app.window);

  plugins_confirm_run(&app, &quiet, "go"); /* while it is open */
  g_assert_cmpstr(gtk_label_get_text(app.status.build), ==, "Quiet waits for your answer: its question is open");

  not_now = gtk_widget_get_ancestor(label_under(GTK_WIDGET(question), "Not Now"), GTK_TYPE_BUTTON);
  g_assert_nonnull(not_now);
  gtk_widget_activate(not_now);
  for (int i = 0; i < 40 && question_of("Quiet"); i++)
    run_for(50);
  g_assert_null(question_of("Quiet"));
  status_say(&app, "");
  plugins_confirm_run(&app, &quiet, "go"); /* answered: it asks again */
  run_for(50);
  question = question_of("Quiet");
  g_assert_nonnull(question);
  g_assert_cmpstr(gtk_label_get_text(app.status.build), ==, "");
  gtk_widget_activate(gtk_widget_get_ancestor(label_under(GTK_WIDGET(question), "Not Now"), GTK_TYPE_BUTTON));
  run_for(50);
  gtk_window_destroy(plugins);
  run_for(50);
}

/* Browse: the registry's index ($TCODE_REGISTRY, a file here) lists its plugins with their review
 * badges; Install on a reviewed one downloads its pinned archive (file:// here), checks it and puts it
 * in the user's plugins folder, switched on, with its record; the button then says Installed. */
static void test_plugins_browse(void)
{
  char *work = g_dir_make_tmp("t-code-registry-XXXXXX", NULL),
       *plugin = g_build_filename(work, "b-c0ffee", "tidy", NULL);
  char *repo = g_build_filename(work, "repo", NULL), *tree, *index, *json, *archive;
  const char *commit = "c0ffee0000000000000000000000000000000000";
  GtkWindow *plugins;
  GtkWidget *stack, *install;
  Plugin *p;

  g_free(write_at(plugin, "plugin.json", "{\"api\": \"tapi_1\", \"name\": \"Tidy\", \"id\": \"tidy\"}", -1, 0644));
  tree = registry_tree_hash(plugin, NULL);
  g_mkdir_with_parents(repo, 0755);
  archive = g_strdup_printf("%s/%s.tar.gz", repo, commit);
  const char *tar[] = { "tar", "-czf", archive, "-C", work, "b-c0ffee", NULL };
  g_assert_true(g_spawn_sync(NULL, (char **)tar, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL));
  json = g_strdup_printf(
      "{\"plugins\": [{\"id\": \"tidy\", \"name\": \"Tidy\", \"provider\": \"Tidy Team\", \"api\": \"tapi_1\", "
      "\"repository\": \"file://%s\", \"commit\": \"%s\", \"path\": \"tidy\", "
      "\"tree-sha256\": \"%s\", \"reviewed\": true, \"categories\": [\"text\"]},"
      " {\"id\": \"wild\", \"name\": \"Wild\", \"api\": \"tapi_1\", \"repository\": \"file://%s\","
      " \"commit\": \"%s\", \"tree-sha256\": \"%s\"},"
      " {\"id\": \"future\", \"name\": \"Future\", \"api\": \"tapi_9\", \"repository\": \"file://%s\","
      " \"commit\": \"%s\", \"tree-sha256\": \"%s\"}]}",
      repo, commit, tree, repo, commit, tree, repo, commit, tree);
  index = write_at(work, "index.json", json, -1, 0644);
  g_setenv("TCODE_REGISTRY", index, TRUE);

  run_command(&app, "Plugins: Manage Plugins");
  run_for(50);
  plugins = window_titled("Plugins");
  stack = find_widget(GTK_WIDGET(plugins), GTK_TYPE_STACK, NULL, NULL);
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "browse");
  run_for(100);
  g_assert_true(says(GTK_WIDGET(plugins), "Reviewed by a maintainer"));
  g_assert_true(says(GTK_WIDGET(plugins), "Not reviewed yet: checks passed, no human has read it"));
  g_assert_true(says(GTK_WIDGET(plugins), "Needs a newer t-code")); /* tapi_9 */
  g_assert_true(says(GTK_WIDGET(plugins), "by Tidy Team · text"));  /* who publishes it, beside the version */

  /* the first row's button: Tidy's */
  install = find_widget(gtk_stack_get_child_by_name(GTK_STACK(stack), "browse"), GTK_TYPE_BUTTON, NULL, NULL);
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(install)), ==, "Install");
  gtk_widget_activate(install);
  for (int i = 0; i < 100 && !plugin_find(&app, "tidy"); i++)
    run_for(50);
  p = plugin_find(&app, "tidy");
  g_assert_nonnull(p);
  g_assert_true(p->enabled); /* reviewed: on at once */
  g_assert_nonnull(p->record);
  g_assert_cmpstr(p->record->commit, ==, commit);
  g_assert_true(p->record->reviewed);
  run_for(100);
  g_assert_true(says(GTK_WIDGET(plugins), "Installed"));

  /* the registry changed meanwhile: Reload fetches its index again */
  g_free(write_at(work, "index.json", "{\"plugins\": []}", -1, 0644));
  GtkWidget *reload = find_widget(gtk_window_get_titlebar(plugins), GTK_TYPE_BUTTON, NULL, NULL);
  g_signal_emit_by_name(reload, "clicked"); /* Reload (activate would click it 250 ms later) */
  run_for(200);
  g_assert_true(says(GTK_WIDGET(plugins), "The registry is empty for now. Be the first to publish one!"));

  p = plugin_find(&app, "tidy"); /* Reload loaded the plugins again */
  g_assert_true(plugin_uninstall(&app, p, NULL));
  gtk_window_destroy(plugins);
  run_for(50);
  g_unsetenv("TCODE_REGISTRY");
  const char *rm[] = { "rm", "-rf", work, NULL };
  g_spawn_sync(NULL, (char **)rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(index);
  g_free(json);
  g_free(archive);
  g_free(tree);
  g_free(repo);
  g_free(plugin);
  g_free(work);
}

/* Whether tab's text is data (a string). */
static gboolean text_is(gconstpointer data)
{
  const char *const *pair = data;
  Tab *tab = find_tab(&app, TAB_EDITOR, pair[0]);
  GtkTextIter start, end;
  char *text;
  gboolean same;

  if (!tab)
    return FALSE;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
  same = strcmp(text, pair[1]) == 0;
  g_free(text);
  return same;
}

/* Whether the editor of path shows a diagnostic whose message holds data[1]. */
static gboolean shows_diagnostic(gconstpointer data)
{
  const char *const *pair = data;
  Tab *tab = find_tab(&app, TAB_EDITOR, pair[0]);
  GPtrArray *list = tab ? diagnostics_of(tab) : NULL;

  for (guint i = 0; list && i < list->len; i++)
    if (strstr(diagnostic_message(g_ptr_array_index(list, i)), pair[1]))
      return TRUE;
  return FALSE;
}

static void write_program(const char *dir, const char *name, const char *text)
{
  g_free(write_at(dir, name, text, -1, 0755));
}

/* plugins/builtin against this window, through the real socket: Text Tools edits the buffer (the
 * whole of it without a selection, the selection with one); ShellCheck, started with the project,
 * turns a fake shellcheck's findings on an opened script into diagnostics; MATLAB runs a .m file in
 * a fake MATLAB's terminal tab and turns its error into a diagnostic, then closes the session; CMake
 * configures with a fake cmake and shows its error on the opened CMakeLists.txt; Java runs a file
 * with a fake JDK and shows its exception. */
static void test_example_plugins(void)
{
  char *python = g_find_program_in_path("python3");
  char *bin = g_dir_make_tmp("t-code-fake-bin-XXXXXX", NULL), *old_path = g_strdup(g_getenv("PATH"));
  char *path_env = g_strconcat(bin, ":", old_path, NULL), *matlab = g_build_filename(bin, "matlab", NULL);
  char *text_file = file("tools.txt"), *script = file("check.sh"), *m_file = file("script1.m");
  char *lists = file("CMakeLists.txt");
  int shells = shell_count(&app);

  if (!python) {
    g_test_skip("no python3");
    g_free(bin);
    g_free(old_path);
    g_free(path_env);
    g_free(matlab);
    g_free(text_file);
    g_free(script);
    g_free(m_file);
    g_unlink(lists);
    g_free(lists);
    return;
  }
  write_program(bin, "cmake",
                "#!/bin/sh\n"
                "echo '-- The C compiler identification is GNU'\n"
                "printf 'CMake Error at CMakeLists.txt:2 (foo_bar):\\n  Unknown CMake command \"foo_bar\".\\n\\n' >&2\n"
                "exit 1\n");
  write_program(bin, "shellcheck",
                "#!/usr/bin/env python3\n"
                "import json\n"
                "print(json.dumps({'comments': [{'line': 2, 'column': 6, 'endLine': 2, 'endColumn': 8, 'level': "
                "'info', 'code': 2086, 'message': 'Double quote to prevent globbing and word splitting.'}]}))\n");
  write_program(
      bin, "matlab",
      "#!/usr/bin/env python3\n"
      "import re, sys\n"
      "def handle(line, message):\n"
      "    m = re.search(r\"diary\\('([^']*)'\\); disp\\('([^']*)'\\).*run\\('([^']*)'\\).*disp\\('([^']*)'\\)\", "
      "line)\n"
      "    if m:\n"
      "        stem = m.group(3)[:-2]\n"
      "        with open(m.group(1), 'w') as f:\n"
      "            f.write(f\"{m.group(2)}\\n{message}\\n\\n\"\n"
      "                    f\"Error in {stem} (line 2)\\ny = x + 1;\\n{m.group(4)}\\n\")\n"
      "if sys.argv[1:2] == ['-batch']:\n"
      "    handle(sys.argv[2], 'Batch: undefined x.')\n"
      "    sys.exit(0)\n"
      "for line in sys.stdin:\n"
      "    if line.strip() == 'exit':\n"
      "        sys.exit(0)\n"
      "    handle(line, \"Undefined function or variable 'x'.\")\n"
      "    print('>> ', end='', flush=True)\n");
  char *octave = g_build_filename(bin, "octave", NULL);
  write_program(
      bin, "octave",
      "#!/usr/bin/env python3\n"
      "import re, sys\n"
      "for line in sys.stdin:\n"
      "    if line.strip() == 'exit':\n"
      "        sys.exit(0)\n"
      "    m = re.search(r\"diary\\('([^']*)'\\); disp\\('([^']*)'\\).*run\\('([^']*)'\\).*disp\\('([^']*)'\\)\", "
      "line)\n"
      "    if m:\n"
      "        folder = re.search(r\"cd\\('([^']*)'\\)\", line).group(1)\n"
      "        with open(m.group(1), 'w') as f:\n"
      "            f.write(f\"{m.group(2)}\\nerror: 'x' undefined near line 2, column 5\\n\"\n"
      "                    f\"    script1 at line 2 column 1 ({folder}/{m.group(3)})\\n\"\n"
      "                    f\"    run at line 93 column 5 (/usr/share/octave/run.m)\\n{m.group(4)}\\n\")\n"
      "    print('>> ', end='', flush=True)\n");
  g_setenv("PATH", path_env, TRUE);
  g_setenv("TCODE_MATLAB", matlab, TRUE);
  g_setenv("TCODE_OCTAVE", octave, TRUE);
  close_everything();
  char *builtin = g_canonicalize_filename("plugins/builtin", NULL); /* the tests run in the source tree */
  plugins_scan_dir(&app, builtin);
  g_free(builtin);
  g_assert_nonnull(plugin_find(&app, "text-tools"));
  g_assert_nonnull(plugin_find(&app, "shellcheck"));
  g_assert_nonnull(plugin_find(&app, "matlab"));
  g_assert_nonnull(plugin_find(&app, "octave"));
  g_assert_nonnull(plugin_find(&app, "cmake"));
  g_assert_nonnull(plugin_find(&app, "java"));

  /* Text Tools */
  g_assert_true(g_file_set_contents(text_file, "b\na\nb\n", -1, NULL));
  open_file(&app, text_file);
  run_until(has_tab, text_file);
  run_until(loaded, find_tab(&app, TAB_EDITOR, text_file));
  plugin_run_command(&app, "text-tools", "unique");
  run_until(text_is, (const char *const[]){ text_file, "b\na\n" });
  Tab *tab = find_tab(&app, TAB_EDITOR, text_file);
  GtkTextIter start, end;
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &start, 1);
  end = start;
  gtk_text_iter_forward_char(&end);
  gtk_text_buffer_select_range(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  plugin_run_command(&app, "text-tools", "upper");
  run_until(text_is, (const char *const[]){ text_file, "b\nA\n" });
  gtk_text_buffer_undo(GTK_TEXT_BUFFER(tab->buffer)); /* one edit, one undo step */
  g_assert_true(text_is((const char *const[]){ text_file, "b\na\n" }));

  /* Word Count (installed in the user's folder by main, started with the project): the active tab's
   * count at the right of the status bar, unsaved edits included, following the typing; its command
   * says the selection's count */
  for (int i = 0; i < 100 && !label_under(GTK_WIDGET(app.status.plugins), "2 words, 2 lines"); i++)
    run_for(50);
  g_assert_nonnull(label_under(GTK_WIDGET(app.status.plugins), "2 words, 2 lines"));
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(tab->buffer), &start);
  gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &start, "c ", -1);
  for (int i = 0; i < 100 && !label_under(GTK_WIDGET(app.status.plugins), "3 words, 2 lines"); i++)
    run_for(50);
  g_assert_nonnull(label_under(GTK_WIDGET(app.status.plugins), "3 words, 2 lines"));
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &start, 1);
  end = start;
  gtk_text_iter_forward_char(&end);
  gtk_text_buffer_select_range(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  status_say(&app, "");
  plugin_run_command(&app, "word-count", "count");
  for (int i = 0; i < 100 && strcmp(gtk_label_get_text(app.status.build), "Word Count: 1 word, 1 line selected") != 0;
       i++)
    run_for(50);
  g_assert_cmpstr(gtk_label_get_text(app.status.build), ==, "Word Count: 1 word, 1 line selected");
  gtk_text_buffer_undo(GTK_TEXT_BUFFER(tab->buffer)); /* the "c " goes */
  g_assert_true(text_is((const char *const[]){ text_file, "b\na\n" }));
  /* Process Monitor (also in the T-Code menu): t-code and Word Count's program under its owner, the
   * terminal's shell too; End stops the plugin the way Stop does (no failure) */
  {
    GtkWidget *bar = find_widget(GTK_WIDGET(app.window), GTK_TYPE_POPOVER_MENU_BAR, NULL, NULL);
    char *target = menu_target(gtk_popover_menu_bar_get_menu_model(GTK_POPOVER_MENU_BAR(bar)), "Process Monitor…");
    g_assert_cmpstr(target, ==, "Help: Process Monitor");
    g_free(target);
    GHashTable *owners = procs_owners_new();
    GHashTableIter iter;
    gpointer key, value;
    int word_count = 0;
    plugins_own_processes(owners);
    g_hash_table_iter_init(&iter, owners);
    while (g_hash_table_iter_next(&iter, &key, &value))
      if (strcmp(((ProcOwner *)value)->label, "Plugin: Word Count") == 0)
        word_count = GPOINTER_TO_INT(key);
    g_hash_table_unref(owners);
    g_assert_cmpint(word_count, >, 0);

    run_command(&app, "Help: Process Monitor");
    run_for(50);
    GtkWindow *monitor = window_titled("Process Monitor");
    g_assert_nonnull(monitor);
    g_assert_true(says(GTK_WIDGET(monitor), "t-code"));
    g_assert_true(says(GTK_WIDGET(monitor), "Plugin: Word Count"));
    g_assert_true(says(GTK_WIDGET(monitor), "Terminal: Terminal"));
    g_assert_true(says(GTK_WIDGET(monitor), "Memory (PSS)"));
    g_assert_true(says(GTK_WIDGET(monitor), "RSS"));
    char *pid = g_strdup_printf("%d", word_count);
    GtkWidget *grid = NULL, *end = NULL;
    for (GtkWidget *w = find_widget(GTK_WIDGET(monitor), GTK_TYPE_GRID, NULL, NULL); w && !end;
         w = gtk_widget_get_next_sibling(w)) {
      if (!GTK_IS_GRID(w))
        continue;
      grid = w;
      for (GtkWidget *c = gtk_widget_get_first_child(grid); c && !end; c = gtk_widget_get_next_sibling(c)) {
        int column, row;
        gtk_grid_query_child(GTK_GRID(grid), c, &column, &row, NULL, NULL);
        if (column == 1 && GTK_IS_LABEL(c) && strcmp(gtk_label_get_text(GTK_LABEL(c)), pid) == 0)
          end = gtk_grid_get_child_at(GTK_GRID(grid), 7, row);
      }
    }
    g_free(pid);
    g_assert_true(GTK_IS_BUTTON(end));
    run_command(&app, "Help: Process Monitor"); /* the same window again */
    g_assert_true(window_titled("Process Monitor") == monitor);
    g_signal_emit_by_name(end, "clicked");
    for (int i = 0; i < 100 && plugin_find(&app, "word-count")->state == PLUGIN_RUNNING; i++)
      run_for(50);
    g_assert_cmpint(plugin_find(&app, "word-count")->state, ==, PLUGIN_STOPPED);
    run_for(400); /* looked at again: its row is gone */
    g_assert_false(says(GTK_WIDGET(monitor), "Plugin: Word Count"));
    gtk_window_destroy(monitor);
    run_for(20);
  }

  /* off from here: the socket tests count the subscribers, and a project switch would start it again */
  plugin_set_enabled(&app, plugin_find(&app, "word-count"), FALSE);
  run_for(200);

  /* ShellCheck */
  g_assert_true(g_file_set_contents(script, "#!/bin/sh\necho $1\n", -1, NULL));
  g_assert_true(plugin_start(&app, plugin_find(&app, "shellcheck")));
  run_for(500); /* it connects and subscribes */
  open_file(&app, script);
  run_until(shows_diagnostic, (const char *const[]){ script, "Double quote" });
  plugin_stop(plugin_find(&app, "shellcheck"));

  /* MATLAB */
  g_assert_true(g_file_set_contents(m_file, "x = 1;\ny = x + 1;\n", -1, NULL));
  open_file(&app, m_file);
  run_until(has_tab, m_file);
  run_until(loaded, find_tab(&app, TAB_EDITOR, m_file));
  plugin_run_command(&app, "matlab", "run");
  run_until(shows_diagnostic, (const char *const[]){ m_file, "Undefined function or variable 'x'." });
  g_assert_cmpint(shell_count(&app), ==, shells + 1);
  g_assert_cmpstr(shell_name(shell_at(&app, shells)), ==, "MATLAB");
  /* Restart: the session types exit, ends by itself and a new one opens in its place */
  VteTerminal *session = shell_at(&app, shells);
  g_object_add_weak_pointer(G_OBJECT(session), (gpointer *)&session); /* NULL once its tab is gone */
  plugin_run_command(&app, "matlab", "restart");
  for (int i = 0; i < 100 && (shell_count(&app) != shells + 1 || session); i++)
    run_for(50);
  g_assert_null(session);
  g_assert_cmpint(shell_count(&app), ==, shells + 1);
  plugin_run_command(&app, "matlab", "close"); /* exit, typed: the fake MATLAB ends, its tab closes */
  for (int i = 0; i < 100 && shell_count(&app) > shells; i++)
    run_for(50);
  g_assert_cmpint(shell_count(&app), ==, shells);
  /* Run in Batch: matlab -batch in a shell tab, its error a diagnostic again */
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(find_tab(&app, TAB_EDITOR, m_file)->buffer), "x = 2;\ny = x + 1;\n", -1);
  plugin_run_command(&app, "matlab", "batch");
  run_until(shows_diagnostic, (const char *const[]){ m_file, "Batch: undefined x." });
  g_assert_true(shows_diagnostic((const char *const[]){ m_file, "Batch: undefined x." }));
  g_assert_cmpint(shell_count(&app), ==, shells + 1);
  g_assert_cmpstr(shell_name(shell_at(&app, shells)), ==, "MATLAB Batch");
  close_shell_tab(&app, shell_at(&app, shells));
  /* Octave: its own report format (each frame's file), its own tab */
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(find_tab(&app, TAB_EDITOR, m_file)->buffer), "x = 3;\ny = x + 1;\n", -1);
  plugin_run_command(&app, "octave", "run");
  run_until(shows_diagnostic, (const char *const[]){ m_file, "'x' undefined near line 2, column 5" });
  g_assert_true(shows_diagnostic((const char *const[]){ m_file, "'x' undefined near line 2, column 5" }));
  g_assert_cmpstr(shell_name(shell_at(&app, shells)), ==, "Octave");
  plugin_run_command(&app, "octave", "close");
  for (int i = 0; i < 100 && shell_count(&app) > shells; i++)
    run_for(50);
  g_assert_cmpint(shell_count(&app), ==, shells);
  /* .m with Objective-C next to it: colored as Objective-C, and Run does not hand it to MATLAB */
  char *objc_dir = g_build_filename(app.project.root, "objc", NULL);
  char *objc_m = write_at(objc_dir, "Widget.m", "", -1, 0), *objc_h = write_at(objc_dir, "Widget.h", "", -1, 0);
  g_assert_cmpstr(gtk_source_language_get_id(editor_language(objc_m, "")), ==, "objc");
  g_assert_cmpstr(gtk_source_language_get_id(editor_language(m_file, "x = 1;\n")), ==, "matlab");
  g_unlink(objc_h);
  g_unlink(objc_m);
  g_rmdir(objc_dir);
  g_free(objc_h);
  g_free(objc_m);
  g_free(objc_dir);

  /* CMake */
  g_assert_true(g_file_set_contents(lists, "project(x)\nfoo_bar()\n", -1, NULL));
  open_file(&app, lists);
  run_until(has_tab, lists);
  run_until(loaded, find_tab(&app, TAB_EDITOR, lists));
  /* colored as CMake: a whole-name glob, matched on the file's name, not its path */
  g_assert_cmpstr(gtk_source_language_get_id(gtk_source_buffer_get_language(find_tab(&app, TAB_EDITOR, lists)->buffer)),
                  ==, "cmake");
  g_assert_cmpstr(gtk_source_language_get_id(editor_language("/some/where/Makefile", NULL)), ==, "makefile");
  plugin_run_command(&app, "cmake", "configure");
  run_until(shows_diagnostic, (const char *const[]){ lists, "Unknown CMake command \"foo_bar\"." });

  /* Java: Run (F5) on a plain folder's file builds it with javac and runs its class in the "Java"
   * tab; the exception's frame in the project becomes a diagnostic */
  write_program(bin, "javac", "#!/bin/sh\nexit 0\n");
  write_program(bin, "java",
                "#!/bin/sh\n"
                "echo 'hello from java'\n"
                "printf 'Exception in thread \"main\" java.lang.ArithmeticException: / by zero\\n"
                "\\tat com.example.App.main(App.java:4)\\n' >&2\n"
                "exit 1\n");
  char *java_dir = g_build_filename(app.project.root, "jsrc", "com", "example", NULL);
  char *java_file = write_at(java_dir, "App.java",
                             "package com.example;\npublic class App {\n  public static void main(String[] a) {\n"
                             "    System.out.println(1 / 0);\n  }\n}\n",
                             -1, 0);
  g_assert_nonnull(plugin_find(&app, "java"));
  PluginCommand *java_run = plugins_run_command_for(&app, java_file);
  g_assert_nonnull(java_run);
  g_assert_cmpstr(java_run->plugin->id, ==, "java");
  open_file(&app, java_file);
  run_until(has_tab, java_file);
  run_until(loaded, find_tab(&app, TAB_EDITOR, java_file));
  shells = shell_count(&app);
  build_project(&app, TRUE);
  run_until(shows_diagnostic, (const char *const[]){ java_file, "ArithmeticException: / by zero" });
  g_assert_cmpint(shell_count(&app), ==, shells + 1);
  g_assert_cmpstr(shell_name(shell_at(&app, shells)), ==, "Java");
  close_shell_tab(&app, shell_at(&app, shells));
  char *java_top = g_build_filename(app.project.root, "jsrc", NULL);
  const char *rm_java[] = { "rm", "-rf", java_top, NULL };
  g_spawn_sync(NULL, (char **)rm_java, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(java_top);
  g_free(java_file);
  g_free(java_dir);

  close_everything();
  g_unlink(lists);
  plugins_reload(&app); /* back to t-code's own */
  g_setenv("PATH", old_path, TRUE);
  g_unsetenv("TCODE_MATLAB");
  g_unsetenv("TCODE_OCTAVE");
  g_free(octave);
  const char *rm[] = { "rm", "-rf", bin, NULL };
  g_spawn_sync(NULL, (char **)rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(python);
  g_free(bin);
  g_free(old_path);
  g_free(path_env);
  g_free(matlab);
  g_free(text_file);
  g_free(script);
  g_free(m_file);
  g_free(lists);
}

static void on_activate(GApplication *gapp, gpointer data G_GNUC_UNUSED)
{
  int *status = g_object_get_data(G_OBJECT(gapp), "status");

  build_window(&app, GTK_APPLICATION(gapp));
  app.settings.check_updates = FALSE; /* the tests must not ask GitHub for new releases */
  run_for(100);
  *status = g_test_run();
  app.closing = TRUE;
  gtk_window_destroy(app.window);
}

/* Debugging ------------------------------------------------------------------------------------ */

static gboolean stopped_at(gconstpointer line)
{
  return app.debug.stop_path && app.debug.stop_line == GPOINTER_TO_INT(line);
}

/* Waits (gdb takes a moment to start) until the program stopped at line, and checks it did. */
static void wait_stop(int line)
{
  gint64 end = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

  while (!stopped_at(GINT_TO_POINTER(line)) && g_get_monotonic_time() < end)
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpint(app.debug.stop_line, ==, line);
}

/* Waits until list() (the Variables or the Watch) holds line ("m = 6*"). */
static void wait_line(char *(*list)(void), const char *line)
{
  gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  char *text = NULL;

  do {
    g_free(text);
    g_main_context_iteration(NULL, FALSE);
    text = list();
  } while (!strstr(text, line) && g_get_monotonic_time() < end);
  if (!strstr(text, line))
    g_error("no \"%s\" in:\n%s", line, text);
  g_free(text);
}

static void wait_variable(const char *line)
{
  wait_line(debug_view_variables_text, line);
}

static void wait_end(void)
{
  gint64 end = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

  while (debug_active(&app) && g_get_monotonic_time() < end)
    g_main_context_iteration(NULL, FALSE);
  g_assert_false(debug_active(&app));
}

/* A program built with -g runs under gdb: it stops at a breakpoint, which the editor shows; it steps
 * over, out, on to a breakpoint put in while it is stopped, and to its end; Stop ends it at once. */
static void test_debugging(void)
{
  static const char source[] = "#include <stdio.h>\n"        /* 1 */
                               "static int twice(int n)\n"   /* 2 */
                               "{\n"                         /* 3 */
                               "  int m = n * 2;\n"          /* 4 */
                               "  return m;\n"               /* 5 */
                               "}\n"                         /* 6 */
                               "int main(void)\n"            /* 7 */
                               "{ int b[3] = { 1, 2, 3 };\n" /* 8 */
                               "  int a = twice(3);\n"       /* 9 */
                               "  printf(\"%d\\n\", a);\n"   /* 10 */
                               "  return 0;\n"               /* 11 */
                               "}\n";                        /* 12 */
  char *gdb = g_find_program_in_path("gdb"), *gcc = g_find_program_in_path("gcc");

  if (!gdb || !gcc) {
    g_test_skip("no gdb or gcc");
    g_free(gdb);
    g_free(gcc);
    return;
  }
  char *path = g_build_filename(app.project.root, "prog.c", NULL),
       *exe = g_build_filename(app.project.root, "prog", NULL);
  const char *cc[] = { "gcc", "-g", "-O0", "-o", exe, path, NULL };
  int status = -1;
  g_assert_true(g_file_set_contents(path, source, -1, NULL));
  g_assert_true(g_spawn_sync(NULL, (char **)cc, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL));
  g_assert_cmpint(status, ==, 0);

  open_file(&app, path);
  run_until(has_tab, path);
  Tab *tab = find_tab(&app, TAB_EDITOR, path);
  while (tab->view->loading)
    g_main_context_iteration(NULL, FALSE);
  breakpoint_toggle(tab, 3); /* line 4 */

  debug_launch(&app, exe);
  g_assert_true(debug_active(&app));
  wait_stop(4);
  g_assert_cmpstr(app.debug.stop_path, ==, path);
  wait_variable("n = 3\n");
  wait_variable("m = "); /* its value before the step, garbage */
  char *stack = debug_view_stack_text();
  g_assert_cmpstr(stack, ==, "twice prog.c:4\nmain prog.c:9\n");
  g_free(stack);
  debug_step_over(&app);
  wait_stop(5);
  wait_variable("m = 6*\n"); /* changed since the last pause */
  wait_variable("n = 3\n");  /* not changed */
  debug_step_out(&app);
  wait_stop(9);
  wait_variable("b = {1, 2, 3}\n"); /* an array's value, made from its elements */
  breakpoint_toggle(tab, 9);        /* line 10, while stopped: gdb hears of it */
  debug_start_or_continue(&app);
  wait_stop(10);
  debug_start_or_continue(&app);
  wait_end();
  g_assert_null(app.debug.stop_path);

  debug_launch(&app, exe); /* again, and Stop while it is stopped */
  wait_stop(4);
  debug_stop(&app);
  wait_end();

  breakpoints_remove_all(&app);
  close_everything();
  g_free(exe);
  g_free(path);
  g_free(gdb);
  g_free(gcc);
}

static int debugged_again;

static void debug_again(App *t G_GNUC_UNUSED)
{
  debugged_again++;
}

/* A test's program runs under gdb with its arguments, in the folder given; Restart asks the test's own
 * way of starting again instead of F5's build. */
static void test_debugging_arguments(void)
{
  static const char source[] = "#include <string.h>\n"                                /* 1 */
                               "int main(int argc, char **argv)\n"                    /* 2 */
                               "{\n"                                                  /* 3 */
                               "  int chosen = 0;\n"                                  /* 4 */
                               "  if (argc > 1 && strcmp(argv[1], \"--yes\") == 0)\n" /* 5 */
                               "    chosen = 1;\n"                                    /* 6 */
                               "  return !chosen;\n"                                  /* 7 */
                               "}\n";                                                 /* 8 */
  char *gdb = g_find_program_in_path("gdb"), *gcc = g_find_program_in_path("gcc");

  if (!gdb || !gcc) {
    g_test_skip("no gdb or gcc");
    g_free(gdb);
    g_free(gcc);
    return;
  }
  char *path = g_build_filename(app.project.root, "args.c", NULL),
       *exe = g_build_filename(app.project.root, "args", NULL);
  const char *cc[] = { "gcc", "-g", "-O0", "-o", exe, path, NULL };
  const char *args[] = { "--yes", NULL };
  int status = -1;
  g_assert_true(g_file_set_contents(path, source, -1, NULL));
  g_assert_true(g_spawn_sync(NULL, (char **)cc, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL));
  g_assert_cmpint(status, ==, 0);

  open_file(&app, path);
  run_until(has_tab, path);
  Tab *tab = find_tab(&app, TAB_EDITOR, path);
  while (tab->view->loading)
    g_main_context_iteration(NULL, FALSE);
  breakpoint_toggle(tab, 5); /* line 6: only with --yes */

  debugged_again = 0;
  debug_launch_test(&app, exe, args, app.project.root, debug_again);
  g_assert_true(debug_active(&app));
  wait_stop(6);
  debug_restart(&app);
  wait_end();
  g_assert_cmpint(debugged_again, ==, 1);

  breakpoints_remove_all(&app);
  close_everything();
  g_free(exe);
  g_free(path);
  g_free(gdb);
  g_free(gcc);
}

/* A breakpoint on a line without code moves to the line gdb stops at; a condition set while paused
 * stops only when it is true; a watch expression is evaluated at each pause. */
static void test_debugging_conditions(void)
{
  static const char source[] = "#include <stdio.h>\n"            /* 1 */
                               "int main(void)\n"                /* 2 */
                               "{\n"                             /* 3 */
                               "  int total = 0;\n"              /* 4 */
                               "\n"                              /* 5 */
                               "  for (int i = 0; i < 5; i++)\n" /* 6 */
                               "    total += i;\n"               /* 7 */
                               "  printf(\"%d\\n\", total);\n"   /* 8 */
                               "  return 0;\n"                   /* 9 */
                               "}\n";                            /* 10 */
  char *gdb = g_find_program_in_path("gdb"), *gcc = g_find_program_in_path("gcc");

  if (!gdb || !gcc) {
    g_test_skip("no gdb or gcc");
    g_free(gdb);
    g_free(gcc);
    return;
  }
  char *path = g_build_filename(app.project.root, "loop.c", NULL),
       *exe = g_build_filename(app.project.root, "loop", NULL);
  const char *cc[] = { "gcc", "-g", "-O0", "-o", exe, path, NULL };
  int status = -1;
  g_assert_true(g_file_set_contents(path, source, -1, NULL));
  g_assert_true(g_spawn_sync(NULL, (char **)cc, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL));
  g_assert_cmpint(status, ==, 0);

  open_file(&app, path);
  run_until(has_tab, path);
  Tab *tab = find_tab(&app, TAB_EDITOR, path);
  while (tab->view->loading)
    g_main_context_iteration(NULL, FALSE);
  breakpoint_toggle(tab, 4); /* line 5: blank */
  debug_view_add_watch(&app, "total * 10");
  char *watch = debug_view_watch_text();
  g_assert_cmpstr(watch, ==, "total * 10 = not available: the program is not paused\n");
  g_free(watch);

  debug_launch(&app, exe);
  wait_stop(6); /* gdb put it on the next line with code, and so does the editor */
  GArray *list = breakpoints_in(&app, path);
  g_assert_cmpint(list->len, ==, 1);
  g_assert_cmpint(g_array_index(list, Breakpoint, 0).line, ==, 6);

  breakpoint_toggle(tab, 5);                  /* off */
  breakpoint_set_condition(tab, 6, "i == 3"); /* line 7, while paused */
  debug_start_or_continue(&app);
  wait_stop(7);
  wait_variable("i = 3\n");
  wait_line(debug_view_watch_text, "total * 10 = 30\n"); /* 0 + 1 + 2 */
  debug_stop(&app);
  wait_end();

  breakpoints_remove_all(&app);
  char *none[] = { NULL };
  debug_view_set_watches(none);
  close_everything();
  g_free(exe);
  g_free(path);
  g_free(gdb);
  g_free(gcc);
}

/* An editor's whole text (free it). */
static char *text_of(Tab *tab)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, FALSE);
}

static gboolean disassembly_ready(gconstpointer data G_GNUC_UNUSED)
{
  Tab *tab = app.debug.disassembly ? find_tab(&app, TAB_EDITOR, app.debug.disassembly) : NULL;

  return tab && !tab->view->loading && app.debug.disassembly_line > 0;
}

/* The line of the Disassembly View's text the arrow is on. */
static char *disassembly_line_text(void)
{
  Tab *tab = find_tab(&app, TAB_EDITOR, app.debug.disassembly);
  GtkTextIter start, end;

  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &start, app.debug.disassembly_line - 1);
  end = start;
  gtk_text_iter_forward_to_line_end(&end);
  return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, FALSE);
}

/* The Disassembly View shows the paused function's instructions with its source lines, the arrow on
 * the next one; in front, F10 steps one instruction (the source line stays); the Registers are listed
 * after the variables. */
static void test_disassembly(void)
{
  static const char source[] = "int main(void)\n"   /* 1 */
                               "{\n"                /* 2 */
                               "  int x = 2;\n"     /* 3 */
                               "  x = x * 21;\n"    /* 4 */
                               "  return x - 42;\n" /* 5 */
                               "}\n";               /* 6 */
  char *gdb = g_find_program_in_path("gdb"), *gcc = g_find_program_in_path("gcc");

  if (!gdb || !gcc) {
    g_test_skip("no gdb or gcc");
    g_free(gdb);
    g_free(gcc);
    return;
  }
  char *path = g_build_filename(app.project.root, "asm.c", NULL),
       *exe = g_build_filename(app.project.root, "asm", NULL);
  const char *cc[] = { "gcc", "-g", "-O0", "-o", exe, path, NULL };
  int status = -1;
  g_assert_true(g_file_set_contents(path, source, -1, NULL));
  g_assert_true(g_spawn_sync(NULL, (char **)cc, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL));
  g_assert_cmpint(status, ==, 0);

  open_file(&app, path);
  run_until(has_tab, path);
  Tab *tab = find_tab(&app, TAB_EDITOR, path);
  while (tab->view->loading)
    g_main_context_iteration(NULL, FALSE);
  breakpoint_toggle(tab, 3); /* line 4 */
  debug_launch(&app, exe);
  wait_stop(4);
  wait_variable("Registers = ");
  disassembly_open(&app);
  gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (!disassembly_ready(NULL) && g_get_monotonic_time() < end)
    g_main_context_iteration(NULL, FALSE);
  g_assert_true(disassembly_ready(NULL));
  Tab *view = find_tab(&app, TAB_EDITOR, app.debug.disassembly);
  g_assert_true(view->view->readonly);
  char *text = text_of(view);
  g_assert_nonnull(strstr(text, "x = x * 21;")); /* the source lines between the instructions */
  g_assert_null(strstr(text, "=>"));
  g_free(text);
  char *first = disassembly_line_text();
  g_assert_true(g_str_has_prefix(first, "   0x"));

  select_tab(view); /* in front: F10 is one instruction */
  int before = app.debug.disassembly_line;
  debug_step_over(&app);
  end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (app.debug.disassembly_line <= before && g_get_monotonic_time() < end)
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpint(app.debug.disassembly_line, >, before);
  g_assert_cmpint(app.debug.stop_line, ==, 4); /* x = x * 21 takes more than one instruction */
  char *second = disassembly_line_text();
  g_assert_cmpstr(first, !=, second);
  g_free(second);
  g_free(first);

  debug_stop(&app);
  wait_end();
  g_assert_cmpint(app.debug.disassembly_line, ==, 0);
  breakpoints_remove_all(&app);
  close_everything();
  g_free(exe);
  g_free(path);
  g_free(gdb);
  g_free(gcc);
}

/* GTK 4.24 under Broadway warns about its own frame timings now and then (no t-code code in the
 * stack): that one is only printed; every other warning still fails the test. */
static GLogWriterOutput log_writer(GLogLevelFlags level, const GLogField *fields, gsize n, gpointer data)
{
  const char *domain = NULL, *message = NULL;

  for (gsize i = 0; i < n; i++) {
    if (strcmp(fields[i].key, "GLIB_DOMAIN") == 0)
      domain = fields[i].value;
    else if (strcmp(fields[i].key, "MESSAGE") == 0)
      message = fields[i].value;
  }
  if (g_strcmp0(domain, "Gdk") == 0 && message && strstr(message, "gdk_frame_timings_submitted")) {
    g_printerr("# ignored: Gdk-WARNING: %s\n", message);
    return G_LOG_WRITER_HANDLED;
  }
  return g_log_writer_default(level, fields, n, data);
}

int main(int argc, char **argv)
{
  char *home = g_dir_make_tmp("t-code-window-XXXXXX", NULL);
  char *config = g_build_filename(home, "config", NULL);
  GtkApplication *gapp;
  int status = 0;

  char *cache = g_build_filename(home, "cache", NULL);

  g_log_set_writer_func(log_writer, NULL, NULL);
  g_setenv("XDG_CONFIG_HOME", config, TRUE); /* not the real session.ini */
  g_setenv("XDG_CACHE_HOME", cache, TRUE);   /* nor the real recovery journals */
  g_free(cache);
  start_broadway();
  gtk_test_init(&argc, &argv, NULL);
  app.project.root = g_build_filename(home, "project", NULL);
  app.settings.font_scale = 1;
  app.settings.preview_scale = 1.0;
  g_mkdir_with_parents(app.project.root, 0755);
  char *session = g_build_filename(config, "t-code", "session.ini", NULL), *dir = g_path_get_dirname(session);
  g_mkdir_with_parents(dir, 0700);
  g_assert_true(g_file_set_contents(session, "[settings]\nminimap=false\n", -1, NULL)); /* before 0.3 */
  g_free(dir);
  g_free(session);
  /* Word Count in the user's plugins folder, as Browse installs it (plugins/builtin is not loaded) */
  char *user_plugins = g_build_filename(config, "t-code", "plugins", NULL);
  const char *copy[] = { "cp", "-r", "plugins/builtin/word-count", user_plugins, NULL };
  g_mkdir_with_parents(user_plugins, 0700);
  g_assert_true(g_spawn_sync(NULL, (char **)copy, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL));
  g_free(user_plugins);
  session_load_settings(&app);
  app.settings.autosave = FALSE; /* edits stay unsaved for the test to see */

  g_test_add_func("/window/first-run-layout", test_first_run_layout);
  g_test_add_func("/window/font-fallback", test_font_fallback);
  g_test_add_func("/window/settings-moved", test_settings_moved);
  g_test_add_func("/window/commit-message-kept", test_commit_message_kept);
  g_test_add_func("/window/settings-edited", test_settings_edited);
  g_test_add_func("/window/open-user-files", test_open_user_files);
  g_test_add_func("/window/settings-pages", test_settings_pages);
  g_test_add_func("/window/plugins-window", test_plugins_window);
  g_test_add_func("/window/plugins-run-question", test_plugins_run_question);
  g_test_add_func("/window/plugins-browse", test_plugins_browse);
  g_test_add_func("/window/example-plugins", test_example_plugins);
  g_test_add_func("/window/panel-menu", test_panel_buttons);
  g_test_add_func("/window/panel-resize", test_panel_resize);
  g_test_add_func("/window/own-icons", test_own_icons);
  g_test_add_func("/window/terminal-tabs", test_terminal_tabs);
  g_test_add_func("/window/temporary-tabs", test_temporary);
  g_test_add_func("/window/temporary-per-group", test_temporary_per_group);
  g_test_add_func("/window/opens-at-left-edge", test_opens_at_left_edge);
  g_test_add_func("/window/comparison-one-tab", test_comparison_one_tab);
  g_test_add_func("/window/hg-comparison", test_hg_comparison);
  g_test_add_func("/window/comparison-first-change", test_comparison_first_change);
  g_test_add_func("/window/comparisons-in-a-row", test_comparisons_in_a_row);
  g_test_add_func("/window/save-conflict", test_save_conflict);
  g_test_add_func("/window/recovery-journal", test_recovery_journal);
  g_test_add_func("/window/socket-requests", test_socket_requests);
  g_test_add_func("/window/socket-while-closing", test_socket_while_closing);
  g_test_add_func("/window/socket-tapi", test_socket_tapi);
  g_test_add_func("/window/socket-tapi-editing", test_socket_tapi_editing);
  g_test_add_func("/window/socket-tapi-pick", test_socket_tapi_pick);
  g_test_add_func("/window/socket-tapi-diagnostics", test_socket_tapi_diagnostics);
  g_test_add_func("/window/socket-tapi-terminals", test_socket_tapi_terminals);
  g_test_add_func("/window/end-programs", test_end_programs);
  g_test_add_func("/window/socket-events", test_socket_events);
  g_test_add_func("/window/socket-stalled-subscriber", test_socket_stalled_subscriber);
  g_test_add_func("/window/update-check-now", test_update_check_now);
  g_test_add_func("/window/status-say", test_status_say);
  g_test_add_func("/window/no-lsp", test_no_lsp);
  g_test_add_func("/window/outline", test_outline);
  g_test_add_func("/window/second-window", test_second_window);
  g_test_add_func("/window/tab-strip", test_tab_strip);
  g_test_add_func("/window/open-tabs", test_open_tabs);
  g_test_add_func("/window/context-menus", test_context_menus);
  g_test_add_func("/window/switch-source-header", test_switch_source_header);
  g_test_add_func("/window/theme-preview", test_theme_preview);
  g_test_add_func("/window/theme-current-line", test_theme_current_line);
  g_test_add_func("/window/theme-renamed", test_theme_renamed);
  g_test_add_func("/window/preview-look", test_preview_look);
  g_test_add_func("/window/preview-math", test_preview_math);
  g_test_add_func("/window/preview-github", test_preview_github);
  g_test_add_func("/window/preview-big", test_preview_big);
  g_test_add_func("/window/open-images", test_open_images);
  g_test_add_func("/window/run-scripts", test_run_scripts);
  g_test_add_func("/window/run-tests", test_run_tests);
  g_test_add_func("/window/run-projects", test_run_projects);
  g_test_add_func("/window/debugging", test_debugging);
  g_test_add_func("/window/debugging-arguments", test_debugging_arguments);
  g_test_add_func("/window/debugging-conditions", test_debugging_conditions);
  g_test_add_func("/window/disassembly", test_disassembly);

  gapp = gtk_application_new("io.github.suacayipkisi.tcode.test", G_APPLICATION_NON_UNIQUE);
  g_object_set_data(G_OBJECT(gapp), "status", &status);
  g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), NULL);
  g_application_run(G_APPLICATION(gapp), 1, argv);
  g_object_unref(gapp);
  stop_broadway();
  stop_socket(&app);
  remove_temp_dir(&app);
  files_delete(home, NULL);
  g_free(config);
  g_free(home);
  return status;
}
