/*
 * A language server's life (see state.h): started on first use in the project folder, initialized,
 * its messages answered (requests, progress, diagnostics), its compile database watched (clangd),
 * restarted after a crash, stopped when idle.
 */
#include "lsp/client/state.h"

#include "editor/editorview.h"
#include "lsp/inline/diagnostics.h"
#include "lsp/inline/inlay.h"
#include "lsp/inline/semantic.h"
#include "lsp/symbols/navigate.h"
#include "window/statusbar.h"

#include <string.h>
#include <unistd.h>

static void progress_free(Progress *p)
{
  g_free(p->title);
  g_free(p->text);
  g_free(p);
}

/* The server ------------------------------------------------------------------------------- */

/* Gets or creates the language server for this tab's language and project. */
Lsp *lsp_of(Tab *tab)
{
  App *t = tab->app;
  ServerKind kind = server_kind_for_tab(tab);
  Lsp *l;

  if (kind == SERVER_NONE)
    return NULL;
  if (!t->lsps)
    t->lsps = g_ptr_array_new();
  for (guint i = 0; i < t->lsps->len; i++) {
    l = g_ptr_array_index(t->lsps, i);
    if (l->kind == kind && g_strcmp0(l->workspace, t->project.root) == 0) {
      if (kind == SERVER_CLANGD)
        t->lsp = l;
      return l;
    }
  }
  l = g_new0(Lsp, 1);
  g_atomic_ref_count_init(&l->refs);
  l->app = t;
  l->kind = kind;
  l->workspace = g_strdup(t->project.root);
  l->pending = g_hash_table_new(g_int64_hash, g_int64_equal);
  l->docs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)doc_free);
  l->progress = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)progress_free);
  g_ptr_array_add(t->lsps, l);
  if (kind == SERVER_CLANGD)
    t->lsp = l;
  return l;
}

/* The compile database changed: the server hears of it, and the open files go again, so that it
 * reads their flags anew (a "No compile flags" hint goes once make wrote the database). */
static gboolean database_written(gpointer data)
{
  Lsp *l = data;
  GHashTableIter iter;
  gpointer doc;

  l->written_id = 0;
  if (!l->ready || !l->written->len)
    return G_SOURCE_REMOVE;
  char *params = g_strdup_printf("{\"changes\":[%s]}", l->written->str);
  lsp_notify(l, "workspace/didChangeWatchedFiles", params);
  g_free(params);
  g_string_truncate(l->written, 0);
  g_hash_table_iter_init(&iter, l->docs);
  while (g_hash_table_iter_next(&iter, NULL, &doc))
    if (((Doc *)doc)->opened) {
      doc_close_on_server(doc);
      doc_send(doc);
    }
  return G_SOURCE_REMOVE;
}

static void on_folder_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file, GFile *other, GFileMonitorEvent event,
                              Lsp *l)
{
  if (event == G_FILE_MONITOR_EVENT_RENAMED || event == G_FILE_MONITOR_EVENT_MOVED_IN)
    file = other ? other : file; /* an atomic save: written to a temporary name, then renamed */
  if (!file || (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && event != G_FILE_MONITOR_EVENT_CREATED &&
                event != G_FILE_MONITOR_EVENT_DELETED && event != G_FILE_MONITOR_EVENT_RENAMED &&
                event != G_FILE_MONITOR_EVENT_MOVED_IN))
    return;
  char *name = g_file_get_basename(file);
  gboolean database =
      !g_strcmp0(name, "compile_commands.json") || !g_strcmp0(name, "compile_flags.txt") || !g_strcmp0(name, ".clangd");
  g_free(name);
  if (!database)
    return;
  char *uri = g_file_get_uri(file), *quoted = json_quote(uri);
  int type = event == G_FILE_MONITOR_EVENT_CREATED ? 1 : event == G_FILE_MONITOR_EVENT_DELETED ? 3 : 2;
  g_string_append_printf(l->written, "%s{\"uri\":%s,\"type\":%d}", l->written->len ? "," : "", quoted, type);
  g_free(quoted);
  g_free(uri);
  g_clear_handle_id(&l->written_id, g_source_remove);
  l->written_id = g_timeout_add(DATABASE_MS, database_written, l);
}

