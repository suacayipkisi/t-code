/*
 * The preview's view (see state.h): the text is a centered column; quotes, code blocks, the rules
 * under h1 / h2 and inline code are drawn below the text, and images and tables are sized to it.
 */
#include "editor/preview/state.h"

#include "window/palette.h"

/* Tags -------------------------------------------------------------------------------------- */

static const double heading_scale[] = { 1.85, 1.45, 1.25, 1.1, 1.0, 0.9 };

/* GitHub's alerts: note, tip, important, warning, caution. */
static const PalColor alert_colors[] = { PAL_INFO, PAL_GIT_ADDED, PAL_ACCENT, PAL_WARNING, PAL_ERROR };

double preview_heading_scale(int level)
{
  return heading_scale[CLAMP(level, 1, 6) - 1];
}

const GdkRGBA *preview_alert_color(int alert)
{
  static const GdkRGBA important = { 0.64f, 0.44f, 0.97f, 1.0f }; /* GitHub's purple: the theme has none */

  return alert == 3 ? &important : palette_rgba(alert_colors[CLAMP(alert, 1, 5) - 1]);
}

/* The tags' colors, from the color theme (again when it changes). */
void preview_color_tags(GtkTextBuffer *b)
{
  static const struct {
    const char *tag, *property;
    PalColor color;
  } colors[] = {
    { "base", "foreground", PAL_FG },
    { "quote", "foreground", PAL_FG_DIM },
    { "h1", "foreground", PAL_FG_STRONG },
    { "h2", "foreground", PAL_FG_STRONG },
    { "h3", "foreground", PAL_FG_BRIGHT },
    { "h4", "foreground", PAL_FG_BRIGHT },
    { "h5", "foreground", PAL_FG_BRIGHT },
    { "h6", "foreground", PAL_FG_DIM },
    { "bold", "foreground", PAL_FG_STRONG },
    { "code", "foreground", PAL_FG_STRONG },
    { "dim", "foreground", PAL_FG_DIM },
    { "error", "foreground", PAL_ERROR },
    { "link", "foreground", PAL_LINK },
    { "caption", "foreground", PAL_FG_DIM },
    { "code-flat", "background", PAL_BORDER_STRONG },
  };
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(b);

  for (size_t i = 0; i < G_N_ELEMENTS(colors); i++) {
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, colors[i].tag);
    if (tag)
      g_object_set(tag, colors[i].property, palette_hex(colors[i].color), NULL);
  }
  for (int i = 1; i <= 5; i++) {
    char name[16];
    g_snprintf(name, sizeof name, "alert%d", i);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);
    if (tag)
      g_object_set(tag, "foreground-rgba", preview_alert_color(i), NULL);
  }
}

/* The tags every rendering uses, made with the buffer. Later tags win where they set the same
 * thing, so the order matters: the base first, deeper indents after shallower ones. */
