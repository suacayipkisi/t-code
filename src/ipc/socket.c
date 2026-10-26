/*
 * The control socket, for scripts and tools (tools/headless.sh, packaging/smoke-test.sh); its path
 * is in $TCODE_SOCKET. One request per line; a client that ends its requests with NUL bytes instead
 * (a path may hold a line break: printf 'open %s\0' "$path") ends them all that way:
 *   open <path>        open-side <path>    open-window <path> preview <path>     project <dir>
 *   compare <path>     history <path>      terminal <dir>     copy <text>
 *   debug <path>       debug-failed <dir>  say <text>         insert <text>      append <text>
 *   replace-all <text> get-file            get-project        get-text           save subscribe
 * Paths are absolute. "compare" and "history" are git comparisons (git/compare.c), "terminal"
 * does cd in the shell, "copy" puts text on the clipboard, "debug" starts the debugger on a program
 * that was just built for it (t-code --send, from project/run.c; a folder: pick one in it), "debug-failed"
 * says that build failed.
 */
#include "ipc/socket.h"

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
  client_send(c, response, sizeof response - 1);
}

/* One request (see the top of this file). */
static void handle(Client *c, const char *command, const char *arg)
{
  App *t = c->app;

  if (strcmp(command, "subscribe") == 0) {
    subscribe_events(c);
    return;
  }
  if (strcmp(command, "say") == 0) {
    status_say(t, arg);
    return;
  }
  if (strcmp(command, "insert") == 0) {
    Tab *tab = current_tab(t);
    if (tab && tab->kind == TAB_EDITOR && tab->buffer)
      gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), arg, -1);
    return;
  }
  if (strcmp(command, "append") == 0) {
    Tab *tab = current_tab(t);
    if (tab && tab->kind == TAB_EDITOR && tab->buffer) {
      GtkTextIter end;
      gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(tab->buffer), &end);
      gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &end, arg, -1);
    }
    return;
  }
  if (strcmp(command, "replace-all") == 0) {
    Tab *tab = current_tab(t);
    if (tab && tab->kind == TAB_EDITOR && tab->buffer)
      gtk_text_buffer_set_text(GTK_TEXT_BUFFER(tab->buffer), arg, -1);
    return;
  }
  if (strcmp(command, "get-file") == 0) {
    Tab *tab = current_tab(t);
    char *line = g_strconcat((tab && tab->kind == TAB_EDITOR && tab->path) ? tab->path : "", "\n", NULL);
    client_send(c, line, strlen(line));
    g_free(line);
    return;
  }
  if (strcmp(command, "get-project") == 0) {
    char *line = g_strconcat(t->project.root ? t->project.root : "", "\n", NULL);
    client_send(c, line, strlen(line));
    g_free(line);
    return;
  }
  if (strcmp(command, "get-text") == 0) {
    Tab *tab = current_tab(t);
    char *text = NULL;
    if (tab && tab->kind == TAB_EDITOR && tab->buffer) {
      GtkTextIter start, end;
      gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
      text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
    }
    client_send(c, text ? text : "", text ? strlen(text) + 1 : 1); /* with its NUL */
    g_free(text);
    return;
  }
  if (strcmp(command, "save") == 0) {
    Tab *tab = current_tab(t);
    if (tab && tab->kind == TAB_EDITOR)
      save_editor(tab);
    return;
  }
  if (strcmp(command, "copy") == 0) {
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(t->window)), arg);
    return;
  }
  if (strcmp(command, "debug-failed") == 0) {
    debug_build_failed(t);
    return;
  }
  if (!g_path_is_absolute(arg))
    return;
  if (strcmp(command, "open") == 0)
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
  else if (strcmp(command, "project") == 0 && g_file_test(arg, G_FILE_TEST_IS_DIR))
    open_project(t, arg);
  else if (strcmp(command, "terminal") == 0)
    cd_in_shell(t, arg);
  else if (strcmp(command, "debug") == 0)
    debug_launch(t, arg);
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

  if (g_utf8_validate(request, length, NULL)) {
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

  t->paths.socket = g_strdup_printf("%s/t-code-%d.sock", g_get_user_runtime_dir(), getpid());
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
