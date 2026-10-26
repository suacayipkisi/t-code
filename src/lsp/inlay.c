/*
 * Inlay hints (see inlay.h).
 */
#include "lsp/inlay.h"

#include "editor/editorview.h"
#include "editor/tabs.h"
#include "lsp/client.h"
#include "lsp/json.h"
#include "window/palette.h"

#define INLAY_MS 400      /* after the last change */
#define INLAY_AROUND 100  /* lines asked for above and below the shown ones */
#define INLAY_PAD 3       /* pixels on each side of a hint's text */
#define INLAY_PEEK_MS 200 /* Ctrl+Alt held this long alone shows (or hides) the hints */

/* Which character holds a hint's gap. */
typedef enum {
  ON_SPACE_BEFORE, /* the space before it ("f(a, |b)") */
  ON_SPACE_AFTER,  /* the space after it ("let x| = 1") */
  REDRAWN,         /* the character before it, drawn again at its cell's start ("f(|a)") */
  AT_END,          /* none: the hint is at the line's end */
} InlayWhere;

typedef struct {
  GtkTextMark *cell; /* the character holding the gap (AT_END: the hint's place) */
  char *text;
  int width;       /* of the gap, in pixels */
  int left, right; /* of it, outside the hint's box: the server's padding */
  InlayWhere where;
} InlayHint;

struct Inlay {
  Tab *tab;
  GtkTextBuffer *buffer; /* outlives the page: the tab holds a reference */
  GArray *hints;         /* InlayHint */
  GHashTable *tags;      /* width * 2 + hidden -> its letter-spacing tag */
  guint update_id;
  GCancellable *asking;
  guint changes;            /* edits so far: an answer about older text is dropped */
  int asked_from, asked_to; /* the lines the hints shown cover; -1 when none were asked for */
};

/* An answer about to come: dropped once cancelled (the Inlay may be gone then). */
typedef struct {
  Inlay *inlay;
  GCancellable *cancel;
  guint changes;
  int from, to;
} InlayAsking;

static Inlay *inlay_of(Tab *tab)
{
  return tab && tab->view ? tab->view->inlay : NULL;
}

static gboolean inlay_wanted(Inlay *in)
{
  /* Ctrl+Alt held turns the setting round for the while. */
  return in->tab->app->settings.inlay_hints != in->tab->app->editors.inlay_peek && !in->tab->view->readonly;
}

static void inlay_clear(Inlay *in)
{
  GtkTextIter start, end;
  GHashTableIter iter;
  gpointer tag;

  gtk_text_buffer_get_bounds(in->buffer, &start, &end);
  g_hash_table_iter_init(&iter, in->tags);
  while (g_hash_table_iter_next(&iter, NULL, &tag))
    gtk_text_buffer_remove_tag(in->buffer, tag, &start, &end);
  for (guint i = 0; i < in->hints->len; i++) {
    InlayHint *h = &g_array_index(in->hints, InlayHint, i);
    gtk_text_buffer_delete_mark(in->buffer, h->cell);
    g_free(h->text);
  }
  g_array_set_size(in->hints, 0);
  in->asked_from = in->asked_to = -1;
}

static void inlay_stop_asking(Inlay *in)
{
  if (in->asking) {
    g_cancellable_cancel(in->asking);
    g_clear_object(&in->asking);
  }
}

/* The tag making a gap of width pixels at its character. Pango splits it around the glyph, which
 * so sits in the middle: the hint's box, drawn over it, hides it there. */
static GtkTextTag *inlay_gap_tag(Inlay *in, int width)
{
  GtkTextTag *tag = g_hash_table_lookup(in->tags, GINT_TO_POINTER(width));

  if (!tag) {
    tag = gtk_text_buffer_create_tag(in->buffer, NULL, "letter-spacing", width * PANGO_SCALE, NULL);
    g_hash_table_insert(in->tags, GINT_TO_POINTER(width), tag);
  }
  return tag;
}

