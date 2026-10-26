/*
 * Code folding (see folding.h). A fold is two marks, at the start of the line after the block's head
 * and at the start of its closing line; one invisible tag hides what is between the marks of every
 * fold. The arrows are a GtkSourceGutterRenderer right of the diff marks.
 */
#include "editor/gutter/folding.h"

#include "editor/text/blocks.h"
#include "editor/gutter/diffgutter.h"
#include "editor/editorview.h"
#include "editor/decor/sticky.h"
#include "window/palette.h"

#include <string.h>

#define FOLD_WIDTH 16 /* pixels */
#define CHECK_MS 300  /* after a change: folds whose block went are dropped */
#define BLOCKS_MS 250 /* after the last change the blocks are listed again (a big file takes ms) */
#define HIDDEN "tcode-folded"

typedef struct {
  GtkTextMark *from, *to;
} Fold;

struct Folding {
  Tab *tab;
  GtkWidget *renderer;
  GtkWidget *numbers; /* the line numbers (TcodeLineNumbers) */
  int digits;         /* the numbers' width is made for */
  GArray *blocks;     /* Block, for the text with this change count: */
  guint blocks_changes;
  GArray *folds;  /* Fold */
  gboolean hover; /* the pointer is over the gutter */
  guint check_id;
  guint blocks_id; /* the blocks are listed again shortly */
};

/* Blocks ------------------------------------------------------------------------------------- */

/* The blocks of the text as it is now. */
static GArray *fresh_blocks(Tab *tab)
{
  Folding *f = tab->view->folding;

  if (!f->blocks || f->blocks_changes != tab->view->changes) {
    GtkTextIter start, end;
    char *text;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
    text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
    if (f->blocks)
      g_array_free(f->blocks, TRUE);
    f->blocks = blocks_of(text);
    f->blocks_changes = tab->view->changes;
    g_free(text);
  }
  return f->blocks;
}

/* Typing has stopped: the arrows and the sticky lines follow the new blocks. */
static gboolean list_blocks_now(gpointer data)
{
  Folding *f = data;

  f->blocks_id = 0;
  fresh_blocks(f->tab);
  gtk_widget_queue_draw(f->renderer);
  sticky_apply_settings(f->tab);
  return G_SOURCE_REMOVE;
}

/* While typing, the last list: the arrows and the sticky lines are drawn after every key, and
 * listing a big file's blocks each time made typing in it four times slower. */
GArray *folding_blocks(Tab *tab)
{
  Folding *f = tab->view->folding;

  if (!f->blocks)
    return fresh_blocks(tab);
  if (f->blocks_changes != tab->view->changes && !f->blocks_id)
    f->blocks_id = g_timeout_add(BLOCKS_MS, list_blocks_now, f);
  return f->blocks;
}

/* The first block named on line that has lines to hide, or NULL. */
static Block *block_at(Tab *tab, int line)
{
  GArray *blocks = folding_blocks(tab);

  for (guint i = 0; i < blocks->len; i++) {
    Block *b = &g_array_index(blocks, Block, i);
    if (b->head == line && b->close_line > b->head + 1)
      return b;
    if (b->head > line)
      break;
  }
  return NULL;
}

/* Folds -------------------------------------------------------------------------------------- */

/* The line a fold's block is named on. */
static int fold_head(Tab *tab, Fold *fold)
{
  GtkTextIter from;

  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &from, fold->from);
  return gtk_text_iter_get_line(&from) - 1;
}

/* Whether line is folded away (its start is hidden). Its height in the view and the gutter is not 0,
 * it is the next shown line's. */
static gboolean line_hidden(Folding *f, int line)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(f->tab->buffer);
  GtkTextTag *hidden = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), HIDDEN);
  GtkTextIter start;

  if (!hidden || f->folds->len == 0)
    return FALSE;
  gtk_text_buffer_get_iter_at_line(buffer, &start, line);
  return gtk_text_iter_has_tag(&start, hidden);
}

