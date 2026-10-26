/*
 * Completion (see completion.h): a GtkSourceCompletionProvider whose proposals are the server's
 * CompletionItems. The server is asked once per word; typing on filters its answer here, unless it
 * said the answer was incomplete (clangd sends at most --limit-results items): then it is asked
 * again with the longer word.
 */
#include "lsp/inline/completion.h"

#include "lsp/client.h"
#include "lsp/symbols/edits.h"
#include "lsp/inline/signature.h"

#include <string.h>

/* LSP's CompletionItemKind, as the icons GtkSourceView brings ("lang-*-symbolic"). */
static const char *icon_for(gint64 kind)
{
  switch (kind) {
    case 2: return "lang-method-symbolic";        /* Method */
    case 3: return "lang-function-symbolic";      /* Function */
    case 4: return "lang-method-symbolic";        /* Constructor */
    case 5: return "lang-struct-field-symbolic";  /* Field */
    case 6: return "lang-variable-symbolic";      /* Variable */
    case 7: return "lang-class-symbolic";         /* Class */
    case 8: return "lang-class-symbolic";         /* Interface */
    case 9: return "lang-namespace-symbolic";     /* Module */
    case 10: return "lang-struct-field-symbolic"; /* Property */
    case 13: return "lang-enum-symbolic";         /* Enum */
    case 14: return "lang-define-symbolic";       /* Keyword */
    case 17: return "lang-include-symbolic";      /* File */
    case 20: return "lang-enum-value-symbolic";   /* EnumMember */
    case 21: return "lang-variable-symbolic";     /* Constant */
    case 22: return "lang-struct-symbolic";       /* Struct */
    case 25: return "lang-typedef-symbolic";      /* TypeParameter */
    default: return "lang-define-symbolic";       /* Text, Snippet, Macro (clangd: Text) ... */
  }
}

#define MAX_TEXT 60   /* characters of a name with its parameters in the list */
#define MAX_DETAIL 30 /* characters of the type on the right */

/* text, at most max characters: longer ones end in "…" (a long signature would make the list as
 * wide as the window). */
static char *shorten(const char *text, glong max)
{
  const char *end;

  if (!text || g_utf8_strlen(text, -1) <= max)
    return g_strdup(text);
  end = g_utf8_offset_to_pointer(text, max - 1);
  return g_strdup_printf("%.*s…", (int)(end - text), text);
}

/* Proposals ---------------------------------------------------------------------------------- */

struct _TcodeProposal {
  GObject parent;
  char *label;  /* the name */
  char *params; /* what follows it in the list: "(int x)" */
  char *detail; /* on the right: the type */
  char *docs;
  char *filter; /* what typing matches against */
  char *sort;
  char *text; /* inserted: plain, or a snippet */
  gboolean snippet;
  gint64 kind;
  JsonNode *edit_start; /* where the inserted text starts (the textEdit's range), or NULL */
  JsonNode *more;       /* additionalTextEdits ("#include <stdio.h>"), or NULL */
  guint priority;       /* from the fuzzy match: lower is better */
};

#define TCODE_TYPE_PROPOSAL (tcode_proposal_get_type())
G_DECLARE_FINAL_TYPE(TcodeProposal, tcode_proposal, TCODE, PROPOSAL, GObject)

static char *proposal_typed_text(GtkSourceCompletionProposal *proposal)
{
  return g_strdup(TCODE_PROPOSAL(proposal)->label);
}

static void proposal_iface_init(GtkSourceCompletionProposalInterface *iface)
{
  iface->get_typed_text = proposal_typed_text;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(TcodeProposal, tcode_proposal, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GTK_SOURCE_TYPE_COMPLETION_PROPOSAL, proposal_iface_init))

static void tcode_proposal_finalize(GObject *object)
{
  TcodeProposal *p = TCODE_PROPOSAL(object);

  g_free(p->label);
  g_free(p->params);
  g_free(p->detail);
  g_free(p->docs);
  g_free(p->filter);
  g_free(p->sort);
  g_free(p->text);
  g_clear_pointer(&p->edit_start, json_node_unref);
  g_clear_pointer(&p->more, json_node_unref);
  G_OBJECT_CLASS(tcode_proposal_parent_class)->finalize(object);
}

