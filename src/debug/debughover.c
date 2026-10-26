/*
 * The debugger's hover (see debughover.h): evaluate {expression, frameId, context "hover"} in the frame
 * the Run and Debug panel shows.
 */
#include "debug/debughover.h"

#include "debug/dap.h"
#include "debug/debugger.h"
#include "debug/debugview.h"
#include "editor/editing/editing.h"
#include "lsp/json.h"

#define MAX_WIDTH_CHARS 90

struct _TcodeDebugHover {
  GObject parent;
  Tab *tab;
};

#define TCODE_TYPE_DEBUG_HOVER (tcode_debug_hover_get_type())
G_DECLARE_FINAL_TYPE(TcodeDebugHover, tcode_debug_hover, TCODE, DEBUG_HOVER, GObject)

static void provider_init(GtkSourceHoverProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(TcodeDebugHover, tcode_debug_hover, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GTK_SOURCE_TYPE_HOVER_PROVIDER, provider_init))

/* One hover being filled: the task returns exactly once, also when gdb's answer never comes. */
typedef struct {
  GTask *task;
  GtkSourceHoverDisplay *display;
  char *expression;
  gboolean done;
} Job;

static void job_free(gpointer data)
{
  Job *job = data;

  if (!job->done)
    g_task_return_new_error(job->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "No answer");
  g_object_unref(job->task);
  g_object_unref(job->display);
  g_free(job->expression);
  g_free(job);
}

static void on_value(gpointer data, JsonNode *body, const char *error)
{
  Job *job = data;
  const char *value = json_string_at(body, "result");
  GtkWidget *label;
  char *text;

  job->done = TRUE;
  if (error || !value || !*value || g_task_return_error_if_cancelled(job->task)) {
    if (!g_task_had_error(job->task))
      g_task_return_new_error(job->task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No value");
    return;
  }
  text = g_strdup_printf("%s = %s", job->expression, value);
  label = gtk_label_new(text);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), MAX_WIDTH_CHARS);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_add_css_class(label, "tcode-hover");
  gtk_widget_add_css_class(label, "symbol");
  gtk_source_hover_display_prepend(job->display, label); /* above clangd's */
  g_task_return_boolean(job->task, TRUE);
  g_free(text);
}

/* The expression at iter: the selection when iter is in it, else the name there with what it is a
 * member of ("p->next->name"); NULL when there is none. */
static char *expression_at(GtkTextBuffer *buffer, const GtkTextIter *iter)
{
  GtkTextIter start, end, before;

  if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end) && gtk_text_iter_in_range(iter, &start, &end))
    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  if (!editor_word_at(iter, &start, &end) || g_ascii_isdigit(gtk_text_iter_get_char(&start)))
    return NULL;
  for (;;) { /* back over ".name" and "->name" */
    GtkTextIter word_start, word_end;
    before = start;
    if (!gtk_text_iter_backward_char(&before))
      break;
    if (gtk_text_iter_get_char(&before) == '>') {
      GtkTextIter dash = before;
      if (!gtk_text_iter_backward_char(&dash) || gtk_text_iter_get_char(&dash) != '-')
        break;
      before = dash;
    } else if (gtk_text_iter_get_char(&before) != '.') {
      break;
    }
    GtkTextIter last = before;
    if (!gtk_text_iter_backward_char(&last) || !editor_word_at(&last, &word_start, &word_end) ||
        !gtk_text_iter_equal(&word_end, &before))
      break;
    start = word_start;
  }
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void populate_async(GtkSourceHoverProvider *provider, GtkSourceHoverContext *context,
                           GtkSourceHoverDisplay *display, GCancellable *cancellable, GAsyncReadyCallback callback,
                           gpointer data)
{
  Tab *tab = TCODE_DEBUG_HOVER(provider)->tab;
  GTask *task = g_task_new(provider, cancellable, callback, data);
  gint64 frame = debug_view_frame_id();
  Dap *dap = debug_dap(tab->app);
  char *expression = NULL, *quoted, *args;
  GtkTextIter iter;
  Job *job;

  if (frame < 0 || !dap || !gtk_source_hover_context_get_iter(context, &iter) ||
      !(expression = expression_at(GTK_TEXT_BUFFER(tab->buffer), &iter))) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Not paused");
    g_object_unref(task);
    return;
  }
  job = g_new0(Job, 1);
  job->task = task;
  job->display = g_object_ref(display);
  job->expression = expression;
  quoted = json_quote(expression);
  args = g_strdup_printf("{\"expression\":%s,\"frameId\":%" G_GINT64_FORMAT ",\"context\":\"hover\"}", quoted, frame);
  dap_request_full(dap, "evaluate", args, on_value, job, job_free);
  g_free(args);
  g_free(quoted);
}

static gboolean populate_finish(GtkSourceHoverProvider *provider G_GNUC_UNUSED, GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void provider_init(GtkSourceHoverProviderInterface *iface)
{
  iface->populate_async = populate_async;
  iface->populate_finish = populate_finish;
}

static void tcode_debug_hover_class_init(TcodeDebugHoverClass *klass G_GNUC_UNUSED)
{
}

static void tcode_debug_hover_init(TcodeDebugHover *self G_GNUC_UNUSED)
{
}

void debug_hover_attach(Tab *tab)
{
  TcodeDebugHover *provider = g_object_new(TCODE_TYPE_DEBUG_HOVER, NULL);

  provider->tab = tab;
  gtk_source_hover_add_provider(gtk_source_view_get_hover(tab->source), GTK_SOURCE_HOVER_PROVIDER(provider));
  g_object_unref(provider); /* the hover holds it */
}