/* Hides the folded lines again: the tag comes off everywhere and goes on each fold. */
static void apply_folds(Folding *f)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(f->tab->buffer);
  GtkTextTag *hidden = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), HIDDEN);
  GtkTextIter start, end;

  if (!hidden)
    hidden = gtk_text_buffer_create_tag(buffer, HIDDEN, "invisible", TRUE, NULL);
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gtk_text_buffer_remove_tag(buffer, hidden, &start, &end);
  for (guint i = 0; i < f->folds->len; i++) {
    Fold *fold = &g_array_index(f->folds, Fold, i);
    gtk_text_buffer_get_iter_at_mark(buffer, &start, fold->from);
    gtk_text_buffer_get_iter_at_mark(buffer, &end, fold->to);
    gtk_text_buffer_apply_tag(buffer, hidden, &start, &end);
  }
  gtk_widget_queue_draw(GTK_WIDGET(f->tab->source));
  gtk_widget_queue_draw(f->renderer);
  gtk_widget_queue_draw(f->numbers);
}

static void remove_fold(Folding *f, guint index)
{
  Fold *fold = &g_array_index(f->folds, Fold, index);

  gtk_text_buffer_delete_mark(GTK_TEXT_BUFFER(f->tab->buffer), fold->from);
  gtk_text_buffer_delete_mark(GTK_TEXT_BUFFER(f->tab->buffer), fold->to);
  g_array_remove_index(f->folds, index);
}

/* The fold of the block named on line, or -1. */
static int fold_on(Folding *f, int line)
{
  for (guint i = 0; i < f->folds->len; i++)
    if (fold_head(f->tab, &g_array_index(f->folds, Fold, i)) == line)
      return (int)i;
  return -1;
}

/* Folds block (not applied yet). */
static void add_fold(Folding *f, Block *block)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(f->tab->buffer);
  GtkTextIter from, to;
  Fold fold;

  if (fold_on(f, block->head) >= 0)
    return;
  gtk_text_buffer_get_iter_at_line(buffer, &from, block->head + 1);
  gtk_text_buffer_get_iter_at_line(buffer, &to, block->close_line);
  fold.from = gtk_text_buffer_create_mark(buffer, NULL, &from, TRUE);
  fold.to = gtk_text_buffer_create_mark(buffer, NULL, &to, FALSE);
  g_array_append_val(f->folds, fold);
}

/* Folds or unfolds the block named on line (a click on its arrow). */
static void toggle(Folding *f, int line)
{
  int index = fold_on(f, line);
  Block *block;

  fresh_blocks(f->tab); /* block_at sees the text as it is */
  if (index >= 0)
    remove_fold(f, index);
  else if ((block = block_at(f->tab, line)))
    add_fold(f, block);
  apply_folds(f);
}

/* The innermost block whose lines (its head to its closing line) hold line. */
static Block *block_around(Tab *tab, int line)
{
  GArray *blocks = fresh_blocks(tab);
  Block *inner = NULL;

  for (guint i = 0; i < blocks->len; i++) {
    Block *b = &g_array_index(blocks, Block, i);
    if (b->head > line)
      break;
    if (line <= b->close_line && b->close_line > b->head + 1)
      inner = b;
  }
  return inner;
}

static int cursor_line(Tab *tab)
{
  GtkTextIter cursor;

  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &cursor,
                                   gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer)));
  return gtk_text_iter_get_line(&cursor);
}

void fold_at_cursor(Tab *tab)
{
  Block *block = block_around(tab, cursor_line(tab));

  if (block && fold_on(tab->view->folding, block->head) < 0) {
    GtkTextIter head;
    add_fold(tab->view->folding, block);
    gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &head, block->head);
    gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &head); /* not left in hidden lines */
    apply_folds(tab->view->folding);
  }
}

void unfold_at_cursor(Tab *tab)
{
  Folding *f = tab->view->folding;
  int index = fold_on(f, cursor_line(tab));

  if (index < 0) {
    Block *block = block_around(tab, cursor_line(tab));
    index = block ? fold_on(f, block->head) : -1;
  }
  if (index >= 0) {
    remove_fold(f, index);
    apply_folds(f);
  }
}

void fold_all(Tab *tab)
{
  GArray *blocks = fresh_blocks(tab);
  GtkTextIter start;

  for (guint i = 0; i < blocks->len; i++)
    if (g_array_index(blocks, Block, i).close_line > g_array_index(blocks, Block, i).head + 1)
      add_fold(tab->view->folding, &g_array_index(blocks, Block, i));
  if (block_around(tab, cursor_line(tab))) { /* to the top, out of the hidden lines */
    gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(tab->buffer), &start);
    gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &start);
  }
  apply_folds(tab->view->folding);
}

