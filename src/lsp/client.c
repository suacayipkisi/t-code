/*
 * Per-language LSP clients (see client.h; the servers themselves are in servers.c). Each starts on
 * first use, keeps a small LRU of open documents and stops IDLE_SECONDS after its last editor closes;
 * a crash restarts that server up to MAX_CRASHES times.
 */
#include "lsp/client.h"

#include "editor/editorview.h"
#include "editor/tabs.h"
#include "lsp/channel.h"
#include "lsp/compiledb.h"
#include "lsp/diagnostics.h"
#include "lsp/inlay.h"
#include "lsp/navigate.h"
#include "lsp/semantic.h"
#include "lsp/servers.h"
#include "lsp/symbols.h"
#include "window/statusbar.h"

#include <string.h>
#include <unistd.h>

#ifndef IDLE_SECONDS /* tests set a shorter one */
#define IDLE_SECONDS 30
#endif
#ifndef FLUSH_MS
#define FLUSH_MS 50 /* changes typed within this go in one didChange */
#endif
#ifndef PULL_MS
#define PULL_MS 300 /* a server that is asked for its diagnostics: this long after the last change */
#endif
#ifndef MAX_OPEN_DOCS
#define MAX_OPEN_DOCS 5 /* per-server LRU of open documents */
#endif
#define MAX_CRASHES 3

/* A compile database written (make, cmake) is told to the server this long after its last change. */
#define DATABASE_MS 1500

/* Where the system headers are looked for (tests use a folder of their own). */
static const char *sysroot = "/";

/* An open editor, as the server knows it. */
typedef struct {
  Tab *tab;
  Lsp *lsp;
  int version;
  guint64 use;      /* LRU order: bumped when opened, shown, edited or queried */
  gboolean opened;  /* didOpen went to the running server */
  GString *changes; /* contentChanges not sent yet, comma separated */
  guint flush_id;
  guint pull_id;            /* asks a server with pull diagnostics (textDocument/diagnostic) for them */
  gboolean include_missing; /* clangd's last diagnostics had an #include it cannot find */
} Doc;
static Doc *doc_of(Tab *tab);

/* A request waiting for its answer. */
typedef struct {
  Lsp *lsp;
  gint64 id;
  Tab *tab; /* NULL for initialize */
  LspReply reply;
  gpointer data;
  GCancellable *cancel;
  gulong cancelled_id;
  gboolean cancelled; /* its answer, if it comes first, is not given */
} Pending;

struct Lsp {
  gatomicrefcount refs;
  ServerKind kind;
  char *workspace; /* the project folder it runs in; fixed: a project switch stops every server (lsp_stop) */
  App *app;
  GSubprocess *process;
  Channel *ch;
  gboolean ready; /* initialize answered */
  gboolean utf16; /* positions count UTF-16 units, not UTF-8 bytes */
  gboolean stopping;
  JsonNode *capabilities;
  GHashTable *pending; /* id -> Pending */
  gint64 next_id;
  GHashTable *docs; /* Tab -> Doc */
  guint64 use;      /* next LRU stamp for docs */
  guint idle_id, restart_id;
  int crashes;
  gboolean missing;       /* the server is not installed: not tried again */
  GFileMonitor *watch[2]; /* the root and its build/ folder, for a compile database written */
  GString *written;       /* didChangeWatchedFiles changes not sent yet, comma separated */
  guint written_id;
  GHashTable *progress; /* $/progress token -> Progress, while that work runs */
  char *progress_last;  /* the token that spoke last */
};

/* Work a server reports with $/progress. */
typedef struct {
  char *title; /* the begin's: "Indexing" */
  char *text;  /* for the status bar: "Indexing 45% (12/40)" */
} Progress;

static void progress_free(Progress *p)
{
  g_free(p->title);
  g_free(p->text);
  g_free(p);
}

static void lsp_ref(Lsp *l)
{
  g_atomic_ref_count_inc(&l->refs);
}