static void watch_databases(Lsp *l)
{
  for (int i = 0; i < 2; i++) {
    char *path = i ? g_build_filename(l->workspace, "build", NULL) : g_strdup(l->workspace);
    GFile *folder = g_file_new_for_path(path);
    l->watch[i] = g_file_monitor_directory(folder, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (l->watch[i])
      g_signal_connect(l->watch[i], "changed", G_CALLBACK(on_folder_changed), l);
    g_object_unref(folder);
    g_free(path);
  }
}

/* Stops the server: stdin closes, and it exits. Requests get NULL; the files are sent again to the
 * next one. */
void lsp_server_stop(Lsp *l)
{
  GHashTableIter iter;
  gpointer doc;

  for (int i = 0; i < 2; i++)
    if (l->watch[i]) {
      g_file_monitor_cancel(l->watch[i]);
      g_signal_handlers_disconnect_by_data(l->watch[i], l);
      g_clear_object(&l->watch[i]);
    }
  g_clear_handle_id(&l->written_id, g_source_remove);
  if (l->written)
    g_string_truncate(l->written, 0);

  if (l->ch) {
    channel_close(l->ch);
    l->ch = NULL;
  }
  g_clear_object(&l->process);
  l->ready = FALSE;
  g_hash_table_remove_all(l->progress); /* its work ends with it */
  g_clear_pointer(&l->progress_last, g_free);
  status_progress_changed(l->app);
  g_clear_pointer(&l->capabilities, json_node_unref);
  lsp_fail_pending(l, NULL);
  g_hash_table_iter_init(&iter, l->docs);
  while (g_hash_table_iter_next(&iter, NULL, &doc)) {
    ((Doc *)doc)->opened = FALSE;
    g_string_truncate(((Doc *)doc)->changes, 0);
  }
}

static gboolean restart(gpointer data)
{
  Lsp *l = data;

  l->restart_id = 0;
  if (!l->ch && g_hash_table_size(l->docs) > 0)
    lsp_server_start(l);
  return G_SOURCE_REMOVE;
}

/* The server went by itself (it crashed): started again for the open files, a few times. */
static void on_server_closed(gpointer owner)
{
  Lsp *l = owner;

  lsp_server_stop(l);
  if (++l->crashes <= MAX_CRASHES && !l->restart_id)
    l->restart_id = g_timeout_add_seconds(1, restart, l);
}

/* initialize answered: the encoding it chose, then the most recently used open files. */
static void on_initialized(JsonNode *result, gpointer data)
{
  Lsp *l = data;
  const char *encoding;
  GHashTableIter iter;
  gpointer doc;

  if (!result || !l->ch)
    return;
  if (json_at(result, "capabilities"))
    l->capabilities = json_node_copy(json_at(result, "capabilities"));
  encoding = json_string_at(result, "capabilities.positionEncoding");
  if (!encoding)
    encoding = json_string_at(result, "offsetEncoding"); /* clangd's own name for it */
  l->utf16 = g_strcmp0(encoding, "utf-8") != 0;
  l->ready = TRUE;
  status_progress_changed(l->app); /* no longer "starting" */
  lsp_notify(l, "initialized", "{}");
  char *settings = server_settings(l->kind, l->app);
  if (settings) {
    char *params = g_strdup_printf("{\"settings\":%s}", settings);
    lsp_notify(l, "workspace/didChangeConfiguration", params);
    g_free(params);
    g_free(settings);
  }
  g_hash_table_iter_init(&iter, l->docs);
  while (g_hash_table_iter_next(&iter, NULL, &doc)) {
    GHashTableIter other_iter;
    gpointer other;
    int newer = 0;
    g_hash_table_iter_init(&other_iter, l->docs);
    while (g_hash_table_iter_next(&other_iter, NULL, &other))
      if (((Doc *)other)->use > ((Doc *)doc)->use)
        newer++;
    if (newer < MAX_OPEN_DOCS)
      doc_send(doc);
  }
}

/* The answer the server gets to one of its own requests. */
static void answer_server(Lsp *l, JsonNode *message)
{
  JsonNode *id = json_at(message, "id"), *items = json_at(message, "params.items");
  const char *method = json_string_at(message, "method");
  char *id_text = json_to_string(id, FALSE);
  GString *result = g_string_new("null");

  if (g_strcmp0(method, "workspace/applyEdit") == 0) { /* a quick fix run as a command */
    navigate_apply_edit(l->app, json_at(message, "params.edit"), FALSE, l->utf16);
    g_string_assign(result, "{\"applied\":true}");
  } else if (g_strcmp0(method, "workspace/inlayHint/refresh") == 0) { /* its hints changed (a build ended) */
    GHashTableIter iter;
    gpointer tab;
    g_hash_table_iter_init(&iter, l->docs);
    while (g_hash_table_iter_next(&iter, &tab, NULL))
      inlay_update(tab);
  } else if (g_strcmp0(method, "window/showDocument") == 0) { /* gopls' "Browse documentation" */
    g_string_printf(result, "{\"success\":%s}",
                    navigate_show_document(l->app, json_at(message, "params")) ? "true" : "false");
  } else if (g_strcmp0(method, "workspace/configuration") == 0 && items && JSON_NODE_HOLDS_ARRAY(items)) {
    char *text = server_settings(l->kind, l->app);
    JsonNode *settings = text ? json_from_string(text, NULL) : NULL;
    g_string_assign(result, "[");
    for (guint i = 0; i < json_array_get_length(json_node_get_array(items)); i++) {
      const char *section = json_string_at(json_array_get_element(json_node_get_array(items), i), "section");
      JsonNode *value = !settings ? NULL : section ? json_at(settings, section) : settings; /* "yaml", "json.format" */
      char *answer = value ? json_to_string(value, FALSE) : g_strdup("null");
      g_string_append_printf(result, "%s%s", i ? "," : "", answer);
      g_free(answer);
    }
    g_string_append(result, "]");
    if (settings)
      json_node_unref(settings);
    g_free(text);
  }
  char *body = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id_text, result->str);
  channel_send(l->ch, body);
  g_free(body);
  g_string_free(result, TRUE);
  g_free(id_text);
}