void preview_create_tags(GtkTextBuffer *b, const char *mono)
{
  char name[16];

  gtk_text_buffer_create_tag(b, "base", NULL);
  gtk_text_buffer_create_tag(b, "indent0", "indent", 0, NULL);
  for (int i = 1; i <= MAX_LEVEL; i++) {
    g_snprintf(name, sizeof name, "indent%d", i);
    gtk_text_buffer_create_tag(b, name, "left-margin", MARGIN + i * INDENT, "indent", 0, NULL);
  }
  /* a list item: its marker hangs left of the text, in the indent (markdown.c widens the space after it to fill it) */
  for (int i = 1; i <= MAX_LEVEL; i++) {
    g_snprintf(name, sizeof name, "li%d", i);
    gtk_text_buffer_create_tag(b, name, "left-margin", MARGIN + i * INDENT, "indent", -INDENT, NULL);
  }
  gtk_text_buffer_create_tag(b, "tight", "pixels-below-lines", 3, NULL);
  /* a table row's padding, above its first line and below its last (before "gap": the last row keeps that) */
  gtk_text_buffer_create_tag(b, "tr-first", "pixels-above-lines", CELL_VPAD, NULL);
  gtk_text_buffer_create_tag(b, "tr-last", "pixels-below-lines", CELL_VPAD, NULL);
  gtk_text_buffer_create_tag(b, "gap", "pixels-below-lines", 16, NULL);
  gtk_text_buffer_create_tag(b, "table-gap", "pixels-below-lines", TABLE_GAP + CELL_VPAD, NULL); /* below the grid */
  gtk_text_buffer_create_tag(b, "bullet", "scale", 1.3, NULL); /* a list item's • as large as a browser's */
  gtk_text_buffer_create_tag(b, "spacer", "size-points", 4.0, NULL);
  gtk_text_buffer_create_tag(b, "quote", NULL);
  for (int i = 0; i < 6; i++) {
    g_snprintf(name, sizeof name, "h%d", i + 1);
    gtk_text_buffer_create_tag(b, name, "weight", PANGO_WEIGHT_SEMIBOLD, "scale", heading_scale[i],
                               "pixels-above-lines", i < 2 ? 24 : 16, "pixels-below-lines", i < 2 ? 16 : 8, NULL);
  }
  gtk_text_buffer_create_tag(b, "bold", "weight", PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(b, "italic", "style", PANGO_STYLE_ITALIC, NULL);
  gtk_text_buffer_create_tag(b, "strike", "strikethrough", TRUE, NULL);
  gtk_text_buffer_create_tag(b, "underline", "underline", PANGO_UNDERLINE_SINGLE, NULL);
  gtk_text_buffer_create_tag(b, "code", "family", mono, "scale", 0.90, NULL);
  gtk_text_buffer_create_tag(b, "code-flat", NULL); /* inline code's background in a very long paragraph */
  /* Wrapped: a line wider than the view would scroll the whole preview sideways. */
  gtk_text_buffer_create_tag(b, "pre", "family", mono, "scale", 0.92, "wrap-mode", GTK_WRAP_WORD_CHAR, NULL);
  for (int i = 0; i <= MAX_LEVEL; i++) { /* a code block's text, inside its box */
    g_snprintf(name, sizeof name, "pre%d", i);
    gtk_text_buffer_create_tag(b, name, "left-margin", MARGIN + i * INDENT + PRE_PAD, "right-margin", MARGIN + PRE_PAD,
                               NULL);
  }
  gtk_text_buffer_create_tag(b, "pre-top", "pixels-above-lines", 12, NULL);
  gtk_text_buffer_create_tag(b, "pre-bottom", "pixels-below-lines", 12, NULL);
  gtk_text_buffer_create_tag(b, "hr", "size-points", 1.0, NULL);
  gtk_text_buffer_create_tag(b, "dim", "style", PANGO_STYLE_ITALIC, NULL);
  gtk_text_buffer_create_tag(b, "error", NULL);
  gtk_text_buffer_create_tag(b, "small", "scale", 0.85, NULL);
  gtk_text_buffer_create_tag(b, "link", NULL);
  gtk_text_buffer_create_tag(b, "link-hover", "underline", PANGO_UNDERLINE_SINGLE, NULL);
  gtk_text_buffer_create_tag(b, "sup", "scale", 0.75, "rise", 5 * PANGO_SCALE, NULL);
  gtk_text_buffer_create_tag(b, "sub", "scale", 0.75, "rise", -2 * PANGO_SCALE, NULL);
  gtk_text_buffer_create_tag(b, "mark", "background-rgba", &(GdkRGBA){ 1.0f, 0.83f, 0.24f, 0.25f }, NULL); /* <mark> */
  gtk_text_buffer_create_tag(b, "center", "justification", GTK_JUSTIFY_CENTER, NULL);
  gtk_text_buffer_create_tag(b, "right", "justification", GTK_JUSTIFY_RIGHT, NULL);
  gtk_text_buffer_create_tag(b, "caption", "style", PANGO_STYLE_ITALIC, "scale", 0.85, NULL);
  gtk_text_buffer_create_tag(b, "hidden", "invisible", TRUE, NULL); /* a closed <details>' content */
  gtk_text_buffer_create_tag(b, "math-display", "justification", GTK_JUSTIFY_CENTER, "pixels-above-lines", 6, NULL);
  for (int i = 1; i <= 5; i++) {
    g_snprintf(name, sizeof name, "alert%d", i);
    gtk_text_buffer_create_tag(b, name, NULL);
  }
  preview_color_tags(b);
}

/* Layout ----------------------------------------------------------------------------------- */

/* The width an image may have in a view this wide. */
static int image_width(GtkWidget *picture, int max_width)
{
  int natural = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picture), "t-code-width"));

  if (g_object_get_data(G_OBJECT(picture), "t-code-actual")) /* an image's preview at its own size */
    return MAX(natural, 1);
  return max_width > 0 ? MAX(MIN(natural, max_width), 16) : 16;
}

