/*
 * Sticky scroll (see sticky.h): a drawing area over the top of the text. Which lines: the heads of
 * the { } blocks (editor/blocks.c) around the first line shown below them. How: each line laid out
 * with Pango in the view's font and tab stops, colored from the buffer's tags (code coloring and
 * clangd's semantic colors), and scrolled sideways with the text.
 */
#include "editor/sticky.h"

#include "editor/blocks.h"
#include "editor/editorview.h"
#include "editor/folding.h"
#include "window/palette.h"

#include <string.h>

#define STICKY_LINES 5  /* block heads shown at most */
#define STICKY_SHADOW 4 /* pixels of shadow under them */

struct Sticky {
  Tab *tab;
  GtkWidget *area;
  int lines[STICKY_LINES]; /* the head lines shown, outermost first */
  int count;
  int row; /* the height of one, in pixels */
  guint update_id;
};

/* The height of one row of text in the view's font. */
static int sticky_row_height(Sticky *s)
{
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(s->tab->source), "Mg");
  int height = 0;

  pango_layout_get_pixel_size(layout, NULL, &height);
  g_object_unref(layout);
  return MAX(height, 1);
}

/* Off: the setting, a comparison (its gap rows keep the sides level), or a file being loaded. */
static gboolean sticky_wanted(Tab *tab)
{
  return tab->app->settings.sticky_scroll && tab->view && !tab->view->loading && !tab->compare.old_side &&
         !tab->compare.with;
}

/* The heads of the blocks around line top that start above it, outermost first, up to STICKY_LINES;
 * how many. */
static int sticky_heads_around(Tab *tab, int top, int *lines)
{
  GArray *blocks = folding_blocks(tab);
  int n = 0;

  for (guint i = 0; i < blocks->len && n < STICKY_LINES; i++) {
    Block *b = &g_array_index(blocks, Block, i);
    if (b->head >= top)
      break;
    if (b->close_line < top || b->open_line > top)
      continue;
    if (n > 0 && lines[n - 1] == b->head) /* "} else {": one line */
      continue;
    lines[n++] = b->head;
  }
  return n;
}

/* Works out the lines again: the first line shown below n sticky lines may be inside more blocks,
 * so it is looked at again until that settles. */
static gboolean update_sticky(gpointer data)
{
  Sticky *s = data;
  GtkTextView *view = GTK_TEXT_VIEW(s->tab->source);
  int lines[STICKY_LINES], n = 0;

  s->update_id = 0;
  if (sticky_wanted(s->tab)) {
    GdkRectangle visible;
    gtk_text_view_get_visible_rect(view, &visible);
    s->row = sticky_row_height(s);
    for (int pass = 0; pass <= STICKY_LINES; pass++) {
      GtkTextIter top;
      int now;
      gtk_text_view_get_line_at_y(view, &top, visible.y + n * s->row, NULL);
      now = sticky_heads_around(s->tab, gtk_text_iter_get_line(&top), lines);
      if (now == n)
        break;
      n = now;
    }
  }
  if (n != s->count || memcmp(lines, s->lines, n * sizeof *lines) != 0) {
    memcpy(s->lines, lines, n * sizeof *lines);
    s->count = n;
    gtk_widget_set_size_request(s->area, -1, n ? n * s->row + STICKY_SHADOW : 0);
    gtk_widget_set_visible(s->area, n > 0);
  }
  gtk_widget_queue_draw(s->area);
  return G_SOURCE_REMOVE;
}

static void update_sticky_soon(Sticky *s)
{
  if (!s->update_id)
    s->update_id = g_idle_add_full(G_PRIORITY_HIGH_IDLE + 15, update_sticky, s, NULL); /* before the redraw */
}

/* Drawing ------------------------------------------------------------------------------------ */

/* The text of a line with its colors and slant, as the buffer's tags give them (the highest
 * priority wins, as in the view). */
