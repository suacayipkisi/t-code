/*
 * Indent guides (see guides.h): TcodeView, a GtkSourceView whose layer below the text (in buffer
 * coordinates, so it scrolls with it) gets the guides of the lines on the screen. The block around
 * the cursor comes from the file's brackets outside strings and comments, listed again only when
 * the text changed; a cursor move only looks the block up in that list.
 */
#include "editor/decor/guides.h"

#include "editor/decor/blame.h"
#include "editor/gutter/breakpoints.h"
#include "editor/text/blocks.h"
#include "editor/gutter/diffgutter.h"
#include "editor/editorview.h"
#include "editor/gutter/folding.h"
#include "editor/editing/multicursor.h"
#include "lsp/inline/inlay.h"
#include "lsp/inline/diagnostics.h"
#include "window/palette.h"

#include <string.h>

struct _TcodeView {
  GtkSourceView parent;
  Tab *tab;
  GArray *brackets; /* Bracket: the file's, for the text with this change count: */
  guint brackets_changes;
  struct {
    guint changes;
    int cursor;      /* offset */
    int open, close; /* lines of the brackets around the cursor, -1 for none */
  } pair;            /* the last pair worked out */
  double char_width; /* of the font in font (0: not measured yet) */
  PangoFontDescription *font;
};

#define TCODE_TYPE_VIEW (tcode_view_get_type())
G_DECLARE_FINAL_TYPE(TcodeView, tcode_view, TCODE, VIEW, GtkSourceView)
G_DEFINE_FINAL_TYPE(TcodeView, tcode_view, GTK_SOURCE_TYPE_VIEW)

/* Dark Classic: editorIndentGuide.background1, .activeBackground1 (without code coloring) and
 * editorBracketHighlight.foreground1..3, the bracket pair colors by depth. */

/* Brackets ----------------------------------------------------------------------------------- */

typedef struct {
  int offset; /* in characters */
  int line;
  char c;
} Bracket;

/* The brackets of text outside strings and comments, in order, with their places. */
static GArray *brackets_of(const char *text)
{
  GArray *found = g_array_new(FALSE, FALSE, sizeof(Bracket));
  const char *counted = text; /* offsets are counted up to here */
  CodeScan scan = { text, text, 0, FALSE };
  int offset = 0;

  for (const char *p; (p = code_next(&scan));)
    if (strchr("([{)]}", *p)) {
      Bracket b = { 0, scan.line, *p };
      offset += (int)g_utf8_strlen(counted, p - counted);
      counted = p;
      b.offset = offset;
      g_array_append_val(found, b);
    }
  return found;
}

static gboolean is_opening(char c)
{
  return c == '(' || c == '[' || c == '{';
}

/* The innermost bracket pair around offset with lines between its two lines: their lines in *open
 * and *close; FALSE when there is none. */
static gboolean enclosing_pair(GArray *brackets, int offset, int *open, int *close)
{
  GArray *stack = g_array_new(FALSE, FALSE, sizeof(int));
  guint i = 0;
  int depth = 0;
  gboolean found = FALSE;

  /* Before the cursor, the stack ends up with the lines of the brackets still open. */
  for (; i < brackets->len && g_array_index(brackets, Bracket, i).offset < offset; i++) {
    Bracket *b = &g_array_index(brackets, Bracket, i);
    if (is_opening(b->c))
      g_array_append_val(stack, b->line);
    else if (stack->len > 0)
      g_array_set_size(stack, stack->len - 1);
  }
  /* After it, a closing bracket at depth 0 closes the innermost one still open. */
  for (; i < brackets->len && stack->len > 0 && !found; i++) {
    Bracket *b = &g_array_index(brackets, Bracket, i);
    if (is_opening(b->c)) {
      depth++;
    } else if (depth > 0) {
      depth--;
    } else {
      *open = g_array_index(stack, int, stack->len - 1);
      *close = b->line;
      g_array_set_size(stack, stack->len - 1);
      found = *close - *open >= 2;
    }
  }
  g_array_free(stack, TRUE);
  return found;
}

/* Drawing ------------------------------------------------------------------------------------ */

/* Columns of leading blanks of a line (tabs to the next stop), -1 when it is blank. */
static int indent_of(GtkTextBuffer *buffer, int line, int tab_size)
{
  GtkTextIter iter;
  int column = 0;

  if (line < 0 || line >= gtk_text_buffer_get_line_count(buffer))
    return -1;
  gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
  for (; !gtk_text_iter_ends_line(&iter); gtk_text_iter_forward_char(&iter)) {
    gunichar c = gtk_text_iter_get_char(&iter);
    if (c == '\t')
      column += tab_size - column % tab_size;
    else if (c == ' ')
      column++;
    else
      return column;
  }
  return -1;
}

