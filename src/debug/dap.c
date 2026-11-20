/*
 * The Debug Adapter Protocol client (see dap.h). Requests are numbered (seq); an answer names the
 * request it answers (request_seq) and goes to that request's reply. A debugger that dials in (Delve)
 * gets a socket of its own; what is sent before it came waits in queued.
 */
#define _DEFAULT_SOURCE /* getpid(), setsid(), TIOCSCTTY */
#include "debug/dap.h"

#include "lsp/channel.h"
#include "lsp/json.h"

#include <fcntl.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SOCKET_ARG "{socket}"

struct Dap {
  GSubprocess *process;
  Channel *ch;       /* NULL while a debugger that dials in has not yet */
  GPtrArray *queued; /* the messages sent before it did (char *) */
  GSocketListener *listener;
  GIOStream *connection; /* a debugger that dialed in, or that t-code connected to: closed at once at the end */
  char *socket_path;
  GCancellable *cancel; /* the accept and the wait for the process */
  gint64 seq;           /* the last request's number */
  GHashTable *pending;  /* seq -> Pending */
  JsonNode *capabilities;
  DapEvent on_event;
  DapHandle on_request;
  gpointer owner;
};

typedef struct {
  gint64 seq;
  DapReply reply;
  gpointer data;
  GDestroyNotify destroy;
} Pending;

/* An answer that will not come (dap_free): its data goes. */
static void drop_pending(gpointer data)
{
  Pending *p = data;

  if (p->destroy)
    p->destroy(p->data);
  g_free(p);
}

/* One message to the debugger, now or once it dialed in. */
static void send_message(Dap *d, const char *body)
{
  if (d->ch)
    channel_send(d->ch, body);
  else
    g_ptr_array_add(d->queued, g_strdup(body));
}

/* The debugger asked something itself (runInTerminal): the owner answers it, or it is refused. */
static void answer(Dap *d, JsonNode *message)
{
  const char *name = json_string_at(message, "command");
  char *command = json_quote(name ? name : ""), *error = NULL;
  char *result = d->on_request && name ? d->on_request(d->owner, name, json_at(message, "arguments"), &error) : NULL;
  char *body, *why = json_quote(error ? error : "not supported");

  if (result)
    body = g_strdup_printf("{\"seq\":%" G_GINT64_FORMAT ",\"type\":\"response\",\"request_seq\":%" G_GINT64_FORMAT
                           ",\"command\":%s,\"success\":true,\"body\":%s}",
                           ++d->seq, json_int_at(message, "seq", 0), command, result);
  else
    body = g_strdup_printf("{\"seq\":%" G_GINT64_FORMAT ",\"type\":\"response\",\"request_seq\":%" G_GINT64_FORMAT
                           ",\"command\":%s,\"success\":false,\"message\":%s}",
                           ++d->seq, json_int_at(message, "seq", 0), command, why);
  send_message(d, body);
  g_free(body);
  g_free(why);
  g_free(result);
  g_free(error);
  g_free(command);
}

/* A message from the debugger: an answer, an event, or a request of its own. Each callback is the
 * last thing done: it may free d. */
static void on_message(gpointer owner, const char *text)
{
  Dap *d = owner;
  JsonParser *parser = json_parser_new_immutable();
  JsonNode *message;
  const char *type;

  if (!json_parser_load_from_data(parser, text, -1, NULL) || !(message = json_parser_get_root(parser))) {
    g_object_unref(parser);
    return;
  }
  type = json_string_at(message, "type");
  if (g_strcmp0(type, "response") == 0) {
    gint64 seq = json_int_at(message, "request_seq", -1);
    Pending *p = g_hash_table_lookup(d->pending, &seq);
    if (p) {
      Pending done = *p;
      JsonNode *success = json_at(message, "success");
      gboolean ok = success && JSON_NODE_HOLDS_VALUE(success) && json_node_get_boolean(success);
      /* the details, when there are any: "Failed to launch: <why>" (Delve, debugpy) */
      const char *short_error = json_string_at(message, "message"),
                 *details = json_string_at(message, "body.error.format");
      char *error = details && g_strcmp0(details, short_error) != 0
                        ? g_strdup_printf("%s%s%s", short_error ? short_error : "", short_error ? ": " : "", details)
                        : g_strdup(short_error);
      g_hash_table_steal(d->pending, &seq);
      g_free(p);
      if (ok && g_strcmp0(json_string_at(message, "command"), "initialize") == 0 && json_at(message, "body"))
        d->capabilities = json_node_copy(json_at(message, "body"));
      if (done.reply) /* it may free d: done.data is ours alone now */
        done.reply(done.data, ok ? json_at(message, "body") : NULL, ok ? NULL : (error ? error : "failed"));
      if (done.destroy)
        done.destroy(done.data);
      g_free(error);
    }
  } else if (g_strcmp0(type, "event") == 0 && json_string_at(message, "event")) {
    d->on_event(d->owner, json_string_at(message, "event"), json_at(message, "body"));
  } else if (g_strcmp0(type, "request") == 0) {
    answer(d, message);
  }
  g_object_unref(parser);
}

