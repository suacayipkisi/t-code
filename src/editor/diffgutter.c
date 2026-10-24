/*
 * The diff gutter (see diffgutter.h): a GtkSourceGutterRenderer drawing each line's mark. The base
 * text comes from git show (HEAD or a commit) or from a file; the marks are worked out again
 * (git/linediff.c) a moment after the text or the base changes.
 */
#include "editor/diffgutter.h"

#include "editor/editor.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "git/linediff.h"
#include "window/palette.h"

#include <string.h>

#define UPDATE_MS 300 /* after the last change */
#define WIDTH 4       /* pixels */
#define CONTEXT 3     /* unchanged lines a comparison keeps around a change */
#define MIN_FOLDED 4  /* fewer unchanged lines than this stay shown */

typedef enum { BASE_HEAD, BASE_REV, BASE_FILE, BASE_NONE } BaseKind;

typedef struct Loading Loading;

struct DiffGutter {
  Tab *tab;
  GtkWidget *renderer;
  BaseKind kind;
  char *value;           /* the commit, or the file */
  char *base;            /* its text; NULL: no marks (not in git, a new file) */
  GArray *marks;         /* guint8 LineMark per line, NULL before the first */
  Tab *partner;          /* the other tab in a side-by-side comparison, or NULL */
  gboolean is_old_side;  /* TRUE for the right-hand (older) tab in a comparison */
  GArray *gaps;          /* DiffGap per hunk gap when partner != NULL */
  GPtrArray *gap_tags;   /* GtkTextTag* created for the current gaps */
  int wrap_width;        /* the width the gaps were made for with word wrap (0: no wrap) */
  GArray *changed;       /* guint8 per line: TRUE where the line differs from the other side */
  GArray *folds;         /* Folded: the unchanged stretches a comparison hides, in line order */
  int fold_px;           /* the height of the row standing for each of them */
  gboolean show_all;     /* the comparison hides no unchanged lines (kept on the new side) */
  char *paired;          /* a checksum of what the gaps were made from: the same again changes nothing */
  guint relayout_id;     /* the gaps are made again (the view's width changed) */
  gboolean jumped;       /* a comparison went to its first change once */
  gboolean base_pending; /* git show waits for the tab to be shown */
  guint update_id;
  Loading *loading; /* the git show in flight */
};

/* count lines from line, folded away; a row above line + count stands for them. */
typedef struct {
  int line, count;
} Folded;

static void sync_pair_gaps(DiffGutter *g);
static gboolean hidden_in(DiffGutter *g, int line);
static DiffGutter *new_side(DiffGutter *g);
static void show_all_lines(DiffGutter *g);

/* A git show; dropped when gutter is NULL (the editor went, or another base was asked for). */
struct Loading {
  DiffGutter *gutter;
};

/* The renderer ----------------------------------------------------------------------------- */

struct _TcodeDiffRenderer {
  GtkSourceGutterRenderer parent;
  DiffGutter *gutter;
};

#define TCODE_TYPE_DIFF_RENDERER (tcode_diff_renderer_get_type())
G_DECLARE_FINAL_TYPE(TcodeDiffRenderer, tcode_diff_renderer, TCODE, DIFF_RENDERER, GtkSourceGutterRenderer)
G_DEFINE_FINAL_TYPE(TcodeDiffRenderer, tcode_diff_renderer, GTK_SOURCE_TYPE_GUTTER_RENDERER)

/* The gutter's added, modified and deleted colors. */

static void snapshot_line(GtkSourceGutterRenderer *renderer, GtkSnapshot *snapshot, GtkSourceGutterLines *lines,
                          guint line)
{
  DiffGutter *g = TCODE_DIFF_RENDERER(renderer)->gutter;
  double y, height;
  int width = gtk_widget_get_width(GTK_WIDGET(renderer));
  guint8 mark;

  if (!g || !g->marks || line >= g->marks->len || !(mark = g_array_index(g->marks, guint8, line)))
    return;
  gtk_source_gutter_lines_get_line_extent(lines, line, GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_FIRST, &y, &height);
  if (mark == LINE_DELETED_ABOVE) { /* a small triangle at the line's top */
    GskPathBuilder *builder = gsk_path_builder_new();
    GskPath *path;
    gsk_path_builder_move_to(builder, 0, y - 4);
    gsk_path_builder_line_to(builder, width, y);
    gsk_path_builder_line_to(builder, 0, y + 4);
    gsk_path_builder_close(builder);
    path = gsk_path_builder_free_to_path(builder);
    gtk_snapshot_append_fill(snapshot, path, GSK_FILL_RULE_WINDING, palette_rgba(PAL_DIFF_DELETED));
    gsk_path_unref(path);
  } else {
    const GdkRGBA *color = palette_rgba(mark != LINE_ADDED             ? PAL_DIFF_MODIFIED
                                        : g->partner && g->is_old_side ? PAL_DIFF_DELETED
                                                                       : PAL_DIFF_ADDED);
    gtk_snapshot_append_color(snapshot, color, &GRAPHENE_RECT_INIT(0, y, MIN(width, 3), height));
  }
}

