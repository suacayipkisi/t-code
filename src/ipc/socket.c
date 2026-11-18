/*
 * The control socket, for plugins, scripts and tools (tools/headless.sh, packaging/smoke-test.sh); its
 * path is in $TCODE_SOCKET. One request per line; a client that ends its requests with NUL bytes
 * instead (a path may hold a line break: printf 'open %s\0' "$path") ends them all that way. The
 * protocol is docs/plugins.md's.
 *
 * A connection that starts with "hello tapi_1" speaks tapi_1: every request gets an answer line, "ok",
 * "ok <value>" or "error <code> <message>" (get-text's "ok" is followed by the text and a NUL). One
 * that does not is a legacy one, answered as before the API had a version: only get-file,
 * get-project, get-text, version and subscribe answer.
 *
 * "_debug <program>" and "_debug-failed <dir>" are internal (F5's build, through t-code --send): they
 * may change; a legacy connection may still call them "debug" and "debug-failed".
 */
#include "ipc/socket.h"

#include "application/identity.h"
#include "debug/debugger.h"
#include "editor/editor.h"
#include "editor/preview/preview.h"
#include "git/compare.h"
#include "project/project.h"
#include "editor/tabs.h"
#include "window/secondwindow.h"
#include "window/statusbar.h"
#include "window/terminal.h"
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

#ifndef TCODE_VERSION /* set by the Makefile */
#define TCODE_VERSION "unknown"
#endif

/* A subscriber that has this much unread output is dropped: it does not read its events. */
#define MAX_BACKLOG (256 * 1024)

/* A connection: its output goes out in order without blocking the main loop (a client that does not
 * read only fills its own queue). Held by its reader and by the write in flight. */
typedef struct {
  App *app;
  GSocketConnection *conn;
  GQueue pending; /* GBytes not written yet */
  gsize backlog;  /* their bytes */
  gboolean writing, failed, subscribed;
  int api; /* the tapi version it said hello with; 0: a legacy connection */
  int refs;
} Client;

static void client_unref(Client *c)
{
  if (--c->refs > 0)
    return;
  if (c->subscribed && c->app->socket_events)
    g_ptr_array_remove(c->app->socket_events, c);
  g_queue_clear_full(&c->pending, (GDestroyNotify)g_bytes_unref);
  g_object_unref(c->conn);
  g_free(c);
}

/* No more output for this client: an event subscriber stops getting events. */
static void client_fail(Client *c)
{
  c->failed = TRUE;
  if (c->subscribed && c->app->socket_events)
    g_ptr_array_remove(c->app->socket_events, c);
  c->subscribed = FALSE;
  g_queue_clear_full(&c->pending, (GDestroyNotify)g_bytes_unref);
  c->backlog = 0;
}

static void write_next(Client *c);

static void on_written(GObject *source, GAsyncResult *result, gpointer data)
{
  Client *c = data;
  GBytes *bytes = g_queue_pop_head(&c->pending);

  c->writing = FALSE;
  if (bytes) {
    c->backlog -= g_bytes_get_size(bytes);
    g_bytes_unref(bytes);
  }
  if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, NULL))
    client_fail(c); /* the client went away */
  else
    write_next(c);
  client_unref(c);
}

/* Writes the queue's head; the next one follows when it is out. */
static void write_next(Client *c)
{
  GBytes *bytes = g_queue_peek_head(&c->pending);
  gsize size;
  gconstpointer data;

  if (c->writing || c->failed || !bytes)
    return;
  data = g_bytes_get_data(bytes, &size);
  c->writing = TRUE;
  c->refs++;
  g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(c->conn)), data, size, G_PRIORITY_DEFAULT,
                                  NULL, on_written, c);
}

/* Queues output for the client. */
static void client_send(Client *c, const char *data, gsize size)
{
  if (c->failed)
    return;
  if (c->subscribed && c->backlog + size > MAX_BACKLOG) { /* it does not read: let it go */
    client_fail(c);
    g_io_stream_close_async(G_IO_STREAM(c->conn), G_PRIORITY_DEFAULT, NULL, NULL, NULL);
    return;
  }
  g_queue_push_tail(&c->pending, g_bytes_new(data, size));
  c->backlog += size;
  write_next(c);
}

