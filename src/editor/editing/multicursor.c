/*
 * Multiple cursors (see multicursor.h).
 */
#include "editor/editing/multicursor.h"

#include "editor/editing/editing.h"
#include "editor/editorview.h"
#include "window/palette.h"

#include <string.h>

#define MC_CARET_WIDTH 2

/* A cursor: where it is and where its selection starts (the same place when nothing is selected). */
typedef struct {
  GtkTextMark *insert, *bound;
} McCaret;

struct MultiCursor {
  Tab *tab;
  GtkTextBuffer *buffer; /* outlives the page: the tab holds a reference */
  GArray *extra;         /* McCaret: the cursors besides the buffer's own */
  int last;              /* the one added last (Ctrl+D, Ctrl+Shift+Up / Down go on from it); -1: the buffer's */
  gboolean boxing;       /* a Shift+Alt drag is selecting a column */
  int box_line, box_col; /* where it started */
  GCancellable *cancel;  /* a paste reading the clipboard */
};

static MultiCursor *mc_of(Tab *tab)
{
  return tab && tab->view ? tab->view->multi : NULL;
}

gboolean multicursor_active(Tab *tab)
{
  MultiCursor *m = mc_of(tab);

  return m && m->extra->len > 0;
}

static McCaret mc_primary(MultiCursor *m)
{
  return (McCaret){ gtk_text_buffer_get_insert(m->buffer), gtk_text_buffer_get_selection_bound(m->buffer) };
}

static void mc_caret_iters(MultiCursor *m, const McCaret *c, GtkTextIter *insert, GtkTextIter *bound)
{
  gtk_text_buffer_get_iter_at_mark(m->buffer, insert, c->insert);
  gtk_text_buffer_get_iter_at_mark(m->buffer, bound, c->bound);
}

static void mc_caret_set(MultiCursor *m, const McCaret *c, const GtkTextIter *insert, const GtkTextIter *bound)
{
  if (c->insert == gtk_text_buffer_get_insert(m->buffer)) {
    gtk_text_buffer_select_range(m->buffer, insert, bound);
  } else {
    gtk_text_buffer_move_mark(m->buffer, c->insert, insert);
    gtk_text_buffer_move_mark(m->buffer, c->bound, bound);
  }
}

static void mc_add_caret(MultiCursor *m, const GtkTextIter *insert, const GtkTextIter *bound)
{
  McCaret c = { gtk_text_buffer_create_mark(m->buffer, NULL, insert, FALSE),
                gtk_text_buffer_create_mark(m->buffer, NULL, bound, FALSE) };

  g_array_append_val(m->extra, c);
  m->last = (int)m->extra->len - 1;
}

static void mc_remove_caret(MultiCursor *m, guint i)
{
  McCaret *c = &g_array_index(m->extra, McCaret, i);

  gtk_text_buffer_delete_mark(m->buffer, c->insert);
  gtk_text_buffer_delete_mark(m->buffer, c->bound);
  g_array_remove_index(m->extra, i);
  if (m->last == (int)i)
    m->last = (int)m->extra->len - 1;
  else if (m->last > (int)i)
    m->last--;
}

/* A cursor's selection as offsets, start first. */
static void mc_caret_range(MultiCursor *m, const McCaret *c, int *start, int *end)
{
  GtkTextIter insert, bound;

  mc_caret_iters(m, c, &insert, &bound);
  *start = MIN(gtk_text_iter_get_offset(&insert), gtk_text_iter_get_offset(&bound));
  *end = MAX(gtk_text_iter_get_offset(&insert), gtk_text_iter_get_offset(&bound));
}

