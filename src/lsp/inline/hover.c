/*
 * The hover (see hover.h): a GtkSourceHoverProvider. GtkSourceView shows its popover after the
 * pointer rests for a moment; the provider fills it, asking the server (textDocument/hover) on the way.
 * On a problem it also asks for its fixes (textDocument/codeAction), each a button under the problems.
 */
#include "lsp/inline/hover.h"

#include "editor/tabs.h"
#include "lsp/client/client.h"
#include "lsp/inline/diagnostics.h"
#include "lsp/symbols/navigate.h"
#include "window/palette.h"

#define MAX_WIDTH_CHARS 90
#define MAX_FIXES 3

struct _TcodeHover {
  GObject parent;
  Tab *tab;
};

#define TCODE_TYPE_HOVER (tcode_hover_get_type())
G_DECLARE_FINAL_TYPE(TcodeHover, tcode_hover, TCODE, HOVER, GObject)

static void hover_provider_init(GtkSourceHoverProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(TcodeHover, tcode_hover, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GTK_SOURCE_TYPE_HOVER_PROVIDER, hover_provider_init))

static const char *const hover_css =
    ".tcode-hover { padding: 4px 6px; }"
    ".tcode-hover label.symbol { font-family: 'Cascadia Code', monospace; font-size: 0.95em; color: var(--tc-fg); }"
    ".tcode-hover separator { margin: 6px 0; background: var(--tc-border-strong); }"
    ".tcode-hover button.fix { margin-top: 4px; padding: 2px 8px; min-height: 0; }";

/* One hover being filled. */
typedef struct {
  GtkSourceHoverDisplay *display;
  App *app;
  char *path;       /* the editor, looked up again when a fix is clicked */
  gboolean utf16;   /* the server's position encoding */
  char *problems;   /* Pango markup, or NULL */
  char *text;       /* the server's hover text, or NULL */
  GPtrArray *fixes; /* JSON of the CodeActions offered for the problems, the preferred one first */
  int pending;      /* answers still to come */
} Job;

static void job_free(Job *job)
{
  g_object_unref(job->display);
  g_free(job->path);
  g_free(job->problems);
  g_free(job->text);
  g_ptr_array_unref(job->fixes);
  g_free(job);
}

/* A fix button: what to apply, to which editor. */
typedef struct {
  App *app;
  char *path;
  char *action;
  gboolean utf16;
} Fix;

static void fix_free(gpointer data, GClosure *closure G_GNUC_UNUSED)
{
  Fix *fix = data;

  g_free(fix->path);
  g_free(fix->action);
  g_free(fix);
}

static void on_fix_clicked(GtkButton *button, Fix *fix)
{
  Tab *tab = find_tab(fix->app, TAB_EDITOR, fix->path);
  GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
  JsonNode *action = json_from_string(fix->action, NULL);

  if (popover)
    gtk_popover_popdown(GTK_POPOVER(popover));
  if (tab && action)
    navigate_apply_action(tab, action, fix->utf16);
  if (action)
    json_node_unref(action);
}

static GtkWidget *fix_button(Job *job, const char *action)
{
  JsonNode *node = json_from_string(action, NULL);
  char *label = g_strdup_printf("Quick Fix: %s", json_string_at(node, "title"));
  GtkWidget *button = gtk_button_new_with_label(label);
  Fix *fix = g_new0(Fix, 1);

  gtk_label_set_ellipsize(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(button))), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(button))), 0);
  gtk_widget_set_halign(button, GTK_ALIGN_START);
  gtk_widget_add_css_class(button, "fix");
  gtk_widget_set_tooltip_text(button, label);
  fix->app = job->app;
  fix->path = g_strdup(job->path);
  fix->action = g_strdup(action);
  fix->utf16 = job->utf16;
  g_signal_connect_data(button, "clicked", G_CALLBACK(on_fix_clicked), fix, fix_free, 0);
  g_free(label);
  json_node_unref(node);
  return button;
}

/* A label of at most MAX_WIDTH_CHARS columns, wrapped. */
static GtkWidget *hover_label(const char *text, gboolean markup, const char *css_class)
{
  GtkWidget *label = gtk_label_new(NULL);

  if (markup)
    gtk_label_set_markup(GTK_LABEL(label), text);
  else
    gtk_label_set_text(GTK_LABEL(label), text);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), MAX_WIDTH_CHARS);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_selectable(GTK_LABEL(label), FALSE);
  if (css_class)
    gtk_widget_add_css_class(label, css_class);
  return label;
}

/* The server's text (clangd's MarkupContent: {"value": ...}), without the blank lines clangd's plain
 * text has between every part. NULL when there is none. */
static char *hover_text(JsonNode *result)
{
  static GRegex *blank_lines;
  const char *value = json_string_at(result, "contents.value");

  if (!value || !*value)
    return NULL;
  if (!blank_lines)
    blank_lines = g_regex_new("\n\\s*\n", G_REGEX_OPTIMIZE, 0, NULL);
  return g_strstrip(g_regex_replace_literal(blank_lines, value, -1, 0, "\n", 0, NULL));
}

/* Fills the popover with the problems, their fixes and the server's text; with neither problems nor
 * text, nothing shows. */