static void lsp_unref(Lsp *l)
{
  if (g_atomic_ref_count_dec(&l->refs))
    g_free(l);
}

char *lsp_uri_to_path(const char *uri)
{
  return g_filename_from_uri(uri, NULL, NULL);
}

char *lsp_path_to_uri(const char *path)
{
  return g_filename_to_uri(path, NULL, NULL);
}

/* Positions ---------------------------------------------------------------------------------- */

/* The LSP position of iter: its line, and how far into it in the server's encoding. */
static void position_of(Lsp *l, const GtkTextIter *iter, int *line, int *character)
{
  GtkTextIter walk = *iter;

  *line = gtk_text_iter_get_line(iter);
  if (!l->utf16) {
    *character = gtk_text_iter_get_line_index(iter);
    return;
  }
  *character = 0;
  for (gtk_text_iter_set_line_offset(&walk, 0); gtk_text_iter_compare(&walk, iter) < 0;
       gtk_text_iter_forward_char(&walk))
    *character += gtk_text_iter_get_char(&walk) >= 0x10000 ? 2 : 1;
}

gboolean lsp_utf16(Tab *tab)
{
  GPtrArray *servers = tab->app->lsps;

  for (guint i = 0; servers && i < servers->len; i++) {
    Lsp *l = g_ptr_array_index(servers, i);
    Doc *doc = g_hash_table_lookup(l->docs, tab);
    if (doc)
      return l->ready ? l->utf16 : TRUE;
  }
  return TRUE; /* LSP's default position encoding is UTF-16. */
}

static void iter_at_position(Tab *tab, gint64 line, gint64 character, GtkTextIter *iter, gboolean utf16)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);

  if (line >= gtk_text_buffer_get_line_count(buffer)) {
    gtk_text_buffer_get_end_iter(buffer, iter);
    return;
  }
  gtk_text_buffer_get_iter_at_line(buffer, iter, (int)MAX(line, 0));
  /* Character by character, so a position inside a character (or past the line) cannot be. */
  for (gint64 at = 0; !gtk_text_iter_ends_line(iter);) {
    at += utf16 ? (gtk_text_iter_get_char(iter) >= 0x10000 ? 2 : 1)
                : g_unichar_to_utf8(gtk_text_iter_get_char(iter), NULL);
    if (at > character)
      break;
    gtk_text_iter_forward_char(iter);
  }
}

void lsp_iter_at(Tab *tab, JsonNode *position, GtkTextIter *iter)
{
  lsp_iter_at_encoding(tab, position, iter, lsp_utf16(tab));
}

void lsp_iter_at_encoding(Tab *tab, JsonNode *position, GtkTextIter *iter, gboolean utf16)
{
  iter_at_position(tab, json_int_at(position, "line", 0), json_int_at(position, "character", 0), iter, utf16);
}

void lsp_iter_at_position(Tab *tab, gint64 line, gint64 character, GtkTextIter *iter)
{
  iter_at_position(tab, line, character, iter, lsp_utf16(tab));
}

char *lsp_document_params(Tab *tab)
{
  char *uri = lsp_path_to_uri(tab->path), *quoted = json_quote(uri ? uri : "");
  char *params = g_strdup_printf("\"textDocument\":{\"uri\":%s}", quoted);

  g_free(quoted);
  g_free(uri);
  return params;
}

char *lsp_position_params(Tab *tab, const GtkTextIter *iter)
{
  char *document = lsp_document_params(tab), *params;
  Doc *doc = doc_of(tab);
  int line = gtk_text_iter_get_line(iter), character = gtk_text_iter_get_line_index(iter);

  if (doc)
    position_of(doc->lsp, iter, &line, &character);
  params = g_strdup_printf("%s,\"position\":{\"line\":%d,\"character\":%d}", document, line, character);
  g_free(document);
  return params;
}