static void tcode_diff_renderer_class_init(TcodeDiffRendererClass *klass)
{
  GTK_SOURCE_GUTTER_RENDERER_CLASS(klass)->snapshot_line = snapshot_line;
}

static void tcode_diff_renderer_init(TcodeDiffRenderer *self)
{
  gtk_widget_set_size_request(GTK_WIDGET(self), WIDTH, -1);
}

/* Marks ------------------------------------------------------------------------------------- */

static gboolean update_marks_now(gpointer data)
{
  DiffGutter *g = data;
  GtkTextIter start, end;
  char *text;

  g->update_id = 0;
  g_clear_pointer(&g->marks, g_array_unref);
  if (g->base) {
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(g->tab->buffer), &start, &end);
    text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(g->tab->buffer), &start, &end, TRUE);
    g->marks = line_diff(g->base, text);
    g_free(text);
  }
  if (g->partner) {
    DiffGutter *ng = new_side(g);
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(ng->tab->buffer);
    sync_pair_gaps(g);
    gtk_text_buffer_get_iter_at_mark(buffer, &start, gtk_text_buffer_get_insert(buffer));
    if (ng->jumped && hidden_in(ng, gtk_text_iter_get_line(&start))) /* an edit put it there (undone) */
      show_all_lines(ng);
  }
  gtk_widget_queue_draw(g->renderer);
  editor_view_show_problems(g->tab); /* the strip beside the minimap marks them too */
  return G_SOURCE_REMOVE;
}

static void update_marks_later(DiffGutter *g)
{
  if (!g->update_id)
    g->update_id = g_timeout_add(UPDATE_MS, update_marks_now, g);
}

static void on_text_changed(GtkTextBuffer *buffer G_GNUC_UNUSED, DiffGutter *g)
{
  if (!g->tab->view->loading)
    update_marks_later(g);
}

/* The base ----------------------------------------------------------------------------------- */

static void set_base(DiffGutter *g, char *text)
{
  g_free(g->base);
  g->base = text;
  update_marks_later(g);
}

static void on_shown(GObject *source, GAsyncResult *result, gpointer data)
{
  Loading *loading = data;
  DiffGutter *g = loading->gutter;
  char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, NULL, NULL) &&
                g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
                g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0;

  if (g && g->loading == loading) {
    g->loading = NULL;
    set_base(g, ok ? g_steal_pointer(&out) : NULL);
  }
  g_free(out);
  g_free(loading);
}

/* Reads the base again: git show in the background, or the file. */
static void load_base(DiffGutter *g)
{
  char *dir, *name, *spec;
  GSubprocess *process;

  if (g->loading) {
    g->loading->gutter = NULL;
    g->loading = NULL;
  }
  if (g->kind == BASE_NONE || !g->tab->path) {
    set_base(g, NULL);
    return;
  }
  if (g->kind == BASE_FILE) {
    char *text = NULL;
    g_file_get_contents(g->value, &text, NULL, NULL);
    set_base(g, text);
    return;
  }
  dir = g_path_get_dirname(g->tab->path);
  name = g_path_get_basename(g->tab->path);
  spec = g_strconcat(g->kind == BASE_HEAD ? "HEAD" : g->value, ":./", name, NULL);
  process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "git", "-C", dir,
                             "show", spec, NULL);
  if (process) {
    g->loading = g_new0(Loading, 1);
    g->loading->gutter = g;
    g_subprocess_communicate_utf8_async(process, NULL, NULL, on_shown, g->loading);
    g_object_unref(process);
  }
  g_free(spec);
  g_free(name);
  g_free(dir);
}