/* The debugger exited (or its pipes, or its socket, broke). */
static void on_closed(gpointer owner)
{
  Dap *d = owner;

  d->on_event(d->owner, NULL, NULL);
}

void dap_request(Dap *d, const char *command, const char *arguments, DapReply reply, gpointer data)
{
  dap_request_full(d, command, arguments, reply, data, NULL);
}

void dap_request_full(Dap *d, const char *command, const char *arguments, DapReply reply, gpointer data,
                      GDestroyNotify destroy)
{
  Pending *p = g_new(Pending, 1);
  char *quoted = json_quote(command);
  char *body = g_strdup_printf("{\"seq\":%" G_GINT64_FORMAT ",\"type\":\"request\",\"command\":%s,\"arguments\":%s}",
                               ++d->seq, quoted, arguments ? arguments : "{}");

  *p = (Pending){ d->seq, reply, data, destroy };
  g_hash_table_insert(d->pending, &p->seq, p);
  send_message(d, body);
  g_free(body);
  g_free(quoted);
}

JsonNode *dap_capabilities(Dap *d)
{
  return d ? d->capabilities : NULL;
}

/* A channel over in and out (of keep), reading. */
static void connect_channel(Dap *d, GInputStream *in, GOutputStream *out, GObject *keep)
{
  d->ch = channel_new(in, out, keep, d);
  d->ch->message = on_message;
  d->ch->on_closed = on_closed;
  channel_read(d->ch);
  for (guint i = 0; i < d->queued->len; i++)
    channel_send(d->ch, g_ptr_array_index(d->queued, i));
  g_ptr_array_set_size(d->queued, 0);
}

/* The debugger dialed in. */
static void on_accepted(GObject *source, GAsyncResult *result, gpointer data)
{
  GError *error = NULL;
  GSocketConnection *connection = g_socket_listener_accept_finish(G_SOCKET_LISTENER(source), result, NULL, &error);
  Dap *d;

  if (!connection) { /* dap_free cancelled it, or the socket broke (the process's end tells) */
    g_error_free(error);
    return;
  }
  d = data;
  d->connection = G_IO_STREAM(connection);
  connect_channel(d, g_io_stream_get_input_stream(G_IO_STREAM(connection)),
                  g_io_stream_get_output_stream(G_IO_STREAM(connection)), G_OBJECT(connection));
}

/* A debugger that dials in ended before it did. */
static void on_process_gone(GObject *source, GAsyncResult *result, gpointer data)
{
  if (!g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL)) /* dap_free */
    return;
  Dap *d = data;
  if (!d->ch)
    d->on_event(d->owner, NULL, NULL);
}

/* Listens on a socket of its own for a debugger that dials in; its path goes to d->socket_path. */
static gboolean listen_socket(Dap *d, GError **error)
{
  static int count;
  char *name = g_strdup_printf("t-code-dap-%d-%d.sock", (int)getpid(), ++count);
  GSocketAddress *address;
  gboolean ok;

  d->socket_path = g_build_filename(g_get_user_runtime_dir(), name, NULL);
  g_free(name);
  g_unlink(d->socket_path);
  address = g_unix_socket_address_new(d->socket_path);
  d->listener = g_socket_listener_new();
  ok = g_socket_listener_add_address(d->listener, address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL,
                                     error);
  g_object_unref(address);
  if (ok)
    g_socket_listener_accept_async(d->listener, d->cancel, on_accepted, d);
  return ok;
}

/* The connection to a debugger that listens is made (or failed: it went away). */
static void on_connected(GObject *source, GAsyncResult *result, gpointer data)
{
  GError *error = NULL;
  GSocketConnection *connection = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(source), result, &error);
  gboolean cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  Dap *d;

  g_clear_error(&error);
  if (cancelled) /* dap_free */
    return;
  d = data;
  if (!connection) {
    d->on_event(d->owner, NULL, NULL);
    return;
  }
  /* the client's timeout was for connecting: a program paused at a breakpoint sends nothing for long */
  g_socket_set_timeout(g_socket_connection_get_socket(connection), 0);
  d->connection = G_IO_STREAM(connection);
  connect_channel(d, g_io_stream_get_input_stream(G_IO_STREAM(connection)),
                  g_io_stream_get_output_stream(G_IO_STREAM(connection)), G_OBJECT(connection));
}

/* "initialize", the first request: the client and what it can do. */
static void send_initialize(Dap *d, const char *adapter_id)
{
  char *id = json_quote(adapter_id ? adapter_id : "gdb");
  char *initialize = g_strdup_printf("{\"clientID\":\"t-code\",\"clientName\":\"t-code\",\"adapterID\":%s,"
                                     "\"linesStartAt1\":true,\"columnsStartAt1\":true,\"pathFormat\":\"path\","
                                     "\"supportsVariableType\":true,\"supportsMemoryReferences\":true%s}",
                                     id, d->on_request ? ",\"supportsRunInTerminalRequest\":true" : "");

  dap_request(d, "initialize", initialize, NULL, NULL);
  g_free(initialize);
  g_free(id);
}

