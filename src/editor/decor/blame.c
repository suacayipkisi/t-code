/*
 * Line blame (see blame.h).
 */
#include "editor/decor/blame.h"

#include "editor/editorview.h"
#include "window/palette.h"

#include <string.h>

#define BLAME_MS 400     /* the cursor rests this long before its line is asked about */
#define BLAME_GAP "    " /* between the line's end and the blame */
#define SUMMARY_MAX 60   /* characters of the commit's subject shown */

struct Blame {
  Tab *tab;
  GtkTextBuffer *buffer; /* outlives the page: the tab holds a reference */
  guint rest_id;
  GCancellable *asking;
  int line;   /* the line text is about, -1 for none */
  char *text; /* NULL: nothing shown */
};

/* An answer about to come: dropped once cancelled (the Blame may be gone then). */
typedef struct {
  Blame *blame;
  GCancellable *cancel;
  guint changes; /* the text it is about (tab->view->changes) */
  int line;
} BlameAsking;

static gboolean blame_wanted(Blame *b)
{
  const char *top = b->tab->app->project.git_top;

  return b->tab->app->settings.line_blame && !b->tab->view->readonly && b->tab->path && top &&
         g_str_has_prefix(b->tab->path, top) && b->tab->path[strlen(top)] == '/';
}

static void blame_clear(Blame *b)
{
  g_cancellable_cancel(b->asking);
  g_clear_object(&b->asking);
  if (b->text)
    gtk_widget_queue_draw(GTK_WIDGET(b->tab->source));
  g_clear_pointer(&b->text, g_free);
  b->line = -1;
}

/* "3 days ago" for a Unix time. */
static char *time_ago(gint64 when)
{
  static const struct {
    gint64 seconds;
    const char *unit;
  } units[] = { { 365 * 86400, "year" }, { 30 * 86400, "month" }, { 7 * 86400, "week" },
                { 86400, "day" },        { 3600, "hour" },        { 60, "minute" } };
  gint64 ago = g_get_real_time() / G_USEC_PER_SEC - when;

  for (guint i = 0; i < G_N_ELEMENTS(units); i++)
    if (ago >= units[i].seconds) {
      gint64 n = ago / units[i].seconds;
      return g_strdup_printf("%" G_GINT64_FORMAT " %s%s ago", n, units[i].unit, n == 1 ? "" : "s");
    }
  return g_strdup("just now");
}

/* git blame --porcelain's answer for one line: "Ada, 3 days ago · Fix the parser", or NULL. */
static char *blame_line_text(const char *porcelain)
{
  char **lines = g_strsplit(porcelain, "\n", -1);
  const char *author = NULL, *summary = "";
  gint64 when = -1;
  char *text = NULL;

  for (int i = 1; lines[0] && lines[i] && lines[i][0] != '\t'; i++)
    if (g_str_has_prefix(lines[i], "author "))
      author = lines[i] + strlen("author ");
    else if (g_str_has_prefix(lines[i], "author-time "))
      when = g_ascii_strtoll(lines[i] + strlen("author-time "), NULL, 10);
    else if (g_str_has_prefix(lines[i], "summary "))
      summary = lines[i] + strlen("summary ");
  if (lines[0] && strspn(lines[0], "0") == 40) /* git's name for the text that is not committed */
    text = g_strdup(BLAME_GAP "Not committed yet");
  else if (author && when >= 0) {
    char *ago = time_ago(when), *subject = g_utf8_substring(summary, 0, SUMMARY_MAX - 1);
    gboolean cut = g_utf8_strlen(summary, -1) > SUMMARY_MAX;
    text = g_strdup_printf(BLAME_GAP "%s, %s%s%s%s", author, ago, *summary ? " · " : "", cut ? subject : summary,
                           cut ? "…" : "");
    g_free(subject);
    g_free(ago);
  }
  g_strfreev(lines);
  return text;
}

static void on_blamed(GObject *source, GAsyncResult *result, gpointer data)
{
  BlameAsking *asking = data;
  char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, NULL, NULL);

  if (!g_cancellable_is_cancelled(asking->cancel)) {
    Blame *b = asking->blame;
    g_clear_object(&b->asking);
    if (ok && g_subprocess_get_successful(G_SUBPROCESS(source)) && asking->changes == b->tab->view->changes) {
      b->line = asking->line;
      b->text = blame_line_text(out);
      gtk_widget_queue_draw(GTK_WIDGET(b->tab->source));
    }
  }
  g_object_unref(asking->cancel);
  g_free(asking);
  g_free(out);
}