static int inlay_text_width(GtkWidget *view, const char *text)
{
  PangoLayout *layout = gtk_widget_create_pango_layout(view, text);
  int width;

  pango_layout_get_pixel_size(layout, &width, NULL);
  g_object_unref(layout);
  return width;
}

static gboolean inlay_padded(JsonNode *hint, const char *side)
{
  JsonNode *padding = json_at(hint, side);

  return padding && JSON_NODE_HOLDS_VALUE(padding) && json_node_get_boolean(padding);
}

/* A hint's text: its label (a string or parts). */
static char *inlay_label_of(JsonNode *hint)
{
  JsonNode *label = json_at(hint, "label");
  GString *text = g_string_new(NULL);

  if (label && JSON_NODE_HOLDS_VALUE(label))
    g_string_append(text, json_node_get_string(label));
  else if (label && JSON_NODE_HOLDS_ARRAY(label))
    for (guint i = 0; i < json_array_get_length(json_node_get_array(label)); i++)
      g_string_append(text, json_string_at(json_array_get_element(json_node_get_array(label), i), "value") ?: "");
  return g_string_free(text, FALSE);
}

/* Puts a hint at iter, with a space's width outside its box on each padded side; FALSE when there is
 * no place for it (the last one took the character). */
static gboolean inlay_place(Inlay *in, GtkTextIter *at, const char *text, gboolean left, gboolean right, int *last_cell)
{
  GtkTextIter before = *at, cell, next;
  gboolean has_before = !gtk_text_iter_starts_line(at) && gtk_text_iter_backward_char(&before);
  InlayHint h = { 0 };

  if (gtk_text_iter_ends_line(at)) {
    h.where = AT_END;
    cell = *at;
  } else if (has_before && gtk_text_iter_get_char(&before) == ' ') {
    h.where = ON_SPACE_BEFORE;
    cell = before;
  } else if (gtk_text_iter_get_char(at) == ' ') {
    h.where = ON_SPACE_AFTER;
    cell = *at;
  } else if (has_before && gtk_text_iter_get_char(&before) != '\t') {
    h.where = REDRAWN;
    cell = before;
  } else {
    return FALSE;
  }
  if (gtk_text_iter_get_offset(&cell) <= *last_cell)
    return FALSE; /* one gap per character */
  h.text = g_strdup(text);
  h.left = left ? inlay_text_width(GTK_WIDGET(in->tab->source), " ") : 0;
  h.right = right ? inlay_text_width(GTK_WIDGET(in->tab->source), " ") : 0;
  h.width = inlay_text_width(GTK_WIDGET(in->tab->source), text) + 2 * INLAY_PAD + h.left + h.right;
  h.cell = gtk_text_buffer_create_mark(in->buffer, NULL, &cell, TRUE);
  if (h.where != AT_END) {
    next = cell;
    gtk_text_iter_forward_char(&next);
    gtk_text_buffer_apply_tag(in->buffer, inlay_gap_tag(in, h.width), &cell, &next);
  }
  *last_cell = gtk_text_iter_get_offset(&cell);
  g_array_append_val(in->hints, h);
  return TRUE;
}

static void inlay_on_hints(JsonNode *result, gpointer data)
{
  InlayAsking *a = data;
  Inlay *in = a->inlay;
  JsonArray *array = result && JSON_NODE_HOLDS_ARRAY(result) ? json_node_get_array(result) : NULL;
  int last_cell = -1;

  if (g_cancellable_is_cancelled(a->cancel) || a->changes != in->changes || !result) {
    g_object_unref(a->cancel);
    g_free(a);
    return;
  }
  g_clear_object(&in->asking);
  inlay_clear(in);
  in->asked_from = a->from;
  in->asked_to = a->to;
  for (guint i = 0; array && i < json_array_get_length(array); i++) {
    JsonNode *hint = json_array_get_element(array, i);
    JsonNode *position = json_at(hint, "position");
    GtkTextIter at;
    char *text;
    if (!position)
      continue;
    lsp_iter_at(in->tab, position, &at);
    text = g_strstrip(inlay_label_of(hint));
    if (*text)
      inlay_place(in, &at, text, inlay_padded(hint, "paddingLeft"), inlay_padded(hint, "paddingRight"), &last_cell);
    g_free(text);
  }
  gtk_widget_queue_draw(GTK_WIDGET(in->tab->source));
  g_object_unref(a->cancel);
  g_free(a);
}

