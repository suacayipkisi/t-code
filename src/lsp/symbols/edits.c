/*
 * Applying TextEdits (see edits.h).
 */
#include "lsp/symbols/edits.h"

#include "lsp/client/client.h"

#include <stdlib.h>

typedef struct {
  GtkTextMark *start, *end;
  const char *text;
} Edit;

void edits_apply(Tab *tab, JsonNode *edits, gboolean utf16)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  JsonArray *array = edits && JSON_NODE_HOLDS_ARRAY(edits) ? json_node_get_array(edits) : NULL;
  guint n = array ? json_array_get_length(array) : 0;
  Edit *list = g_new0(Edit, MAX(n, 1));

  /* Marks first: the edits move the text, the marks move with it. They stay right of text
     * inserted at them, so two edits at one place come in the list's order. */
  for (guint i = 0; i < n; i++) {
    JsonNode *edit = json_array_get_element(array, i);
    GtkTextIter start, end;
    lsp_iter_at_encoding(tab, json_at(edit, "range.start"), &start, utf16);
    lsp_iter_at_encoding(tab, json_at(edit, "range.end"), &end, utf16);
    list[i].start = gtk_text_buffer_create_mark(buffer, NULL, &start, FALSE);
    list[i].end = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
    list[i].text = json_string_at(edit, "newText");
  }
  gtk_text_buffer_begin_user_action(buffer);
  for (guint i = 0; i < n; i++) {
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_mark(buffer, &start, list[i].start);
    gtk_text_buffer_get_iter_at_mark(buffer, &end, list[i].end);
    gtk_text_buffer_delete(buffer, &start, &end);
    if (list[i].text && *list[i].text)
      gtk_text_buffer_insert(buffer, &start, list[i].text, -1);
    gtk_text_buffer_delete_mark(buffer, list[i].start);
    gtk_text_buffer_delete_mark(buffer, list[i].end);
  }
  gtk_text_buffer_end_user_action(buffer);
  g_free(list);
}

/* The byte offset in text of an LSP position. */
static gsize offset_of(const char *text, JsonNode *position, gboolean utf16)
{
  gint64 line = json_int_at(position, "line", 0), character = json_int_at(position, "character", 0);
  const char *p = text;

  for (; line > 0 && *p; p++)
    if (*p == '\n')
      line--;
  for (gint64 at = 0; *p && *p != '\n';) {
    gunichar c = g_utf8_get_char(p);
    at += utf16 ? (c >= 0x10000 ? 2 : 1) : (gint64)(g_utf8_next_char(p) - p);
    if (at > character)
      break;
    p = g_utf8_next_char(p);
  }
  return p - text;
}

typedef struct {
  gsize start, end;
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