static void tcode_proposal_class_init(TcodeProposalClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = tcode_proposal_finalize;
}

static void tcode_proposal_init(TcodeProposal *self G_GNUC_UNUSED)
{
}

/* A copy of the node at path, or NULL. */
static JsonNode *copy_at(JsonNode *node, const char *path)
{
  node = json_at(node, path);
  return node ? json_node_copy(node) : NULL;
}

/* A proposal from a CompletionItem. */
static TcodeProposal *proposal_new(JsonNode *item)
{
  TcodeProposal *p = g_object_new(TCODE_TYPE_PROPOSAL, NULL);
  const char *label = json_string_at(item, "label");
  const char *text = json_string_at(item, "textEdit.newText");

  /* clangd puts " " or "•" (an #include comes with it) before the name. */
  while (label && (*label == ' ' || g_str_has_prefix(label, "•")))
    label = *label == ' ' ? label + 1 : label + strlen("•");
  p->label = g_strdup(label ? label : "");
  p->params = g_strdup(json_string_at(item, "labelDetails.detail"));
  if (p->params && *p->params && g_str_has_suffix(p->label, p->params)) /* shown apart */
    p->label[strlen(p->label) - strlen(p->params)] = '\0';
  p->detail = g_strdup(json_string_at(item, "detail"));
  p->docs = g_strdup(json_string_at(item, "documentation.value") ? json_string_at(item, "documentation.value")
                                                                 : json_string_at(item, "documentation"));
  p->filter = g_strdup(json_string_at(item, "filterText") ? json_string_at(item, "filterText") : p->label);
  p->sort = g_strdup(json_string_at(item, "sortText") ? json_string_at(item, "sortText") : p->label);
  if (!text)
    text = json_string_at(item, "insertText");
  p->text = g_strdup(text ? text : p->label);
  p->snippet = json_int_at(item, "insertTextFormat", 1) == 2;
  p->kind = json_int_at(item, "kind", 1);
  p->edit_start = copy_at(item, "textEdit.range.start");
  p->more = copy_at(item, "additionalTextEdits");
  return p;
}

/* The provider ------------------------------------------------------------------------------- */

struct _TcodeCompletion {
  GObject parent;
  Tab *tab;
  GPtrArray *all;        /* every proposal of the last answer */
  GListStore *shown;     /* the ones matching the word, best first */
  char *asked_word;      /* the word the last answer was for */
  gboolean incomplete;   /* the server has more than it sent */
  GCancellable *requery; /* asking again for a longer word */
};

#define TCODE_TYPE_COMPLETION (tcode_completion_get_type())
G_DECLARE_FINAL_TYPE(TcodeCompletion, tcode_completion, TCODE, COMPLETION, GObject)

static void provider_iface_init(GtkSourceCompletionProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(TcodeCompletion, tcode_completion, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GTK_SOURCE_TYPE_COMPLETION_PROVIDER, provider_iface_init))

/* Best first: the fuzzy match's priority, then the server's own order. */
static int by_rank(gconstpointer a, gconstpointer b)
{
  const TcodeProposal *x = *(TcodeProposal *const *)a, *y = *(TcodeProposal *const *)b;

  if (x->priority != y->priority)
    return x->priority < y->priority ? -1 : 1;
  return g_strcmp0(x->sort, y->sort);
}

/* The proposals matching word go to the shown list. */
static void refill(TcodeCompletion *self, const char *word)
{
  char *folded = g_utf8_casefold(word ? word : "", -1);
  GPtrArray *matches = g_ptr_array_new();

  for (guint i = 0; self->all && i < self->all->len; i++) {
    TcodeProposal *p = g_ptr_array_index(self->all, i);
    p->priority = 0;
    if (!*folded || gtk_source_completion_fuzzy_match(p->filter, folded, &p->priority))
      g_ptr_array_add(matches, p);
  }
  g_ptr_array_sort(matches, by_rank);
  g_list_store_splice(self->shown, 0, g_list_model_get_n_items(G_LIST_MODEL(self->shown)), matches->pdata,
                      matches->len);
  g_ptr_array_free(matches, TRUE);
  g_free(folded);
}