static void broadcast_event(App *t, const char *event, const char *path)
{
  JsonNode *node = json_node_init_string(json_node_alloc(), path);
  char *quoted = json_to_string(node, FALSE);
  char *message = g_strdup_printf("{\"event\":\"%s\",\"path\":%s}\n", event, quoted);
  gsize length = strlen(message);

  json_node_unref(node);
  g_free(quoted);
  /* client_send() may drop a subscriber from the list: go through a copy */
  GPtrArray *clients = t->socket_events ? g_ptr_array_copy(t->socket_events, NULL, NULL) : NULL;
  for (guint i = 0; clients && i < clients->len; i++)
    client_send(g_ptr_array_index(clients, i), message, length);
  if (clients)
    g_ptr_array_unref(clients);
  g_free(message);
}

void socket_did_open(App *t, const char *path)
{
  broadcast_event(t, "did_open", path);
}

void socket_did_save(App *t, const char *path)
{
  broadcast_event(t, "did_save", path);
}

static void subscribe_events(Client *c)
{
  static const char response[] = "{\"event\":\"subscribed\"}\n";

  if (c->failed)
    return;
  if (!c->subscribed) {
    if (!c->app->socket_events)
      c->app->socket_events = g_ptr_array_new();
    g_ptr_array_add(c->app->socket_events, c);
    c->subscribed = TRUE;
  }
  if (c->api)
    client_send(c, "ok\n", 3);
  else
    client_send(c, response, sizeof response - 1);
}

/* Answers with text and a line break. */
static void send_line(Client *c, const char *text)
{
  char *line = g_strconcat(text, "\n", NULL);

  client_send(c, line, strlen(line));
  g_free(line);
}

/* tapi answers (a legacy connection gets none): "ok" or "ok <value>". */
static void answer_ok(Client *c, const char *value)
{
  char *line;

  if (!c->api)
    return;
  line = value ? g_strconcat("ok ", value, NULL) : g_strdup("ok");
  send_line(c, line);
  g_free(line);
}

/* "error <code> <message>": code is one word a plugin can test, message is for people. */
static void answer_error(Client *c, const char *code, const char *message)
{
  char *line;

  if (!c->api)
    return;
  line = g_strdup_printf("error %s %s", code, message);
  send_line(c, line);
  g_free(line);
}

/* "hello [tapi_N]": the connection speaks tapi_N from now on (the latest when no version is named). */
static void hello(Client *c, const char *arg)
{
  int version = 0;

  if (!*arg)
    version = TAPI_LATEST;
  else if (g_str_has_prefix(arg, "tapi_")) {
    char *end = NULL;
    gint64 number = g_ascii_strtoll(arg + 5, &end, 10);
    if (end && !*end && number >= 1 && number <= TAPI_LATEST)
      version = (int)number;
  }
  if (!version) {
    char *message =
        g_strdup_printf("error unsupported-api t-code %s speaks tapi_1 to tapi_%d", TCODE_VERSION, TAPI_LATEST);
    send_line(c, message);
    g_free(message);
    return;
  }
  c->api = version;
  char *line = g_strdup_printf("ok tapi_%d t-code %s", version, TCODE_VERSION);
  send_line(c, line);
  g_free(line);
}