/* Reads the base again, now or (git show, for a tab not shown: a restored session would start one
 * git per tab) once the tab is shown. */
static void load_base_when_shown(DiffGutter *g)
{
  g->base_pending = (g->kind == BASE_HEAD || g->kind == BASE_REV) && !gtk_widget_get_mapped(GTK_WIDGET(g->tab->source));
  if (!g->base_pending)
    load_base(g);
}

static void set_kind(Tab *tab, BaseKind kind, const char *value)
{
  DiffGutter *g = tab->view->diff;

  g->kind = kind;
  g_free(g->value);
  g->value = g_strdup(value);
  if (!tab->view->loading)
    load_base_when_shown(g);
}

void diff_gutter_follow_head(Tab *tab)
{
  set_kind(tab, BASE_HEAD, NULL);
}

void diff_gutter_compare_rev(Tab *tab, const char *rev)
{
  set_kind(tab, rev ? BASE_REV : BASE_NONE, rev);
}

void diff_gutter_compare_file(Tab *tab, const char *path)
{
  set_kind(tab, path ? BASE_FILE : BASE_NONE, path);
}

#define CHANGED_WORDS "tcode-changed-words"
#define UNCHANGED "tcode-unchanged" /* the unchanged lines a comparison hides */

/* Takes the gap rows and the changed lines' and words' colors out of the buffer (the tags that make
 * them). The view is not touched: it may be gone already (the tab closing). */
static void remove_gap_tags(DiffGutter *g)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(g->tab->buffer);
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(buffer, &start, &end);
  if (gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), CHANGED_WORDS))
    gtk_text_buffer_remove_tag_by_name(buffer, CHANGED_WORDS, &start, &end);
  if (g->folds && gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), UNCHANGED))
    gtk_text_buffer_remove_tag_by_name(buffer, UNCHANGED, &start, &end);
  g_clear_pointer(&g->changed, g_array_unref);
  g_clear_pointer(&g->folds, g_array_unref);
  g_clear_pointer(&g->paired, g_free);
  if (g->gap_tags) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(g->tab->buffer));
    for (guint i = 0; i < g->gap_tags->len; i++)
      gtk_text_tag_table_remove(table, g_ptr_array_index(g->gap_tags, i));
    g_ptr_array_set_size(g->gap_tags, 0);
  }
  g_clear_pointer(&g->gaps, g_array_unref);
}

/* The same, and the view draws again. */
static void clear_side_gaps(DiffGutter *g)
{
  remove_gap_tags(g);
  gtk_widget_queue_draw(GTK_WIDGET(g->tab->source));
}

static void apply_side_gaps(DiffGutter *g, GArray *gaps)
{
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(g->tab->buffer);

  if (!g->gap_tags)
    g->gap_tags = g_ptr_array_new();
  g->gaps = gaps;
  for (guint i = 0; i < gaps->len; i++) {
    DiffGap *gap = &g_array_index(gaps, DiffGap, i);
    GtkTextTag *tag = gtk_text_buffer_create_tag(buf, NULL, gap->above ? "pixels-above-lines" : "pixels-below-lines",
                                                 gap->count + gap->pad, NULL);
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(buf, &start, gap->line);
    end = start;
    if (!gtk_text_iter_ends_line(&end))
      gtk_text_iter_forward_to_line_end(&end);
    else
      gtk_text_iter_forward_char(&end);
    gtk_text_buffer_apply_tag(buf, tag, &start, &end);
    g_ptr_array_add(g->gap_tags, tag);
  }
  gtk_widget_queue_draw(GTK_WIDGET(g->tab->source));
}

/* Hides the unchanged stretches of folds (this side's lines) and puts a row of one line's height
 * (line_px) above the line after each, where the view draws how many lines it stands for. */