/* Extra cursors that met another one (an edit or a move brought them together) go. */
static void mc_merge(MultiCursor *m)
{
  McCaret first = mc_primary(m);
  int start, end;

  mc_caret_range(m, &first, &start, &end);
  for (guint i = 0; i < m->extra->len;) {
    int s, e, os = start, oe = end;
    gboolean overlaps;
    mc_caret_range(m, &g_array_index(m->extra, McCaret, i), &s, &e);
    overlaps = (s == os && e == oe) || (s < oe && os < e);
    for (guint j = 0; !overlaps && j < i; j++) {
      mc_caret_range(m, &g_array_index(m->extra, McCaret, j), &os, &oe);
      overlaps = (s == os && e == oe) || (s < oe && os < e);
    }
    if (overlaps)
      mc_remove_caret(m, i);
    else
      i++;
  }
}

static void mc_changed(MultiCursor *m)
{
  mc_merge(m);
  gtk_widget_queue_draw(GTK_WIDGET(m->tab->source));
}

void multicursor_clear(Tab *tab)
{
  MultiCursor *m = mc_of(tab);

  if (!m || m->extra->len == 0)
    return;
  while (m->extra->len > 0)
    mc_remove_caret(m, m->extra->len - 1);
  m->last = -1;
  gtk_widget_queue_draw(GTK_WIDGET(tab->source));
}

/* Every cursor, the buffer's first. */
static GArray *mc_all_carets(MultiCursor *m)
{
  GArray *all = g_array_new(FALSE, FALSE, sizeof(McCaret));
  McCaret first = mc_primary(m);

  g_array_append_val(all, first);
  g_array_append_vals(all, m->extra->data, m->extra->len);
  return all;
}

static int mc_by_start(gconstpointer a, gconstpointer b, gpointer data)
{
  int sa, ea, sb, eb;

  mc_caret_range(data, a, &sa, &ea);
  mc_caret_range(data, b, &sb, &eb);
  return sa - sb;
}

/* Every cursor, in the file's order. */
static GArray *mc_in_file_order(MultiCursor *m)
{
  GArray *all = mc_all_carets(m);

  g_array_sort_with_data(all, mc_by_start, m);
  return all;
}

static gboolean mc_editable(Tab *tab)
{
  return gtk_text_view_get_editable(GTK_TEXT_VIEW(tab->source));
}

/* Editing ---------------------------------------------------------------------------------- */

void multicursor_type(Tab *tab, const char *text)
{
  MultiCursor *m = mc_of(tab);
  GArray *all;

  if (!m || !mc_editable(tab))
    return;
  all = mc_all_carets(m);
  gtk_text_buffer_begin_user_action(m->buffer);
  for (guint i = 0; i < all->len; i++) {
    McCaret *c = &g_array_index(all, McCaret, i);
    GtkTextIter insert, bound;
    mc_caret_iters(m, c, &insert, &bound);
    if (!gtk_text_iter_equal(&insert, &bound))
      gtk_text_buffer_delete(m->buffer, &insert, &bound);
    gtk_text_buffer_insert(m->buffer, &insert, text, -1);
    mc_caret_set(m, c, &insert, &insert);
  }
  gtk_text_buffer_end_user_action(m->buffer);
  g_array_free(all, TRUE);
  mc_changed(m);
}

void multicursor_erase(Tab *tab, gboolean forward)
{
  MultiCursor *m = mc_of(tab);
  GArray *all;

  if (!m || !mc_editable(tab))
    return;
  all = mc_all_carets(m);
  gtk_text_buffer_begin_user_action(m->buffer);
  for (guint i = 0; i < all->len; i++) {
    McCaret *c = &g_array_index(all, McCaret, i);
    GtkTextIter insert, bound;
    mc_caret_iters(m, c, &insert, &bound);
    if (gtk_text_iter_equal(&insert, &bound)) {
      if (forward)
        gtk_text_iter_forward_cursor_position(&bound);
      else
        gtk_text_iter_backward_cursor_position(&bound);
    }
    gtk_text_buffer_delete(m->buffer, &insert, &bound);
    mc_caret_set(m, c, &insert, &insert);
  }
  gtk_text_buffer_end_user_action(m->buffer);
  g_array_free(all, TRUE);
  mc_changed(m);
}