/* One request (see the top of this file). */
static void handle(Client *c, const char *command, const char *arg)
{
  App *t = c->app;
  Tab *tab = current_tab(t);
  gboolean editor = tab && tab->kind == TAB_EDITOR;
  GtkTextBuffer *buffer = editor && tab->buffer ? GTK_TEXT_BUFFER(tab->buffer) : NULL; /* NULL: no editor */
  gboolean legacy = c->api == 0;
  const char *failure = NULL, *message = NULL; /* an error's code and its words */
  GtkTextIter start, end;

  if (buffer)
    gtk_text_buffer_get_bounds(buffer, &start, &end);
  if (strcmp(command, "hello") == 0) {
    hello(c, arg);
    return;
  }
  if (strcmp(command, "version") == 0) { /* new with tapi_1: answers legacy connections too */
    char *line = g_strdup_printf("ok tapi_%d t-code %s", TAPI_LATEST, TCODE_VERSION);
    send_line(c, line);
    g_free(line);
    return;
  }
  if (strcmp(command, "subscribe") == 0) {
    subscribe_events(c);
    return;
  }
  if (strcmp(command, "get-file") == 0 || strcmp(command, "get-project") == 0) {
    const char *value = command[4] == 'f' ? (editor && tab->path ? tab->path : "") : t->project.root ?: "";
    if (legacy)
      send_line(c, value);
    else
      answer_ok(c, value);
    return;
  }
  if (strcmp(command, "get-text") == 0) {
    char *text = buffer ? gtk_text_buffer_get_text(buffer, &start, &end, TRUE) : g_strdup("");
    if (!legacy)
      client_send(c, "ok\n", 3);
    client_send(c, text, strlen(text) + 1); /* with its NUL */
    g_free(text);
    return;
  }

  gboolean edits = strcmp(command, "insert") == 0 || strcmp(command, "append") == 0 ||
                   strcmp(command, "replace-all") == 0 || strcmp(command, "save") == 0;
  gboolean on_path = strcmp(command, "open") == 0 || strcmp(command, "open-side") == 0 ||
                     strcmp(command, "open-window") == 0 || strcmp(command, "preview") == 0 ||
                     strcmp(command, "compare") == 0 || strcmp(command, "history") == 0 ||
                     strcmp(command, "project") == 0 || strcmp(command, "terminal") == 0 ||
                     strcmp(command, "_debug") == 0 || (legacy && strcmp(command, "debug") == 0);

  if (edits && !buffer) {
    failure = "no-editor";
    message = "the active tab is not an editor";
  } else if (on_path && !g_path_is_absolute(arg)) {
    failure = "bad-argument";
    message = "needs an absolute path";
  } else if (strcmp(command, "say") == 0)
    status_say(t, arg);
  else if (strcmp(command, "insert") == 0)
    gtk_text_buffer_insert_at_cursor(buffer, arg, -1);
  else if (strcmp(command, "append") == 0)
    gtk_text_buffer_insert(buffer, &end, arg, -1);
  else if (strcmp(command, "replace-all") == 0)
    gtk_text_buffer_set_text(buffer, arg, -1);
  else if (strcmp(command, "save") == 0)
    save_editor(tab);
  else if (strcmp(command, "copy") == 0)
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(t->window)), arg);
  else if (strcmp(command, "_debug-failed") == 0 || (legacy && strcmp(command, "debug-failed") == 0))
    debug_build_failed(t);
  else if (strcmp(command, "open") == 0)
    open_file(t, arg);
  else if (strcmp(command, "open-side") == 0)
    open_to_side(t, arg);
  else if (strcmp(command, "open-window") == 0)
    open_in_new_window(t, arg);
  else if (strcmp(command, "preview") == 0)
    open_preview(t, arg);
  else if (strcmp(command, "compare") == 0)
    compare_with_head(t, arg);
  else if (strcmp(command, "history") == 0)
    file_history(t, arg);
  else if (strcmp(command, "project") == 0) {
    if (g_file_test(arg, G_FILE_TEST_IS_DIR))
      open_project(t, arg);
    else {
      failure = "bad-argument";
      message = "not a folder";
    }
  } else if (strcmp(command, "terminal") == 0)
    cd_in_shell(t, arg);
  else if (on_path) /* _debug, or a legacy debug */
    debug_launch(t, arg);
  else {
    failure = "unknown-command";
    message = command;
  }
  if (failure)
    answer_error(c, failure, message);
  else
    answer_ok(c, NULL);
}

static void read_request(GDataInputStream *in, Client *c);

