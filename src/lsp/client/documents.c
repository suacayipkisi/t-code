/*
 * The server's copy of each open file (see state.h): didOpen with all of it, then each insert and
 * delete as a didChange (gathered for FLUSH_MS); a small LRU of open documents per server, the
 * rest closed on the server until shown again; pull diagnostics asked for PULL_MS after a change.
 */
#include "lsp/client/state.h"

#include "editor/editorview.h"
#include "lsp/inline/diagnostics.h"
#include "lsp/inline/inlay.h"
#include "lsp/inline/semantic.h"
#include "lsp/symbols/symbols.h"

#include <string.h>

/* Documents ---------------------------------------------------------------------------------- */

Doc *doc_of(Tab *tab)
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
void doc_pull_later(Doc *doc)
{
  if (!doc->lsp->capabilities || !json_at(doc->lsp->capabilities, "diagnosticProvider"))
    return;
  g_clear_handle_id(&doc->pull_id, g_source_remove);
  doc->pull_id = g_timeout_add(PULL_MS, pull, doc);
}

/* Sends the changes collected for a document. */
void doc_flush(Doc *doc)
{
  char *document, *params;

  g_clear_handle_id(&doc->flush_id, g_source_remove);
  if (doc->changes->len == 0)
    return;
  document = lsp_document_params(doc->tab);
  document[strlen(document) - 1] = '\0'; /* the version goes into textDocument */
  params = g_strdup_printf("{%s,\"version\":%d},\"contentChanges\":[%s]}", document, ++doc->version, doc->changes->str);
  lsp_notify(doc->lsp, "textDocument/didChange", params);
  g_string_truncate(doc->changes, 0);
  doc_pull_later(doc);
  g_free(params);
  g_free(document);
}

static gboolean flush_later(gpointer data)
{
  Doc *doc = data;

  doc->flush_id = 0;
  doc_flush(doc);
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

/* Before the buffer takes the text (the signal's default handler inserts it). */
void doc_on_insert(GtkTextBuffer *buffer G_GNUC_UNUSED, const GtkTextIter *where, const char *text, int len, Doc *doc)
{
  char *range, *inserted;

  if (!tracking(doc))
    return;
  range = lsp_range_json(doc->lsp, where, where);
  inserted = g_strndup(text, len);
  add_change(doc, range, inserted);
  g_free(inserted);
  g_free(range);
}

/* Before the buffer deletes the range. */
void doc_on_delete(GtkTextBuffer *buffer G_GNUC_UNUSED, const GtkTextIter *start, const GtkTextIter *end, Doc *doc)
{
  char *range;

  if (!tracking(doc))
    return;
  range = lsp_range_json(doc->lsp, start, end);
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
void doc_close_on_server(Doc *doc)
{
  if (!doc->opened)
    return;
  doc_flush(doc);
  lsp_fail_pending(doc->lsp, doc->tab);
  char *document = lsp_document_params(doc->tab), *params = g_strdup_printf("{%s}", document);
  lsp_notify(doc->lsp, "textDocument/didClose", params);
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
    doc_close_on_server(oldest_hidden ? oldest_hidden : oldest);
  }
}

/* Tells the server about the file: all of it, as it is now. */
void doc_send(Doc *doc)
{
  char *document = lsp_document_params(doc->tab), *text = quoted_text(doc->tab), *params;

  doc->use = ++doc->lsp->use;
  g_string_truncate(doc->changes, 0);
  document[strlen(document) - 1] = '\0';
  if (doc->opened) {
    params = g_strdup_printf("{%s,\"version\":%d},\"contentChanges\":[{\"text\":%s}]}", document, ++doc->version, text);
    lsp_notify(doc->lsp, "textDocument/didChange", params);
  } else {
    const char *id = server_language_id(doc->lsp->kind, doc->tab);
    params =
        g_strdup_printf("{%s,\"languageId\":\"%s\",\"version\":%d,\"text\":%s}}", document, id, ++doc->version, text);
    lsp_notify(doc->lsp, "textDocument/didOpen", params);
    doc->opened = TRUE;
    evict_docs(doc->lsp);
  }
  semantic_update(doc->tab);
  inlay_update(doc->tab);
  symbols_update(doc->tab);
  doc_pull_later(doc);
  g_free(params);
  g_free(text);
  g_free(document);
}

void doc_free(Doc *doc)
{
  g_clear_handle_id(&doc->flush_id, g_source_remove);
  g_clear_handle_id(&doc->pull_id, g_source_remove);
  g_signal_handlers_disconnect_by_data(doc->tab->buffer, doc);
  g_string_free(doc->changes, TRUE);
  g_free(doc);
}