/* Sending ------------------------------------------------------------------------------------ */

static void notify(Lsp *l, const char *method, const char *params)
{
  char *body = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params);

  if (l->ch)
    channel_send(l->ch, body);
  g_free(body);
}

/* Hands a request its answer and forgets it. */
static void finish(Pending *p, JsonNode *result)
{
  Lsp *l = p->lsp;

  g_hash_table_steal(l->pending, &p->id);
  if (p->cancelled_id)
    g_cancellable_disconnect(p->cancel, p->cancelled_id);
  if (p->reply)
    p->reply(result, p->data);
  g_clear_object(&p->cancel);
  g_free(p);
}

/* Every waiting request (of tab only, unless NULL) gets NULL. */
static void fail_pending(Lsp *l, Tab *tab)
{
  GList *all = g_hash_table_get_values(l->pending);

  for (GList *each = all; each; each = each->next) {
    Pending *p = each->data;
    if (!tab || p->tab == tab)
      finish(p, NULL);
  }
  g_list_free(all);
}

typedef struct {
  Lsp *lsp;
  gint64 id;
} Cancelled;

static void cancelled_free(gpointer data)
{
  Cancelled *c = data;

  lsp_unref(c->lsp);
  g_free(c);
}

/* After a request was cancelled (not inside the cancel signal, where disconnecting would hang). */
static gboolean cancel_now(gpointer data)
{
  Cancelled *c = data;
  Lsp *l = c->lsp;
  Pending *p = l->stopping ? NULL : g_hash_table_lookup(l->pending, &c->id);

  if (p) {
    char *params = g_strdup_printf("{\"id\":%" G_GINT64_FORMAT "}", c->id);
    notify(l, "$/cancelRequest", params);
    g_free(params);
    finish(p, NULL);
  }
  return G_SOURCE_REMOVE;
}

static void on_cancelled(GCancellable *cancel G_GNUC_UNUSED, Pending *p)
{
  Cancelled *c = g_new(Cancelled, 1);

  p->cancelled = TRUE;
  *c = (Cancelled){ p->lsp, p->id };
  lsp_ref(c->lsp);
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, cancel_now, c, cancelled_free);
}

static void send_request(Lsp *l, Tab *tab, const char *method, const char *params, GCancellable *cancel, LspReply reply,
                         gpointer data)
{
  Pending *p = g_new0(Pending, 1);
  char *body;

  p->lsp = l;
  p->id = l->next_id++;
  p->tab = tab;
  p->reply = reply;
  p->data = data;
  g_hash_table_insert(l->pending, &p->id, p);
  if (cancel) {
    p->cancel = g_object_ref(cancel);
    p->cancelled_id = g_cancellable_connect(cancel, G_CALLBACK(on_cancelled), p, NULL);
  }
  body = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"method\":\"%s\",\"params\":%s}", p->id,
                         method, params);
  channel_send(l->ch, body);
  g_free(body);
}

/* Documents ---------------------------------------------------------------------------------- */

static Doc *doc_of(Tab *tab)
{
  GPtrArray *servers = tab->app->lsps;

  for (guint i = 0; servers && i < servers->len; i++) {
    Doc *doc = g_hash_table_lookup(((Lsp *)g_ptr_array_index(servers, i))->docs, tab);
    if (doc)
      return doc;
  }
  return NULL;
}

static void on_pulled(JsonNode *result, gpointer data)
{
  Tab *tab = data; /* still there: a request about a closed editor gets NULL */

  if (result && g_strcmp0(json_string_at(result, "kind"), "full") == 0 && json_at(result, "items"))
    diagnostics_show(tab, json_at(result, "items"));
}

static gboolean pull(gpointer data)
{
  Doc *doc = data;
  char *document = lsp_document_params(doc->tab), *params = g_strdup_printf("{%s}", document);

  doc->pull_id = 0;
  lsp_request(doc->tab, "textDocument/diagnostic", params, NULL, on_pulled, doc->tab);
  g_free(params);
  g_free(document);
  return G_SOURCE_REMOVE;
}