/* Guides on a blank line follow the lines around it. */
static int guide_count(GtkTextBuffer *buffer, int line, int tab_size)
{
  int indent = indent_of(buffer, line, tab_size), above = -1, below = -1;

  if (indent >= 0)
    return (indent + tab_size - 1) / tab_size;
  for (int i = line - 1; i >= 0 && i > line - 200 && above < 0; i--)
    above = indent_of(buffer, i, tab_size);
  for (int i = line + 1, n = gtk_text_buffer_get_line_count(buffer); i < n && i < line + 200 && below < 0; i++)
    below = indent_of(buffer, i, tab_size);
  if (above < 0 && below < 0)
    return 0;
  if (above < 0 || below < 0 || above == below)
    return (MAX(above, below) + tab_size - 1) / tab_size;
  return 1 + MIN(above, below) / tab_size; /* inside the block one of them opens or closes */
}

/* The block around the cursor, again only when the text or the cursor changed. */
static gboolean cursor_pair(TcodeView *self, int *open, int *close)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(self->tab->buffer);
  GtkTextIter cursor;
  int offset;

  gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
  offset = gtk_text_iter_get_offset(&cursor);
  if (!self->brackets || self->brackets_changes != self->tab->view->changes) {
    GtkTextIter start, end;
    char *text;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    text = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
    if (self->brackets)
      g_array_free(self->brackets, TRUE);
    self->brackets = brackets_of(text);
    self->brackets_changes = self->tab->view->changes;
    self->pair.open = -2; /* worked out again below */
    g_free(text);
  }
  if (self->pair.cursor != offset || self->pair.open == -2) {
    self->pair.cursor = offset;
    if (!enclosing_pair(self->brackets, offset, &self->pair.open, &self->pair.close))
      self->pair.open = self->pair.close = -1;
  }
  *open = self->pair.open;
  *close = self->pair.close;
  return *open >= 0;
}

static void draw_guide(GtkSnapshot *snapshot, const GdkRGBA *ink, double x, int y, int height)
{
  gtk_snapshot_append_color(snapshot, ink, &GRAPHENE_RECT_INIT((float)x, (float)y, 1, (float)height));
}

/* A gap row block across the view: filled, with a line at its top and bottom. */
static void draw_gap(GtkSnapshot *snapshot, const GdkRGBA *fill, const GdkRGBA *edge, const GdkRectangle *visible,
                     int y, int height)
{
  float x = (float)visible->x, width = (float)visible->width;

  gtk_snapshot_append_color(snapshot, fill, &GRAPHENE_RECT_INIT(x, (float)y, width, (float)height));
  gtk_snapshot_append_color(snapshot, edge, &GRAPHENE_RECT_INIT(x, (float)y, width, 1));
  gtk_snapshot_append_color(snapshot, edge, &GRAPHENE_RECT_INIT(x, (float)(y + height - 1), width, 1));
}

/* The row standing for count unchanged lines a comparison hides: "⋯ N unchanged lines". */
static void draw_folded(TcodeView *self, GtkSnapshot *snapshot, const GdkRectangle *visible, int y, int height,
                        int count)
{
  char *text = g_strdup_printf("⋯ %d unchanged line%s", count, count == 1 ? "" : "s");
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), text);
  int text_height;

  draw_gap(snapshot, palette_rgba(PAL_INPUT), palette_rgba(PAL_GUIDE), visible, y, height);
  pango_layout_get_pixel_size(layout, NULL, &text_height);
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot,
                         &GRAPHENE_POINT_INIT((float)(visible->x + gtk_text_view_get_left_margin(GTK_TEXT_VIEW(self))),
                                              (float)(y + (height - text_height) / 2)));
  gtk_snapshot_append_layout(snapshot, layout, palette_rgba(PAL_FG_DIM));
  gtk_snapshot_restore(snapshot);
  g_object_unref(layout);
  g_free(text);
}

/* A comparison's changed lines, tinted (green on the new side, red on the old one), its gap
 * rows: on the new side red where lines were deleted, on the old side black where lines were added,
 * and the rows standing for hidden unchanged lines. */