/* Takes a completion answer (a CompletionList or a plain array of items). */
static void take_answer(TcodeCompletion *self, JsonNode *result, const char *word)
{
  JsonNode *items = json_at(result, "items") ? json_at(result, "items") : result;
  JsonArray *array = items && JSON_NODE_HOLDS_ARRAY(items) ? json_node_get_array(items) : NULL;
  JsonNode *incomplete = json_at(result, "isIncomplete");

  g_ptr_array_set_size(self->all, 0);
  for (guint i = 0; array && i < json_array_get_length(array); i++)
    g_ptr_array_add(self->all, proposal_new(json_array_get_element(array, i)));
  self->incomplete = incomplete && json_node_get_boolean(incomplete);
  g_free(self->asked_word);
  self->asked_word = g_strdup(word);
  refill(self, word);
}

/* "position" plus the context: how the list was opened (a trigger character, or else typing or
 * Ctrl+Space). */
static char *completion_params(Tab *tab, const GtkTextIter *at, gunichar trigger)
{
  char *position = lsp_position_params(tab, at), *params;

  if (trigger)
    params =
        g_strdup_printf("{%s,\"context\":{\"triggerKind\":2,\"triggerCharacter\":\"%c\"}}", position, (char)trigger);
  else
    params = g_strdup_printf("{%s,\"context\":{\"triggerKind\":1}}", position);
  g_free(position);
  return params;
}

/* The trigger character right before the word, if the list opened because of it. */
static gunichar trigger_before(const GtkTextIter *begin)
{
  GtkTextIter before = *begin;
  gunichar c;

  if (!gtk_text_iter_backward_char(&before))
    return 0;
  c = gtk_text_iter_get_char(&before);
  return c == '.' || c == '>' || c == ':' ? c : 0;
}

typedef struct {
  TcodeCompletion *self;
  char *word;
} Query;

static void on_items(JsonNode *result, gpointer data)
{
  GTask *task = data;
  Query *query = g_task_get_task_data(task);

  if (!result) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No completion");
  } else {
    take_answer(query->self, result, query->word);
    g_task_return_pointer(task, g_object_ref(query->self->shown), g_object_unref);
  }
  g_object_unref(task);
}

static void query_free(Query *query)
{
  g_free(query->word);
  g_free(query);
}

static void populate_async(GtkSourceCompletionProvider *provider, GtkSourceCompletionContext *context,
                           GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  TcodeCompletion *self = TCODE_COMPLETION(provider);
  GTask *task = g_task_new(provider, cancellable, callback, data);
  Query *query = g_new0(Query, 1);
  GtkTextIter begin, end;
  char *params;

  if (!lsp_has(self->tab)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "No language server");
    g_object_unref(task);
    g_free(query);
    return;
  }
  gtk_source_completion_context_get_bounds(context, &begin, &end);
  query->self = self;
  query->word = gtk_source_completion_context_get_word(context);
  g_task_set_task_data(task, query, (GDestroyNotify)query_free);
  params = completion_params(self->tab, &end, gtk_text_iter_equal(&begin, &end) ? trigger_before(&begin) : 0);
  lsp_request(self->tab, "textDocument/completion", params, cancellable, on_items, task);
  g_free(params);
}

