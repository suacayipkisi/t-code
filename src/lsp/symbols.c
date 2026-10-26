/*
 * An editor's symbols (see symbols.h).
 */
#include "lsp/symbols.h"

#include "editor/breadcrumbs.h"
#include "editor/editorview.h"
#include "lsp/client.h"
#include "lsp/json.h"
#include "lsp/outline.h"

#define SYMBOLS_MS 500 /* after the last change */
#define CURSOR_MS 120  /* the cursor rests this long before the breadcrumbs follow it */

struct Symbols {
  Tab *tab;
  GtkTextBuffer *buffer; /* outlives the page: the tab symbols_holds a reference */
  GPtrArray *roots;      /* Sym; NULL before the first answer */
  guint update_id, cursor_id;
  GCancellable *asking;
  guint changes; /* edits so far: an answer about older text is dropped */
};

typedef struct {
  Symbols *symbols;
  GCancellable *cancel;
  guint changes;
  gboolean utf16;
} SymbolsAsking;

static Symbols *symbols_of(Tab *tab)
{
  return tab && tab->view ? tab->view->symbols : NULL;
}

static void sym_free(Sym *sym)
{
  g_free(sym->name);
  g_free(sym->detail);
  g_ptr_array_unref(sym->children);
  g_free(sym);
}

static Sym *sym_new(void)
{
  Sym *sym = g_new0(Sym, 1);

  sym->children = g_ptr_array_new_with_free_func((GDestroyNotify)sym_free);
  return sym;
}

/* A position of the answer as the editor's line and character offset. */
static void symbols_position(Tab *tab, JsonNode *position, gboolean utf16, int *line, int *column)
{
  GtkTextIter iter;

  lsp_iter_at_encoding(tab, position, &iter, utf16);
  *line = gtk_text_iter_get_line(&iter);
  *column = gtk_text_iter_get_line_offset(&iter);
}

static void symbols_range(Tab *tab, Sym *sym, JsonNode *range, gboolean utf16)
{
  symbols_position(tab, json_at(range, "start"), utf16, &sym->line, &sym->column);
  symbols_position(tab, json_at(range, "end"), utf16, &sym->end_line, &sym->end_column);
}

/* A DocumentSymbol and its children. */
static Sym *symbols_from_document(Tab *tab, JsonNode *node, gboolean utf16)
{
  Sym *sym = sym_new();
  JsonNode *children = json_at(node, "children"), *selection = json_at(node, "selectionRange.start");

  sym->name = g_strdup(json_string_at(node, "name") ?: "");
  sym->detail = g_strdup(json_string_at(node, "detail"));
  sym->kind = (int)json_int_at(node, "kind", 0);
  symbols_range(tab, sym, json_at(node, "range"), utf16);
  if (selection)
    symbols_position(tab, selection, utf16, &sym->name_line, &sym->name_column);
  else {
    sym->name_line = sym->line;
    sym->name_column = sym->column;
  }
  for (guint i = 0;
       children && JSON_NODE_HOLDS_ARRAY(children) && i < json_array_get_length(json_node_get_array(children)); i++)
    g_ptr_array_add(sym->children,
                    symbols_from_document(tab, json_array_get_element(json_node_get_array(children), i), utf16));
  return sym;
}

static int symbols_by_place(gconstpointer a, gconstpointer b)
{
  const Sym *x = *(Sym **)a, *y = *(Sym **)b;

  if (x->line != y->line)
    return x->line - y->line;
  if (x->column != y->column)
    return x->column - y->column;
  return (y->end_line - y->line) - (x->end_line - x->line); /* the bigger one first: it symbols_holds the other */
}

static gboolean symbols_holds(const Sym *outer, const Sym *inner)
{
  gboolean starts = outer->line < inner->line || (outer->line == inner->line && outer->column <= inner->column);
  gboolean ends = outer->end_line > inner->end_line ||
                  (outer->end_line == inner->end_line && outer->end_column >= inner->end_column);
  return starts && ends;
}

/* SymbolInformation[]: flat, nested here by their ranges. */
static GPtrArray *symbols_from_information(Tab *tab, JsonArray *array, gboolean utf16)
{
  GPtrArray *all = g_ptr_array_new(), *roots = g_ptr_array_new_with_free_func((GDestroyNotify)sym_free);
  GPtrArray *open = g_ptr_array_new(); /* the symbols holding the next one, outermost first */

  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonNode *node = json_array_get_element(array, i);
    Sym *sym = sym_new();
    sym->name = g_strdup(json_string_at(node, "name") ?: "");
    sym->detail = g_strdup(json_string_at(node, "containerName"));
    sym->kind = (int)json_int_at(node, "kind", 0);
    symbols_range(tab, sym, json_at(node, "location.range"), utf16);
    sym->name_line = sym->line;
    sym->name_column = sym->column;
    g_ptr_array_add(all, sym);
  }
  g_ptr_array_sort(all, symbols_by_place);
  for (guint i = 0; i < all->len; i++) {
    Sym *sym = all->pdata[i];
    while (open->len > 0 && !symbols_holds(open->pdata[open->len - 1], sym))
      g_ptr_array_remove_index(open, open->len - 1);
    g_ptr_array_add(open->len > 0 ? ((Sym *)open->pdata[open->len - 1])->children : roots, sym);
    g_ptr_array_add(open, sym);
  }
  g_ptr_array_unref(open);
  g_ptr_array_unref(all);
  return roots;
}