/* The lines shown, from first to last. */
static void inlay_shown_lines(Inlay *in, int *first, int *last)
{
  GtkTextView *view = GTK_TEXT_VIEW(in->tab->source);
  GdkRectangle visible;
  GtkTextIter top, bottom;

  gtk_text_view_get_visible_rect(view, &visible);
  if (visible.height <= 0) { /* not shown yet */
    *first = 0;
    *last = 2 * INLAY_AROUND;
    return;
  }
  gtk_text_view_get_line_at_y(view, &top, visible.y, NULL);
  gtk_text_view_get_line_at_y(view, &bottom, visible.y + visible.height, NULL);
  *first = gtk_text_iter_get_line(&top);
  *last = gtk_text_iter_get_line(&bottom);
}

static gboolean inlay_ask(gpointer data)
{
  Inlay *in = data;
  GtkTextIter start, end;
  int first, last;

  in->update_id = 0;
  if (!inlay_wanted(in) || !lsp_has(in->tab))
    return G_SOURCE_REMOVE;
  inlay_shown_lines(in, &first, &last);
  first = MAX(first - INLAY_AROUND, 0);
  last = MIN(last + INLAY_AROUND, gtk_text_buffer_get_line_count(in->buffer) - 1);
  gtk_text_buffer_get_iter_at_line(in->buffer, &start, first);
  gtk_text_buffer_get_iter_at_line(in->buffer, &end, last);
  if (!gtk_text_iter_ends_line(&end))
    gtk_text_iter_forward_to_line_end(&end);

  InlayAsking *a = g_new0(InlayAsking, 1);
  char *document = lsp_document_params(in->tab), *range = lsp_range(in->tab, &start, &end);
  char *params = g_strdup_printf("{%s,\"range\":%s}", document, range);
  inlay_stop_asking(in);
  in->asking = g_cancellable_new();
  a->inlay = in;
  a->cancel = g_object_ref(in->asking);
  a->changes = in->changes;
  a->from = first;
  a->to = last;
  lsp_request(in->tab, "textDocument/inlayHint", params, in->asking, inlay_on_hints, a);
  g_free(params);
  g_free(range);
  g_free(document);
  return G_SOURCE_REMOVE;
}

void inlay_update(Tab *tab)
{
  Inlay *in = inlay_of(tab);

  if (!in || !inlay_wanted(in))
    return;
  g_clear_handle_id(&in->update_id, g_source_remove);
  in->update_id = g_timeout_add(INLAY_MS, inlay_ask, in);
}

static void inlay_on_changed(GtkTextBuffer *buffer G_GNUC_UNUSED, Inlay *in)
{
  in->changes++; /* the hints move with the text until the new ones come */
}

void inlay_apply_setting(Tab *tab)
{
  Inlay *in = inlay_of(tab);

  if (!in)
    return;
  if (inlay_wanted(in)) {
    if (in->asked_from < 0 && !in->update_id)
      in->update_id = g_idle_add(inlay_ask, in); /* at once: Ctrl+Alt may be held for them */
    return;
  }
  inlay_stop_asking(in);
  g_clear_handle_id(&in->update_id, g_source_remove);
  inlay_clear(in); /* also what was asked for: the text may change while they are off */
  gtk_widget_queue_draw(GTK_WIDGET(tab->source));
}