static void apply_side_folds(DiffGutter *g, GArray *folds, int line_px)
{
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(g->tab->buffer);
  GtkTextTag *hidden = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf), UNCHANGED);

  if (!hidden)
    hidden = gtk_text_buffer_create_tag(buf, UNCHANGED, "invisible", TRUE, NULL);
  g->folds = g_array_new(FALSE, FALSE, sizeof(Folded));
  g->fold_px = line_px;
  for (guint i = 0; i < folds->len; i++) {
    DiffFold *fold = &g_array_index(folds, DiffFold, i);
    Folded folded = { (int)(g->is_old_side ? fold->old_line : fold->new_line), (int)fold->count };
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(buf, &start, folded.line);
    gtk_text_buffer_get_iter_at_line(buf, &end, folded.line + folded.count);
    gtk_text_buffer_apply_tag(buf, hidden, &start, &end);
    start = end;
    if (!gtk_text_iter_ends_line(&end))
      gtk_text_iter_forward_to_line_end(&end);
    GtkTextTag *row = gtk_text_buffer_create_tag(buf, NULL, "pixels-above-lines", line_px, NULL);
    gtk_text_buffer_apply_tag(buf, row, &start, &end);
    g_ptr_array_add(g->gap_tags, row);
    g_array_append_val(g->folds, folded);
  }
}

/* The fold that hides line or ends right above it (or the next one), as an index into g->folds. */
static guint fold_from(DiffGutter *g, int line)
{
  guint low = 0, high = g->folds ? g->folds->len : 0;

  while (low < high) {
    guint middle = (low + high) / 2;
    Folded *f = &g_array_index(g->folds, Folded, middle);
    if (f->line + f->count < line)
      low = middle + 1;
    else
      high = middle;
  }
  return low;
}

static gboolean hidden_in(DiffGutter *g, int line)
{
  guint i = fold_from(g, line);
  Folded *f = g->partner && g->folds && i < g->folds->len ? &g_array_index(g->folds, Folded, i) : NULL;

  return f && line >= f->line && line < f->line + f->count;
}

/* The width view wraps its lines at: its text area without the margins, and without the pixel
 * GtkTextView keeps for the cursor at the end of a row (SPACE_FOR_CURSOR). Without that pixel, a line
 * that just fits was one row here and two in the view, and every line below it out of step. */
static int wrap_width(GtkTextView *view)
{
  GdkRectangle visible;

  gtk_text_view_get_visible_rect(view, &visible);
  return visible.width - 1 - gtk_text_view_get_left_margin(view) - gtk_text_view_get_right_margin(view);
}

/* The height of one row of text in view's font. */
static int row_height(GtkTextView *view)
{
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(view), "x");
  int height;

  pango_layout_get_size(layout, NULL, &height);
  g_object_unref(layout);
  return PANGO_PIXELS(height);
}

/* The height in pixels each line of text takes in tab's view, as the view lays it out: with word
 * wrap at its width, font and tab stops (a line's height is rounded once, for all its rows: three
 * rows are not three times one). *width_out: the wrap width (0: no wrap). */
static int *line_heights(Tab *tab, const char *text, int *width_out)
{
  GtkTextView *view = GTK_TEXT_VIEW(tab->source);
  GtkWrapMode mode = gtk_text_view_get_wrap_mode(view);
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(view), "x");
  guint n = line_count(text), i = 0;
  int *heights = g_new(int, MAX(n, 1)), width, one;
  PangoTabArray *tabs;

  width = wrap_width(view);
  *width_out = mode == GTK_WRAP_NONE ? 0 : MAX(width, 0);
  pango_layout_get_size(layout, NULL, &one);
  one = PANGO_PIXELS(one);
  if (mode == GTK_WRAP_NONE || width <= 0) { /* every line one row */
    for (i = 0; i < n; i++)
      heights[i] = one;
    g_object_unref(layout);
    return heights;
  }
  pango_layout_set_width(layout, width * PANGO_SCALE);
  pango_layout_set_wrap(layout, mode == GTK_WRAP_CHAR   ? PANGO_WRAP_CHAR
                                : mode == GTK_WRAP_WORD ? PANGO_WRAP_WORD
                                                        : PANGO_WRAP_WORD_CHAR);
  if ((tabs = gtk_text_view_get_tabs(view))) {
    pango_layout_set_tabs(layout, tabs);
    pango_tab_array_free(tabs);
  }
  for (const char *p = text; i < n; i++) {
    const char *nl = strchr(p, '\n');
    int height;
    pango_layout_set_text(layout, p, nl ? (int)(nl - p) : -1);
    pango_layout_get_size(layout, NULL, &height);
    heights[i] = pango_layout_get_line_count(layout) > 1 ? PANGO_PIXELS(height) : one;
    p = nl ? nl + 1 : p + strlen(p);
  }
  g_object_unref(layout);
  return heights;
}