void preview_size_picture(GtkWidget *picture, int max_width)
{
  GdkPaintable *paintable = gtk_picture_get_paintable(GTK_PICTURE(picture));
  int w = image_width(picture, max_width);
  int pw = gdk_paintable_get_intrinsic_width(paintable), ph = gdk_paintable_get_intrinsic_height(paintable);

  gtk_widget_set_size_request(picture, w, pw > 0 ? w * ph / pw : w);
}

/* The margin left and right of the text in a view this wide: the text is a centered column. */
static int side_margin(Preview *p, int width)
{
  int max_w = (int)(MAX_CONTENT_WIDTH * p->scale);

  if (p->image) /* an image may use the whole width */
    return MARGIN;
  return width > max_w + 2 * MARGIN ? (width - max_w) / 2 : MARGIN;
}

/* The width the text has: images and tables are sized to it. */
int preview_content_width(Preview *p)
{
  int width = p->width > 0 ? p->width : gtk_widget_get_width(GTK_WIDGET(p->view));

  return width > 0 ? width - 2 * side_margin(p, width) : (int)(MAX_CONTENT_WIDTH * p->scale);
}

/* Adjusts left and right margins to center content on wide displays and sizes images and tables. */
void preview_update_layout(Preview *p, int width)
{
  if (width <= 0)
    width = gtk_widget_get_width(GTK_WIDGET(p->view));
  if (width <= 0)
    return;
  p->width = width;

  int margin = side_margin(p, width);

  gtk_text_view_set_left_margin(p->view, margin);
  gtk_text_view_set_right_margin(p->view, margin);

  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(p->buffer);
  for (int i = 1; i <= MAX_LEVEL; i++) {
    char name[16];
    g_snprintf(name, sizeof name, "indent%d", i);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);
    if (tag)
      g_object_set(tag, "left-margin", margin + i * INDENT, NULL);

    g_snprintf(name, sizeof name, "li%d", i);
    tag = gtk_text_tag_table_lookup(table, name);
    if (tag)
      g_object_set(tag, "left-margin", margin + i * INDENT, NULL);
  }
  for (int i = 0; i <= MAX_LEVEL; i++) {
    char name[16];
    g_snprintf(name, sizeof name, "pre%d", i);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);
    if (tag)
      g_object_set(tag, "left-margin", margin + i * INDENT + PRE_PAD, "right-margin", margin + PRE_PAD, NULL);
  }

  int content_w = width - 2 * margin;
  for (guint i = 0; i < p->pictures->len; i++)
    preview_size_picture(g_ptr_array_index(p->pictures, i), content_w);
  for (guint i = 0; i < p->tables->len; i++) {
    const PreviewTable *t = g_ptr_array_index(p->tables, i);
    g_object_set(t->tag, "left-margin", margin + t->level * INDENT + CELL_PAD, NULL);
  }
  if (!preview_tables_fit(p, content_w)) { /* their cells wrap differently at this width */
    preview_reload_later(p);
  }
}

/* The view subclass that centers content and adapts on any size allocation */
#define TCODE_TYPE_PREVIEW_VIEW (tcode_preview_view_get_type())
G_DECLARE_FINAL_TYPE(TcodePreviewView, tcode_preview_view, TCODE, PREVIEW_VIEW, GtkTextView)

struct _TcodePreviewView {
  GtkTextView parent;
  Preview *preview;
};

G_DEFINE_FINAL_TYPE(TcodePreviewView, tcode_preview_view, GTK_TYPE_TEXT_VIEW)

static void tcode_preview_view_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
  TcodePreviewView *self = TCODE_PREVIEW_VIEW(widget);

  GTK_WIDGET_CLASS(tcode_preview_view_parent_class)->size_allocate(widget, width, height, baseline);

  if (self->preview && width > 0 && width != self->preview->width) {
    preview_update_layout(self->preview, width);
  }
  if (self->preview && width > 0)
    preview_sized(self->preview);
}

/* The buffer lines from first to last (the line after the end of a block that ends with its line
 * break is not its). */
static void block_lines(GtkTextBuffer *buffer, int start, int end, int *first, int *last)
{
  GtkTextIter from, to;

  gtk_text_buffer_get_iter_at_offset(buffer, &from, start);
  gtk_text_buffer_get_iter_at_offset(buffer, &to, end);
  if (end > start && gtk_text_iter_starts_line(&to))
    gtk_text_iter_backward_char(&to);
  *first = gtk_text_iter_get_line(&from);
  *last = gtk_text_iter_get_line(&to);
}