void inlay_peek(App *t, gboolean on)
{
  int n = tab_count(t);

  g_clear_handle_id(&t->editors.peek_id, g_source_remove);
  if (t->editors.inlay_peek == on)
    return;
  t->editors.inlay_peek = on;
  for (int i = 0; i < n; i++)
    if (tab_nth(t, i)->kind == TAB_EDITOR)
      inlay_apply_setting(tab_nth(t, i));
}

static gboolean inlay_peek_start(gpointer data)
{
  App *t = data;

  t->editors.peek_id = 0;
  inlay_peek(t, TRUE);
  return G_SOURCE_REMOVE;
}

static gboolean is_control_or_alt(guint key)
{
  return key == GDK_KEY_Control_L || key == GDK_KEY_Control_R || key == GDK_KEY_Alt_L || key == GDK_KEY_Alt_R;
}

/* Ctrl+Alt alone held a moment: the peek starts; any other key (Ctrl+Alt+D) ends it or stops it from
 * starting, so shortcuts do not flash the hints. */
static gboolean on_peek_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                            GdkModifierType state, App *t)
{
  GdkModifierType held = state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK | GDK_SUPER_MASK);

  if (key == GDK_KEY_Control_L || key == GDK_KEY_Control_R)
    held |= GDK_CONTROL_MASK;
  else if (key == GDK_KEY_Alt_L || key == GDK_KEY_Alt_R)
    held |= GDK_ALT_MASK;
  if (is_control_or_alt(key) && held == (GDK_CONTROL_MASK | GDK_ALT_MASK)) {
    if (!t->editors.inlay_peek && !t->editors.peek_id)
      t->editors.peek_id = g_timeout_add(INLAY_PEEK_MS, inlay_peek_start, t);
  } else {
    inlay_peek(t, FALSE);
  }
  return GDK_EVENT_PROPAGATE;
}

static void on_peek_release(GtkEventControllerKey *keys G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                            GdkModifierType state G_GNUC_UNUSED, App *t)
{
  if (is_control_or_alt(key))
    inlay_peek(t, FALSE);
}

static void on_peek_window_active(GtkWindow *window, GParamSpec *spec G_GNUC_UNUSED, App *t)
{
  if (!gtk_window_is_active(window)) /* the release went to another window */
    inlay_peek(t, FALSE);
}

void inlay_peek_keys(App *t, GtkWindow *window)
{
  GtkEventController *keys = gtk_event_controller_key_new();

  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_peek_key), t);
  g_signal_connect(keys, "key-released", G_CALLBACK(on_peek_release), t);
  gtk_widget_add_controller(GTK_WIDGET(window), keys);
  g_signal_connect(window, "notify::is-active", G_CALLBACK(on_peek_window_active), t);
}

/* Drawing ---------------------------------------------------------------------------------- */

static void inlay_draw_text(GtkSnapshot *snapshot, GtkWidget *view, const char *text, int x, const GdkRectangle *line,
                            const GdkRGBA *color)
{
  PangoLayout *layout = gtk_widget_create_pango_layout(view, text);
  int height;

  pango_layout_get_pixel_size(layout, NULL, &height);
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT((float)x, (float)(line->y + (line->height - height) / 2)));
  gtk_snapshot_append_layout(snapshot, layout, color);
  gtk_snapshot_restore(snapshot);
  g_object_unref(layout);
}