/* Servers that do not send their diagnostics (neocmakelsp) are asked for them, once the typing
 * stops: the others send them by themselves (t-code does not offer pulling). */
static void pull_later(Doc *doc)
{
  if (!doc->lsp->capabilities || !json_at(doc->lsp->capabilities, "diagnosticProvider"))
    return;
  g_clear_handle_id(&doc->pull_id, g_source_remove);
  doc->pull_id = g_timeout_add(PULL_MS, pull, doc);
}

/* Sends the changes collected for a document. */
static void flush(Doc *doc)
{
  char *document, *params;

  g_clear_handle_id(&doc->flush_id, g_source_remove);
  if (doc->changes->len == 0)
    return;
  document = lsp_document_params(doc->tab);
  document[strlen(document) - 1] = '\0'; /* the version goes into textDocument */
  params = g_strdup_printf("{%s,\"version\":%d},\"contentChanges\":[%s]}", document, ++doc->version, doc->changes->str);
  notify(doc->lsp, "textDocument/didChange", params);
  g_string_truncate(doc->changes, 0);
  pull_later(doc);
  g_free(params);
  g_free(document);
}

static gboolean flush_later(gpointer data)
{
  Doc *doc = data;

  doc->flush_id = 0;
  flush(doc);
  return G_SOURCE_REMOVE;
}

/* One change: range (as an LSP range object) replaced by text. */
static void add_change(Doc *doc, const char *range, const char *text)
{
  char *quoted = json_quote(text);

  doc->use = ++doc->lsp->use;
  g_string_append_printf(doc->changes, "%s{\"range\":%s,\"text\":%s}", doc->changes->len ? "," : "", range, quoted);
  if (!doc->flush_id)
    doc->flush_id = g_timeout_add(FLUSH_MS, flush_later, doc);
  semantic_update(doc->tab);
  inlay_update(doc->tab);
  symbols_update(doc->tab);
  g_free(quoted);
}

/* Whether a change in the buffer goes to the server now (not before it opened the file, nor
 * while the file loads: then it gets all of it). */
static gboolean tracking(Doc *doc)
{
  return doc->opened && doc->lsp->ready && !doc->tab->view->loading;
}

static char *range_json(Lsp *l, const GtkTextIter *start, const GtkTextIter *end)
{
  int line1, character1, line2, character2;

  position_of(l, start, &line1, &character1);
  position_of(l, end, &line2, &character2);
  return g_strdup_printf("{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}", line1,
                         character1, line2, character2);
}

char *lsp_range(Tab *tab, const GtkTextIter *start, const GtkTextIter *end)
{
  Doc *doc = doc_of(tab);

  return doc ? range_json(doc->lsp, start, end) : g_strdup("null");
}

/* Before the buffer takes the text (the signal's default handler inserts it). */
static void on_insert(GtkTextBuffer *buffer G_GNUC_UNUSED, const GtkTextIter *where, const char *text, int len,
                      Doc *doc)
{
  char *range, *inserted;

  if (!tracking(doc))
    return;
  range = range_json(doc->lsp, where, where);
  inserted = g_strndup(text, len);
  add_change(doc, range, inserted);
  g_free(inserted);
  g_free(range);
}

/* Before the buffer deletes the range. */
static void on_delete(GtkTextBuffer *buffer G_GNUC_UNUSED, const GtkTextIter *start, const GtkTextIter *end, Doc *doc)
{
  char *range;

  if (!tracking(doc))
    return;
  range = range_json(doc->lsp, start, end);
  add_change(doc, range, "");
  g_free(range);
}

/* The whole text, as a JSON string. */
static char *quoted_text(Tab *tab)
{
  GtkTextIter start, end;
  char *text, *quoted;

  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
  quoted = json_quote(text);
  g_free(text);
  return quoted;
}