void unfold_all(Tab *tab)
{
  Folding *f = tab->view->folding;

  while (f->folds->len > 0)
    remove_fold(f, f->folds->len - 1);
  apply_folds(f);
}

void folding_reset(Tab *tab)
{
  if (tab->view->folding && tab->view->folding->folds->len > 0)
    unfold_all(tab);
}

/* The cursor went into folded lines: they open. */
static void on_cursor_in_fold(GtkTextBuffer *buffer, const GtkTextIter *where, GtkTextMark *mark, Folding *f)
{
  gboolean opened = FALSE;

  if (mark != gtk_text_buffer_get_insert(buffer) || f->folds->len == 0)
    return;
  for (guint i = f->folds->len; i-- > 0;) {
    GtkTextIter from, to;
    gtk_text_buffer_get_iter_at_mark(buffer, &from, g_array_index(f->folds, Fold, i).from);
    gtk_text_buffer_get_iter_at_mark(buffer, &to, g_array_index(f->folds, Fold, i).to);
    if (gtk_text_iter_compare(where, &from) >= 0 && gtk_text_iter_compare(where, &to) < 0) {
      remove_fold(f, i);
      opened = TRUE;
    }
  }
  if (opened)
    apply_folds(f);
}

/* A while after a change: a fold whose block is not there any more (its brace was deleted) opens. */
static gboolean check_folds(gpointer data)
{
  Folding *f = data;
  gboolean dropped = FALSE;

  f->check_id = 0;
  fresh_blocks(f->tab);
  for (guint i = f->folds->len; i-- > 0;) {
    Fold *fold = &g_array_index(f->folds, Fold, i);
    Block *block = block_at(f->tab, fold_head(f->tab, fold));
    GtkTextIter to;
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(f->tab->buffer), &to, fold->to);
    if (!block || block->close_line != gtk_text_iter_get_line(&to)) {
      remove_fold(f, i);
      dropped = TRUE;
    }
  }
  if (dropped)
    apply_folds(f);
  return G_SOURCE_REMOVE;
}

static void on_folds_text_changed(GtkTextBuffer *buffer G_GNUC_UNUSED, Folding *f)
{
  if (f->folds->len > 0 && !f->tab->view->loading && !f->check_id)
    f->check_id = g_timeout_add(CHECK_MS, check_folds, f);
}

/* The "⋯" ------------------------------------------------------------------------------------ */

void folding_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot)
{
  Folding *f = tab->view->folding;
  const GdkRGBA *fill = palette_rgba(PAL_INPUT), *ink = palette_rgba(PAL_FG_DIM);
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  PangoLayout *layout = NULL;
  GdkRectangle visible;

  if (!f || f->folds->len == 0)
    return;
  gtk_text_view_get_visible_rect(view, &visible);
  for (guint i = 0; i < f->folds->len; i++) {
    GtkTextIter end;
    GdkRectangle at;
    int width, height;
    gtk_text_buffer_get_iter_at_line(buffer, &end, fold_head(tab, &g_array_index(f->folds, Fold, i)));
    if (!gtk_text_iter_ends_line(&end))
      gtk_text_iter_forward_to_line_end(&end);
    if (line_hidden(f, gtk_text_iter_get_line(&end))) /* inside another fold */
      continue;
    gtk_text_view_get_iter_location(view, &end, &at);
    if (at.y + at.height < visible.y || at.y > visible.y + visible.height)
      continue;
    if (!layout)
      layout = gtk_widget_create_pango_layout(GTK_WIDGET(view), "⋯");
    pango_layout_get_pixel_size(layout, &width, &height);
    GskRoundedRect box;
    graphene_rect_t rect = GRAPHENE_RECT_INIT(at.x + 6, at.y + 2, width + 10, at.height - 4);
    gsk_rounded_rect_init_from_rect(&box, &rect, 3);
    gtk_snapshot_push_rounded_clip(snapshot, &box);
    gtk_snapshot_append_color(snapshot, fill, &rect);
    gtk_snapshot_pop(snapshot);
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(at.x + 11, at.y + (at.height - height) / 2.0));
    gtk_snapshot_append_layout(snapshot, layout, ink);
    gtk_snapshot_restore(snapshot);
  }
  if (layout)
    g_object_unref(layout);
}