void inlay_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot)
{
  Inlay *in = inlay_of(tab);
  GdkRectangle visible;
  int first, last;

  if (!in || !inlay_wanted(in))
    return;
  inlay_shown_lines(in, &first, &last);
  if (in->asked_from >= 0 && (first < in->asked_from || last > in->asked_to) && !in->update_id)
    inlay_update(tab); /* scrolled past what was asked for */
  if (in->hints->len == 0)
    return;
  gtk_text_view_get_visible_rect(view, &visible);
  /* Opaque: a REDRAWN character's own glyph is under the box. */
  const GdkRGBA *panel = palette_rgba(PAL_PANEL), *dim = palette_rgba(PAL_FG_DIM);
  GdkRGBA fill = { panel->red + (dim->red - panel->red) * 0.15f, panel->green + (dim->green - panel->green) * 0.15f,
                   panel->blue + (dim->blue - panel->blue) * 0.15f, 1 };
  for (guint i = 0; i < in->hints->len; i++) {
    InlayHint *h = &g_array_index(in->hints, InlayHint, i);
    GtkTextIter cell, next;
    GdkRectangle at, after;
    int x, line;
    gtk_text_buffer_get_iter_at_mark(in->buffer, &cell, h->cell);
    line = gtk_text_iter_get_line(&cell);
    if (line < first || line > last)
      continue;
    gtk_text_view_get_iter_location(view, &cell, &at);
    next = cell;
    if (h->where != AT_END && gtk_text_iter_forward_char(&next))
      gtk_text_view_get_iter_location(view, &next, &after);
    else
      after = at;
    switch (h->where) {
      case ON_SPACE_BEFORE: x = after.x - h->width; break; /* the space's own width first */
      case ON_SPACE_AFTER: x = at.x; break;
      case REDRAWN: {
        char *ch = gtk_text_iter_get_text(&cell, &next);
        inlay_draw_text(snapshot, GTK_WIDGET(view), ch, at.x, &at, palette_rgba(PAL_FG));
        x = after.x - h->width;
        g_free(ch);
        break;
      }
      default: x = at.x + INLAY_PAD; break;
    }
    x += h->left;
    gtk_snapshot_append_color(snapshot, &fill,
                              &GRAPHENE_RECT_INIT((float)x + 1, (float)at.y + 1,
                                                  (float)(h->width - h->left - h->right) - 2, (float)at.height - 2));
    inlay_draw_text(snapshot, GTK_WIDGET(view), h->text, x + INLAY_PAD, &at, palette_rgba(PAL_FG_DIM));
  }
}

char *inlay_text(Tab *tab)
{
  Inlay *in = inlay_of(tab);
  GString *out = g_string_new(NULL);

  for (guint i = 0; in && i < in->hints->len; i++) {
    InlayHint *h = &g_array_index(in->hints, InlayHint, i);
    GtkTextIter cell;
    gtk_text_buffer_get_iter_at_mark(in->buffer, &cell, h->cell);
    if (h->where == ON_SPACE_BEFORE || h->where == REDRAWN)
      gtk_text_iter_forward_char(&cell); /* where the hint is: after its cell */
    g_string_append_printf(out, "%d:%d %s\n", gtk_text_iter_get_line(&cell), gtk_text_iter_get_line_offset(&cell),
                           h->text);
  }
  return g_string_free(out, FALSE);
}

/* Setup ------------------------------------------------------------------------------------ */

void inlay_attach(Tab *tab)
{
  Inlay *in = g_new0(Inlay, 1);

  in->tab = tab;
  in->buffer = GTK_TEXT_BUFFER(tab->buffer);
  in->hints = g_array_new(FALSE, FALSE, sizeof(InlayHint));
  in->tags = g_hash_table_new(NULL, NULL);
  in->asked_from = in->asked_to = -1;
  g_signal_connect(in->buffer, "changed", G_CALLBACK(inlay_on_changed), in);
  tab->view->inlay = in;
}

void inlay_free(Inlay *in)
{
  GHashTableIter iter;
  gpointer tag;

  if (!in)
    return;
  inlay_stop_asking(in);
  g_clear_handle_id(&in->update_id, g_source_remove);
  g_signal_handlers_disconnect_by_data(in->buffer, in);
  inlay_clear(in);
  g_hash_table_iter_init(&iter, in->tags);
  while (g_hash_table_iter_next(&iter, NULL, &tag))
    gtk_text_tag_table_remove(gtk_text_buffer_get_tag_table(in->buffer), tag);
  g_hash_table_destroy(in->tags);
  g_array_free(in->hints, TRUE);
  g_free(in);
}