/* Closes the document on the server (so clangd drops its AST from RAM), keeping Doc in l->docs. */
static void close_on_server(Doc *doc)
{
  if (!doc->opened)
    return;
  flush(doc);
  fail_pending(doc->lsp, doc->tab);
  char *document = lsp_document_params(doc->tab), *params = g_strdup_printf("{%s}", document);
  notify(doc->lsp, "textDocument/didClose", params);
  g_free(params);
  g_free(document);
  doc->opened = FALSE;
}

/* Keeps at most MAX_OPEN_DOCS files open per server: least-recently-used hidden tabs get didClose
 * and didOpen again when shown. */
static void evict_docs(Lsp *l)
{
  for (;;) {
    GHashTableIter iter;
    gpointer value;
    Doc *oldest = NULL, *oldest_hidden = NULL;
    int opened = 0;

    g_hash_table_iter_init(&iter, l->docs);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      Doc *doc = value;
      if (!doc->opened)
        continue;
      opened++;
      if (!oldest || doc->use < oldest->use)
        oldest = doc;
      if ((!doc->tab->source || !gtk_widget_get_mapped(GTK_WIDGET(doc->tab->source))) &&
          (!oldest_hidden || doc->use < oldest_hidden->use))
        oldest_hidden = doc;
    }
    if (opened <= MAX_OPEN_DOCS)
      break;
    close_on_server(oldest_hidden ? oldest_hidden : oldest);
  }
}

/* Tells the server about the file: all of it, as it is now. */
static void send_document(Doc *doc)
{
  char *document = lsp_document_params(doc->tab), *text = quoted_text(doc->tab), *params;

  doc->use = ++doc->lsp->use;
  g_string_truncate(doc->changes, 0);
  document[strlen(document) - 1] = '\0';
  if (doc->opened) {
    params = g_strdup_printf("{%s,\"version\":%d},\"contentChanges\":[{\"text\":%s}]}", document, ++doc->version, text);
    notify(doc->lsp, "textDocument/didChange", params);
  } else {
    const char *id = server_language_id(doc->lsp->kind, doc->tab);
    params =
        g_strdup_printf("{%s,\"languageId\":\"%s\",\"version\":%d,\"text\":%s}}", document, id, ++doc->version, text);
    notify(doc->lsp, "textDocument/didOpen", params);
    doc->opened = TRUE;
    evict_docs(doc->lsp);
  }
  semantic_update(doc->tab);
  inlay_update(doc->tab);
  symbols_update(doc->tab);
  pull_later(doc);
  g_free(params);
  g_free(text);
  g_free(document);
}

static void doc_free(Doc *doc)
{
  g_clear_handle_id(&doc->flush_id, g_source_remove);
  g_clear_handle_id(&doc->pull_id, g_source_remove);
  g_signal_handlers_disconnect_by_data(doc->tab->buffer, doc);
  g_string_free(doc->changes, TRUE);
  g_free(doc);
}

/* The server ------------------------------------------------------------------------------- */

/* Gets or creates the language server for this tab's language and project. */
static Lsp *lsp_of(Tab *tab)
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
static void server_start(Lsp *l);

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
  notify(l, "workspace/didChangeWatchedFiles", params);
  g_free(params);
  g_string_truncate(l->written, 0);
  g_hash_table_iter_init(&iter, l->docs);
  while (g_hash_table_iter_next(&iter, NULL, &doc))
    if (((Doc *)doc)->opened) {
      close_on_server(doc);
      send_document(doc);
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
static void server_stop(Lsp *l)
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
  fail_pending(l, NULL);
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
    server_start(l);
  return G_SOURCE_REMOVE;
}