static void draw_diff_gaps(TcodeView *self, GtkSnapshot *snapshot)
{
  GdkRGBA deleted_fill = *palette_rgba(PAL_DIFF_DELETED), deleted_edge = deleted_fill;
  GdkRGBA added_fill = { 0, 0, 0, 0.45f }, added_edge = { 0, 0, 0, 0.70f };
  GdkRGBA inserted_line = *palette_rgba(PAL_DIFF_ADDED), removed_line = *palette_rgba(PAL_DIFF_DELETED);

  deleted_fill.alpha = 0.22f;
  deleted_edge.alpha = 0.45f;
  inserted_line.alpha = 0.13f;
  removed_line.alpha = 0.13f;
  if (palette_light()) { /* black gaps would be holes in a light editor */
    added_fill = *palette_rgba(PAL_GUIDE);
    added_edge = *palette_rgba(PAL_GUIDE_ACTIVE);
  }
  GtkTextView *view = GTK_TEXT_VIEW(self);
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(self->tab->buffer);
  GdkRectangle visible;
  GtkTextIter iter;
  int first, last;

  gtk_text_view_get_visible_rect(view, &visible);
  gtk_text_view_get_line_at_y(view, &iter, visible.y, NULL);
  first = gtk_text_iter_get_line(&iter);
  gtk_text_view_get_line_at_y(view, &iter, visible.y + visible.height, NULL);
  last = gtk_text_iter_get_line(&iter);

  for (int line = first; line <= last; line++) {
    int above_px = 0, below_px = 0, y = 0, height = 0;
    gboolean new_side = diff_gutter_line_gaps(self->tab, line, &above_px, &below_px);
    const GdkRGBA *fill = new_side ? &deleted_fill : &added_fill, *edge = new_side ? &deleted_edge : &added_edge;
    gboolean changed = diff_gutter_line_changed(self->tab, line);
    int row_px, folded = diff_gutter_line_folded(self->tab, line, &row_px);

    if ((above_px <= 0 && below_px <= 0 && !changed) || diff_gutter_line_hidden(self->tab, line))
      continue;
    gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
    gtk_text_view_get_line_yrange(view, &iter, &y, &height);
    if (folded) {
      draw_folded(self, snapshot, &visible, y, row_px, folded);
      y += row_px; /* the line and its gaps below the row */
      height -= row_px;
      above_px -= row_px;
    }
    if (changed) /* the text's rows only, not the gaps around them */
      gtk_snapshot_append_color(snapshot, new_side ? &inserted_line : &removed_line,
                                &GRAPHENE_RECT_INIT((float)visible.x, (float)(y + above_px), (float)visible.width,
                                                    (float)(height - above_px - below_px)));
    if (above_px > 0)
      draw_gap(snapshot, fill, edge, &visible, y, above_px);
    int pad = diff_gutter_line_pad(self->tab, line); /* blank: the facing line wraps to more rows */
    if (below_px > pad)
      draw_gap(snapshot, fill, edge, &visible, y + height - below_px + pad, below_px - pad);
  }
}

/* A character's width in the view's font, measured again only when the font changed (Settings,
 * zoom). */
static double char_width(TcodeView *self)
{
  const PangoFontDescription *font = pango_context_get_font_description(gtk_widget_get_pango_context(GTK_WIDGET(self)));

  if (self->char_width <= 0 || !self->font || !pango_font_description_equal(font, self->font)) {
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), "0000000000");
    int width = 0;
    pango_layout_get_pixel_size(layout, &width, NULL);
    g_object_unref(layout);
    self->char_width = width / 10.0;
    if (self->font)
      pango_font_description_free(self->font);
    self->font = pango_font_description_copy(font);
  }
  return self->char_width;
}

static void draw_guides(TcodeView *self, GtkSnapshot *snapshot)
{
  GtkTextView *view = GTK_TEXT_VIEW(self);
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(self->tab->buffer);
  int ts = MAX((int)gtk_source_view_get_tab_width(GTK_SOURCE_VIEW(self)), 1), first, last, open, close;
  GdkRectangle visible, start;
  GtkTextIter iter;
  double cw = char_width(self), left;

  gtk_text_view_get_visible_rect(view, &visible);
  gtk_text_view_get_line_at_y(view, &iter, visible.y, NULL);
  first = gtk_text_iter_get_line(&iter);
  gtk_text_view_get_iter_location(view, &iter, &start); /* a line's start: the text's left edge */
  left = start.x;
  gtk_text_view_get_line_at_y(view, &iter, visible.y + visible.height, NULL);
  last = gtk_text_iter_get_line(&iter);

  for (int line = first; line <= last; line++) {
    int y, height, above_px = 0, below_px = 0, count;
    if (diff_gutter_line_hidden(self->tab, line))
      continue;
    count = guide_count(buffer, line, ts);
    diff_gutter_line_gaps(self->tab, line, &above_px, &below_px);
    gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
    gtk_text_view_get_line_yrange(view, &iter, &y, &height);
    int ty = y + above_px, th = MAX(height - above_px - below_px, 0);
    for (int k = 0; k < count; k++)
      draw_guide(snapshot, palette_rgba(PAL_GUIDE), left + k * ts * cw, ty, th);
  }
  if (cursor_pair(self, &open, &close)) {
    int column = MIN(indent_of(buffer, open, ts) < 0 ? G_MAXINT : indent_of(buffer, open, ts),
                     indent_of(buffer, close, ts) < 0 ? G_MAXINT : indent_of(buffer, close, ts));
    const GdkRGBA *ink = palette_rgba(self->tab->app->settings.plain ? PAL_GUIDE_ACTIVE : PAL_PAIR1 + column / ts % 3);
    for (int line = MAX(open + 1, first); column != G_MAXINT && line < close && line <= last; line++) {
      int y, height, above_px = 0, below_px = 0, indent = indent_of(buffer, line, ts);
      if ((indent >= 0 && indent <= column) || diff_gutter_line_hidden(self->tab, line))
        continue;
      diff_gutter_line_gaps(self->tab, line, &above_px, &below_px);
      gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
      gtk_text_view_get_line_yrange(view, &iter, &y, &height);
      draw_guide(snapshot, ink, left + column * cw, y + above_px, MAX(height - above_px - below_px, 0));
    }
  }
}