/* Enter: a new line at every cursor, with its line's indentation. */
static void mc_new_lines(MultiCursor *m)
{
  GArray *all = mc_all_carets(m);

  gtk_text_buffer_begin_user_action(m->buffer);
  for (guint i = 0; i < all->len; i++) {
    McCaret *c = &g_array_index(all, McCaret, i);
    GtkTextIter insert, bound, start, blank;
    mc_caret_iters(m, c, &insert, &bound);
    if (!gtk_text_iter_equal(&insert, &bound))
      gtk_text_buffer_delete(m->buffer, &insert, &bound);
    start = blank = insert;
    gtk_text_iter_set_line_offset(&start, 0);
    blank = start;
    while (!gtk_text_iter_ends_line(&blank) && gtk_text_iter_compare(&blank, &insert) < 0 &&
           (gtk_text_iter_get_char(&blank) == ' ' || gtk_text_iter_get_char(&blank) == '\t'))
      gtk_text_iter_forward_char(&blank);
    char *indent = gtk_text_buffer_get_text(m->buffer, &start, &blank, FALSE);
    char *text = g_strconcat("\n", indent, NULL);
    gtk_text_buffer_insert(m->buffer, &insert, text, -1);
    mc_caret_set(m, c, &insert, &insert);
    g_free(text);
    g_free(indent);
  }
  gtk_text_buffer_end_user_action(m->buffer);
  g_array_free(all, TRUE);
  mc_changed(m);
}

/* Tab: one level of indentation at every cursor, as the editor indents. */
static void mc_indent(MultiCursor *m)
{
  GtkSourceView *view = m->tab->source;
  int width = gtk_source_view_get_indent_width(view);
  char *text;

  if (width <= 0)
    width = (int)gtk_source_view_get_tab_width(view);
  text = gtk_source_view_get_insert_spaces_instead_of_tabs(view) ? g_strnfill(width, ' ') : g_strdup("\t");
  multicursor_type(m->tab, text);
  g_free(text);
}

char *multicursor_selected_text(Tab *tab)
{
  MultiCursor *m = mc_of(tab);
  GArray *all;
  GString *text;
  gboolean any = FALSE;

  if (!m)
    return NULL;
  all = mc_in_file_order(m);
  text = g_string_new(NULL);
  for (guint i = 0; i < all->len; i++) {
    GtkTextIter insert, bound;
    mc_caret_iters(m, &g_array_index(all, McCaret, i), &insert, &bound);
    gtk_text_iter_order(&insert, &bound);
    any = any || !gtk_text_iter_equal(&insert, &bound);
    char *part = gtk_text_buffer_get_text(m->buffer, &insert, &bound, FALSE);
    g_string_append_printf(text, "%s%s", i ? "\n" : "", part);
    g_free(part);
  }
  g_array_free(all, TRUE);
  if (!any) {
    g_string_free(text, TRUE);
    return NULL;
  }
  return g_string_free(text, FALSE);
}

void multicursor_paste(Tab *tab, const char *text)
{
  MultiCursor *m = mc_of(tab);
  GArray *all;
  char **lines;

  if (!m || !mc_editable(tab) || !text)
    return;
  all = mc_all_carets(m);
  lines = g_strsplit(text, "\n", -1);
  if (g_str_has_suffix(text, "\n") && g_strv_length(lines) > 1) { /* a last line break ends the last line */
    g_free(lines[g_strv_length(lines) - 1]);
    lines[g_strv_length(lines) - 1] = NULL;
  }
  if (g_strv_length(lines) != all->len || all->len < 2) {
    g_strfreev(lines);
    g_array_free(all, TRUE);
    multicursor_type(tab, text);
    return;
  }
  /* One line each, in the file's order. */
  g_array_free(all, TRUE);
  all = mc_in_file_order(m);
  gtk_text_buffer_begin_user_action(m->buffer);
  for (guint i = 0; i < all->len; i++) {
    McCaret *c = &g_array_index(all, McCaret, i);
    GtkTextIter insert, bound;
    mc_caret_iters(m, c, &insert, &bound);
    if (!gtk_text_iter_equal(&insert, &bound))
      gtk_text_buffer_delete(m->buffer, &insert, &bound);
    gtk_text_buffer_insert(m->buffer, &insert, lines[i], -1);
    mc_caret_set(m, c, &insert, &insert);
  }
  gtk_text_buffer_end_user_action(m->buffer);
  g_strfreev(lines);
  g_array_free(all, TRUE);
  mc_changed(m);
}