static gboolean blame_rested(gpointer data)
{
  Blame *b = data;
  GtkTextIter cursor, start, end;
  GSubprocess *git;

  b->rest_id = 0;
  if (!blame_wanted(b) || b->tab->view->loading)
    return G_SOURCE_REMOVE;
  gtk_text_buffer_get_iter_at_mark(b->buffer, &cursor, gtk_text_buffer_get_insert(b->buffer));
  int line = gtk_text_iter_get_line(&cursor);
  char *range = g_strdup_printf("%d,%d", line + 1, line + 1), *name = g_path_get_basename(b->tab->path);
  char *folder = g_path_get_dirname(b->tab->path);
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_set_cwd(launcher, folder);
  git = g_subprocess_launcher_spawn(launcher, NULL, "git", "blame", "--porcelain", "-L", range, "--contents", "-", "--",
                                    name, NULL);
  if (git) {
    BlameAsking *asking = g_new0(BlameAsking, 1);
    char *text;
    gtk_text_buffer_get_bounds(b->buffer, &start, &end);
    text = gtk_text_buffer_get_text(b->buffer, &start, &end, TRUE);
    b->asking = g_cancellable_new();
    asking->blame = b;
    asking->cancel = g_object_ref(b->asking);
    asking->changes = b->tab->view->changes;
    asking->line = line;
    g_subprocess_communicate_utf8_async(git, text, b->asking, on_blamed, asking);
    g_free(text);
    g_object_unref(git);
  }
  g_object_unref(launcher);
  g_free(folder);
  g_free(name);
  g_free(range);
  return G_SOURCE_REMOVE;
}

/* What is shown is wrong now: it goes, and the line is asked about once the cursor rests. */
static void blame_later(Blame *b)
{
  blame_clear(b);
  g_clear_handle_id(&b->rest_id, g_source_remove);
  if (blame_wanted(b))
    b->rest_id = g_timeout_add(BLAME_MS, blame_rested, b);
}

static void on_blame_mark_set(GtkTextBuffer *buffer, const GtkTextIter *where, GtkTextMark *mark, Blame *b)
{
  if (mark == gtk_text_buffer_get_insert(buffer) && (gtk_text_iter_get_line(where) != b->line || !b->text))
    blame_later(b);
}

void blame_attach(Tab *tab)
{
  Blame *b = g_new0(Blame, 1);

  b->tab = tab;
  b->buffer = GTK_TEXT_BUFFER(tab->buffer);
  b->line = -1;
  g_signal_connect(b->buffer, "mark-set", G_CALLBACK(on_blame_mark_set), b);
  g_signal_connect_swapped(b->buffer, "changed", G_CALLBACK(blame_later), b);
  tab->view->blame = b;
}

void blame_free(Blame *b)
{
  if (!b)
    return;
  g_cancellable_cancel(b->asking);
  g_clear_object(&b->asking);
  g_clear_handle_id(&b->rest_id, g_source_remove);
  g_signal_handlers_disconnect_by_data(b->buffer, b);
  g_free(b->text);
  g_free(b);
}

void blame_apply_setting(Tab *tab)
{
  Blame *b = tab->view ? tab->view->blame : NULL;

  if (b && blame_wanted(b) != (b->text || b->rest_id || b->asking))
    blame_later(b);
}

void blame_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot)
{
  Blame *b = tab->view->blame;
  GtkTextIter end;
  GdkRectangle at;
  int height;

  if (!b || !b->text)
    return;
  gtk_text_buffer_get_iter_at_line(b->buffer, &end, b->line);
  if (!gtk_text_iter_ends_line(&end))
    gtk_text_iter_forward_to_line_end(&end);
  gtk_text_view_get_iter_location(view, &end, &at);
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(view), b->text);
  pango_layout_get_pixel_size(layout, NULL, &height);
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT((float)at.x, (float)(at.y + (at.height - height) / 2)));
  gtk_snapshot_append_layout(snapshot, layout, palette_rgba(PAL_FG_FAINT));
  gtk_snapshot_restore(snapshot);
  g_object_unref(layout);
}

const char *blame_text(Tab *tab)
{
  Blame *b = tab->view ? tab->view->blame : NULL;

  return b && b->text ? b->text + strlen(BLAME_GAP) : NULL;
}