static GListModel *populate_finish(GtkSourceCompletionProvider *provider G_GNUC_UNUSED, GAsyncResult *result,
                                   GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* The answer to asking again with a longer word. */
static void on_requery(JsonNode *result, gpointer data)
{
  Query *query = data;

  if (result)
    take_answer(query->self, result, query->word);
  g_object_unref(query->self);
  query_free(query);
}

/* More was typed: filter what came; when that was not all the server had, ask it again. */
static void refilter(GtkSourceCompletionProvider *provider, GtkSourceCompletionContext *context,
                     GListModel *model G_GNUC_UNUSED)
{
  TcodeCompletion *self = TCODE_COMPLETION(provider);
  char *word = gtk_source_completion_context_get_word(context);
  GtkTextIter begin, end;

  refill(self, word);
  if (self->incomplete && g_strcmp0(word, self->asked_word) != 0) {
    Query *query = g_new0(Query, 1);
    char *params;
    gtk_source_completion_context_get_bounds(context, &begin, &end);
    if (self->requery) {
      g_cancellable_cancel(self->requery);
      g_object_unref(self->requery);
    }
    self->requery = g_cancellable_new();
    query->self = g_object_ref(self);
    query->word = g_strdup(word);
    params = completion_params(self->tab, &end, 0);
    lsp_request(self->tab, "textDocument/completion", params, self->requery, on_requery, query);
    g_free(params);
  }
  g_free(word);
}

/* A row: icon, the name with the typed letters highlighted and its parameters, the type on the
 * right, and the documentation under the list. */
static void display(GtkSourceCompletionProvider *provider G_GNUC_UNUSED, GtkSourceCompletionContext *context,
                    GtkSourceCompletionProposal *proposal, GtkSourceCompletionCell *cell)
{
  TcodeProposal *p = TCODE_PROPOSAL(proposal);

  switch (gtk_source_completion_cell_get_column(cell)) {
    case GTK_SOURCE_COMPLETION_COLUMN_ICON: gtk_source_completion_cell_set_icon_name(cell, icon_for(p->kind)); break;
    case GTK_SOURCE_COMPLETION_COLUMN_TYPED_TEXT: {
      char *word = gtk_source_completion_context_get_word(context);
      char *full = g_strconcat(p->label, p->params, NULL);
      char *text = shorten(full, MAX_TEXT);
      PangoAttrList *attrs = gtk_source_completion_fuzzy_highlight(p->label, word);
      if (p->params && *p->params) { /* the parameters dimmed */
        PangoAttribute *dim = pango_attr_foreground_alpha_new(0.6 * G_MAXUINT16);
        dim->start_index = strlen(p->label);
        if (!attrs)
          attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, dim);
      }
      gtk_source_completion_cell_set_text_with_attributes(cell, text, attrs);
      if (attrs)
        pango_attr_list_unref(attrs);
      g_free(text);
      g_free(full);
      g_free(word);
      break;
    }
    case GTK_SOURCE_COMPLETION_COLUMN_AFTER: {
      char *detail = shorten(p->detail, MAX_DETAIL);
      gtk_source_completion_cell_set_text(cell, detail);
      g_free(detail);
      break;
    }
    case GTK_SOURCE_COMPLETION_COLUMN_DETAILS: gtk_source_completion_cell_set_text(cell, p->docs); break;
    default: gtk_source_completion_cell_set_text(cell, NULL); break;
  }
}

/* A snippet GtkSourceView could not take, as plain text: "f(${1:int x}, $2)$0" -> "f(int x, )". */
static char *snippet_text(const char *snippet)
{
  GString *text = g_string_new(NULL);

  for (const char *p = snippet; *p; p++) {
    if (*p == '\\' && p[1]) { /* an escaped "$", "}" or "\\" */
      g_string_append_c(text, *++p);
    } else if (*p == '$' && p[1] == '{') { /* ${n:default}: the default */
      p += 2;
      while (g_ascii_isdigit(*p))
        p++;
      if (*p != ':')
        p--;
    } else if (*p == '$' && g_ascii_isdigit(p[1])) { /* $n: nothing */
      while (g_ascii_isdigit(p[1]))
        p++;
    } else if (*p != '}') {
      g_string_append_c(text, *p);
    }
  }
  return g_string_free(text, FALSE);
}

/* The proposal goes in for the word typed so far; a function's parameters as a snippet. Its extra
 * edits (an #include) go in too, and for a function the parameter hint opens. */