/* $/progress: work the server reports ("begin" with a title, "report"s, "end"), like loading the
 * project or indexing; kept by token for the status bar. */
static void progress(Lsp *l, JsonNode *message)
{
  JsonNode *token = json_at(message, "params.token");
  const char *kind = json_string_at(message, "params.value.kind");
  char *key = token ? json_to_string(token, FALSE) : NULL;

  if (!key || !kind) {
    g_free(key);
    return;
  }
  if (strcmp(kind, "end") == 0) {
    g_hash_table_remove(l->progress, key);
    if (g_strcmp0(key, l->progress_last) == 0)
      g_clear_pointer(&l->progress_last, g_free);
    g_free(key);
  } else {
    const char *detail = json_string_at(message, "params.value.message");
    JsonNode *percentage = json_at(message, "params.value.percentage");
    Progress *p = g_hash_table_lookup(l->progress, key);
    if (!p) {
      p = g_new0(Progress, 1);
      p->title = g_strdup(json_string_at(message, "params.value.title"));
      g_hash_table_insert(l->progress, g_strdup(key), p);
    }
    GString *text = g_string_new(p->title ? p->title : "Working");
    if (percentage && JSON_NODE_HOLDS_VALUE(percentage))
      g_string_append_printf(text, " %d%%", (int)json_node_get_int(percentage));
    if (detail && *detail)
      g_string_append_printf(text, " (%s)", detail);
    g_free(p->text);
    p->text = g_string_free(text, FALSE);
    g_free(l->progress_last);
    l->progress_last = key;
  }
  status_progress_changed(l->app);
}