static void mc_copy(MultiCursor *m, gboolean cut)
{
  char *text = multicursor_selected_text(m->tab);

  if (!text)
    return;
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(m->tab->source)), text);
  if (cut && mc_editable(m->tab))
    multicursor_type(m->tab, ""); /* the selections go */
  g_free(text);
}

static void mc_on_clipboard_text(GObject *source, GAsyncResult *result, gpointer data)
{
  MultiCursor *m = data; /* still there unless cancelled */
  GError *error = NULL;
  char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);

  if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && text)
    multicursor_paste(m->tab, text);
  g_clear_error(&error);
  g_free(text);
}

/* Moving ----------------------------------------------------------------------------------- */

static void mc_move_iter(GtkTextIter *iter, MultiMove move)
{
  int offset = gtk_text_iter_get_line_offset(iter);
  GtkTextIter blank;

  switch (move) {
    case MULTI_LEFT: gtk_text_iter_backward_cursor_position(iter); break;
    case MULTI_RIGHT: gtk_text_iter_forward_cursor_position(iter); break;
    case MULTI_UP:
    case MULTI_DOWN:
      if (move == MULTI_UP ? !gtk_text_iter_backward_line(iter) : !gtk_text_iter_forward_line(iter)) {
        if (move == MULTI_UP)
          gtk_text_iter_set_line_offset(iter, 0);
        else
          gtk_text_iter_forward_to_end(iter);
        break;
      }
      if (offset >= gtk_text_iter_get_chars_in_line(iter)) {
        if (!gtk_text_iter_ends_line(iter))
          gtk_text_iter_forward_to_line_end(iter);
      } else {
        gtk_text_iter_set_line_offset(iter, offset);
      }
      break;
    case MULTI_HOME: /* the first non-blank, or the line's start when already there */
      blank = *iter;
      gtk_text_iter_set_line_offset(&blank, 0);
      while (!gtk_text_iter_ends_line(&blank) &&
             (gtk_text_iter_get_char(&blank) == ' ' || gtk_text_iter_get_char(&blank) == '\t'))
        gtk_text_iter_forward_char(&blank);
      if (gtk_text_iter_equal(&blank, iter))
        gtk_text_iter_set_line_offset(iter, 0);
      else
        *iter = blank;
      break;
    case MULTI_END:
      if (!gtk_text_iter_ends_line(iter))
        gtk_text_iter_forward_to_line_end(iter);
      break;
    case MULTI_WORD_LEFT: gtk_text_iter_backward_word_start(iter); break;
    case MULTI_WORD_RIGHT: gtk_text_iter_forward_word_end(iter); break;
  }
}

void multicursor_move(Tab *tab, MultiMove move, gboolean extend)
{
  MultiCursor *m = mc_of(tab);
  GArray *all;

  if (!m)
    return;
  all = mc_all_carets(m);
  for (guint i = 0; i < all->len; i++) {
    McCaret *c = &g_array_index(all, McCaret, i);
    GtkTextIter insert, bound;
    mc_caret_iters(m, c, &insert, &bound);
    if (!extend && !gtk_text_iter_equal(&insert, &bound) && (move == MULTI_LEFT || move == MULTI_RIGHT)) {
      gtk_text_iter_order(&insert, &bound); /* a selection collapses to its side */
      if (move == MULTI_RIGHT)
        insert = bound;
    } else {
      mc_move_iter(&insert, move);
    }
    mc_caret_set(m, c, &insert, extend ? &bound : &insert);
  }
  g_array_free(all, TRUE);
  mc_changed(m);
}