static void fill(GtkSnapshot *snapshot, const GdkRGBA *color, float x, float y, float w, float h, float radius)
{
  graphene_rect_t rect = GRAPHENE_RECT_INIT(x, y, w, h);
  GskRoundedRect rounded;

  if (w <= 0 || h <= 0)
    return;
  gsk_rounded_rect_init_from_rect(&rounded, &rect, radius);
  gtk_snapshot_push_rounded_clip(snapshot, &rounded);
  gtk_snapshot_append_color(snapshot, color, &rect);
  gtk_snapshot_pop(snapshot);
}

/* Quotes (a bar and a light background), code blocks (a box), the line under h1 and h2 and horizontal
 * rules: within the text's column, not to the view's edge. */
static void draw_blocks(GtkTextView *view, Preview *p, GtkSnapshot *snapshot, int first_line, int last_line)
{
  int margin = side_margin(p, p->width), right = p->width - margin;
  GdkRGBA box = *palette_rgba(PAL_FRAME), rule = *palette_rgba(PAL_BORDER), hr = *palette_rgba(PAL_BORDER_STRONG);

  for (guint i = 0; i < p->blocks->len; i++) {
    const Block *block = &g_array_index(p->blocks, Block, i);
    GtkTextIter iter;
    int first, last, top, bottom, height, left = margin + block->level * INDENT;

    block_lines(p->buffer, block->start, block->end, &first, &last);
    if (last < first_line || first > last_line)
      continue;
    gtk_text_buffer_get_iter_at_line(p->buffer, &iter, first);
    gtk_text_view_get_line_yrange(view, &iter, &top, &height);
    gtk_text_buffer_get_iter_at_line(p->buffer, &iter, last);
    gtk_text_view_get_line_yrange(view, &iter, &bottom, &height);
    bottom += height;
    switch (block->kind) {
      case BLOCK_QUOTE: { /* a bar left of it, as GitHub draws it; its last line's gap below it left out */
        GdkRGBA line = block->alert ? *preview_alert_color(block->alert) : *palette_rgba(PAL_BORDER_STRONG);
        fill(snapshot, &line, left, top - 4, BAR_WIDTH, bottom - top - 6, 0);
        break;
      }
      case BLOCK_PRE: fill(snapshot, &box, left, top, right - left, bottom - top, 6); break;
      case BLOCK_RULE: fill(snapshot, &rule, left, bottom - 8, right - left, 1, 0); break;
      case BLOCK_HR: fill(snapshot, &hr, left, (top + bottom) / 2 - 1, right - left, 2, 0); break;
    }
  }
}

/* The y of a table row's top and bottom: its lines' (its padding is theirs), the table's gap below its last row
 * left out. A row as tall as a picture in it is measured with it. */
static void row_range(GtkTextView *view, GtkTextBuffer *buffer, int start, int end, gboolean last, int *top,
                      int *bottom)
{
  GtkTextIter iter;
  int y, height;

  gtk_text_buffer_get_iter_at_offset(buffer, &iter, start);
  gtk_text_view_get_line_yrange(view, &iter, top, &height);
  gtk_text_buffer_get_iter_at_offset(buffer, &iter, end - 1); /* its last line break */
  gtk_text_view_get_line_yrange(view, &iter, &y, &height);
  *bottom = y + height - (last ? TABLE_GAP : 0);
}

/* Tables: the header's background, every other row's, and the grid, below the text's tab stops. */
static void draw_tables(GtkTextView *view, Preview *p, GtkSnapshot *snapshot, int first_line, int last_line)
{
  int margin = side_margin(p, p->width);
  GdkRGBA head = *palette_rgba(PAL_BORDER), odd = *palette_rgba(PAL_HOVER), line = *palette_rgba(PAL_BORDER_STRONG);

  for (guint i = 0; i < p->tables->len; i++) {
    const PreviewTable *table = g_ptr_array_index(p->tables, i);
    int rows = (int)table->rows->len - 1, first, last;
    block_lines(p->buffer, table->start, g_array_index(table->rows, int, rows), &first, &last);
    if (rows <= 0 || last < first_line || first > last_line)
      continue;
    int left = margin + table->level * INDENT, width = table->edges[table->columns], top = 0, bottom = 0;
    for (int row = 0; row < rows; row++) {
      int start = g_array_index(table->rows, int, row), end = g_array_index(table->rows, int, row + 1);
      block_lines(p->buffer, start, end, &first, &last);
      if (last < first_line - 1 || first > last_line + 1)
        continue;
      row_range(view, p->buffer, start, end, row == rows - 1, &top, &bottom);
      if (row == 0)
        fill(snapshot, &head, left, top, width, bottom - top, 0);
      else if (row % 2 == 0)
        fill(snapshot, &odd, left, top, width, bottom - top, 0);
      fill(snapshot, &line, left, top, width, 1, 0); /* the line above it */
      for (int c = 0; c <= table->columns; c++)
        fill(snapshot, &line, left + MIN(table->edges[c], width - 1), top, 1, bottom - top, 0);
    }
    /* the line below the last row drawn: the table's end when it is on screen */
    int end = g_array_index(table->rows, int, rows);
    block_lines(p->buffer, g_array_index(table->rows, int, rows - 1), end, &first, &last);
    if (last >= first_line - 1 && first <= last_line + 1)
      fill(snapshot, &line, left, bottom, width, 1, 0);
  }
}