/* The arrows ------------------------------------------------------------------------------- */

struct _TcodeFoldRenderer {
  GtkSourceGutterRenderer parent;
  Folding *folding;
};

#define TCODE_TYPE_FOLD_RENDERER (tcode_fold_renderer_get_type())
G_DECLARE_FINAL_TYPE(TcodeFoldRenderer, tcode_fold_renderer, TCODE, FOLD_RENDERER, GtkSourceGutterRenderer)
G_DEFINE_FINAL_TYPE(TcodeFoldRenderer, tcode_fold_renderer, GTK_SOURCE_TYPE_GUTTER_RENDERER)

/* Neither side of a comparison folds: its gap rows keep the two sides' lines level. */
static gboolean can_fold(Folding *f)
{
  return f && f->tab->view && !f->tab->view->loading && !f->tab->compare.old_side && !f->tab->compare.with;
}

/* A chevron: pointing down (open) or right (folded), in a box size wide. */
static void draw_chevron(GtkSnapshot *snapshot, double x, double y, double size, gboolean folded)
{
  const GdkRGBA *ink = palette_rgba(PAL_FG);
  GskPathBuilder *builder = gsk_path_builder_new();
  GskStroke *stroke = gsk_stroke_new(1.4);
  double c = size / 2, r = size / 5;
  GskPath *path;

  if (folded) {
    gsk_path_builder_move_to(builder, x + c - r / 2, y + c - r);
    gsk_path_builder_line_to(builder, x + c + r / 2, y + c);
    gsk_path_builder_line_to(builder, x + c - r / 2, y + c + r);
  } else {
    gsk_path_builder_move_to(builder, x + c - r, y + c - r / 2);
    gsk_path_builder_line_to(builder, x + c, y + c + r / 2);
    gsk_path_builder_line_to(builder, x + c + r, y + c - r / 2);
  }
  path = gsk_path_builder_free_to_path(builder);
  gtk_snapshot_append_stroke(snapshot, path, stroke, ink);
  gsk_path_unref(path);
  gsk_stroke_free(stroke);
}

static void snapshot_arrow(GtkSourceGutterRenderer *renderer, GtkSnapshot *snapshot, GtkSourceGutterLines *lines,
                           guint line)
{
  Folding *f = TCODE_FOLD_RENDERER(renderer)->folding;
  gboolean folded;
  double y, height;

  if (!can_fold(f))
    return;
  folded = fold_on(f, (int)line) >= 0;
  if (!folded && (!f->hover || !block_at(f->tab, (int)line)))
    return;
  if (line_hidden(f, (int)line)) /* inside another fold */
    return;
  gtk_source_gutter_lines_get_line_extent(lines, line, GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_FIRST, &y, &height);
  draw_chevron(snapshot, 0, y + (height - FOLD_WIDTH) / 2, FOLD_WIDTH, folded);
}

static gboolean query_arrow(GtkSourceGutterRenderer *renderer, GtkTextIter *iter, GdkRectangle *area G_GNUC_UNUSED)
{
  Folding *f = TCODE_FOLD_RENDERER(renderer)->folding;
  int line = gtk_text_iter_get_line(iter);

  return can_fold(f) && (fold_on(f, line) >= 0 || block_at(f->tab, line));
}

static void activate_arrow(GtkSourceGutterRenderer *renderer, GtkTextIter *iter, GdkRectangle *area G_GNUC_UNUSED,
                           guint button G_GNUC_UNUSED, GdkModifierType state G_GNUC_UNUSED, int n_presses G_GNUC_UNUSED)
{
  Folding *f = TCODE_FOLD_RENDERER(renderer)->folding;

  if (can_fold(f))
    toggle(f, gtk_text_iter_get_line(iter));
}

static void tcode_fold_renderer_class_init(TcodeFoldRendererClass *klass)
{
  GtkSourceGutterRendererClass *renderer = GTK_SOURCE_GUTTER_RENDERER_CLASS(klass);

  renderer->snapshot_line = snapshot_arrow;
  renderer->query_activatable = query_arrow;
  renderer->activate = activate_arrow;
}