/* The editor whose file uri is, or NULL. */
static Tab *tab_of_uri(Lsp *l, const char *uri)
{
  char *path = uri ? lsp_uri_to_path(uri) : NULL;
  GHashTableIter iter;
  gpointer tab;
  Tab *found = NULL;

  g_hash_table_iter_init(&iter, l->docs);
  while (path && !found && g_hash_table_iter_next(&iter, &tab, NULL))
    if (g_strcmp0(((Tab *)tab)->path, path) == 0)
      found = tab;
  g_free(path);
  return found;
}

/* Whether diagnostics has an #include not found (clangd's code "pp_file_not_found"). */
static gboolean include_not_found(JsonNode *diagnostics)
{
  JsonArray *array = diagnostics && JSON_NODE_HOLDS_ARRAY(diagnostics) ? json_node_get_array(diagnostics) : NULL;

  for (guint i = 0; array && i < json_array_get_length(array); i++)
    if (g_strcmp0(json_string_at(json_array_get_element(array, i), "code"), "pp_file_not_found") == 0)
      return TRUE;
  return FALSE;
}

/* A message from the server: an answer, one of its own requests, or a notification. */
static void on_message(gpointer owner, const char *body)
{
  Lsp *l = owner;
  JsonParser *parser = json_parser_new_immutable();
  JsonNode *message, *id;
  const char *method;

  if (!json_parser_load_from_data(parser, body, -1, NULL) || !(message = json_parser_get_root(parser))) {
    g_object_unref(parser);
    return;
  }
  method = json_string_at(message, "method");
  id = json_at(message, "id");
  if (method && id) {
    answer_server(l, message);
  } else if (g_strcmp0(method, "textDocument/publishDiagnostics") == 0) {
    Tab *tab = tab_of_uri(l, json_string_at(message, "params.uri"));
    if (tab) {
      doc_of(tab)->include_missing = include_not_found(json_at(message, "params.diagnostics"));
      diagnostics_show(tab, json_at(message, "params.diagnostics")); /* updates the status bar */
    }
  } else if (g_strcmp0(method, "$/progress") == 0) {
    progress(l, message);
  } else if (g_strcmp0(method, "textDocument/inactiveRegions") == 0) { /* clangd's: #if branches left out */
    Tab *tab = tab_of_uri(l, json_string_at(message, "params.textDocument.uri"));
    if (tab)
      semantic_inactive(tab, json_at(message, "params.regions"));
  } else if (!method && id && JSON_NODE_HOLDS_VALUE(id)) {
    gint64 number = json_node_get_int(id);
    Pending *p = g_hash_table_lookup(l->pending, &number);
    if (p)
      lsp_finish(p, p->cancelled ? NULL : json_at(message, "result"));
  }
  g_object_unref(parser);
}

/* "GLIBC_TUNABLES=glibc.malloc.arena_max=2" for the server: clangd's threads each grow a malloc
 * arena, and with 25 files it held 1.5 GB for 130 MB of its own data; two arenas kept it at about
 * 700 MB, as fast. */
static void set_malloc_arenas(GSubprocessLauncher *launcher)
{
  const char *tunables = g_getenv("GLIBC_TUNABLES");
  char *value;

  if (tunables && strstr(tunables, "glibc.malloc.arena_max"))
    return;
  value = tunables && *tunables ? g_strconcat(tunables, ":glibc.malloc.arena_max=2", NULL)
                                : g_strdup("glibc.malloc.arena_max=2");
  g_subprocess_launcher_setenv(launcher, "GLIBC_TUNABLES", value, TRUE);
  g_free(value);
}