static PangoLayout *sticky_line_layout(Sticky *s, int line)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(s->tab->buffer);
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(s->tab->source), NULL);
  PangoAttrList *attrs = pango_attr_list_new();
  PangoTabArray *tabs = gtk_text_view_get_tabs(GTK_TEXT_VIEW(s->tab->source));
  GtkTextIter start, end, at;
  char *text;

  gtk_text_buffer_get_iter_at_line(buffer, &start, line);
  end = start;
  if (!gtk_text_iter_ends_line(&end))
    gtk_text_iter_forward_to_line_end(&end);
  text = gtk_text_iter_get_slice(&start, &end);
  for (at = start; gtk_text_iter_compare(&at, &end) < 0;) {
    GtkTextIter next = at;
    GSList *tags = gtk_text_iter_get_tags(&at); /* by priority, lowest first */
    GdkRGBA *color = NULL;
    PangoStyle style = PANGO_STYLE_NORMAL;
    PangoWeight weight = PANGO_WEIGHT_NORMAL;
    gtk_text_iter_forward_to_tag_toggle(&next, NULL);
    if (gtk_text_iter_compare(&next, &end) > 0)
      next = end;
    for (GSList *each = tags; each; each = each->next) {
      gboolean has_color, has_style, has_weight;
      g_object_get(each->data, "foreground-set", &has_color, "style-set", &has_style, "weight-set", &has_weight, NULL);
      if (has_color) {
        if (color)
          gdk_rgba_free(color);
        g_object_get(each->data, "foreground-rgba", &color, NULL);
      }
      if (has_style)
        g_object_get(each->data, "style", &style, NULL);
      if (has_weight)
        g_object_get(each->data, "weight", &weight, NULL);
    }
    guint from = gtk_text_iter_get_line_index(&at), to = gtk_text_iter_get_line_index(&next);
    PangoAttribute *attr;
    if (color) {
      attr = pango_attr_foreground_new(color->red * 65535, color->green * 65535, color->blue * 65535);
      attr->start_index = from;
      attr->end_index = to;
      pango_attr_list_insert(attrs, attr);
      gdk_rgba_free(color);
    }
    attr = pango_attr_style_new(style);
    attr->start_index = from;
    attr->end_index = to;
    pango_attr_list_insert(attrs, attr);
    attr = pango_attr_weight_new(weight);
    attr->start_index = from;
    attr->end_index = to;
    pango_attr_list_insert(attrs, attr);
    g_slist_free(tags);
    if (gtk_text_iter_equal(&next, &at))
      break;
    at = next;
  }
  pango_layout_set_text(layout, text, -1);
  pango_layout_set_attributes(layout, attrs);
  if (tabs) {
    pango_layout_set_tabs(layout, tabs);
    pango_tab_array_free(tabs);
  }
  pango_attr_list_unref(attrs);
  g_free(text);
  return layout;
}

/* Where the text starts (the gutter's right edge, sideways scrolling) and where the line numbers end,
 * in the area's coordinates (the same as the view's: both start at the overlay's left). */
static void sticky_columns(Sticky *s, double *text_x, double *gutter_right, double *numbers_right)
{
  GtkTextView *view = GTK_TEXT_VIEW(s->tab->source);
  GtkWidget *gutter = gtk_text_view_get_gutter(view, GTK_TEXT_WINDOW_LEFT);
  GtkWidget *numbers = folding_line_numbers(s->tab);
  graphene_rect_t bounds;
  int x = 0;

  gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, gtk_text_view_get_left_margin(view), 0, &x, NULL);
  *text_x = x;
  *gutter_right = gutter ? gtk_widget_get_width(gutter) : 0;
  *numbers_right = *gutter_right;
  if (numbers && gtk_widget_compute_bounds(numbers, GTK_WIDGET(view), &bounds))
    *numbers_right =
        bounds.origin.x + bounds.size.width - gtk_source_gutter_renderer_get_xpad(GTK_SOURCE_GUTTER_RENDERER(numbers));
}

/* The widget ------------------------------------------------------------------------------- */

struct _TcodeSticky {
  GtkWidget parent;
  Sticky *sticky;
};

#define TCODE_TYPE_STICKY (tcode_sticky_get_type())
G_DECLARE_FINAL_TYPE(TcodeSticky, tcode_sticky, TCODE, STICKY, GtkWidget)
G_DEFINE_FINAL_TYPE(TcodeSticky, tcode_sticky, GTK_TYPE_WIDGET)

static void snapshot_sticky(GtkWidget *widget, GtkSnapshot *snapshot)
{
  Sticky *s = TCODE_STICKY(widget)->sticky;
  const GdkRGBA *background = palette_rgba(PAL_PANEL), *number_ink = palette_rgba(PAL_LINE_NUMBER),
                *text_ink = palette_rgba(PAL_FG), *line = palette_rgba(PAL_BORDER);
  int width = gtk_widget_get_width(widget);
  double text_x, gutter_right, numbers_right;

  if (!s || !s->count)
    return;
  sticky_columns(s, &text_x, &gutter_right, &numbers_right);
  gtk_snapshot_append_color(snapshot, background, &GRAPHENE_RECT_INIT(0, 0, width, s->count * s->row));
  for (int i = 0; i < s->count; i++) {
    char number[24];
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(s->tab->source), NULL);
    int number_width;
    g_snprintf(number, sizeof number, "%d", s->lines[i] + 1);
    pango_layout_set_text(layout, number, -1);
    pango_layout_get_pixel_size(layout, &number_width, NULL);
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(numbers_right - number_width, i * s->row));
    gtk_snapshot_append_layout(snapshot, layout, number_ink);
    gtk_snapshot_restore(snapshot);
    g_object_unref(layout);

    layout = sticky_line_layout(s, s->lines[i]);
    gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(gutter_right, i * s->row, width - gutter_right, s->row));
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(text_x, i * s->row));
    gtk_snapshot_append_layout(snapshot, layout, text_ink);
    gtk_snapshot_restore(snapshot);
    gtk_snapshot_pop(snapshot);
    g_object_unref(layout);
  }
  /* A line under them, and a shadow fading into the text. */
  gtk_snapshot_append_color(snapshot, line, &GRAPHENE_RECT_INIT(0, s->count * s->row - 1, width, 1));
  GskColorStop stops[] = { { 0, { 0, 0, 0, 0.45f } }, { 1, { 0, 0, 0, 0 } } };
  gtk_snapshot_append_linear_gradient(snapshot, &GRAPHENE_RECT_INIT(0, s->count * s->row, width, STICKY_SHADOW),
                                      &GRAPHENE_POINT_INIT(0, s->count * s->row),
                                      &GRAPHENE_POINT_INIT(0, s->count * s->row + STICKY_SHADOW), stops,
                                      G_N_ELEMENTS(stops));
}