/* The server went by itself (it crashed): started again for the open files, a few times. */
static void on_server_closed(gpointer owner)
{
  Lsp *l = owner;

  server_stop(l);
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
  notify(l, "initialized", "{}");
  char *settings = server_settings(l->kind, l->app);
  if (settings) {
    char *params = g_strdup_printf("{\"settings\":%s}", settings);
    notify(l, "workspace/didChangeConfiguration", params);
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
      send_document(doc);
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
      finish(p, p->cancelled ? NULL : json_at(message, "result"));
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
static void server_start(Lsp *l)
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
  send_request(l, NULL, "initialize", params, NULL, on_initialized, l);
  g_free(params);
  g_free(options);
  g_free(quoted);
  g_free(uri);
}

/* IDLE_SECONDS without editors: the server stops. */
static gboolean idle_stop(gpointer data)
{
  Lsp *l = data;

  l->idle_id = 0;
  if (g_hash_table_size(l->docs) == 0)
    server_stop(l);
  return G_SOURCE_REMOVE;
}

/* Editors ------------------------------------------------------------------------------------ */

/* C/C++ use clangd; other supported languages use their server when installed. */
gboolean lsp_wants(Tab *tab)
{
  ServerKind kind = server_kind_for_tab(tab);

  return !tab->view->readonly && !tab->compare.with && kind != SERVER_NONE && server_installed(tab->app, kind);
}

const char *lsp_missing_server(Tab *tab)
{
  ServerKind kind = server_kind_for_tab(tab); /* SERVER_NONE without a tab */

  if (kind == SERVER_NONE || tab->kind != TAB_EDITOR || tab->view->readonly || tab->compare.with)
    return NULL; /* a Markdown preview has the .md path too */
  return server_missing(tab->app, kind);
}

void lsp_open(Tab *tab)
{
  Lsp *l;
  Doc *doc;

  if (!lsp_wants(tab))
    return;
  l = lsp_of(tab);
  if (!l || l->missing)
    return;
  doc = g_hash_table_lookup(l->docs, tab);
  if (!doc) {
    doc = g_new0(Doc, 1);
    doc->tab = tab;
    doc->lsp = l;
    doc->changes = g_string_new(NULL);
    g_hash_table_insert(l->docs, tab, doc);
    g_signal_connect(tab->buffer, "insert-text", G_CALLBACK(on_insert), doc);
    g_signal_connect(tab->buffer, "delete-range", G_CALLBACK(on_delete), doc);
  }
  doc->use = ++l->use;
  g_clear_handle_id(&l->idle_id, g_source_remove);
  if (!l->ch)
    server_start(l); /* the file goes once it has answered initialize */
  else if (l->ready)
    send_document(doc);
}

void lsp_touch(Tab *tab)
{
  Doc *doc = doc_of(tab);

  if (!doc)
    return;
  doc->use = ++doc->lsp->use;
  if (!doc->opened)
    lsp_open(tab);
}

void lsp_saved(Tab *tab)
{
  Doc *doc = doc_of(tab);
  char *document;

  if (!doc || !doc->opened)
    return;
  flush(doc);
  document = lsp_document_params(tab);
  char *params = g_strdup_printf("{%s}", document);
  notify(doc->lsp, "textDocument/didSave", params);
  pull_later(doc); /* some lint only when saved */
  g_free(params);
  g_free(document);
}

void lsp_close(Tab *tab)
{
  Doc *doc = doc_of(tab);
  Lsp *l;

  if (!doc)
    return;
  l = doc->lsp;
  fail_pending(l, tab);
  if (doc->opened) {
    char *document = lsp_document_params(tab), *params = g_strdup_printf("{%s}", document);
    notify(l, "textDocument/didClose", params);
    g_free(params);
    g_free(document);
  }
  g_hash_table_remove(l->docs, tab);
  if (g_hash_table_size(l->docs) == 0 && l->ch && !l->idle_id)
    l->idle_id = g_timeout_add_seconds(IDLE_SECONDS, idle_stop, l);
}

gboolean lsp_has(Tab *tab)
{
  Doc *doc = doc_of(tab);

  return doc && doc->opened;
}

void lsp_stop(App *t)
{
  if (!t->lsps) {
    t->lsp = NULL;
    return;
  }
  for (guint i = 0; i < t->lsps->len; i++) {
    Lsp *l = g_ptr_array_index(t->lsps, i);

    l->stopping = TRUE;
    server_stop(l);
    g_clear_handle_id(&l->idle_id, g_source_remove);
    g_clear_handle_id(&l->restart_id, g_source_remove);
    g_hash_table_destroy(l->docs);
    g_hash_table_destroy(l->pending);
    g_hash_table_destroy(l->progress);
    if (l->written)
      g_string_free(l->written, TRUE);
    g_free(l->workspace);
    lsp_unref(l);
  }
  g_ptr_array_free(t->lsps, TRUE);
  t->lsps = NULL;
  t->lsp = NULL;
}

static void restart_one(Lsp *l)
{
  GHashTableIter iter;
  gpointer doc;

  g_clear_handle_id(&l->idle_id, g_source_remove);
  g_clear_handle_id(&l->restart_id, g_source_remove);
  l->idle_id = l->restart_id = 0;
  server_stop(l);
  l->crashes = 0;
  l->missing = FALSE; /* it may have been installed meanwhile */
  if (l->kind == SERVER_CLANGD) {
    g_hash_table_iter_init(&iter, l->docs);
    while (g_hash_table_iter_next(&iter, NULL, &doc))
      ((Doc *)doc)->include_missing = FALSE;
  }
  if (g_hash_table_size(l->docs) > 0)
    server_start(l); /* else the next file starts it */
}

void lsp_restart(App *t)
{
  if (!t->lsps)
    return;
  for (guint i = 0; i < t->lsps->len; i++)
    restart_one(g_ptr_array_index(t->lsps, i));
  update_status(t);
}

void lsp_restart_language(App *t, const char *language)
{
  if (!t->lsps)
    return;
  for (guint i = 0; i < t->lsps->len; i++) {
    Lsp *l = g_ptr_array_index(t->lsps, i);
    if (g_strcmp0(server_language(l->kind), language) == 0)
      restart_one(l);
  }
  update_status(t);
}

void lsp_open_installed(App *t)
{
  int n = tab_count(t);

  for (guint i = 0; t->lsps && i < t->lsps->len; i++) {
    Lsp *l = g_ptr_array_index(t->lsps, i);
    if (l->missing)
      restart_one(l);
  }
  for (int i = 0; i < n; i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR && !tab->view->loading && !doc_of(tab) && lsp_wants(tab))
      lsp_open(tab);
  }
  update_status(t);
}