/* Starts the server in the project folder and sends initialize; the files follow its answer. */
void lsp_server_start(Lsp *l)
{
  char **argv = server_argv(l->app, l->kind);
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  GError *error = NULL;
  char *uri, *quoted, *params, *options;

  g_subprocess_launcher_set_cwd(launcher, l->workspace);
  if (l->kind == SERVER_CLANGD)
    set_malloc_arenas(launcher);
  l->process = g_subprocess_launcher_spawnv(launcher, (const char *const *)argv, &error);
  g_object_unref(launcher);
  g_strfreev(argv);
  if (!l->process) {
    g_warning("t-code: cannot start %s: %s", server_name(l->kind), error->message);
    l->missing = TRUE;
    g_clear_error(&error);
    return;
  }
  if (l->kind == SERVER_CLANGD) {
    if (!l->written)
      l->written = g_string_new(NULL);
    watch_databases(l);
  }
  l->ch = channel_new(g_subprocess_get_stdout_pipe(l->process), g_subprocess_get_stdin_pipe(l->process),
                      G_OBJECT(l->process), l);
  l->ch->message = on_message;
  status_progress_changed(l->app); /* "starting" */
  l->ch->on_closed = on_server_closed;
  channel_read(l->ch);

  options = server_options(l->kind, l->app);
  uri = lsp_path_to_uri(l->workspace);
  quoted = json_quote(uri ? uri : "");
  params = g_strdup_printf(
      "{\"processId\":%d,\"rootUri\":%s,\"workspaceFolders\":[{\"uri\":%s,\"name\":\"root\"}],"
      "\"capabilities\":{"
      "\"general\":{\"positionEncodings\":[\"utf-8\",\"utf-16\"]},"
      "\"window\":{\"workDoneProgress\":true,\"showDocument\":{\"support\":true}},"
      "\"workspace\":{\"inlayHint\":{\"refreshSupport\":true}},"
      "\"offsetEncoding\":[\"utf-8\",\"utf-16\"],"
      "\"textDocument\":{"
      "\"synchronization\":{\"didSave\":true},"
      "\"publishDiagnostics\":{\"relatedInformation\":false},"
      "\"inactiveRegionsCapabilities\":{\"inactiveRegions\":true},"
      "\"completion\":{\"contextSupport\":true,\"completionItem\":{\"snippetSupport\":true,"
      "\"documentationFormat\":[\"plaintext\"],\"labelDetailsSupport\":true}},"
      "\"hover\":{\"contentFormat\":[\"plaintext\"]},"
      "\"signatureHelp\":{\"signatureInformation\":{\"documentationFormat\":[\"plaintext\"],"
      "\"parameterInformation\":{\"labelOffsetSupport\":true},\"activeParameterSupport\":true}},"
      "\"codeAction\":{\"isPreferredSupport\":true,\"codeActionLiteralSupport\":{\"codeActionKind\":"
      "{\"valueSet\":[\"quickfix\",\"refactor\",\"refactor.extract\",\"refactor.inline\",\"refactor.rewrite\","
      "\"source\",\"source.organizeImports\"]}}},"
      "\"definition\":{},\"declaration\":{},\"references\":{},\"formatting\":{},\"rename\":{},\"inlayHint\":{},"
      "\"callHierarchy\":{},\"typeHierarchy\":{},"
      "\"semanticTokens\":{\"requests\":{\"full\":{\"delta\":true}},\"formats\":[\"relative\"],"
      "\"tokenTypes\":[\"namespace\",\"type\",\"class\",\"enum\",\"interface\",\"struct\",\"typeParameter\","
      "\"parameter\",\"variable\",\"property\",\"enumMember\",\"function\",\"method\",\"macro\",\"concept\"],"
      "\"tokenModifiers\":[\"declaration\",\"definition\",\"readonly\",\"static\",\"deprecated\"]}}}}",
      getpid(), quoted, quoted);
  if (options) {
    params[strlen(params) - 1] = '\0'; /* its closing brace */
    char *with = g_strdup_printf("%s,\"initializationOptions\":%s}", params, options);
    g_free(params);
    params = with;
  }
  lsp_send(l, NULL, "initialize", params, NULL, on_initialized, l);
  g_free(params);
  g_free(options);
  g_free(quoted);
  g_free(uri);
}

/* IDLE_SECONDS without editors: the server stops. */
gboolean lsp_idle_stop(gpointer data)
{
  Lsp *l = data;

  l->idle_id = 0;
  if (g_hash_table_size(l->docs) == 0)
    lsp_server_stop(l);
  return G_SOURCE_REMOVE;
}