/* The ruler's x after its column (Settings), in buffer coordinates; -1 when it is off. */
static double ruler_x(TcodeView *self)
{
  int column = self->tab->app->settings.ruler;
  GdkRectangle start;
  GtkTextIter iter;

  if (column <= 0)
    return -1;
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(self->tab->buffer), &iter);
  gtk_text_view_get_iter_location(GTK_TEXT_VIEW(self), &iter, &start); /* the text's left edge */
  return start.x + column * char_width(self);
}

/* The ruler, our own: GtkSourceView's right margin moves on the cursor's line when there is a gutter.
 * Over the current line's color, under the text. */
static void draw_ruler(TcodeView *self, GtkSnapshot *snapshot)
{
  double x = ruler_x(self);
  GdkRectangle visible;

  if (x < 0)
    return;
  gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(self), &visible);
  draw_guide(snapshot, palette_rgba(PAL_GUIDE), x, visible.y, visible.height);
}

static void snapshot_layer(GtkTextView *view, GtkTextViewLayer layer, GtkSnapshot *snapshot)
{
  TcodeView *self = TCODE_VIEW(view);

  GTK_TEXT_VIEW_CLASS(tcode_view_parent_class)->snapshot_layer(view, layer, snapshot);
  if (layer == GTK_TEXT_VIEW_LAYER_BELOW_TEXT && self->tab && self->tab->view && !self->tab->view->loading) {
    diff_gutter_check_width(self->tab);
    draw_diff_gaps(self, snapshot);
    draw_ruler(self, snapshot);
    if (self->tab->app->settings.guides)
      draw_guides(self, snapshot);
    diagnostics_draw(self->tab, view, snapshot);
    folding_draw(self->tab, view, snapshot);
    breakpoints_draw(self->tab, view, snapshot);
    multicursor_draw(self->tab, view, snapshot, FALSE);
  } else if (layer == GTK_TEXT_VIEW_LAYER_ABOVE_TEXT && self->tab && self->tab->view && !self->tab->view->loading) {
    inlay_draw(self->tab, view, snapshot);
    blame_draw(self->tab, view, snapshot);
    multicursor_draw(self->tab, view, snapshot, TRUE);
  }
}

static void tcode_view_finalize(GObject *object)
{
  TcodeView *self = TCODE_VIEW(object);

  if (self->brackets)
    g_array_free(self->brackets, TRUE);
  if (self->font)
    pango_font_description_free(self->font);
  G_OBJECT_CLASS(tcode_view_parent_class)->finalize(object);
}

static void tcode_view_class_init(TcodeViewClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = tcode_view_finalize;
  GTK_TEXT_VIEW_CLASS(klass)->snapshot_layer = snapshot_layer;
}

static void tcode_view_init(TcodeView *self)
{
  self->pair.open = -2; /* not worked out yet */
}

/* The cursor moved: when the block around it is another one, its guide is drawn again. */
static void on_mark_set(GtkTextBuffer *buffer, const GtkTextIter *where G_GNUC_UNUSED, GtkTextMark *mark,
                        TcodeView *self)
{
  int open = self->pair.open, close = self->pair.close, now_open, now_close;

  if (mark != gtk_text_buffer_get_insert(buffer) || !self->tab->app->settings.guides || !self->tab->view ||
      self->tab->view->loading)
    return;
  cursor_pair(self, &now_open, &now_close);
  if (now_open != open || now_close != close)
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

GtkSourceView *guides_view_new(Tab *tab, GtkSourceBuffer *buffer)
{
  TcodeView *self = g_object_new(TCODE_TYPE_VIEW, "buffer", buffer, NULL);

  self->tab = tab;
  g_signal_connect_object(buffer, "mark-set", G_CALLBACK(on_mark_set), self, 0);
  return GTK_SOURCE_VIEW(self);
}