static void tcode_sticky_class_init(TcodeStickyClass *klass)
{
  GTK_WIDGET_CLASS(klass)->snapshot = snapshot_sticky;
}

static void tcode_sticky_init(TcodeSticky *self G_GNUC_UNUSED)
{
}

/* A click on a sticky line goes to it, at the top of the view. */
static void on_sticky_click(GtkGestureClick *click G_GNUC_UNUSED, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y,
                            Sticky *s)
{
  int index = s->row > 0 ? (int)(y / s->row) : -1;
  GtkTextIter at;

  if (index < 0 || index >= s->count)
    return;
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(s->tab->buffer), &at, s->lines[index]);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(s->tab->buffer), &at);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(s->tab->source), &at, 0, TRUE, 0, 0);
  gtk_widget_grab_focus(GTK_WIDGET(s->tab->source));
}

/* Updates -------------------------------------------------------------------------------------- */

/* The view scrolled: the area is connected (its handlers go with it), the Sticky looked up through it. */
static void on_sticky_scrolled(GtkWidget *area)
{
  Sticky *s = TCODE_STICKY(area)->sticky;

  if (s)
    update_sticky_soon(s);
}

/* The view's adjustments come once it is in its scrolled window. */
static void watch_sticky_adjustments(GtkWidget *area, GParamSpec *spec G_GNUC_UNUSED, GObject *view)
{
  GtkAdjustment *vertical = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view));
  GtkAdjustment *horizontal = gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(view));

  if (vertical)
    g_signal_connect_object(vertical, "value-changed", G_CALLBACK(on_sticky_scrolled), area, G_CONNECT_SWAPPED);
  if (horizontal)
    g_signal_connect_object(horizontal, "value-changed", G_CALLBACK(gtk_widget_queue_draw), area, G_CONNECT_SWAPPED);
}

static void on_sticky_text_changed(Sticky *s)
{
  update_sticky_soon(s);
}

/* Colors came (code coloring, clangd's): drawn again, once for many tags. */
static void on_sticky_tagged(Sticky *s)
{
  if (s->count)
    update_sticky_soon(s);
}

void sticky_attach(Tab *tab)
{
  Sticky *s = g_new0(Sticky, 1);
  GtkGesture *click = gtk_gesture_click_new();

  s->tab = tab;
  s->area = g_object_ref_sink(g_object_new(TCODE_TYPE_STICKY, NULL)); /* ours too: see sticky_free */
  TCODE_STICKY(s->area)->sticky = s;
  gtk_widget_set_valign(s->area, GTK_ALIGN_START);
  gtk_widget_set_visible(s->area, FALSE);
  gtk_widget_set_cursor_from_name(s->area, "pointer");
  g_signal_connect(click, "released", G_CALLBACK(on_sticky_click), s);
  gtk_widget_add_controller(s->area, GTK_EVENT_CONTROLLER(click));
  gtk_overlay_add_overlay(GTK_OVERLAY(tab->view->overlay), s->area);
  g_signal_connect_object(tab->source, "notify::vadjustment", G_CALLBACK(watch_sticky_adjustments), s->area,
                          G_CONNECT_SWAPPED);
  watch_sticky_adjustments(s->area, NULL, G_OBJECT(tab->source));
  g_signal_connect_swapped(tab->buffer, "changed", G_CALLBACK(on_sticky_text_changed), s);
  g_signal_connect_swapped(tab->buffer, "apply-tag", G_CALLBACK(on_sticky_tagged), s);
  g_signal_connect_swapped(tab->buffer, "remove-tag", G_CALLBACK(on_sticky_tagged), s);
  tab->view->sticky = s;
}

void sticky_apply_settings(Tab *tab)
{
  if (tab->view->sticky)
    update_sticky_soon(tab->view->sticky);
}

void sticky_free(Sticky *s)
{
  if (!s)
    return;
  g_signal_handlers_disconnect_by_data(s->tab->buffer, s);
  /* The page went first: the area lives on with our reference, and must not reach us. */
  TCODE_STICKY(s->area)->sticky = NULL;
  g_object_unref(s->area);
  g_clear_handle_id(&s->update_id, g_source_remove);
  g_free(s);
}