static void tcode_fold_renderer_init(TcodeFoldRenderer *self)
{
  gtk_widget_set_size_request(GTK_WIDGET(self), FOLD_WIDTH, -1);
}

/* Line numbers ------------------------------------------------------------------------------ */

/* GtkSourceView's own line numbers draw every line, also the folded ones, which have no height: their
 * numbers were drawn over each other. These leave them out. */
struct _TcodeLineNumbers {
  GtkSourceGutterRendererText parent;
  Folding *folding;
};

#define TCODE_TYPE_LINE_NUMBERS (tcode_line_numbers_get_type())
G_DECLARE_FINAL_TYPE(TcodeLineNumbers, tcode_line_numbers, TCODE, LINE_NUMBERS, GtkSourceGutterRendererText)
G_DEFINE_FINAL_TYPE(TcodeLineNumbers, tcode_line_numbers, GTK_SOURCE_TYPE_GUTTER_RENDERER_TEXT)

static void query_number(GtkSourceGutterRenderer *renderer, GtkSourceGutterLines *lines, guint line)
{
  GtkSourceGutterRendererText *text = GTK_SOURCE_GUTTER_RENDERER_TEXT(renderer);
  Folding *f = TCODE_LINE_NUMBERS(renderer)->folding;
  char number[24];

  if (f && (line_hidden(f, (int)line) || diff_gutter_line_hidden(f->tab, (int)line))) {
    gtk_source_gutter_renderer_text_set_text(text, "", 0);
    return;
  }
  g_snprintf(number, sizeof number, "%u", line + 1);
  if (gtk_source_gutter_lines_is_cursor(lines, line)) { /* Dark Classic's editorLineNumber.activeForeground */
    char *markup = g_strdup_printf("<span foreground=\"%s\">%s</span>", palette_hex(PAL_FG), number);
    gtk_source_gutter_renderer_text_set_markup(text, markup, -1);
    g_free(markup);
  } else {
    gtk_source_gutter_renderer_text_set_text(text, number, -1);
  }
}

/* As wide as the last line's number. */
static void fit_numbers(Folding *f)
{
  int lines = gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(f->tab->buffer)), digits = 1, width = 0;
  char sample[24];

  for (int n = lines; n >= 10; n /= 10)
    digits++;
  digits = MAX(digits, 2);
  if (digits == f->digits)
    return;
  f->digits = digits;
  memset(sample, '0', digits);
  sample[digits] = '\0';
  gtk_source_gutter_renderer_text_measure(GTK_SOURCE_GUTTER_RENDERER_TEXT(f->numbers), sample, &width, NULL);
  /* A little room left of the digits: the breakpoints' column (breakpoints.c) has the rest, so a click
   * left of a number puts a breakpoint instead of selecting the line. */
  gtk_widget_set_size_request(f->numbers, width + 8, -1);
}

/* A click on a number selects its line. */
static gboolean query_number_activatable(GtkSourceGutterRenderer *renderer G_GNUC_UNUSED,
                                         GtkTextIter *iter G_GNUC_UNUSED, GdkRectangle *area G_GNUC_UNUSED)
{
  return TRUE;
}

static void activate_number(GtkSourceGutterRenderer *renderer, GtkTextIter *iter, GdkRectangle *area G_GNUC_UNUSED,
                            guint button G_GNUC_UNUSED, GdkModifierType state G_GNUC_UNUSED,
                            int n_presses G_GNUC_UNUSED)
{
  GtkTextBuffer *buffer = gtk_text_iter_get_buffer(iter);
  GtkTextIter start = *iter, end;

  gtk_text_iter_set_line_offset(&start, 0);
  end = start;
  if (!gtk_text_iter_forward_line(&end))
    gtk_text_iter_forward_to_line_end(&end);
  gtk_text_buffer_select_range(buffer, &end, &start);
  gtk_widget_grab_focus(GTK_WIDGET(gtk_source_gutter_renderer_get_view(renderer)));
}

/* The font changed (Settings, zoom): the width is measured again. */
static void numbers_css_changed(GtkWidget *widget, GtkCssStyleChange *change)
{
  Folding *f = TCODE_LINE_NUMBERS(widget)->folding;

  GTK_WIDGET_CLASS(tcode_line_numbers_parent_class)->css_changed(widget, change);
  if (f) {
    f->digits = 0;
    fit_numbers(f);
  }
}