/* Marks the changed lines (guides.c tints them) and colors their changed words: green
 * on the new side, red on the old one. */
static void apply_side_changes(DiffGutter *g, GArray *spans)
{
  /* Inserted and removed text backgrounds. */
  GdkRGBA inserted = *palette_rgba(PAL_DIFF_ADDED), removed = *palette_rgba(PAL_DIFF_DELETED);
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(g->tab->buffer);
  GtkTextTag *words = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), CHANGED_WORDS);
  int lines = gtk_text_buffer_get_line_count(buffer);

  inserted.alpha = 0.28f;
  removed.alpha = 0.30f;
  if (!words)
    words = gtk_text_buffer_create_tag(buffer, CHANGED_WORDS, NULL);
  g_object_set(words, "background-rgba", g->is_old_side ? &removed : &inserted, NULL);
  g->changed = g_array_new(FALSE, TRUE, sizeof(guint8));
  g_array_set_size(g->changed, lines);
  for (guint i = 0; i < spans->len; i++) {
    DiffSpan *span = &g_array_index(spans, DiffSpan, i);
    GtkTextIter start, end;
    if (span->line >= lines)
      continue;
    if (span->end < 0) {
      g_array_index(g->changed, guint8, span->line) = TRUE;
      continue;
    }
    gtk_text_buffer_get_iter_at_line(buffer, &end, span->line);
    if (!gtk_text_iter_ends_line(&end))
      gtk_text_iter_forward_to_line_end(&end);
    if (span->end > gtk_text_iter_get_line_index(&end)) /* the text changed meanwhile */
      continue;
    gtk_text_buffer_get_iter_at_line_index(buffer, &start, span->line, span->start);
    gtk_text_buffer_get_iter_at_line_index(buffer, &end, span->line, span->end);
    gtk_text_buffer_apply_tag(buffer, words, &start, &end);
  }
  g_array_unref(spans);
}

gboolean diff_gutter_line_changed(Tab *tab, int line)
{
  DiffGutter *g = tab && tab->view ? tab->view->diff : NULL;

  return g && g->partner && g->changed && line >= 0 && (guint)line < g->changed->len &&
         g_array_index(g->changed, guint8, line);
}

/* A checksum of what a comparison's gaps are made from. */
static char *pair_key(const char *new_text, const char *old_text, const int *new_heights, guint new_lines,
                      const int *old_heights, guint old_lines, gboolean show_all)
{
  GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
  char *key;

  g_checksum_update(sum, (const guchar *)new_text, (gssize)strlen(new_text) + 1); /* with its '\0' */
  g_checksum_update(sum, (const guchar *)old_text, (gssize)strlen(old_text) + 1);
  g_checksum_update(sum, (const guchar *)new_heights, (gssize)(new_lines * sizeof(int)));
  g_checksum_update(sum, (const guchar *)old_heights, (gssize)(old_lines * sizeof(int)));
  g_checksum_update(sum, (const guchar *)&show_all, sizeof show_all);
  key = g_strdup(g_checksum_get_string(sum));
  g_checksum_free(sum);
  return key;
}

/* Both sides' gap rows, changed lines and words, and folded unchanged lines, made again (each tag
 * change lays the views out again: nothing happens when nothing they depend on changed). */