static Dap *dap_new(DapEvent on_event, DapHandle on_request, gpointer owner)
{
  Dap *d = g_new0(Dap, 1);

  d->pending = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, drop_pending);
  d->queued = g_ptr_array_new_with_free_func(g_free);
  d->cancel = g_cancellable_new();
  d->on_event = on_event;
  d->on_request = on_request;
  d->owner = owner;
  return d;
}

Dap *dap_connect(const char *host, int port, const char *adapter_id, DapEvent on_event, DapHandle on_request,
                 gpointer owner)
{
  Dap *d = dap_new(on_event, on_request, owner);
  GSocketClient *client = g_socket_client_new();

  g_socket_client_set_timeout(client, 10);
  g_socket_client_connect_to_host_async(client, host, (guint16)port, d->cancel, on_connected, d);
  g_object_unref(client); /* the connecting holds it */
  send_initialize(d, adapter_id);
  return d;
}

/* Child setup: the debugger in a session of its own, whose controlling terminal tty is. */
static void take_terminal(gpointer tty)
{
  int fd;

  setsid();
  if ((fd = open(tty, O_RDWR)) >= 0) {
    ioctl(fd, TIOCSCTTY, 0);
    close(fd);
  }
}

Dap *dap_start(const char *const *argv, const char *cwd, char **env, const char *tty, const char *adapter_id,
               DapEvent on_event, DapHandle on_request, gpointer owner, GError **error)
{
  gboolean dials = FALSE;
  GSubprocessLauncher *launcher;
  GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
  Dap *d = dap_new(on_event, on_request, owner);

  for (int i = 0; argv[i]; i++)
    dials |= strstr(argv[i], SOCKET_ARG) != NULL;
  if (dials && !listen_socket(d, error)) {
    dap_free(d);
    g_ptr_array_free(args, TRUE);
    return NULL;
  }
  for (int i = 0; argv[i]; i++) {
    const char *at = strstr(argv[i], SOCKET_ARG);
    g_ptr_array_add(args, at ? g_strdup_printf("%.*sunix:%s%s", (int)(at - argv[i]), argv[i], d->socket_path,
                                               at + strlen(SOCKET_ARG))
                             : g_strdup(argv[i]));
  }
  g_ptr_array_add(args, NULL);
  launcher = g_subprocess_launcher_new(dials ? G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE
                                             : G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                   G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_set_cwd(launcher, cwd);
  if (env)
    g_subprocess_launcher_set_environ(launcher, env);
  /* Its stdin is that terminal too (Delve hands its own fd 0 to its programs' foreground), else not
   * t-code's, if it has one */
  if (dials)
    g_subprocess_launcher_set_stdin_file_path(launcher, tty ? tty : "/dev/null");
  if (tty) /* the string lives until the spawn returned */
    g_subprocess_launcher_set_child_setup(launcher, take_terminal, (gpointer)tty, NULL);
  d->process = g_subprocess_launcher_spawnv(launcher, (const char *const *)args->pdata, error);
  g_object_unref(launcher);
  g_ptr_array_free(args, TRUE);
  if (!d->process) {
    dap_free(d);
    return NULL;
  }
  if (dials)
    g_subprocess_wait_async(d->process, d->cancel, on_process_gone, d);
  else
    connect_channel(d, g_subprocess_get_stdout_pipe(d->process), g_subprocess_get_stdin_pipe(d->process),
                    G_OBJECT(d->process));
  send_initialize(d, adapter_id);
  return d;
}

void dap_free(Dap *d)
{
  if (!d)
    return;
  g_cancellable_cancel(d->cancel);
  if (d->process) {
    g_subprocess_force_exit(d->process); /* the program goes with it: gdb has it killed when it dies */
    g_object_unref(d->process);
  }
  if (d->ch)
    channel_close(d->ch); /* holds the process, or the connection */
  if (d->connection) {    /* the other side hears of the end now, not once the channel's read is done */
    g_io_stream_close(d->connection, NULL, NULL);
    g_object_unref(d->connection);
  }
  if (d->listener) {
    g_socket_listener_close(d->listener);
    g_object_unref(d->listener);
  }
  if (d->socket_path)
    g_unlink(d->socket_path);
  g_free(d->socket_path);
  g_object_unref(d->cancel);
  g_ptr_array_free(d->queued, TRUE);
  g_hash_table_destroy(d->pending);
  if (d->capabilities)
    json_node_unref(d->capabilities);
  g_free(d);
}

int dap_pid(Dap *d)
{
  const char *id = d && d->process ? g_subprocess_get_identifier(d->process) : NULL;

  return id ? atoi(id) : 0;
}
