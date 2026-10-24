/*
 * Semantic token colors (see semantic.h): textDocument/semanticTokens/full, a while after the last
 * change, then full/delta: only what changed since, colored again on its lines only (all of a big
 * file's tokens took a quarter of a second each time). Each token type has a tag; the tags sit above
 * GtkSourceView's own, so they win.
 */
#include "lsp/semantic.h"

#include "editor/editorview.h"
#include "lsp/client.h"
#include "window/palette.h"

#include <string.h>

#define INACTIVE_TAG "tcode-inactive" /* lines the preprocessor leaves out */

#define UPDATE_MS 300 /* after the last change */

typedef struct TokensRequest TokensRequest;

struct Semantic {
  Tab *tab;
  guint update_id;
  TokensRequest *request;
  GCancellable *cancel;
  GArray *data;            /* guint32: the tokens last received (five numbers each), for full/delta */
  char *result_id;         /* theirs, to ask for what changed since; NULL: ask for all */
  GtkTextMark *dirty_from; /* text whose colors an answer changed while it could not be applied */
  GtkTextMark *dirty_to;
};

/* One request; dropped when semantic is NULL (the editor went) or a newer one went. */
struct TokensRequest {
  Semantic *semantic;
  guint changes; /* the buffer's change count when it was asked */
};

/* The kinds of colored tokens: a tag each ("semantic-function", ...), in the color theme's color. */
typedef enum { KIND_FUNCTION, KIND_TYPE, KIND_MACRO, KIND_CONSTANT, KIND_VARIABLE, KINDS } Kind;

static const char *const kind_names[KINDS] = { "function", "type", "macro", "constant", "variable" };
static const PalColor kind_colors[KINDS] = { PAL_FUNCTION, PAL_TYPE, PAL_KEYWORD, PAL_CONSTANT, PAL_VARIABLE };

/* clangd's token types; a readonly variable is a constant. */
typedef struct {
  const char *type;
  Kind kind;
} TokenKind;

/* clang-format off */
static const TokenKind token_kinds[] = {
  { "function", KIND_FUNCTION }, { "method", KIND_FUNCTION },
  { "type", KIND_TYPE }, { "class", KIND_TYPE }, { "struct", KIND_TYPE }, { "enum", KIND_TYPE },
  { "interface", KIND_TYPE }, { "typeParameter", KIND_TYPE }, { "concept", KIND_TYPE }, { "namespace", KIND_TYPE },
  { "macro", KIND_MACRO },
  { "enumMember", KIND_CONSTANT },
  { "variable", KIND_VARIABLE }, { "parameter", KIND_VARIABLE }, { "property", KIND_VARIABLE },
};
/* clang-format on */

#define TAG_PREFIX "semantic-"

/* The tag for a kind, made the first time. */
static GtkTextTag *token_tag(Tab *tab, Kind kind)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(tab->buffer));
  char *name = g_strconcat(TAG_PREFIX, kind_names[kind], NULL);
  GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);

  if (!tag)
    tag = gtk_text_buffer_create_tag(GTK_TEXT_BUFFER(tab->buffer), name, "foreground", palette_hex(kind_colors[kind]),
                                     NULL);
  g_free(name);
  return tag;
}

void semantic_recolor(Tab *tab)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(tab->buffer));

  for (int k = 0; k < KINDS; k++) {
    char *name = g_strconcat(TAG_PREFIX, kind_names[k], NULL);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);
    if (tag)
      g_object_set(tag, "foreground", palette_hex(kind_colors[k]), NULL);
    g_free(name);
  }
  GtkTextTag *inactive = gtk_text_tag_table_lookup(table, INACTIVE_TAG);
  if (inactive)
    g_object_set(inactive, "foreground-rgba", palette_rgba(PAL_FG_FAINT), NULL);
}

