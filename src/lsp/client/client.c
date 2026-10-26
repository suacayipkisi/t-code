/*
 * Per-language LSP clients (see client.h; the servers themselves are in lsp/servers.c). Each starts
 * on first use, keeps a small LRU of open documents and stops IDLE_SECONDS after its last editor
 * closes; a crash restarts that server up to MAX_CRASHES times. This file sends the requests and has
 * what the editors call; positions.c, documents.c and server.c the rest (state.h).
 */
#include "lsp/client/state.h"

#include "editor/editorview.h"
#include "editor/tabs.h"
#include "lsp/channel.h"
#include "lsp/compiledb.h"
#include "lsp/servers.h"
#include "window/statusbar.h"

#include <string.h>

/* Where the system headers are looked for (tests use a folder of their own). */
static const char *sysroot = "/";

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

/* Sending ------------------------------------------------------------------------------------ */

void lsp_notify(Lsp *l, const char *method, const char *params)
{
  char *body = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params);

  if (l->ch)
    channel_send(l->ch, body);
  g_free(body);
}

/* Hands a request its answer and forgets it. */
void lsp_finish(Pending *p, JsonNode *result)
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
void lsp_fail_pending(Lsp *l, Tab *tab)
{
  GList *all = g_hash_table_get_values(l->pending);

  for (GList *each = all; each; each = each->next) {
    Pending *p = each->data;
    if (!tab || p->tab == tab)
      lsp_finish(p, NULL);
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
    lsp_notify(l, "$/cancelRequest", params);
    g_free(params);
    lsp_finish(p, NULL);
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

void lsp_send(Lsp *l, Tab *tab, const char *method, const char *params, GCancellable *cancel, LspReply reply,
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
    g_signal_connect(tab->buffer, "insert-text", G_CALLBACK(doc_on_insert), doc);
    g_signal_connect(tab->buffer, "delete-range", G_CALLBACK(doc_on_delete), doc);
  }
  doc->use = ++l->use;
  g_clear_handle_id(&l->idle_id, g_source_remove);
  if (!l->ch)
    lsp_server_start(l); /* the file goes once it has answered initialize */
  else if (l->ready)
    doc_send(doc);
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
  doc_flush(doc);
  document = lsp_document_params(tab);
  char *params = g_strdup_printf("{%s}", document);
  lsp_notify(doc->lsp, "textDocument/didSave", params);
  doc_pull_later(doc); /* some lint only when saved */
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
  lsp_fail_pending(l, tab);
  if (doc->opened) {
    char *document = lsp_document_params(tab), *params = g_strdup_printf("{%s}", document);
    lsp_notify(l, "textDocument/didClose", params);
    g_free(params);
    g_free(document);
  }
  g_hash_table_remove(l->docs, tab);
  if (g_hash_table_size(l->docs) == 0 && l->ch && !l->idle_id)
    l->idle_id = g_timeout_add_seconds(IDLE_SECONDS, lsp_idle_stop, l);
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
    lsp_server_stop(l);
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
  lsp_server_stop(l);
  l->crashes = 0;
  l->missing = FALSE; /* it may have been installed meanwhile */
  if (l->kind == SERVER_CLANGD) {
    g_hash_table_iter_init(&iter, l->docs);
    while (g_hash_table_iter_next(&iter, NULL, &doc))
      ((Doc *)doc)->include_missing = FALSE;
  }
  if (g_hash_table_size(l->docs) > 0)
    lsp_server_start(l); /* else the next file starts it */
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
  doc_flush(doc);
  lsp_send(doc->lsp, tab, method, params, cancel, reply, data);
}

JsonNode *lsp_capability(Tab *tab, const char *path)
{
  Doc *doc = doc_of(tab);

  return doc && doc->lsp->capabilities ? json_at(doc->lsp->capabilities, path) : NULL;
}
