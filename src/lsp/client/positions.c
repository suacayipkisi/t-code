/*
 * LSP positions (see state.h): a line and a character, counted in UTF-8 bytes or UTF-16 units as the
 * server said, to and from text iters; the params most requests take.
 */
#include "lsp/client/state.h"

#include "editor/editorview.h"

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

char *lsp_range_json(Lsp *l, const GtkTextIter *start, const GtkTextIter *end)
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

  return doc ? lsp_range_json(doc->lsp, start, end) : g_strdup("null");
}