/* GtkTextTagTableForeach: collects the semantic tags. */
static void collect(GtkTextTag *tag, gpointer data)
{
  char *name = NULL;

  g_object_get(tag, "name", &name, NULL);
  if (name && g_str_has_prefix(name, TAG_PREFIX))
    g_ptr_array_add(data, tag);
  g_free(name);
}

/* The semantic tags go above all others: GtkSourceView makes its own as it colors more of the text,
 * and a newer tag would win. */
static void raise_tags(GtkTextTagTable *table)
{
  GPtrArray *tags = g_ptr_array_new();

  gtk_text_tag_table_foreach(table, collect, tags);
  for (guint i = 0; i < tags->len; i++)
    gtk_text_tag_set_priority(g_ptr_array_index(tags, i), gtk_text_tag_table_get_size(table) - 1);
  g_ptr_array_free(tags, TRUE);
  GtkTextTag *inactive = gtk_text_tag_table_lookup(table, INACTIVE_TAG); /* dims them too */
  if (inactive)
    gtk_text_tag_set_priority(inactive, gtk_text_tag_table_get_size(table) - 1);
}

static void on_tag_added(GtkTextTagTable *table, GtkTextTag *tag, gpointer data G_GNUC_UNUSED)
{
  char *name = NULL;

  g_object_get(tag, "name", &name, NULL);
  if (!name || !g_str_has_prefix(name, TAG_PREFIX))
    raise_tags(table);
  g_free(name);
}

static void remove_colors(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GPtrArray *tags = g_ptr_array_new();
  GtkTextIter start, end;

  gtk_text_tag_table_foreach(gtk_text_buffer_get_tag_table(buffer), collect, tags);
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  for (guint i = 0; i < tags->len; i++)
    gtk_text_buffer_remove_tag(buffer, g_ptr_array_index(tags, i), &start, &end);
  g_ptr_array_free(tags, TRUE);
}

/* The kind of a token type name, or -1 (not colored). */
static int kind_of(const char *type)
{
  for (size_t i = 0; type && i < G_N_ELEMENTS(token_kinds); i++)
    if (strcmp(token_kinds[i].type, type) == 0)
      return token_kinds[i].kind;
  return -1;
}

/* The tags for the server's token types, from its legend, and which types are variables (a readonly
 * one is a constant). */
typedef struct {
  GtkTextTag **tags;
  gboolean *variable;
  guint n_types;
  gint64 readonly; /* the modifier bit */
  GtkTextTag *constant;
} Legend;

static void legend_init(Legend *legend, Tab *tab)
{
  JsonNode *types = lsp_capability(tab, "semanticTokensProvider.legend.tokenTypes");
  JsonNode *modifiers = lsp_capability(tab, "semanticTokensProvider.legend.tokenModifiers");

  legend->n_types = types && JSON_NODE_HOLDS_ARRAY(types) ? json_array_get_length(json_node_get_array(types)) : 0;
  legend->tags = g_new0(GtkTextTag *, MAX(legend->n_types, 1));
  legend->variable = g_new0(gboolean, MAX(legend->n_types, 1));
  legend->constant = token_tag(tab, KIND_CONSTANT);
  legend->readonly = 0;
  for (guint k = 0; k < legend->n_types; k++) {
    const char *type = json_array_get_string_element(json_node_get_array(types), k);
    int kind = kind_of(type);
    legend->tags[k] = kind >= 0 ? token_tag(tab, kind) : NULL;
    legend->variable[k] = g_strcmp0(type, "variable") == 0 || g_strcmp0(type, "property") == 0;
  }
  for (guint bit = 0; modifiers && JSON_NODE_HOLDS_ARRAY(modifiers) &&
                      bit < json_array_get_length(json_node_get_array(modifiers)) && bit < 63;
       bit++)
    if (g_strcmp0(json_array_get_string_element(json_node_get_array(modifiers), bit), "readonly") == 0)
      legend->readonly = (gint64)1 << bit;
}

static void legend_clear(Legend *legend)
{
  g_free(legend->tags);
  g_free(legend->variable);
}