/* Adding cursors --------------------------------------------------------------------------- */

/* Whether some cursor selects exactly start..end. */
static gboolean mc_has_selection(MultiCursor *m, int start, int end)
{
  GArray *all = mc_all_carets(m);
  gboolean found = FALSE;

  for (guint i = 0; i < all->len && !found; i++) {
    int s, e;
    mc_caret_range(m, &g_array_index(all, McCaret, i), &s, &e);
    found = s == start && e == end;
  }
  g_array_free(all, TRUE);
  return found;
}

/* The name at the cursor, selected; FALSE when there is none. */
static gboolean mc_select_name(MultiCursor *m)
{
  GtkTextIter cursor, start, end;

  gtk_text_buffer_get_iter_at_mark(m->buffer, &cursor, gtk_text_buffer_get_insert(m->buffer));
  if (!editor_word_at(&cursor, &start, &end))
    return FALSE;
  gtk_text_buffer_select_range(m->buffer, &end, &start);
  return TRUE;
}

void multicursor_add_next(Tab *tab)
{
  MultiCursor *m = mc_of(tab);
  GtkTextIter start, end, from, found_start, found_end;
  McCaret last;
  char *text;

  if (!m)
    return;
  if (!gtk_text_buffer_get_selection_bounds(m->buffer, &start, &end)) {
    mc_select_name(m);
    return;
  }
  text = gtk_text_buffer_get_text(m->buffer, &start, &end, FALSE);
  last = m->last >= 0 ? g_array_index(m->extra, McCaret, m->last) : mc_primary(m);
  mc_caret_iters(m, &last, &from, &start);
  if (gtk_text_iter_compare(&start, &from) > 0)
    from = start;
  for (int round = 0; round < 2; round++) {
    gboolean found = gtk_text_iter_forward_search(&from, text, 0, &found_start, &found_end, NULL);
    while (found && mc_has_selection(m, gtk_text_iter_get_offset(&found_start), gtk_text_iter_get_offset(&found_end)))
      found = gtk_text_iter_forward_search(&found_end, text, 0, &found_start, &found_end, NULL);
    if (found) {
      mc_add_caret(m, &found_end, &found_start);
      gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(tab->source), g_array_index(m->extra, McCaret, m->last).insert);
      break;
    }
    gtk_text_buffer_get_start_iter(m->buffer, &from); /* round to the top once */
  }
  g_free(text);
  mc_changed(m);
}

void multicursor_select_all(Tab *tab)
{
  MultiCursor *m = mc_of(tab);
  GtkTextIter start, end, from, found_start, found_end;
  char *text;

  if (!m)
    return;
  if (!gtk_text_buffer_get_selection_bounds(m->buffer, &start, &end) && !mc_select_name(m))
    return;
  gtk_text_buffer_get_selection_bounds(m->buffer, &start, &end);
  text = gtk_text_buffer_get_text(m->buffer, &start, &end, FALSE);
  multicursor_clear(tab);
  gtk_text_buffer_get_start_iter(m->buffer, &from);
  while (gtk_text_iter_forward_search(&from, text, 0, &found_start, &found_end, NULL)) {
    if (!mc_has_selection(m, gtk_text_iter_get_offset(&found_start), gtk_text_iter_get_offset(&found_end)))
      mc_add_caret(m, &found_end, &found_start);
    from = found_end;
  }
  g_free(text);
  mc_changed(m);
}

void multicursor_add_line(Tab *tab, gboolean below)
{
  MultiCursor *m = mc_of(tab);
  GtkTextIter insert, bound;
  McCaret last;

  if (!m)
    return;
  last = m->last >= 0 ? g_array_index(m->extra, McCaret, m->last) : mc_primary(m);
  mc_caret_iters(m, &last, &insert, &bound);
  if (below ? gtk_text_iter_get_line(&insert) >= gtk_text_buffer_get_line_count(m->buffer) - 1
            : gtk_text_iter_get_line(&insert) == 0)
    return;
  mc_move_iter(&insert, below ? MULTI_DOWN : MULTI_UP);
  if (!mc_has_selection(m, gtk_text_iter_get_offset(&insert), gtk_text_iter_get_offset(&insert)))
    mc_add_caret(m, &insert, &insert);
  mc_changed(m);
}

