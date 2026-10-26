#define _DEFAULT_SOURCE /* kill() under -std=c23 with musl */
/*
 * Tests for how tabs open and close, in a whole t-code window (every module but main.c) on a
 * temporary project with a temporary config: temporary tabs (a click in the Files panel),
 * comparisons in one tab, the first run's layout and fonts, the files edited by
 * hand, debugging with gdb. Needs a display; skipped without one.
 */
#include "editor/editor.h"
#include "debug/debugger.h"
#include "debug/debugview.h"
#include "debug/disassembly.h"
#include "dialogs/settings.h"
#include "editor/breakpoints.h"
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
#include "lsp/navigate.h"
#include "lsp/outline.h"
#include "lsp/symbols.h"
#include "popups/contextmenu.h"
#include "project/run/run.h"
#include "project/session.h"
#include "project/toolchain.h"
#include "project/userfiles.h"
#include "update/update.h"
#include "window/commands.h"
#include "window/secondwindow.h"
#include "window/statusbar.h"
#include "window/palette.h"
#include "window/theme.h"
#include "window/window.h"
#include "broadway.h"
#include "util.h"

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
  app.project.git_top = g_canonicalize_filename(app.project.root, NULL);

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
  g_set_str(&app.project.git_top, NULL);
  app.project.git_top = g_canonicalize_filename(app.project.root, NULL);
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

static gboolean no_socket_event_clients(gconstpointer data G_GNUC_UNUSED);
static void test_socket_events(void)
{
  char *path = file("socket-events.c");
  GSocketClient *client = g_socket_client_new();
  GSocketAddress *address = g_unix_socket_address_new(app.paths.socket);
  GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  GOutputStream *out;
  GDataInputStream *input;

  g_assert_nonnull(conn);
  out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
  g_assert_true(g_output_stream_write_all(out, "subscribe\n", 10, NULL, NULL, NULL));
  char *message = read_socket_line(input);
  g_assert_cmpstr(message, ==, "{\"event\":\"subscribed\"}");
  g_free(message);

  open_file(&app, path);
  run_until(has_tab, path);
  Tab *tab = find_tab(&app, TAB_EDITOR, path);
  g_assert_nonnull(tab);
  run_until(loaded, tab);
  g_assert_false(tab->view->loading);
  message = read_socket_line(input);
  char *expected = g_strdup_printf("{\"event\":\"did_open\",\"path\":\"%s\"}", path);
  g_assert_cmpstr(message, ==, expected);
  g_free(expected);
  g_free(message);

  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "edited", -1);
  save_editor(tab);
  run_until(saved, tab);
  message = read_socket_line(input);
  expected = g_strdup_printf("{\"event\":\"did_save\",\"path\":\"%s\"}", path);
  g_assert_cmpstr(message, ==, expected);
  g_free(expected);
  g_free(message);

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

/* The Markdown preview looks: quotes are not italic, <sub> text is smaller, a link is
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
  g_assert_true(has_tag(&iter, "small"));

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

/* The first run (no session.ini yet): Files shows on the left, the shell below the editors. */
static void test_first_run_layout(void)
{
  g_assert_cmpint(panel_dock(&app, "files"), ==, 0);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(app.dock.stacks[0]), ==, "files");
  g_assert_cmpint(panel_dock(&app, "shell"), ==, 4);
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
  app.project.git_top = g_canonicalize_filename(app.project.root, NULL);
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
  g_assert_false(gtk_widget_get_visible(app.layout.right)); /* nothing left there */
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

/* The first widget under root of type (any for G_TYPE_NONE) with the CSS class (any for NULL). */
static GtkWidget *descendant(GtkWidget *root, GType type, const char *css_class)
{
  for (GtkWidget *child = root ? gtk_widget_get_first_child(root) : NULL; child;
       child = gtk_widget_get_next_sibling(child)) {
    if ((type == G_TYPE_NONE || G_TYPE_CHECK_INSTANCE_TYPE(child, type)) &&
        (!css_class || gtk_widget_has_css_class(child, css_class)))
      return child;
    GtkWidget *found = descendant(child, type, css_class);
    if (found)
      return found;
  }
  return NULL;
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
  GtkListBox *list = GTK_LIST_BOX(
      descendant(descendant(GTK_WIDGET(app.window), G_TYPE_NONE, "tcode-outline"), GTK_TYPE_LIST_BOX, NULL));
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
  close_all_tabs(&app);
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
  static const char *const pages[] = { "general", "editor", "indentation", "appearance", "c", "files", NULL };
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
  g_assert_cmpint(switches, ==, 14); /* none lost on the way (inlay hints and breadcrumbs the last) */
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

int main(int argc, char **argv)
{
  char *home = g_dir_make_tmp("t-code-window-XXXXXX", NULL);
  char *config = g_build_filename(home, "config", NULL);
  GtkApplication *gapp;
  int status = 0;

  char *cache = g_build_filename(home, "cache", NULL);

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
  session_load_settings(&app);
  app.settings.autosave = FALSE; /* edits stay unsaved for the test to see */

  g_test_add_func("/window/first-run-layout", test_first_run_layout);
  g_test_add_func("/window/font-fallback", test_font_fallback);
  g_test_add_func("/window/settings-moved", test_settings_moved);
  g_test_add_func("/window/commit-message-kept", test_commit_message_kept);
  g_test_add_func("/window/settings-edited", test_settings_edited);
  g_test_add_func("/window/open-user-files", test_open_user_files);
  g_test_add_func("/window/settings-pages", test_settings_pages);
  g_test_add_func("/window/panel-menu", test_panel_buttons);
  g_test_add_func("/window/temporary-tabs", test_temporary);
  g_test_add_func("/window/temporary-per-group", test_temporary_per_group);
  g_test_add_func("/window/opens-at-left-edge", test_opens_at_left_edge);
  g_test_add_func("/window/comparison-one-tab", test_comparison_one_tab);
  g_test_add_func("/window/comparison-first-change", test_comparison_first_change);
  g_test_add_func("/window/comparisons-in-a-row", test_comparisons_in_a_row);
  g_test_add_func("/window/save-conflict", test_save_conflict);
  g_test_add_func("/window/recovery-journal", test_recovery_journal);
  g_test_add_func("/window/socket-requests", test_socket_requests);
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
  g_test_add_func("/window/run-scripts", test_run_scripts);
  g_test_add_func("/window/run-tests", test_run_tests);
  g_test_add_func("/window/run-projects", test_run_projects);
  g_test_add_func("/window/debugging", test_debugging);
  g_test_add_func("/window/debugging-conditions", test_debugging_conditions);
  g_test_add_func("/window/disassembly", test_disassembly);

  gapp = gtk_application_new("io.github.tcode.test", G_APPLICATION_NON_UNIQUE);
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