static void activate(GtkSourceCompletionProvider *provider, GtkSourceCompletionContext *context,
                     GtkSourceCompletionProposal *proposal)
{
  TcodeCompletion *self = TCODE_COMPLETION(provider);
  TcodeProposal *p = TCODE_PROPOSAL(proposal);
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(self->tab->buffer);
  GtkSourceSnippet *snippet = p->snippet ? gtk_source_snippet_new_parsed(p->text, NULL) : NULL;
  GtkTextMark *from, *to;
  GtkTextIter begin, end;

  /* What is replaced: the word, or from where the server's edit starts. Marks keep it while the
     * extra edits (whose positions are the server's, before any change) go in. */
  gtk_source_completion_context_get_bounds(context, &begin, &end);
  if (p->edit_start) {
    GtkTextIter start;
    lsp_iter_at(self->tab, p->edit_start, &start);
    if (gtk_text_iter_compare(&start, &end) <= 0)
      begin = start;
  }
  from = gtk_text_buffer_create_mark(buffer, NULL, &begin, TRUE);
  to = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
  gtk_text_buffer_begin_user_action(buffer);
  if (p->more)
    edits_apply(self->tab, p->more, lsp_utf16(self->tab));
  gtk_text_buffer_get_iter_at_mark(buffer, &begin, from);
  gtk_text_buffer_get_iter_at_mark(buffer, &end, to);
  gtk_text_buffer_delete(buffer, &begin, &end);
  if (snippet) {
    gtk_source_view_push_snippet(self->tab->source, snippet, &begin);
  } else {
    char *text = p->snippet ? snippet_text(p->text) : g_strdup(p->text);
    gtk_text_buffer_insert(buffer, &begin, text, -1);
    g_free(text);
  }
  gtk_text_buffer_end_user_action(buffer);
  gtk_text_buffer_delete_mark(buffer, from);
  gtk_text_buffer_delete_mark(buffer, to);
  g_clear_object(&snippet);
  if (p->kind == 2 || p->kind == 3 || p->kind == 4)
    signature_show(self->tab);
}

/* ".", "->" and "::" open the list (after a member's object, a namespace or class). */
static gboolean is_trigger(GtkSourceCompletionProvider *provider G_GNUC_UNUSED, const GtkTextIter *iter, gunichar c)
{
  GtkTextIter before = *iter;
  gunichar previous = 0;

  if (gtk_text_iter_backward_chars(&before, 2))
    previous = gtk_text_iter_get_char(&before);
  return c == '.' || (c == '>' && previous == '-') || (c == ':' && previous == ':');
}

static void provider_iface_init(GtkSourceCompletionProviderInterface *iface)
{
  iface->populate_async = populate_async;
  iface->populate_finish = populate_finish;
  iface->refilter = refilter;
  iface->display = display;
  iface->activate = activate;
  iface->is_trigger = is_trigger;
}

static void tcode_completion_finalize(GObject *object)
{
  TcodeCompletion *self = TCODE_COMPLETION(object);

  if (self->requery) { /* its answer goes to on_requery, which holds its own reference */
    g_cancellable_cancel(self->requery);
    g_object_unref(self->requery);
  }
  g_ptr_array_unref(self->all);
  g_object_unref(self->shown);
  g_free(self->asked_word);
  G_OBJECT_CLASS(tcode_completion_parent_class)->finalize(object);
}

static void tcode_completion_class_init(TcodeCompletionClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = tcode_completion_finalize;
}

static void tcode_completion_init(TcodeCompletion *self)
{
  self->all = g_ptr_array_new_with_free_func(g_object_unref);
  self->shown = g_list_store_new(TCODE_TYPE_PROPOSAL);
}

void completion_attach(Tab *tab)
{
  TcodeCompletion *provider = g_object_new(TCODE_TYPE_COMPLETION, NULL);
  GtkSourceCompletion *completion = gtk_source_view_get_completion(tab->source);

  provider->tab = tab;
  g_object_set(completion, "page-size", 12, "select-on-show", TRUE, "show-icons", TRUE, "remember-info-visibility",
               TRUE, NULL);
  gtk_source_completion_add_provider(completion, GTK_SOURCE_COMPLETION_PROVIDER(provider));
  g_object_unref(provider); /* the completion holds it */
}

void completion_use_words(Tab *tab)
{
  GtkSourceCompletion *completion = gtk_source_view_get_completion(tab->source);
  GtkSourceCompletionWords *words;

  if (g_object_get_data(G_OBJECT(completion), "t-code-words"))
    return;
  words = gtk_source_completion_words_new("Words");
  gtk_source_completion_words_register(words, GTK_TEXT_BUFFER(tab->buffer));
  gtk_source_completion_add_provider(completion, GTK_SOURCE_COMPLETION_PROVIDER(words));
  g_object_set_data_full(G_OBJECT(completion), "t-code-words", words, g_object_unref);
}

void completion_show(Tab *tab)
{
  gtk_source_completion_show(gtk_source_view_get_completion(tab->source));
}