void multicursor_toggle_at(Tab *tab, const GtkTextIter *iter)
{
  MultiCursor *m = mc_of(tab);
  int offset = gtk_text_iter_get_offset(iter);

  if (!m)
    return;
  for (guint i = 0; i < m->extra->len; i++) {
    int s, e;
    mc_caret_range(m, &g_array_index(m->extra, McCaret, i), &s, &e);
    if (s == offset && e == offset) {
      mc_remove_caret(m, i);
      mc_changed(m);
      return;
    }
  }
  if (!mc_has_selection(m, offset, offset))
    mc_add_caret(m, iter, iter);
  mc_changed(m);
}

/* The iter at line's column col, or its end when it is shorter. */
static void mc_line_column(GtkTextBuffer *buffer, int line, int col, GtkTextIter *iter)
{
  gtk_text_buffer_get_iter_at_line(buffer, iter, line);
  if (col >= gtk_text_iter_get_chars_in_line(iter)) {
    if (!gtk_text_iter_ends_line(iter))
      gtk_text_iter_forward_to_line_end(iter);
  } else {
    gtk_text_iter_set_line_offset(iter, col);
  }
}

void multicursor_box(Tab *tab, int line0, int col0, int line1, int col1)
{
  MultiCursor *m = mc_of(tab);
  int step = line1 >= line0 ? 1 : -1;

  if (!m)
    return;
  multicursor_clear(tab);
  for (int line = line0;; line += step) {
    GtkTextIter insert, bound;
    mc_line_column(m->buffer, line, col1, &insert);
    mc_line_column(m->buffer, line, col0, &bound);
    if (line == line0)
      gtk_text_buffer_select_range(m->buffer, &insert, &bound);
    else
      mc_add_caret(m, &insert, &bound);
    if (line == line1)
      break;
  }
  mc_changed(m);
}

/* Drawing ---------------------------------------------------------------------------------- */

static void mc_draw_selection(MultiCursor *m, GtkTextView *view, GtkSnapshot *snapshot, const McCaret *c,
                              const GdkRectangle *visible)
{
  GtkTextIter start, end, line;
  GdkRGBA color = *palette_rgba(PAL_EDITOR_SELECTION);

  mc_caret_iters(m, c, &start, &end);
  if (gtk_text_iter_equal(&start, &end))
    return;
  gtk_text_iter_order(&start, &end);
  for (line = start; gtk_text_iter_compare(&line, &end) < 0;) {
    GdkRectangle from, to;
    GtkTextIter stop = line;
    if (!gtk_text_iter_ends_line(&stop))
      gtk_text_iter_forward_to_line_end(&stop);
    if (gtk_text_iter_compare(&stop, &end) > 0)
      stop = end;
    gtk_text_view_get_iter_location(view, &line, &from);
    gtk_text_view_get_iter_location(view, &stop, &to);
    int right = gtk_text_iter_equal(&stop, &end) ? to.x : to.x + MAX(to.width, 6); /* the line break */
    if (from.y + from.height >= visible->y && from.y <= visible->y + visible->height)
      gtk_snapshot_append_color(
          snapshot, &color,
          &GRAPHENE_RECT_INIT((float)from.x, (float)from.y, (float)MAX(right - from.x, 0), (float)from.height));
    if (!gtk_text_iter_forward_line(&line))
      break;
  }
}