/* A request came on a connection: handle it and read the next one; the connection ends at EOF. */
static void on_request(GObject *source, GAsyncResult *result, gpointer data)
{
  GDataInputStream *in = G_DATA_INPUT_STREAM(source);
  Client *c = data;
  gsize length = 0;
  char *request = g_data_input_stream_read_upto_finish(in, result, &length, NULL);
  GError *error = NULL;

  /* The end it stopped at is still there; at EOF there is none. */
  g_data_input_stream_read_byte(in, NULL, &error);
  if (!request || (error && length == 0)) { /* closed: what is still queued goes out first */
    g_clear_error(&error);
    g_free(request);
    g_object_unref(in);
    client_unref(c);
    return;
  }
  g_clear_error(&error);

  /* While the window closes its groups and panels go: a request read then (a script's, a build's
   * t-code --send) is dropped. */
  if (!c->app->closing && g_utf8_validate(request, length, NULL)) {
    char *space = strchr(request, ' ');
    if (space) {
      *space = '\0';
      handle(c, request, space + 1);
    } else {
      handle(c, request, "");
    }
  }
  g_free(request);
  read_request(in, c);
}

/* Data came: a connection that sends a NUL byte ends its requests with NULs only (a path may hold a
 * line break), else with line breaks. */
static void on_filled(GObject *source, GAsyncResult *result, gpointer data)
{
  GDataInputStream *in = G_DATA_INPUT_STREAM(source);
  Client *c = data;
  gssize got = g_buffered_input_stream_fill_finish(G_BUFFERED_INPUT_STREAM(in), result, NULL);
  gsize size = 0;
  const char *buffer = g_buffered_input_stream_peek_buffer(G_BUFFERED_INPUT_STREAM(in), &size);

  if (got <= 0 && size == 0) { /* closed */
    g_object_unref(in);
    client_unref(c);
    return;
  }
  if (memchr(buffer, '\0', size))
    g_object_set_data(G_OBJECT(in), "nul", GINT_TO_POINTER(TRUE));
  read_request(in, c);
}

static void read_request(GDataInputStream *in, Client *c)
{
  gsize size = 0;

  if (g_object_get_data(G_OBJECT(in), "nul"))
    g_data_input_stream_read_upto_async(in, "", 1, G_PRIORITY_DEFAULT, NULL, on_request, c); /* up to a NUL */
  else if (g_buffered_input_stream_peek_buffer(G_BUFFERED_INPUT_STREAM(in), &size) && size > 0)
    g_data_input_stream_read_upto_async(in, "\n", 1, G_PRIORITY_DEFAULT, NULL, on_request, c);
  else
    g_buffered_input_stream_fill_async(G_BUFFERED_INPUT_STREAM(in), -1, G_PRIORITY_DEFAULT, NULL, on_filled, c);
}

/* A client connected; its lines are read until it closes the connection. */
static gboolean on_incoming(GSocketService *service G_GNUC_UNUSED, GSocketConnection *conn,
                            GObject *source G_GNUC_UNUSED, App *t)
{
  GDataInputStream *in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
  Client *c = g_new0(Client, 1);

  c->app = t;
  c->conn = g_object_ref(conn);
  c->refs = 1; /* the reader's */
  read_request(in, c);
  return TRUE;
}

void start_socket(App *t)
{
  GSocketAddress *address;
  GError *error = NULL;

  t->paths.socket = g_strdup_printf("%s/" TCODE_NAME "-%d.sock", g_get_user_runtime_dir(), getpid());
  unlink(t->paths.socket);
  address = g_unix_socket_address_new(t->paths.socket);
  t->service = g_socket_service_new();
  if (!g_socket_listener_add_address(G_SOCKET_LISTENER(t->service), address, G_SOCKET_TYPE_STREAM,
                                     G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error)) {
    g_warning("t-code: cannot listen on %s: %s", t->paths.socket, error->message);
    g_clear_error(&error);
  }
  g_object_unref(address);
  g_signal_connect(t->service, "incoming", G_CALLBACK(on_incoming), t);
  g_socket_service_start(t->service);
}

void stop_socket(App *t)
{
  if (t->service)
    g_socket_service_stop(t->service);
  if (t->paths.socket)
    unlink(t->paths.socket);
  for (guint i = 0; t->socket_events && i < t->socket_events->len; i++)
    ((Client *)g_ptr_array_index(t->socket_events, i))->subscribed = FALSE; /* the list goes */
  g_clear_pointer(&t->socket_events, g_ptr_array_unref);
}