/* "rust-analyzer: Indexing 45%" for l, or NULL when it is idle. */
static char *progress_text(Lsp *l)
{
  Progress *p = l->progress_last ? g_hash_table_lookup(l->progress, l->progress_last) : NULL;
  char *name = g_path_get_basename(server_name(l->kind)), *text = NULL;
  GHashTableIter iter;

  if (!p) { /* the last one ended: another still running */
    g_hash_table_iter_init(&iter, l->progress);
    g_hash_table_iter_next(&iter, NULL, (gpointer *)&p);
  }
  if (p)
    text = g_strdup_printf("%s: %s", name, p->text);
  else if (l->ch && !l->ready)
    text = g_strdup_printf("%s: starting", name);
  g_free(name);
  return text;
}

char *lsp_progress(App *t)
{
  Tab *tab = current_tab(t);
  Doc *doc = tab ? doc_of(tab) : NULL;
  char *text = doc ? progress_text(doc->lsp) : NULL;

  for (guint i = 0; !text && t->lsps && i < t->lsps->len; i++)
    text = progress_text(g_ptr_array_index(t->lsps, i));
  return text;
}

void lsp_restart_clangd(App *t)
{
  if (!t->lsps)
    return;
  for (guint i = 0; i < t->lsps->len; i++) {
    Lsp *l = g_ptr_array_index(t->lsps, i);
    if (l->kind == SERVER_CLANGD)
      restart_one(l);
  }
  update_status(t);
}

