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
  return palette_rgba(alert_colors[CLAMP(alert, 1, 5) - 1]);
}

/* The tags' colors, from the color theme (again when it changes). */
void preview_color_tags(GtkTextBuffer *b)
{
  static const struct {
    const char *tag, *property;
    PalColor color;
  } colors[] = {
    { "base", "foreground", PAL_FG },        { "quote", "foreground", PAL_FG_DIM },
    { "h1", "foreground", PAL_FG_STRONG },   { "h2", "foreground", PAL_FG_STRONG },
    { "h3", "foreground", PAL_FG_BRIGHT },   { "h4", "foreground", PAL_FG_BRIGHT },
    { "h5", "foreground", PAL_FG_BRIGHT },   { "h6", "foreground", PAL_FG_DIM },
    { "bold", "foreground", PAL_FG_STRONG }, { "code", "foreground", PAL_FG_STRONG },
    { "dim", "foreground", PAL_FG_DIM },     { "error", "foreground", PAL_ERROR },
    { "link", "foreground", PAL_LINK },      { "caption", "foreground", PAL_FG_DIM },
    { "mark", "background", PAL_MATCH },
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
      g_object_set(tag, "foreground", palette_hex(alert_colors[i - 1]), NULL);
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
  for (int i = 1; i <= MAX_LEVEL; i++) {
    g_snprintf(name, sizeof name, "li%d", i);
    gtk_text_buffer_create_tag(b, name, "left-margin", MARGIN + i * INDENT, "indent", -INDENT, NULL);
  }
  gtk_text_buffer_create_tag(b, "tight", "pixels-below-lines", 3, NULL);
  gtk_text_buffer_create_tag(b, "gap", "pixels-below-lines", 14, NULL);
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
  gtk_text_buffer_create_tag(b, "mark", NULL);
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
  for (guint i = 0; i < p->tables->len; i++)
    preview_size_table(g_ptr_array_index(p->tables, i), content_w);
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
  GdkRGBA bar = *palette_rgba(PAL_ACCENT), quote = *palette_rgba(PAL_FG);
  GdkRGBA box = *palette_rgba(PAL_FRAME), rule = *palette_rgba(PAL_BORDER), hr = *palette_rgba(PAL_BORDER_STRONG);

  bar.alpha = 0.5f;
  quote.alpha = 0.05f;
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
      case BLOCK_QUOTE: { /* its last line has a paragraph's gap below it: most of it is outside */
        GdkRGBA tint = quote, line = bar;
        if (block->alert) {
          tint = line = *preview_alert_color(block->alert);
          tint.alpha = 0.07f;
        }
        fill(snapshot, &tint, left, top - 6, right - left, bottom - top - 2, 0);
        fill(snapshot, &line, left, top - 6, BAR_WIDTH, bottom - top - 2, 0);
        break;
      }
      case BLOCK_PRE: fill(snapshot, &box, left, top, right - left, bottom - top, 6); break;
      case BLOCK_RULE: fill(snapshot, &rule, left, bottom - 8, right - left, 1, 0); break;
      case BLOCK_HR: fill(snapshot, &hr, left, (top + bottom) / 2 - 1, right - left, 2, 0); break;
    }
  }
}

/* Inline code's background, a rectangle per row it takes on screen (it may wrap). */
static void draw_code_spans(GtkTextView *view, Preview *p, GtkSnapshot *snapshot, int first_line, int last_line)
{
  GdkRGBA bg = *palette_rgba(PAL_BORDER_STRONG);

  bg.alpha = 0.75f;
  for (guint i = 0; i < p->code_spans->len; i++) {
    const CodeSpan *span = &g_array_index(p->code_spans, CodeSpan, i);
    GtkTextIter iter, end;
    int first, last;
    GdkRectangle row = { 0 }, at;

    block_lines(p->buffer, span->start, span->end, &first, &last);
    if (last < first_line || first > last_line)
      continue;
    gtk_text_buffer_get_iter_at_offset(p->buffer, &iter, span->start);
    gtk_text_buffer_get_iter_at_offset(p->buffer, &end, span->end);
    for (; gtk_text_iter_compare(&iter, &end) < 0; gtk_text_iter_forward_char(&iter)) {
      gtk_text_view_get_iter_location(view, &iter, &at);
      if (row.width > 0 && at.y == row.y) {
        row.width = at.x + at.width - row.x;
        continue;
      }
      if (row.width > 0) /* a little above and below the letters */
        fill(snapshot, &bg, row.x, row.y - 2, row.width, row.height + 4, 4);
      row = at;
    }
    if (row.width > 0)
      fill(snapshot, &bg, row.x, row.y - 2, row.width, row.height + 4, 4);
  }
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
    draw_code_spans(view, self->preview, snapshot, first_line, last_line);
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
