/*
 * The control socket's connections (see socket.h; the protocol is docs/plugins.md's): reading their
 * requests (requests.c answers them), writing their output in order without blocking the main loop,
 * and the events subscribers get. Its path is in $TCODE_SOCKET.
 *
 * One request per line; a client that ends its requests with NUL bytes instead (a path may hold a
 * line break: printf 'open %s\0' "$path") ends them all that way.
 */
#include "ipc/socket.h"

#include "application/identity.h"
#include "editor/tabs.h"
#include "ipc/client.h"
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

/* did_change waits until typing pauses this long. */
#define CHANGE_MS 300

/* A subscriber that has this much unread output is dropped: it does not read its events. */
#define MAX_BACKLOG (256 * 1024)

gboolean socket_handling;

/* A connection is held by its reader and by the write in flight. */
void client_ref(Client *c)
{
  c->refs++;
}

void client_unref(Client *c)
{
  if (--c->refs > 0)
    return;
  if (c->subscribed && c->app->socket_events)
    g_ptr_array_remove(c->app->socket_events, c);
  g_queue_clear_full(&c->pending, (GDestroyNotify)g_bytes_unref);
  g_object_unref(c->conn);
  g_free(c->owner);
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
void client_send(Client *c, const char *data, gsize size)
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

/* {"event":<event>,"path":<path>} (with "by":"plugin" when the socket caused it) to every subscriber. */
static void broadcast_event(App *t, const char *event, const char *path, gboolean by_plugin)
{
  if (!t->socket_events || t->socket_events->len == 0)
    return;
  JsonNode *node = json_node_init_string(json_node_alloc(), path ? path : "");
  char *quoted = json_to_string(node, FALSE);
  char *message =
      g_strdup_printf("{\"event\":\"%s\",\"path\":%s%s}\n", event, quoted, by_plugin ? ",\"by\":\"plugin\"" : "");
  gsize length = strlen(message);

  json_node_unref(node);
  g_free(quoted);
  /* client_send() may drop a subscriber from the list: go through a copy */
  GPtrArray *clients = g_ptr_array_copy(t->socket_events, NULL, NULL);
  for (guint i = 0; i < clients->len; i++)
    client_send(g_ptr_array_index(clients, i), message, length);
  g_ptr_array_unref(clients);
  g_free(message);
}

void socket_did_open(App *t, const char *path)
{
  broadcast_event(t, "did_open", path, FALSE);
}

/* The files whose save a socket request asked for (path → TRUE): their did_save is "by":"plugin". */
static GHashTable *plugin_saves;

void socket_saving(App *t G_GNUC_UNUSED, const char *path)
{
  if (!plugin_saves)
    plugin_saves = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_add(plugin_saves, g_strdup(path));
}

void socket_did_save(App *t, const char *path)
{
  gboolean by_plugin = plugin_saves && g_hash_table_remove(plugin_saves, path);

  broadcast_event(t, "did_save", path, by_plugin);
}

void socket_did_close(App *t, const char *path)
{
  broadcast_event(t, "did_close", path, FALSE);
}

/* A did_change waiting for typing to pause. */
typedef struct {
  App *app;
  char *path;
  guint id;
  gboolean by_plugin; /* every change since the last did_change came over the socket */
} Change;

static GHashTable *changes; /* path → Change */

static void change_free(Change *change)
{
  g_clear_handle_id(&change->id, g_source_remove);
  g_free(change->path);
  g_free(change);
}

static gboolean change_now(gpointer data)
{
  Change *change = data;

  change->id = 0;
  broadcast_event(change->app, "did_change", change->path, change->by_plugin);
  g_hash_table_remove(changes, change->path);
  return G_SOURCE_REMOVE;
}

void socket_did_change(App *t, const char *path)
{
  Change *change;

  if (!t->socket_events || t->socket_events->len == 0 || !path) /* nobody listens: no timer either */
    return;
  if (!changes)
    changes = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)change_free);
  change = g_hash_table_lookup(changes, path);
  if (!change) {
    change = g_new0(Change, 1);
    change->app = t;
    change->path = g_strdup(path);
    change->by_plugin = TRUE;
    g_hash_table_insert(changes, change->path, change);
  }
  change->by_plugin = change->by_plugin && socket_handling;
  g_clear_handle_id(&change->id, g_source_remove);
  change->id = g_timeout_add(CHANGE_MS, change_now, change);
}