static void tcode_line_numbers_class_init(TcodeLineNumbersClass *klass)
{
  GtkSourceGutterRendererClass *renderer = GTK_SOURCE_GUTTER_RENDERER_CLASS(klass);

  GTK_WIDGET_CLASS(klass)->css_changed = numbers_css_changed;
  renderer->query_data = query_number;
  renderer->query_activatable = query_number_activatable;
  renderer->activate = activate_number;
}

static void tcode_line_numbers_init(TcodeLineNumbers *self)
{
  gtk_source_gutter_renderer_set_alignment_mode(GTK_SOURCE_GUTTER_RENDERER(self),
                                                GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_FIRST);
  gtk_source_gutter_renderer_set_xalign(GTK_SOURCE_GUTTER_RENDERER(self), 1);
}

static void on_lines_changed(GtkTextBuffer *buffer G_GNUC_UNUSED, Folding *f)
{
  fit_numbers(f);
}

/* The arrows of unfolded blocks show while the pointer is over the gutter. */
static void on_gutter_enter(GtkEventControllerMotion *motion G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                            double y G_GNUC_UNUSED, Folding *f)
{
  f->hover = TRUE;
  gtk_widget_queue_draw(f->renderer);
}

static void on_gutter_leave(GtkEventControllerMotion *motion G_GNUC_UNUSED, Folding *f)
{
  f->hover = FALSE;
  gtk_widget_queue_draw(f->renderer);
}

void folding_attach(Tab *tab)
{
  Folding *f = g_new0(Folding, 1);
  GtkSourceGutter *gutter = gtk_source_view_get_gutter(tab->source, GTK_TEXT_WINDOW_LEFT);
  GtkEventController *motion = gtk_event_controller_motion_new();

  f->tab = tab;
  f->folds = g_array_new(FALSE, FALSE, sizeof(Fold));
  f->renderer = g_object_ref_sink(g_object_new(TCODE_TYPE_FOLD_RENDERER, NULL)); /* ours too: see free */
  TCODE_FOLD_RENDERER(f->renderer)->folding = f;
  gtk_source_gutter_insert(gutter, GTK_SOURCE_GUTTER_RENDERER(f->renderer), -5); /* right of the diff marks */
  f->numbers = g_object_ref_sink(g_object_new(TCODE_TYPE_LINE_NUMBERS, NULL));
  TCODE_LINE_NUMBERS(f->numbers)->folding = f;
  gtk_source_gutter_insert(gutter, GTK_SOURCE_GUTTER_RENDERER(f->numbers), -30); /* where GtkSourceView's go */
  g_signal_connect(tab->buffer, "changed", G_CALLBACK(on_lines_changed), f);
  fit_numbers(f);
  g_signal_connect(motion, "enter", G_CALLBACK(on_gutter_enter), f);
  g_signal_connect(motion, "leave", G_CALLBACK(on_gutter_leave), f);
  gtk_widget_add_controller(GTK_WIDGET(gutter), motion);
  g_signal_connect(tab->buffer, "mark-set", G_CALLBACK(on_cursor_in_fold), f);
  g_signal_connect(tab->buffer, "changed", G_CALLBACK(on_folds_text_changed), f);
  tab->view->folding = f;
}

GtkWidget *folding_line_numbers(Tab *tab)
{
  return tab->view && tab->view->folding ? tab->view->folding->numbers : NULL;
}

void folding_free(Folding *f)
{
  if (!f)
    return;
  g_signal_handlers_disconnect_by_data(f->tab->buffer, f);
  /* The page went first: the renderer lives on with our reference, and must not reach us. */
  TCODE_FOLD_RENDERER(f->renderer)->folding = NULL;
  g_object_unref(f->renderer);
  TCODE_LINE_NUMBERS(f->numbers)->folding = NULL;
  g_object_unref(f->numbers);
  g_clear_handle_id(&f->check_id, g_source_remove);
  g_clear_handle_id(&f->blocks_id, g_source_remove);
  g_array_free(f->folds, TRUE); /* the marks go with the buffer */
  if (f->blocks)
    g_array_free(f->blocks, TRUE);
  g_free(f);
}