static void sync_pair_gaps(DiffGutter *g)
{
  if (!g || !g->partner)
    return;
  Tab *new_tab = g->is_old_side ? g->partner : g->tab;
  Tab *old_tab = g->is_old_side ? g->tab : g->partner;
  if (!new_tab->view || !old_tab->view || new_tab->view->loading || old_tab->view->loading)
    return;
  DiffGutter *ng = new_tab->view->diff, *og = old_tab->view->diff;
  GtkTextIter s1, e1, s2, e2;

  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(new_tab->buffer), &s1, &e1);
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(old_tab->buffer), &s2, &e2);
  char *new_text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(new_tab->buffer), &s1, &e1, TRUE);
  char *old_text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(old_tab->buffer), &s2, &e2, TRUE);

  int *new_heights = line_heights(new_tab, new_text, &ng->wrap_width);
  int *old_heights = line_heights(old_tab, old_text, &og->wrap_width);
  guint new_lines = line_count(new_text);
  char *key = pair_key(new_text, old_text, new_heights, new_lines, old_heights, line_count(old_text), ng->show_all);

  if (g_strcmp0(key, ng->paired) != 0 || g_strcmp0(key, og->paired) != 0) {
    GArray *hunks = line_diff_hunks(old_text, new_text);
    GArray *folds = ng->show_all ? g_array_new(FALSE, FALSE, sizeof(DiffFold))
                                 : line_diff_folds(hunks, new_lines, CONTEXT, MIN_FOLDED);
    int row = row_height(GTK_TEXT_VIEW(new_tab->source));
    clear_side_gaps(ng);
    clear_side_gaps(og);
    ng->paired = key;
    og->paired = g_strdup(key);
    apply_side_gaps(ng, line_diff_gaps(old_text, new_text, hunks, old_heights, new_heights, FALSE));
    apply_side_gaps(og, line_diff_gaps(old_text, new_text, hunks, old_heights, new_heights, TRUE));
    apply_side_folds(ng, folds, row);
    apply_side_folds(og, folds, row);
    apply_side_changes(ng, line_diff_spans(old_text, new_text, hunks, FALSE));
    apply_side_changes(og, line_diff_spans(old_text, new_text, hunks, TRUE));
    g_array_unref(folds);
    g_array_unref(hunks);
  } else {
    g_free(key);
  }
  if (!ng->jumped) { /* a comparison opens at its first change, not at the file's top */
    int first = -1;
    for (guint i = 0; ng->changed && i < ng->changed->len && first < 0; i++)
      if (g_array_index(ng->changed, guint8, i))
        first = (int)i;
    for (guint i = 0; ng->gaps && i < ng->gaps->len; i++) { /* lines deleted there */
      DiffGap *gap = &g_array_index(ng->gaps, DiffGap, i);
      if (gap->count && (first < 0 || gap->line < first))
        first = gap->above ? 0 : gap->line;
    }
    ng->jumped = TRUE;
    if (first >= 0)
      editor_goto(new_tab, first + 1, 1);
  }
  g_free(new_heights);
  g_free(old_heights);
  g_free(new_text);
  g_free(old_text);
}

void diff_gutter_pair(Tab *new_tab, Tab *old_tab)
{
  if (new_tab && new_tab->view && new_tab->view->diff) {
    DiffGutter *ng = new_tab->view->diff;
    if (ng->partner && ng->partner != old_tab && ng->partner->view && ng->partner->view->diff) {
      clear_side_gaps(ng->partner->view->diff);
      ng->partner->view->diff->partner = NULL;
    }
    if (!old_tab) {
      clear_side_gaps(ng);
      ng->partner = NULL;
      return;
    }
  }
  if (!new_tab || !old_tab || !new_tab->view || !old_tab->view)
    return;
  DiffGutter *ng = new_tab->view->diff, *og = old_tab->view->diff;
  ng->partner = old_tab;
  ng->is_old_side = FALSE;
  ng->jumped = FALSE;
  ng->show_all = FALSE;
  og->partner = new_tab;
  og->is_old_side = TRUE;
  sync_pair_gaps(ng);
}

gboolean diff_gutter_mark(Tab *tab, int line, PalColor *color)
{
  DiffGutter *g = tab && tab->view ? tab->view->diff : NULL;
  guint8 mark;

  if (!g || !g->marks || line < 0 || (guint)line >= g->marks->len || !(mark = g_array_index(g->marks, guint8, line)))
    return FALSE;
  *color = mark == LINE_DELETED_ABOVE     ? PAL_DIFF_DELETED
           : mark != LINE_ADDED           ? PAL_DIFF_MODIFIED
           : g->partner && g->is_old_side ? PAL_DIFF_DELETED
                                          : PAL_DIFF_ADDED; /* as the gutter draws it */
  return TRUE;
}

int diff_gutter_line_total(Tab *tab)
{
  DiffGutter *g = tab && tab->view ? tab->view->diff : NULL;

  return g && g->marks ? (int)g->marks->len : 0;
}