/* Whether tab is C++ (its system headers are the C++ library's too). */
static gboolean is_cxx(Tab *tab)
{
  GtkSourceLanguage *language = gtk_source_buffer_get_language(tab->buffer);
  const char *id = language ? gtk_source_language_get_id(language) : "";

  return strcmp(id, "cpp") == 0 || strcmp(id, "cpphdr") == 0;
}

char *lsp_flags_hint(App *t, const char **problem)
{
  Lsp *l = t->lsp;
  GHashTableIter iter;
  gpointer tab, doc;

  if (!l)
    return NULL;
  g_hash_table_iter_init(&iter, l->docs);
  while (g_hash_table_iter_next(&iter, &tab, &doc)) {
    const char *path = ((Tab *)tab)->path;
    char *database, *misnamed, *name, *hint;
    if (!((Doc *)doc)->include_missing || !path)
      continue;
    name = g_path_get_basename(path);
    if (!compile_db_system_headers(sysroot, is_cxx(tab))) {
      *problem = "No C headers";
      hint = g_strdup_printf("clangd cannot find the headers %s includes: the C library's headers (stdio.h and the "
                             "others) are not installed. They come with the compiler:\n"
                             "  • Fedora: sudo dnf install gcc gcc-c++ make\n"
                             "  • Debian / Ubuntu: sudo apt install build-essential\n"
                             "  • Arch Linux: sudo pacman -S --needed base-devel\n"
                             "  • Void Linux: sudo xbps-install base-devel\n\n"
                             "Then run \"Restart Language Servers\".",
                             name);
      g_free(name);
      return hint;
    }
    database = compile_db_find(path);
    if (database) {
      g_free(database);
      g_free(name);
      continue;
    }
    *problem = "No compile flags";
    misnamed = compile_db_misnamed(path, t->project.root);
    if (misnamed) {
      char *found = g_path_get_basename(misnamed);
      hint = g_strdup_printf("clangd cannot find the headers %s includes, as it has no compile flags for it.\n\n"
                             "clangd does not read \"%s\": rename it to %s, then run "
                             "\"Restart Language Servers\".",
                             name, found,
                             g_str_has_suffix(found, ".json") ? "compile_commands.json" : "compile_flags.txt");
      g_free(found);
    } else {
      hint = g_strdup_printf("clangd cannot find the headers %s includes, as it has no compile flags for it.\n\n"
                             "clangd reads them from compile_commands.json, build/compile_commands.json or "
                             "compile_flags.txt in the file's folder or above it:\n"
                             "  • make: bear -- make (writes compile_commands.json)\n"
                             "  • CMake: -DCMAKE_EXPORT_COMPILE_COMMANDS=ON\n"
                             "  • or compile_flags.txt with one flag per line (-I/usr/include/glib-2.0)\n\n"
                             "Then run \"Restart Language Servers\".",
                             name);
    }
    g_free(name);
    g_free(misnamed);
    return hint;
  }
  return NULL;
}

void lsp_request(Tab *tab, const char *method, const char *params, GCancellable *cancel, LspReply reply, gpointer data)
{
  Doc *doc = doc_of(tab);

  if (!doc || !doc->opened || !doc->lsp->ready || (cancel && g_cancellable_is_cancelled(cancel))) {
    reply(NULL, data);
    return;
  }
  doc->use = ++doc->lsp->use;
  flush(doc);
  send_request(doc->lsp, tab, method, params, cancel, reply, data);
}

JsonNode *lsp_capability(Tab *tab, const char *path)
{
  Doc *doc = doc_of(tab);

  return doc && doc->lsp->capabilities ? json_at(doc->lsp->capabilities, path) : NULL;
}