void multicursor_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot, gboolean above)
{
  MultiCursor *m = mc_of(tab);
  GdkRectangle visible;

  if (!m || m->extra->len == 0)
    return;
  gtk_text_view_get_visible_rect(view, &visible);
  for (guint i = 0; i < m->extra->len; i++) {
    McCaret *c = &g_array_index(m->extra, McCaret, i);
    if (!above) {
      mc_draw_selection(m, view, snapshot, c, &visible);
      continue;
    }
    GtkTextIter insert;
    GdkRectangle at;
    gtk_text_buffer_get_iter_at_mark(m->buffer, &insert, c->insert);
    gtk_text_view_get_iter_location(view, &insert, &at);
    if (at.y + at.height >= visible.y && at.y <= visible.y + visible.height)
      gtk_snapshot_append_color(snapshot, palette_rgba(PAL_CURSOR),
                                &GRAPHENE_RECT_INIT((float)at.x, (float)at.y, MC_CARET_WIDTH, (float)at.height));
  }
}

/* Input ------------------------------------------------------------------------------------ */

static gboolean mc_on_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                          GdkModifierType state, MultiCursor *m)
{
  gboolean shift = (state & GDK_SHIFT_MASK) != 0, control = (state & GDK_CONTROL_MASK) != 0;
  gunichar c;

  if (m->extra->len == 0 || (state & (GDK_ALT_MASK | GDK_SUPER_MASK)))
    return FALSE;
  if (key == GDK_KEY_Escape) {
    multicursor_clear(m->tab);
    return TRUE;
  }
  if (control) {
    if (key == GDK_KEY_c || key == GDK_KEY_C || key == GDK_KEY_x || key == GDK_KEY_X) {
      mc_copy(m, key == GDK_KEY_x || key == GDK_KEY_X);
      return TRUE;
    }
    if (key == GDK_KEY_v || key == GDK_KEY_V) {
      if (mc_editable(m->tab))
        gdk_clipboard_read_text_async(gtk_widget_get_clipboard(GTK_WIDGET(m->tab->source)), m->cancel,
                                      mc_on_clipboard_text, m);
      return TRUE;
    }
    if (key == GDK_KEY_Left || key == GDK_KEY_Right) {
      multicursor_move(m->tab, key == GDK_KEY_Left ? MULTI_WORD_LEFT : MULTI_WORD_RIGHT, shift);
      return TRUE;
    }
    if (key == GDK_KEY_a || key == GDK_KEY_Home || key == GDK_KEY_End)
      multicursor_clear(m->tab); /* select all, the file's start or end: one cursor */
    return FALSE;
  }
  switch (key) {
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left: multicursor_move(m->tab, MULTI_LEFT, shift); return TRUE;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right: multicursor_move(m->tab, MULTI_RIGHT, shift); return TRUE;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up: multicursor_move(m->tab, MULTI_UP, shift); return TRUE;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down: multicursor_move(m->tab, MULTI_DOWN, shift); return TRUE;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home: multicursor_move(m->tab, MULTI_HOME, shift); return TRUE;
    case GDK_KEY_End:
    case GDK_KEY_KP_End: multicursor_move(m->tab, MULTI_END, shift); return TRUE;
    default: break;
  }
  if (!mc_editable(m->tab))
    return FALSE;
  switch (key) {
    case GDK_KEY_BackSpace: multicursor_erase(m->tab, FALSE); return TRUE;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete: multicursor_erase(m->tab, TRUE); return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter: mc_new_lines(m); return TRUE;
    case GDK_KEY_Tab: mc_indent(m); return TRUE;
    default: break;
  }
  c = gdk_keyval_to_unicode(key);
  if (c >= 0x20 && c != 0x7f) {
    char text[8] = { 0 };
    g_unichar_to_utf8(c, text);
    multicursor_type(m->tab, text);
    return TRUE;
  }
  return FALSE;
}

/* The buffer position under a point of the view. */
static void mc_iter_at_point(MultiCursor *m, double x, double y, GtkTextIter *iter)
{
  int bx, by;

  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(m->tab->source), GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx,
                                        &by);
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(m->tab->source), iter, bx, by);
}

