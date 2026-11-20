/*
 * The Debug Adapter Protocol client (see dap.h). Requests are numbered (seq); an answer names the
 * request it answers (request_seq) and goes to that request's reply.
 */
#include "debug/dap.h"

#include "lsp/channel.h"
#include "lsp/json.h"

#include <stdlib.h>

struct Dap {
  GSubprocess *process;
  Channel *ch;
  gint64 seq;          /* the last request's number */
  GHashTable *pending; /* seq -> Pending */
  JsonNode *capabilities;
  DapEvent on_event;
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

/* The debugger asked something itself (runInTerminal, startDebugging): none of it is done here. */
static void refuse(Dap *d, JsonNode *message)
{
  char *command = json_quote(json_string_at(message, "command") ? json_string_at(message, "command") : "");
  char *body = g_strdup_printf("{\"seq\":%" G_GINT64_FORMAT ",\"type\":\"response\",\"request_seq\":%" G_GINT64_FORMAT
                               ",\"command\":%s,\"success\":false,\"message\":\"not supported\"}",
                               ++d->seq, json_int_at(message, "seq", 0), command);

  channel_send(d->ch, body);
  g_free(body);
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
      const char *error = json_string_at(message, "message");
      g_hash_table_steal(d->pending, &seq);
      g_free(p);
      if (ok && g_strcmp0(json_string_at(message, "command"), "initialize") == 0 && json_at(message, "body"))
        d->capabilities = json_node_copy(json_at(message, "body"));
      if (done.reply) /* it may free d: done.data is ours alone now */
        done.reply(done.data, ok ? json_at(message, "body") : NULL, ok ? NULL : (error ? error : "failed"));
      if (done.destroy)
        done.destroy(done.data);
    }
  } else if (g_strcmp0(type, "event") == 0 && json_string_at(message, "event")) {
    d->on_event(d->owner, json_string_at(message, "event"), json_at(message, "body"));
  } else if (g_strcmp0(type, "request") == 0) {
    refuse(d, message);
  }
  g_object_unref(parser);
}

/* gdb exited (or its pipes broke). */
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
  channel_send(d->ch, body);
  g_free(body);
  g_free(quoted);
}

JsonNode *dap_capabilities(Dap *d)
{
  return d ? d->capabilities : NULL;
}

Dap *dap_start(const char *const *argv, const char *cwd, char **env, DapEvent on_event, gpointer owner, GError **error)
{
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  GSubprocess *process;
  Dap *d;

  g_subprocess_launcher_set_cwd(launcher, cwd);
  if (env)
    g_subprocess_launcher_set_environ(launcher, env);
  process = g_subprocess_launcher_spawnv(launcher, argv, error);
  g_object_unref(launcher);
  if (!process)
    return NULL;
  d = g_new0(Dap, 1);
  d->process = process;
  d->pending = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, drop_pending);
  d->on_event = on_event;
  d->owner = owner;
  d->ch =
      channel_new(g_subprocess_get_stdout_pipe(process), g_subprocess_get_stdin_pipe(process), G_OBJECT(process), d);
  d->ch->message = on_message;
  d->ch->on_closed = on_closed;
  channel_read(d->ch);
  dap_request(d, "initialize",
              "{\"clientID\":\"t-code\",\"clientName\":\"t-code\",\"adapterID\":\"gdb\",\"linesStartAt1\":true,"
              "\"columnsStartAt1\":true,\"pathFormat\":\"path\",\"supportsVariableType\":true}",
              NULL, NULL);
  return d;
}

void dap_free(Dap *d)
{
  if (!d)
    return;
  g_subprocess_force_exit(d->process); /* the program goes with it: gdb has it killed when it dies */
  channel_close(d->ch);                /* holds the process */
  g_object_unref(d->process);
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