/* Colors the tokens on lines from_line to to_line (both included; -1: to the end). The numbers are
 * five per token: line and start relative to the token before, length, type, modifiers. */
static void color_tokens(Tab *tab, GArray *data, gint64 from_line, gint64 to_line)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  const guint32 *n = (const guint32 *)data->data;
  gint64 line = 0, start = 0;
  Legend legend;

  legend_init(&legend, tab);
  for (guint i = 0; i + 4 < data->len; i += 5) {
    GtkTextTag *tag;
    GtkTextIter from, to;
    line += n[i];
    start = n[i] ? n[i + 1] : start + n[i + 1];
    if (to_line >= 0 && line > to_line)
      break;
    if (line < from_line || n[i + 3] >= legend.n_types || !(tag = legend.tags[n[i + 3]]))
      continue;
    if (legend.variable[n[i + 3]] && (n[i + 4] & legend.readonly))
      tag = legend.constant;
    lsp_iter_at_position(tab, line, start, &from);
    lsp_iter_at_position(tab, line, start + n[i + 2], &to);
    gtk_text_buffer_apply_tag(buffer, tag, &from, &to);
  }
  raise_tags(gtk_text_buffer_get_tag_table(buffer));
  legend_clear(&legend);
}

/* The numbers of a JSON array, as guint32. */
static GArray *numbers_of(JsonNode *node)
{
  JsonArray *array = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
  guint n = array ? json_array_get_length(array) : 0;
  GArray *numbers = g_array_sized_new(FALSE, FALSE, sizeof(guint32), n);

  for (guint i = 0; i < n; i++) {
    guint32 value = (guint32)json_array_get_int_element(array, i);
    g_array_append_val(numbers, value);
  }
  return numbers;
}

/* All the tokens (a full answer): every color again. */
static void apply(Tab *tab, JsonNode *result)
{
  GArray *data = numbers_of(json_at(result, "data"));
  Semantic *s = tab->view->semantic;

  remove_colors(tab);
  color_tokens(tab, data, 0, -1);
  if (s) {
    if (s->data)
      g_array_unref(s->data);
    s->data = data;
    g_free(s->result_id);
    s->result_id = g_strdup(json_string_at(result, "resultId"));
  } else {
    g_array_unref(data);
  }
}

/* The line the token at index (a multiple of 5, or the end) starts on, and the line the token before
 * it starts on. */
static void token_lines(GArray *data, guint index, gint64 *before, gint64 *at)
{
  const guint32 *n = (const guint32 *)data->data;
  gint64 line = 0;

  *before = 0;
  for (guint i = 0; i < index && i + 4 < data->len; i += 5) {
    line += n[i];
    *before = line;
  }
  *at = index + 4 < data->len ? line + n[index] : -1;
}

/* Puts the marks around lines from_line to to_line (-1: the end), or widens them to take those in. */
static void mark_dirty(Semantic *s, gint64 from_line, gint64 to_line)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(s->tab->buffer);
  GtkTextIter from, to;

  gtk_text_buffer_get_iter_at_line(buffer, &from, (int)MAX(from_line, 0));
  if (to_line < 0 || to_line + 1 >= gtk_text_buffer_get_line_count(buffer))
    gtk_text_buffer_get_end_iter(buffer, &to);
  else
    gtk_text_buffer_get_iter_at_line(buffer, &to, (int)to_line + 1);
  if (s->dirty_from) {
    GtkTextIter old_from, old_to;
    gtk_text_buffer_get_iter_at_mark(buffer, &old_from, s->dirty_from);
    gtk_text_buffer_get_iter_at_mark(buffer, &old_to, s->dirty_to);
    if (gtk_text_iter_compare(&old_from, &from) < 0)
      from = old_from;
    if (gtk_text_iter_compare(&old_to, &to) > 0)
      to = old_to;
    gtk_text_buffer_move_mark(buffer, s->dirty_from, &from);
    gtk_text_buffer_move_mark(buffer, s->dirty_to, &to);
  } else {
    s->dirty_from = gtk_text_buffer_create_mark(buffer, NULL, &from, TRUE);
    s->dirty_to = gtk_text_buffer_create_mark(buffer, NULL, &to, FALSE);
  }
}