static void mc_on_drag_begin(GtkGestureDrag *drag, double x, double y, MultiCursor *m)
{
  GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(drag));
  GtkTextIter iter;

  if ((state & GDK_ALT_MASK) && (state & GDK_SHIFT_MASK)) { /* a column selection */
    mc_iter_at_point(m, x, y, &iter);
    m->boxing = TRUE;
    m->box_line = gtk_text_iter_get_line(&iter);
    m->box_col = gtk_text_iter_get_line_offset(&iter);
    multicursor_box(m->tab, m->box_line, m->box_col, m->box_line, m->box_col);
    gtk_widget_grab_focus(GTK_WIDGET(m->tab->source));
    gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_CLAIMED);
  } else if (state & GDK_ALT_MASK) { /* a cursor more (or less) */
    mc_iter_at_point(m, x, y, &iter);
    multicursor_toggle_at(m->tab, &iter);
    gtk_widget_grab_focus(GTK_WIDGET(m->tab->source));
    gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_CLAIMED);
  } else { /* a plain click: back to one cursor, the view does the rest */
    multicursor_clear(m->tab);
    gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_DENIED);
  }
}

static void mc_on_drag_update(GtkGestureDrag *drag, double dx, double dy, MultiCursor *m)
{
  double x, y;
  GtkTextIter iter;

  if (!m->boxing || !gtk_gesture_drag_get_start_point(drag, &x, &y))
    return;
  mc_iter_at_point(m, x + dx, y + dy, &iter);
  /* The column under the pointer, also past a short line's end. */
  int bx, by, column = gtk_text_iter_get_line_offset(&iter);
  GdkRectangle at;
  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(m->tab->source), GTK_TEXT_WINDOW_WIDGET, (int)(x + dx),
                                        (int)(y + dy), &bx, &by);
  gtk_text_view_get_iter_location(GTK_TEXT_VIEW(m->tab->source), &iter, &at);
  if (gtk_text_iter_ends_line(&iter) && at.width > 0 && bx > at.x)
    column += (bx - at.x) / MAX(at.width, 1);
  multicursor_box(m->tab, m->box_line, m->box_col, gtk_text_iter_get_line(&iter), column);
}

static void mc_on_drag_end(GtkGestureDrag *drag G_GNUC_UNUSED, double dx G_GNUC_UNUSED, double dy G_GNUC_UNUSED,
                           MultiCursor *m)
{
  m->boxing = FALSE;
}

void multicursor_attach(Tab *tab)
{
  MultiCursor *m = g_new0(MultiCursor, 1);
  GtkEventController *keys = gtk_event_controller_key_new();
  GtkGesture *drag = gtk_gesture_drag_new();

  m->tab = tab;
  m->buffer = GTK_TEXT_BUFFER(tab->buffer);
  m->extra = g_array_new(FALSE, FALSE, sizeof(McCaret));
  m->last = -1;
  m->cancel = g_cancellable_new();
  tab->view->multi = m;

  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(mc_on_key), m);
  gtk_widget_add_controller(GTK_WIDGET(tab->source), keys);
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(mc_on_drag_begin), m);
  g_signal_connect(drag, "drag-update", G_CALLBACK(mc_on_drag_update), m);
  g_signal_connect(drag, "drag-end", G_CALLBACK(mc_on_drag_end), m);
  gtk_widget_add_controller(GTK_WIDGET(tab->source), GTK_EVENT_CONTROLLER(drag));
}

void multicursor_free(MultiCursor *m)
{
  if (!m)
    return;
  g_cancellable_cancel(m->cancel);
  g_object_unref(m->cancel);
  for (guint i = 0; i < m->extra->len; i++) {
    McCaret *c = &g_array_index(m->extra, McCaret, i);
    gtk_text_buffer_delete_mark(m->buffer, c->insert);
    gtk_text_buffer_delete_mark(m->buffer, c->bound);
  }
  g_array_free(m->extra, TRUE);
  g_free(m);
}
