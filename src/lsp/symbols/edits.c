/*
 * Applying TextEdits (see edits.h).
 */
#include "lsp/symbols/edits.h"

#include "lsp/client/client.h"

#include <stdlib.h>

typedef struct {
  gsize start, end; /* bytes in edits_apply_text, characters in edits_apply */
  const char *text;
  guint index; /* in the list: of two at one place, the later one is applied first */
} Span;

/* Later spans first: applying them from the end keeps the earlier offsets right. */
static int later_first(const void *a, const void *b)
{
  const Span *x = a, *y = b;

  if (x->start != y->start)
    return x->start < y->start ? 1 : -1;
  return x->index < y->index ? 1 : -1;
}

void edits_apply(Tab *tab, JsonNode *edits, gboolean utf16)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  JsonArray *array = edits && JSON_NODE_HOLDS_ARRAY(edits) ? json_node_get_array(edits) : NULL;
  guint n = array ? json_array_get_length(array) : 0;
  Span *spans = g_new0(Span, MAX(n, 1));

  /* All positions first, in the text as the server saw it; then from the last edit to the first, so
   * each one leaves the positions before it as they were, whatever order the list has. */
  for (guint i = 0; i < n; i++) {
    JsonNode *edit = json_array_get_element(array, i);
    GtkTextIter start, end;
    lsp_iter_at_encoding(tab, json_at(edit, "range.start"), &start, utf16);
    lsp_iter_at_encoding(tab, json_at(edit, "range.end"), &end, utf16);
    spans[i].start = (gsize)gtk_text_iter_get_offset(&start);
    spans[i].end = MAX((gsize)gtk_text_iter_get_offset(&end), spans[i].start);
    spans[i].text = json_string_at(edit, "newText");
    spans[i].index = i;
  }
  qsort(spans, n, sizeof *spans, later_first);
  gtk_text_buffer_begin_user_action(buffer);
  for (guint i = 0; i < n; i++) {
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_offset(buffer, &start, (int)spans[i].start);
    gtk_text_buffer_get_iter_at_offset(buffer, &end, (int)spans[i].end);
    gtk_text_buffer_delete(buffer, &start, &end);
    if (spans[i].text && *spans[i].text)
      gtk_text_buffer_insert(buffer, &start, spans[i].text, -1);
  }
  gtk_text_buffer_end_user_action(buffer);
  g_free(spans);
}

/* The byte offset in text of an LSP position. */
static gsize offset_of(const char *text, JsonNode *position, gboolean utf16)
{
  gint64 line = json_int_at(position, "line", 0), character = json_int_at(position, "character", 0);
  const char *p = text;

  for (; line > 0 && *p; p++)
    if (*p == '\n')
      line--;
  for (gint64 at = 0; *p && *p != '\n' && !(p[0] == '\r' && p[1] == '\n');) { /* never between \r and \n */
    gunichar c = g_utf8_get_char_validated(p, -1);
    /* A byte that does not start a whole character (a file that is not UTF-8) counts as one: a cut
     * sequence must not step over the NUL. */
    gsize size = c < 0x80000000u ? (gsize)(g_utf8_next_char(p) - p) : 1;
    at += utf16 ? (c >= 0x10000 && c < 0x80000000u ? 2 : 1) : (gint64)size;
    if (at > character)
      break;
    p += size;
  }
  return p - text;
}

char *edits_apply_text(const char *text, JsonNode *edits, gboolean utf16)
{
  JsonArray *array = edits && JSON_NODE_HOLDS_ARRAY(edits) ? json_node_get_array(edits) : NULL;
  guint n = array ? json_array_get_length(array) : 0;
  Span *spans = g_new0(Span, MAX(n, 1));
  GString *result = g_string_new(text);

  for (guint i = 0; i < n; i++) {
    JsonNode *edit = json_array_get_element(array, i);
    spans[i].start = offset_of(text, json_at(edit, "range.start"), utf16);
    spans[i].end = MAX(offset_of(text, json_at(edit, "range.end"), utf16), spans[i].start);
    spans[i].text = json_string_at(edit, "newText");
    spans[i].index = i;
  }
  qsort(spans, n, sizeof *spans, later_first);
  for (guint i = 0; i < n; i++) {
    g_string_erase(result, (gssize)spans[i].start, (gssize)(spans[i].end - spans[i].start));
    g_string_insert(result, (gssize)spans[i].start, spans[i].text ? spans[i].text : "");
  }
  g_free(spans);
  return g_string_free(result, FALSE);
}