/* The first span at or after `low` (up to `high`) whose last letter is not above `top`: the spans are in the order of
 * the text, so their rows only go down. */
static guint first_span_below(GtkTextView *view, Preview *p, guint low, guint high, int top)
{
  while (low < high) {
    guint middle = low + (high - low) / 2;
    const CodeSpan *span = &g_array_index(p->code_spans, CodeSpan, middle);
    GtkTextIter last;
    GdkRectangle at;

    gtk_text_buffer_get_iter_at_offset(p->buffer, &last, MAX(span->start, span->end - 1));
    gtk_text_view_get_iter_location(view, &last, &at);
    if (at.y + at.height < top)
      low = middle + 1;
    else
      high = middle;
  }
  return low;
}

/* Inline code's background, a rectangle per row it takes on screen (it may wrap). Only the spans in `visible` are
 * looked at, and a span's rows by their two ends: one long paragraph may hold thousands of spans, and each position
 * asked of its layout costs the paragraph's length. */
static void draw_code_spans(GtkTextView *view, Preview *p, GtkSnapshot *snapshot, const GdkRectangle *visible,
                            int first_line, int last_line)
{
  GdkRGBA bg = *palette_rgba(PAL_BORDER_STRONG);

  bg.alpha = 0.75f;
  /* The spans on the visible lines, found by halving over offsets... */
  GtkTextIter top;
  gtk_text_buffer_get_iter_at_line(p->buffer, &top, first_line);
  int from = gtk_text_iter_get_offset(&top);
  int to = G_MAXINT;
  if (gtk_text_buffer_get_iter_at_line(p->buffer, &top, last_line + 1))
    to = gtk_text_iter_get_offset(&top);
  guint low = 0, high = p->code_spans->len;
  while (low < high) {
    guint middle = low + (high - low) / 2;
    if (g_array_index(p->code_spans, CodeSpan, middle).end <= from)
      low = middle + 1;
    else
      high = middle;
  }
  guint end_index = low;
  high = p->code_spans->len;
  while (end_index < high) {
    guint middle = end_index + (high - end_index) / 2;
    if (g_array_index(p->code_spans, CodeSpan, middle).start < to)
      end_index = middle + 1;
    else
      high = middle;
  }
  /* ... then by rows, as a visible line may be a paragraph thousands of rows tall. */
  for (guint i = first_span_below(view, p, low, end_index, visible->y); i < end_index; i++) {
    const CodeSpan *span = &g_array_index(p->code_spans, CodeSpan, i);
    GtkTextIter iter, end;
    GdkRectangle at;

    if (span->end <= span->start)
      continue;
    gtk_text_buffer_get_iter_at_offset(p->buffer, &iter, span->start);
    gtk_text_buffer_get_iter_at_offset(p->buffer, &end, span->end);
    gtk_text_view_get_iter_location(view, &iter, &at);
    if (at.y > visible->y + visible->height)
      break;
    /* One rectangle per row: from the row's first letter of the span to its last. */
    while (gtk_text_iter_compare(&iter, &end) < 0) {
      GtkTextIter next = iter, last;
      GdkRectangle first_at, last_at;

      gtk_text_view_get_iter_location(view, &iter, &first_at);
      if (!gtk_text_view_forward_display_line(view, &next) || gtk_text_iter_compare(&next, &end) > 0)
        next = end;
      last = next;
      gtk_text_iter_backward_char(&last);
      gtk_text_view_get_iter_location(view, &last, &last_at);
      if (last_at.y != first_at.y) /* the row ended at a line break: its letter is on the next row */
        last_at = first_at;
      if (first_at.y > visible->y + visible->height)
        break;
      /* a little above and below the letters */
      fill(snapshot, &bg, first_at.x, first_at.y - 2, last_at.x + last_at.width - first_at.x, first_at.height + 4, 4);
      if (gtk_text_iter_compare(&next, &iter) <= 0)
        break;
      iter = next;
    }
  }
}