/* The last active_changed sent, so tab switches that land on the same file send none. */
static char *active_path;

void socket_active_changed(App *t, const char *path)
{
  if (g_strcmp0(active_path, path ? path : "") == 0)
    return;
  g_free(active_path);
  active_path = g_strdup(path ? path : "");
  broadcast_event(t, "active_changed", active_path, FALSE);
}

void socket_project_changed(App *t)
{
  broadcast_event(t, "project_changed", t->project.root, FALSE);
}

/* {"event":<event>,"plugin":<owner>,"id":<id>, ...} to every subscriber; line < 1: no line and text.
 * extra: more string members, name, value, ..., NULL (NULL: none; a NULL value leaves its member out). */
static void broadcast_view(App *t, const char *event, const char *owner, const char *id, int line, const char *text,
                           const char *const *extra)
{
  if (!t->socket_events || t->socket_events->len == 0)
    return;
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "event");
  json_builder_add_string_value(b, event);
  json_builder_set_member_name(b, "plugin");
  json_builder_add_string_value(b, owner);
  json_builder_set_member_name(b, "id");
  json_builder_add_string_value(b, id);
  if (line >= 1) {
    json_builder_set_member_name(b, "line");
    json_builder_add_int_value(b, line);
    json_builder_set_member_name(b, "text");
    json_builder_add_string_value(b, text ? text : "");
  }
  for (int i = 0; extra && extra[i]; i += 2)
    if (extra[i + 1]) {
      json_builder_set_member_name(b, extra[i]);
      json_builder_add_string_value(b, extra[i + 1]);
    }
  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  char *json = json_to_string(root, FALSE), *message = g_strconcat(json, "\n", NULL);
  gsize length = strlen(message);
  GPtrArray *clients = g_ptr_array_copy(t->socket_events, NULL, NULL); /* client_send() may drop one */

  for (guint i = 0; i < clients->len; i++)
    client_send(g_ptr_array_index(clients, i), message, length);
  g_ptr_array_unref(clients);
  g_free(message);
  g_free(json);
  json_node_unref(root);
  g_object_unref(b);
}

void socket_view_activated(App *t, const char *owner, const char *id, int line, const char *text)
{
  broadcast_view(t, "view_activated", owner, id, line, text, NULL);
}

void socket_view_closed(App *t, const char *owner, const char *id)
{
  broadcast_view(t, "view_closed", owner, id, 0, NULL, NULL);
}

void socket_view_input(App *t, const char *owner, const char *id, const char *text, const char *mode)
{
  const char *const extra[] = { "text", text, "mode", mode, NULL };

  broadcast_view(t, "view_input", owner, id, 0, NULL, extra);
}

void socket_view_choice(App *t, const char *owner, const char *id, const char *prompt, const char *choice)
{
  const char *const extra[] = { "prompt", prompt, "choice", choice, NULL };

  broadcast_view(t, "view_choice", owner, id, 0, NULL, extra);
}

void socket_view_stop(App *t, const char *owner, const char *id)
{
  broadcast_view(t, "view_stop", owner, id, 0, NULL, NULL);
}

void client_subscribe(Client *c)
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
void send_line(Client *c, const char *text)
{
  char *line = g_strconcat(text, "\n", NULL);

  client_send(c, line, strlen(line));
  g_free(line);
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
    if (space)
      *space = '\0';
    socket_handling = TRUE;
    c->held = !handle(c, request, space ? space + 1 : "");
    socket_handling = FALSE;
  }
  g_free(request);
  if (!c->held)
    read_request(in, c);
}

void client_resume(Client *c)
{
  if (!c->held)
    return;
  c->held = FALSE;
  read_request(c->in, c);
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
  c->in = in;
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
  g_clear_pointer(&changes, g_hash_table_unref);
  g_clear_pointer(&plugin_saves, g_hash_table_unref);
  g_clear_pointer(&active_path, g_free);
}