gboolean diff_gutter_line_gaps(Tab *tab, int line, int *above_px, int *below_px)
{
  *above_px = *below_px = 0;
  if (!tab || !tab->view || !tab->view->diff || !tab->view->diff->partner || !tab->view->diff->gaps)
    return FALSE;
  DiffGutter *g = tab->view->diff;
  guint low = 0, high = g->gaps->len;

  while (low < high) { /* the gaps are in line order: the first one at line or after */
    guint middle = (low + high) / 2;
    if (g_array_index(g->gaps, DiffGap, middle).line < line)
      low = middle + 1;
    else
      high = middle;
  }
  for (guint i = low; i < g->gaps->len && g_array_index(g->gaps, DiffGap, i).line == line; i++) {
    DiffGap *gap = &g_array_index(g->gaps, DiffGap, i);
    if (gap->above)
      *above_px += gap->count + gap->pad;
    else
      *below_px += gap->count + gap->pad;
  }
  int row_px;
  if (diff_gutter_line_folded(tab, line, &row_px))
    *above_px += row_px;
  return !g->is_old_side;
}

int diff_gutter_line_folded(Tab *tab, int line, int *row_px)
{
  DiffGutter *g = tab && tab->view ? tab->view->diff : NULL;
  guint i = g && g->partner ? fold_from(g, line) : 0;
  Folded *f = g && g->partner && g->folds && i < g->folds->len ? &g_array_index(g->folds, Folded, i) : NULL;

  *row_px = f && f->line + f->count == line ? g->fold_px : 0;
  return *row_px ? f->count : 0;
}

gboolean diff_gutter_line_hidden(Tab *tab, int line)
{
  return tab && tab->view && tab->view->diff && hidden_in(tab->view->diff, line);
}

/* The comparison's new side (the gutter holding whether it shows every line). */
static DiffGutter *new_side(DiffGutter *g)
{
  return g->is_old_side && g->partner && g->partner->view ? g->partner->view->diff : g;
}

static void show_all_lines(DiffGutter *g)
{
  g = new_side(g);
  if (!g->show_all) {
    g->show_all = TRUE;
    sync_pair_gaps(g);
  }
}

void diff_gutter_toggle_unchanged(Tab *tab)
{
  DiffGutter *g = tab && tab->view ? tab->view->diff : NULL;
  GtkTextIter cursor;

  if (!g || !g->partner)
    return;
  g = new_side(g);
  g->show_all = !g->show_all;
  sync_pair_gaps(g);
  /* The cursor stays in sight: at the line after the lines hidden around it. */
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(g->tab->buffer);
  gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
  if (hidden_in(g, gtk_text_iter_get_line(&cursor))) {
    Folded *f = &g_array_index(g->folds, Folded, fold_from(g, gtk_text_iter_get_line(&cursor)));
    gtk_text_buffer_get_iter_at_line(buffer, &cursor, f->line + f->count);
    gtk_text_buffer_place_cursor(buffer, &cursor);
  }
}

/* The cursor went into hidden lines: the comparison shows them all. */
static void on_cursor_into_folded(GtkTextBuffer *buffer, const GtkTextIter *where, GtkTextMark *mark, DiffGutter *g)
{
  if (mark == gtk_text_buffer_get_insert(buffer) && g->partner && g->folds && g->folds->len &&
      hidden_in(g, gtk_text_iter_get_line(where)))
    show_all_lines(g);
}

/* A click on the row standing for hidden lines shows them all. */
static void on_view_pressed(GtkGestureClick *click, int n G_GNUC_UNUSED, double x, double y, DiffGutter *g)
{
  GtkTextView *view = GTK_TEXT_VIEW(g->tab->source);
  GtkTextIter at;
  int bx, by, line, top, height, row_px;

  if (!g->partner || !g->folds || !g->folds->len)
    return;
  gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
  gtk_text_view_get_line_at_y(view, &at, by, NULL);
  line = gtk_text_iter_get_line(&at);
  while (hidden_in(g, line)) /* hidden lines take no height: the row is the next shown line's */
    line++;
  if (!diff_gutter_line_folded(g->tab, line, &row_px))
    return;
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(g->tab->buffer), &at, line);
  gtk_text_view_get_line_yrange(view, &at, &top, &height);
  if (by < top || by >= top + row_px)
    return;
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  show_all_lines(g);
}