static void forget_dirty(Semantic *s)
{
  if (!s->dirty_from)
    return;
  gtk_text_buffer_delete_mark(GTK_TEXT_BUFFER(s->tab->buffer), s->dirty_from);
  gtk_text_buffer_delete_mark(GTK_TEXT_BUFFER(s->tab->buffer), s->dirty_to);
  s->dirty_from = s->dirty_to = NULL;
}

/* What changed since the last answer (full/delta): the edits go into the kept tokens, and only the
 * lines they cover are colored again (the colors elsewhere moved with the text). An answer for an
 * older text marks those lines, to be colored with the next one. */
static void apply_delta(Semantic *s, JsonNode *result, gboolean current)
{
  JsonNode *edits_node = json_at(result, "edits");
  JsonArray *edits = JSON_NODE_HOLDS_ARRAY(edits_node) ? json_node_get_array(edits_node) : NULL;
  guint n = edits ? json_array_get_length(edits) : 0, old_len = s->data->len, low = old_len, high = 0;
  gint64 from_line, to_line, before, unused;

  for (guint k = n; k-- > 0;) { /* from the last, so earlier starts stay right */
    JsonNode *edit = json_array_get_element(edits, k);
    guint start = MIN((guint)json_int_at(edit, "start", 0), s->data->len);
    guint count = MIN((guint)json_int_at(edit, "deleteCount", 0), s->data->len - start);
    GArray *inserted = numbers_of(json_at(edit, "data"));
    g_array_remove_range(s->data, start, count);
    g_array_insert_vals(s->data, start, inserted->data, inserted->len);
    low = MIN(low, start);
    high = MAX(high, start + count);
    g_array_unref(inserted);
  }
  g_free(s->result_id);
  s->result_id = g_strdup(json_string_at(result, "resultId"));
  if (!n && !s->dirty_from)
    return;
  if (n) {
    /* In the new tokens: from the first changed one up to where the unchanged tail starts. */
    guint tail = old_len - MAX(high, low), end = s->data->len - MIN(tail, s->data->len);
    low = low / 5 * 5;
    end = MIN((end + 4) / 5 * 5, s->data->len / 5 * 5);
    token_lines(s->data, low, &before, &unused);
    from_line = before;
    token_lines(s->data, end, &unused, &to_line);
    mark_dirty(s, from_line, to_line);
  }
  if (current) {
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(s->tab->buffer);
    GtkTextIter from, to;
    GPtrArray *tags = g_ptr_array_new();
    gtk_text_buffer_get_iter_at_mark(buffer, &from, s->dirty_from);
    gtk_text_buffer_get_iter_at_mark(buffer, &to, s->dirty_to);
    gtk_text_tag_table_foreach(gtk_text_buffer_get_tag_table(buffer), collect, tags);
    for (guint i = 0; i < tags->len; i++)
      gtk_text_buffer_remove_tag(buffer, g_ptr_array_index(tags, i), &from, &to);
    g_ptr_array_free(tags, TRUE);
    color_tokens(s->tab, s->data, gtk_text_iter_get_line(&from),
                 gtk_text_iter_is_end(&to) ? -1 : gtk_text_iter_get_line(&to));
    forget_dirty(s);
  }
}

/* Asks for all the tokens next time (the kept ones no longer match the colors). */
static void forget_tokens(Semantic *s)
{
  g_clear_pointer(&s->data, g_array_unref);
  g_clear_pointer(&s->result_id, g_free);
  forget_dirty(s);
}