static void symbols_on_answer(JsonNode *result, gpointer data)
{
  SymbolsAsking *a = data;
  Symbols *s = a->symbols;
  JsonArray *array = result && JSON_NODE_HOLDS_ARRAY(result) ? json_node_get_array(result) : NULL;

  if (g_cancellable_is_cancelled(a->cancel) || a->changes != s->changes || !array) {
    g_object_unref(a->cancel);
    g_free(a);
    return;
  }
  g_clear_object(&s->asking);
  if (s->roots)
    g_ptr_array_unref(s->roots);
  if (json_array_get_length(array) > 0 && json_at(json_array_get_element(array, 0), "location")) {
    s->roots = symbols_from_information(s->tab, array, a->utf16);
  } else {
    s->roots = g_ptr_array_new_with_free_func((GDestroyNotify)sym_free);
    for (guint i = 0; i < json_array_get_length(array); i++)
      g_ptr_array_add(s->roots, symbols_from_document(s->tab, json_array_get_element(array, i), a->utf16));
  }
  breadcrumbs_update(s->tab);
  outline_symbols_changed(s->tab);
  g_object_unref(a->cancel);
  g_free(a);
}

static gboolean symbols_ask(gpointer data)
{
  Symbols *s = data;
  SymbolsAsking *a;
  char *document, *params;

  s->update_id = 0;
  if (!lsp_has(s->tab) || !lsp_capability(s->tab, "documentSymbolProvider"))
    return G_SOURCE_REMOVE;
  if (s->asking)
    g_cancellable_cancel(s->asking);
  g_clear_object(&s->asking);
  s->asking = g_cancellable_new();
  a = g_new0(SymbolsAsking, 1);
  a->symbols = s;
  a->cancel = g_object_ref(s->asking);
  a->changes = s->changes;
  a->utf16 = lsp_utf16(s->tab);
  document = lsp_document_params(s->tab);
  params = g_strdup_printf("{%s}", document);
  lsp_request(s->tab, "textDocument/documentSymbol", params, s->asking, symbols_on_answer, a);
  g_free(params);
  g_free(document);
  return G_SOURCE_REMOVE;
}

void symbols_update(Tab *tab)
{
  Symbols *s = symbols_of(tab);

  if (!s)
    return;
  g_clear_handle_id(&s->update_id, g_source_remove);
  s->update_id = g_timeout_add(SYMBOLS_MS, symbols_ask, s);
}

GPtrArray *symbols_roots(Tab *tab)
{
  Symbols *s = symbols_of(tab);

  return s ? s->roots : NULL;
}

GPtrArray *symbols_at(Tab *tab, const GtkTextIter *iter)
{
  GPtrArray *chain = g_ptr_array_new(), *level = symbols_roots(tab);
  Sym here = { .line = gtk_text_iter_get_line(iter), .column = gtk_text_iter_get_line_offset(iter) };

  here.end_line = here.line;
  here.end_column = here.column;
  while (level) {
    Sym *inside = NULL;
    for (guint i = 0; i < level->len && !inside; i++)
      if (symbols_holds(level->pdata[i], &here))
        inside = level->pdata[i];
    if (!inside)
      break;
    g_ptr_array_add(chain, inside);
    level = inside->children;
  }
  return chain;
}

const char *symbols_kind_mark(int kind)
{
  switch (kind) {
    case 2:
    case 3:
    case 4: return "{}"; /* module, namespace, package */
    case 5: return "C";  /* class */
    case 6:
    case 9:
    case 12: return "ƒ"; /* method, constructor, function */
    case 7:
    case 8: return "·";  /* property, field */
    case 10: return "E"; /* enum */
    case 11: return "I"; /* interface */
    case 13:
    case 14: return "v"; /* variable, constant */
    case 22: return "e"; /* enum member */
    case 23: return "S"; /* struct */
    case 26: return "T"; /* type parameter */
    default: return "•";
  }
}

/* The cursor moved: the breadcrumbs and the outline follow once it rests. */
static gboolean symbols_cursor_rested(gpointer data)
{
  Symbols *s = data;

  s->cursor_id = 0;
  breadcrumbs_update(s->tab);
  outline_cursor_moved(s->tab);
  return G_SOURCE_REMOVE;
}

static void on_symbols_mark_set(GtkTextBuffer *buffer, const GtkTextIter *where G_GNUC_UNUSED, GtkTextMark *mark,
                                Symbols *s)
{
  if (mark != gtk_text_buffer_get_insert(buffer))
    return;
  g_clear_handle_id(&s->cursor_id, g_source_remove);
  s->cursor_id = g_timeout_add(CURSOR_MS, symbols_cursor_rested, s);
}

static void on_symbols_changed(GtkTextBuffer *buffer G_GNUC_UNUSED, Symbols *s)
{
  s->changes++;
}

void symbols_attach(Tab *tab)
{
  Symbols *s = g_new0(Symbols, 1);

  s->tab = tab;
  s->buffer = GTK_TEXT_BUFFER(tab->buffer);
  g_signal_connect(s->buffer, "mark-set", G_CALLBACK(on_symbols_mark_set), s);
  g_signal_connect(s->buffer, "changed", G_CALLBACK(on_symbols_changed), s);
  tab->view->symbols = s;
}

void symbols_free(Symbols *s)
{
  if (!s)
    return;
  if (s->asking)
    g_cancellable_cancel(s->asking);
  g_clear_object(&s->asking);
  g_clear_handle_id(&s->update_id, g_source_remove);
  g_clear_handle_id(&s->cursor_id, g_source_remove);
  g_signal_handlers_disconnect_by_data(s->buffer, s);
  if (s->roots)
    g_ptr_array_unref(s->roots);
  g_free(s);
}