int diff_gutter_line_pad(Tab *tab, int line)
{
  DiffGutter *g = tab && tab->view ? tab->view->diff : NULL;

  for (guint i = 0; g && g->partner && g->gaps && i < g->gaps->len; i++) {
    DiffGap *gap = &g_array_index(g->gaps, DiffGap, i);
    if (gap->line == line && !gap->above)
      return gap->pad;
    if (gap->line > line)
      break;
  }
  return 0;
}

static gboolean relayout_now(gpointer data)
{
  DiffGutter *g = data;

  g->relayout_id = 0;
  sync_pair_gaps(g);
  return G_SOURCE_REMOVE;
}

void diff_gutter_check_width(Tab *tab)
{
  DiffGutter *g = tab && tab->view ? tab->view->diff : NULL;
  GtkTextView *view;
  int width;

  if (!g || !g->partner || g->relayout_id)
    return;
  view = GTK_TEXT_VIEW(tab->source);
  width = wrap_width(view);
  if (gtk_text_view_get_wrap_mode(view) == GTK_WRAP_NONE)
    width = 0;
  if (width != g->wrap_width)
    g->relayout_id = g_idle_add(relayout_now, g);
}

static void on_source_mapped(GtkWidget *widget G_GNUC_UNUSED, DiffGutter *g)
{
  if (g->base_pending) {
    g->base_pending = FALSE;
    load_base(g);
  }
  if (g->partner)
    sync_pair_gaps(g);
}

void diff_gutter_loaded(Tab *tab)
{
  load_base_when_shown(tab->view->diff);
  if (tab->view->diff->partner)
    sync_pair_gaps(tab->view->diff);
}

void diff_gutter_head_moved(App *t)
{
  int n = tab_count(t);

  for (int i = 0; i < n; i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR && tab->view->diff->kind == BASE_HEAD && !tab->view->loading)
      load_base_when_shown(tab->view->diff);
  }
}

void diff_gutter_attach(Tab *tab)
{
  DiffGutter *g = g_new0(DiffGutter, 1);
  GtkSourceGutter *gutter = gtk_source_view_get_gutter(tab->source, GTK_TEXT_WINDOW_LEFT);

  g->tab = tab;
  g->kind = BASE_HEAD;
  g->renderer = g_object_ref_sink(g_object_new(TCODE_TYPE_DIFF_RENDERER, NULL)); /* ours too: see below */
  TCODE_DIFF_RENDERER(g->renderer)->gutter = g;
  /* Right of the line numbers (-30) and the marks (-20). */
  gtk_source_gutter_insert(gutter, GTK_SOURCE_GUTTER_RENDERER(g->renderer), -10);
  g_signal_connect(tab->buffer, "changed", G_CALLBACK(on_text_changed), g);
  g_signal_connect(tab->buffer, "mark-set", G_CALLBACK(on_cursor_into_folded), g);
  g_signal_connect(tab->source, "map", G_CALLBACK(on_source_mapped), g);
  GtkGesture *click = gtk_gesture_click_new();
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE); /* before the view's */
  g_signal_connect(click, "pressed", G_CALLBACK(on_view_pressed), g);
  gtk_widget_add_controller(GTK_WIDGET(tab->source), GTK_EVENT_CONTROLLER(click));
  tab->view->diff = g;
}

void diff_gutter_free(DiffGutter *g)
{
  if (!g)
    return;
  if (g->partner && g->partner->view && g->partner->view->diff) { /* the other side stays: no gaps there */
    clear_side_gaps(g->partner->view->diff);
    g->partner->view->diff->partner = NULL;
  }
  remove_gap_tags(g);
  if (g->gap_tags)
    g_ptr_array_free(g->gap_tags, TRUE);
  /* The view's "map" handler went with the view, which is gone (the page goes first). */
  g_signal_handlers_disconnect_by_data(g->tab->buffer, g);
  /* The page went first: the renderer lives on with our reference, and must not draw for us. */
  TCODE_DIFF_RENDERER(g->renderer)->gutter = NULL;
  g_object_unref(g->renderer);
  if (g->update_id)
    g_source_remove(g->update_id);
  if (g->relayout_id)
    g_source_remove(g->relayout_id);
  if (g->loading)
    g->loading->gutter = NULL;
  if (g->marks)
    g_array_unref(g->marks);
  g_free(g->base);
  g_free(g->value);
  g_free(g);
}