static void on_tokens(JsonNode *result, gpointer data)
{
  TokensRequest *request = data;
  Semantic *s = request->semantic;

  if (s && s->request == request) {
    gboolean current = request->changes == s->tab->view->changes;
    s->request = NULL;
    if (!result || s->tab->app->plain)
      ;
    else if (json_at(result, "edits") && s->data)
      apply_delta(s, result, current);
    else if (json_at(result, "data") && current)
      apply(s->tab, result);
    else /* all of them, for an older text: the next answer brings all again */
      forget_tokens(s);
  }
  g_free(request);
}

static gboolean update_now(gpointer data)
{
  Semantic *s = data;
  char *document, *params, *previous;
  JsonNode *delta;
  gboolean ask_delta;

  s->update_id = 0;
  if (!s->tab || !lsp_has(s->tab) || !lsp_capability(s->tab, "semanticTokensProvider"))
    return G_SOURCE_REMOVE;
  delta = lsp_capability(s->tab, "semanticTokensProvider.full.delta");
  ask_delta = s->result_id && s->data && delta && JSON_NODE_HOLDS_VALUE(delta) && json_node_get_boolean(delta);
  if (s->request)
    s->request->semantic = NULL;
  if (s->cancel) {
    g_cancellable_cancel(s->cancel);
    g_object_unref(s->cancel);
  }
  s->cancel = g_cancellable_new();
  s->request = g_new0(TokensRequest, 1);
  s->request->semantic = s;
  s->request->changes = s->tab->view->changes;
  document = lsp_document_params(s->tab);
  previous = ask_delta ? json_quote(s->result_id) : NULL;
  params = ask_delta ? g_strdup_printf("{%s,\"previousResultId\":%s}", document, previous)
                     : g_strdup_printf("{%s}", document);
  lsp_request(s->tab, ask_delta ? "textDocument/semanticTokens/full/delta" : "textDocument/semanticTokens/full", params,
              s->cancel, on_tokens, s->request);
  g_free(previous);
  g_free(params);
  g_free(document);
  return G_SOURCE_REMOVE;
}

void semantic_update(Tab *tab)
{
  Semantic *s = tab->view->semantic;

  if (tab->app->plain) {
    remove_colors(tab);
    if (s)
      forget_tokens(s);
    return;
  }
  if (!s) {
    s = tab->view->semantic = g_new0(Semantic, 1);
    s->tab = tab;
    g_signal_connect(gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(tab->buffer)), "tag-added", G_CALLBACK(on_tag_added),
                     NULL);
  }
  if (s->update_id)
    g_source_remove(s->update_id);
  s->update_id = g_timeout_add(UPDATE_MS, update_now, s);
}

void semantic_free(Semantic *s)
{
  if (!s)
    return;
  s->tab = NULL;
  if (s->update_id) {
    guint update_id = s->update_id;
    s->update_id = 0;
    g_source_remove(update_id);
  }
  if (s->request)
    s->request->semantic = NULL;
  if (s->cancel) {
    g_cancellable_cancel(s->cancel);
    g_object_unref(s->cancel);
  }
  if (s->data)
    g_array_unref(s->data);
  g_free(s->result_id);
  g_free(s); /* the marks go with the buffer */
}

void semantic_inactive(Tab *tab, JsonNode *regions)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
  GtkTextTag *tag = gtk_text_tag_table_lookup(table, INACTIVE_TAG);
  JsonArray *array = regions && JSON_NODE_HOLDS_ARRAY(regions) ? json_node_get_array(regions) : NULL;
  GtkTextIter start, end;

  if (!tag) {
    tag = gtk_text_buffer_create_tag(buffer, INACTIVE_TAG, NULL);
    semantic_recolor(tab);
  }
  gtk_text_tag_set_priority(tag, gtk_text_tag_table_get_size(table) - 1); /* over every color */
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gtk_text_buffer_remove_tag(buffer, tag, &start, &end);
  for (guint i = 0; array && i < json_array_get_length(array); i++) {
    JsonNode *range = json_array_get_element(array, i);
    lsp_iter_at(tab, json_at(range, "start"), &start);
    lsp_iter_at(tab, json_at(range, "end"), &end);
    gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
  }
}