static void fill(GTask *task)
{
  Job *job = g_task_get_task_data(task);
  const char *text = job->text;
  GtkWidget *box;

  if (!job->problems && !text) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Nothing to show");
    return;
  }
  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(box, "tcode-hover");
  if (job->problems)
    gtk_box_append(GTK_BOX(box), hover_label(job->problems, TRUE, NULL));
  for (guint i = 0; job->problems && i < job->fixes->len && i < MAX_FIXES; i++)
    gtk_box_append(GTK_BOX(box), fix_button(job, g_ptr_array_index(job->fixes, i)));
  if (job->problems && text)
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  if (text)
    gtk_box_append(GTK_BOX(box), hover_label(text, FALSE, "symbol"));
  gtk_source_hover_display_append(job->display, box);
  g_task_return_boolean(task, TRUE);
}

/* One answer came: the last fills the popover. */
static void answered(GTask *task)
{
  Job *job = g_task_get_task_data(task);

  if (--job->pending == 0)
    fill(task);
  g_object_unref(task);
}

static void on_hover(JsonNode *result, gpointer data)
{
  GTask *task = data;
  Job *job = g_task_get_task_data(task);

  job->text = result ? hover_text(result) : NULL;
  answered(task);
}

static void on_fixes(JsonNode *result, gpointer data)
{
  GTask *task = data;
  Job *job = g_task_get_task_data(task);
  JsonArray *array = result && JSON_NODE_HOLDS_ARRAY(result) ? json_node_get_array(result) : NULL;
  Tab *tab = array ? find_tab(job->app, TAB_EDITOR, job->path) : NULL;

  for (guint i = 0; array && i < json_array_get_length(array); i++) {
    JsonNode *action = json_array_get_element(array, i);
    JsonNode *preferred = json_at(action, "isPreferred");
    const char *kind = json_string_at(action, "kind");
    if (!json_string_at(action, "title") || (kind && g_strcmp0(kind, "quickfix") != 0))
      continue; /* a CodeAction of another kind (a Command has none: the server picked it for "only") */
    if (!navigate_action_runnable(tab, action))
      continue;
    if (preferred && json_node_get_boolean(preferred))
      g_ptr_array_insert(job->fixes, 0, json_to_string(action, FALSE));
    else
      g_ptr_array_add(job->fixes, json_to_string(action, FALSE));
  }
  answered(task);
}

/* Asks for the fixes of the problems at iter. */
static void ask_fixes(Tab *tab, const GtkTextIter *iter, GTask *task, GCancellable *cancellable)
{
  Job *job = g_task_get_task_data(task);
  GtkTextIter start, end;
  char *diagnostics = diagnostics_json_at(tab, iter, &start, &end);

  if (!diagnostics || !gtk_text_view_get_editable(GTK_TEXT_VIEW(tab->source))) {
    g_free(diagnostics);
    return;
  }
  char *document = lsp_document_params(tab), *range = lsp_range(tab, &start, &end);
  char *params = g_strdup_printf("{%s,\"range\":%s,\"context\":{\"diagnostics\":%s,\"only\":[\"quickfix\"]}}", document,
                                 range, diagnostics);
  job->pending++;
  lsp_request(tab, "textDocument/codeAction", params, cancellable, on_fixes, g_object_ref(task));
  g_free(params);
  g_free(range);
  g_free(document);
  g_free(diagnostics);
}

/* Fills display with what is at iter (nothing when NULL). */
static void populate(gpointer source, Tab *tab, const GtkTextIter *iter, GtkSourceHoverDisplay *display,
                     GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  GTask *task = g_task_new(source, cancellable, callback, data);
  Job *job = g_new0(Job, 1);

  job->display = g_object_ref(display);
  job->app = tab->app;
  job->path = g_strdup(tab->path);
  job->fixes = g_ptr_array_new_with_free_func(g_free);
  job->pending = 1; /* the hover */
  g_task_set_task_data(task, job, (GDestroyNotify)job_free);
  if (!iter) {
    on_hover(NULL, task);
    return;
  }
  job->problems = diagnostics_at(tab, iter, TRUE);
  if (lsp_has(tab)) {
    char *position = lsp_position_params(tab, iter), *params = g_strdup_printf("{%s}", position);
    job->utf16 = lsp_utf16(tab);
    if (job->problems)
      ask_fixes(tab, iter, task, cancellable);
    lsp_request(tab, "textDocument/hover", params, cancellable, on_hover, task);
    g_free(params);
    g_free(position);
  } else {
    on_hover(NULL, task);
  }
}

static void hover_populate_async(GtkSourceHoverProvider *provider, GtkSourceHoverContext *context,
                                 GtkSourceHoverDisplay *display, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer data)
{
  GtkTextIter iter;
  gboolean found = gtk_source_hover_context_get_iter(context, &iter);

  populate(provider, TCODE_HOVER(provider)->tab, found ? &iter : NULL, display, cancellable, callback, data);
}

static gboolean hover_populate_finish(GtkSourceHoverProvider *provider G_GNUC_UNUSED, GAsyncResult *result,
                                      GError **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void hover_provider_init(GtkSourceHoverProviderInterface *iface)
{
  iface->populate_async = hover_populate_async;
  iface->populate_finish = hover_populate_finish;
}

static void tcode_hover_class_init(TcodeHoverClass *klass G_GNUC_UNUSED)
{
}

static void tcode_hover_init(TcodeHover *self G_GNUC_UNUSED)
{
}

void hover_attach(Tab *tab)
{
  TcodeHover *provider = g_object_new(TCODE_TYPE_HOVER, NULL);
  GtkSourceHover *hover = gtk_source_view_get_hover(tab->source);

  palette_add_css(hover_css);
  provider->tab = tab;
  gtk_source_hover_add_provider(hover, GTK_SOURCE_HOVER_PROVIDER(provider));
  g_object_unref(provider); /* the hover holds it */
}