/* The selection, drawn here instead of by GtkTextView (its own is transparent, preview.c's CSS): one band across
 * the text's column, over code boxes, quotes, list indents and tables alike, as GtkTextView's stops at each line's
 * margins and leaves gaps beside them. */
static void draw_selection(GtkTextView *view, Preview *p, GtkSnapshot *snapshot, const GdkRectangle *visible)
{
  GtkTextIter start, end, top, bottom;

  if (!gtk_text_buffer_get_selection_bounds(p->buffer, &start, &end))
    return;
  gtk_text_view_get_line_at_y(view, &top, visible->y, NULL);
  gtk_text_view_get_line_at_y(view, &bottom, visible->y + visible->height, NULL);
  gtk_text_iter_forward_to_line_end(&bottom);
  if (gtk_text_iter_compare(&end, &top) < 0 || gtk_text_iter_compare(&start, &bottom) > 0)
    return;
  int margin = side_margin(p, p->width), left = margin, right = p->width - margin;
  GdkRGBA color =
      *palette_rgba(gtk_widget_has_focus(GTK_WIDGET(view)) ? PAL_EDITOR_SELECTION : PAL_SELECTION_UNFOCUSED);
  GdkRectangle from, to;

  color.alpha = 0.7f; /* code boxes and table grids show through */
  gtk_text_view_get_iter_location(view, &start, &from);
  gtk_text_view_get_iter_location(view, &end, &to);
  if (from.y == to.y) { /* on one row */
    fill(snapshot, &color, from.x, from.y, to.x - from.x, from.height, 0);
    return;
  }
  fill(snapshot, &color, from.x, from.y, right - from.x, from.height, 0);
  /* the rows between: the whole column, the gaps between paragraphs included */
  fill(snapshot, &color, left, from.y + from.height, right - left, to.y - from.y - from.height, 0);
  /* the last row, up to where it ends (nothing when it ends at a line's start) */
  if (!gtk_text_iter_starts_line(&end))
    fill(snapshot, &color, left, to.y, to.x - left, to.height, 0);
}

static void tcode_preview_view_snapshot_layer(GtkTextView *view, GtkTextViewLayer layer, GtkSnapshot *snapshot)
{
  TcodePreviewView *self = TCODE_PREVIEW_VIEW(view);

  if (layer == GTK_TEXT_VIEW_LAYER_BELOW_TEXT && self->preview && self->preview->code_spans) {
    GdkRectangle visible;
    GtkTextIter iter;

    gtk_text_view_get_visible_rect(view, &visible);
    gtk_text_view_get_line_at_y(view, &iter, visible.y, NULL);
    int first_line = gtk_text_iter_get_line(&iter);
    gtk_text_view_get_line_at_y(view, &iter, visible.y + visible.height, NULL);
    int last_line = gtk_text_iter_get_line(&iter);
    draw_blocks(view, self->preview, snapshot, first_line, last_line);
    draw_tables(view, self->preview, snapshot, first_line, last_line);
    draw_code_spans(view, self->preview, snapshot, &visible, first_line, last_line);
    draw_selection(view, self->preview, snapshot, &visible);
  }

  if (GTK_TEXT_VIEW_CLASS(tcode_preview_view_parent_class)->snapshot_layer)
    GTK_TEXT_VIEW_CLASS(tcode_preview_view_parent_class)->snapshot_layer(view, layer, snapshot);
}

static void tcode_preview_view_class_init(TcodePreviewViewClass *klass)
{
  GTK_WIDGET_CLASS(klass)->size_allocate = tcode_preview_view_size_allocate;
  GTK_TEXT_VIEW_CLASS(klass)->snapshot_layer = tcode_preview_view_snapshot_layer;
}

static void tcode_preview_view_init(TcodePreviewView *self G_GNUC_UNUSED)
{
}

GtkTextView *preview_view_new(Preview *p)
{
  TcodePreviewView *view = g_object_new(TCODE_TYPE_PREVIEW_VIEW, "buffer", p->buffer, NULL);

  view->preview = p;
  return GTK_TEXT_VIEW(view);
}
