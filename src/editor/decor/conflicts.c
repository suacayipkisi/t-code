/*
 * Merge conflicts (see conflicts.h).
 */
#include "editor/decor/conflicts.h"

#include "editor/editorview.h"
#include "window/palette.h"

#include <string.h>

#define SCAN_MS 300    /* after the last change */
#define MARKER_SIZE 7  /* git's "<<<<<<<" */
#define ACTIONS_GAP 24 /* pixels between the "<<<<<<<" line's end and the links */

typedef struct {
  GtkTextMark *start, *base, *middle, *end; /* the marker lines; base is NULL without "|||||||" */
  GtkWidget *actions;                       /* the links, over the text */
  int x, y;                                 /* where they are */
} Conflict;

struct Conflicts {
  Tab *tab;
  GtkTextBuffer *buffer; /* outlives the page: the tab holds a reference */
  GArray *list;          /* Conflict, in the text's order */
  GPtrArray *links;      /* the links made so far (held: the view may go first): GTK cannot take an overlay
                          * off a GtkTextView, so those of conflicts gone are hidden and used for the next */
  guint scan_id, place_id;
};

/* Which marker line is line ('<', '|', '=' or '>'), or 0: seven of the same character, then the
 * line's end or (but after "=======") a space and a label. */
static char marker_at(GtkTextBuffer *buffer, int line)
{
  GtkTextIter start, end;
  char kind = 0;

  gtk_text_buffer_get_iter_at_line(buffer, &start, line);
  end = start;
  if (!gtk_text_iter_ends_line(&end))
    gtk_text_iter_forward_to_line_end(&end);
  if (gtk_text_iter_get_line_offset(&end) < MARKER_SIZE)
    return 0;
  char *text = gtk_text_iter_get_slice(&start, &end);
  if (strchr("<|=>", text[0]) && strspn(text, (char[]){ text[0], '\0' }) == MARKER_SIZE &&
      (text[MARKER_SIZE] == '\0' || (text[MARKER_SIZE] == ' ' && text[0] != '=')))
    kind = text[0];
  g_free(text);
  return kind;
}

static int mark_line(GtkTextBuffer *buffer, GtkTextMark *mark)
{
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_mark(buffer, &iter, mark);
  return gtk_text_iter_get_line(&iter);
}

static void conflicts_clear(Conflicts *c)
{
  for (guint i = 0; i < c->list->len; i++) {
    Conflict *k = &g_array_index(c->list, Conflict, i);
    gtk_text_buffer_delete_mark(c->buffer, k->start);
    if (k->base)
      gtk_text_buffer_delete_mark(c->buffer, k->base);
    gtk_text_buffer_delete_mark(c->buffer, k->middle);
    gtk_text_buffer_delete_mark(c->buffer, k->end);
    gtk_widget_set_visible(k->actions, FALSE);
  }
  g_array_set_size(c->list, 0);
}

/* The text of lines from up to (not with) to. */
static char *lines_text(GtkTextBuffer *buffer, int from, int to)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_iter_at_line(buffer, &start, from);
  gtk_text_buffer_get_iter_at_line(buffer, &end, to);
  return gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
}

static void conflicts_scan(Conflicts *c);

/* A link: the block gives way to the current side, the incoming one or both. */
static gboolean on_accept(GtkLabel *label, const char *which, Conflicts *c)
{
  Conflict *k = NULL;

  for (guint i = 0; i < c->list->len; i++)
    if (g_array_index(c->list, Conflict, i).actions == GTK_WIDGET(label))
      k = &g_array_index(c->list, Conflict, i);
  if (!k)
    return TRUE;
  int start = mark_line(c->buffer, k->start), middle = mark_line(c->buffer, k->middle);
  int end = mark_line(c->buffer, k->end), base = k->base ? mark_line(c->buffer, k->base) : middle;
  char *current = lines_text(c->buffer, start + 1, base), *incoming = lines_text(c->buffer, middle + 1, end);
  char *kept = strcmp(which, "current") == 0    ? g_strdup(current)
               : strcmp(which, "incoming") == 0 ? g_strdup(incoming)
                                                : g_strconcat(current, incoming, NULL);
  GtkTextIter from, to;

  gtk_text_buffer_get_iter_at_line(c->buffer, &from, start);
  gtk_text_buffer_get_iter_at_line(c->buffer, &to, end);
  if (!gtk_text_iter_forward_line(&to)) /* the last line: up to the text's end */
    gtk_text_buffer_get_end_iter(c->buffer, &to);
  gtk_text_buffer_begin_user_action(c->buffer);
  gtk_text_buffer_delete(c->buffer, &from, &to);
  gtk_text_buffer_insert(c->buffer, &from, kept, -1);
  gtk_text_buffer_end_user_action(c->buffer);
  gtk_text_buffer_place_cursor(c->buffer, &from);
  g_free(kept);
  g_free(incoming);
  g_free(current);
  conflicts_scan(c); /* the links of the block go now; the label goes with them */
  return TRUE;
}

/* The links for the next conflict (the index-th): a hidden one, or a new one over the text. */
static GtkWidget *actions_for(Conflicts *c, guint index)
{
  if (index < c->links->len)
    return c->links->pdata[index];
  GtkWidget *label = gtk_label_new(NULL);

  palette_add_css(".tcode-conflict-actions { font-size: 0.85em; color: var(--tc-fg-dim); }"
                  ".tcode-conflict-actions link { color: var(--tc-link); }");
  gtk_label_set_markup(GTK_LABEL(label), "<a href=\"current\">Accept Current Change</a> | "
                                         "<a href=\"incoming\">Accept Incoming Change</a> | "
                                         "<a href=\"both\">Accept Both Changes</a>");
  gtk_widget_add_css_class(label, "tcode-conflict-actions");
  gtk_widget_set_cursor_from_name(label, "default");
  g_signal_connect(label, "activate-link", G_CALLBACK(on_accept), c);
  gtk_text_view_add_overlay(GTK_TEXT_VIEW(c->tab->source), label, 0, 0);
  g_ptr_array_add(c->links, g_object_ref(label));
  return label;
}

/* Where a conflict's links go: after the "<<<<<<<" line's end, in buffer coordinates. */
static void actions_place(Conflicts *c, Conflict *k, int *x, int *y)
{
  GtkTextView *view = GTK_TEXT_VIEW(c->tab->source);
  GtkTextIter end;
  GdkRectangle at;
  int height;

  gtk_text_buffer_get_iter_at_mark(c->buffer, &end, k->start);
  if (!gtk_text_iter_ends_line(&end))
    gtk_text_iter_forward_to_line_end(&end);
  gtk_text_view_get_iter_location(view, &end, &at);
  gtk_widget_measure(k->actions, GTK_ORIENTATION_VERTICAL, -1, NULL, &height, NULL, NULL);
  *x = at.x + ACTIONS_GAP;
  *y = at.y + (at.height - height) / 2;
}

static gboolean conflicts_place(gpointer data)
{
  Conflicts *c = data;

  c->place_id = 0;
  for (guint i = 0; i < c->list->len; i++) {
    Conflict *k = &g_array_index(c->list, Conflict, i);
    int x, y;
    actions_place(c, k, &x, &y);
    if (x != k->x || y != k->y)
      gtk_text_view_move_overlay(GTK_TEXT_VIEW(c->tab->source), k->actions, k->x = x, k->y = y);
  }
  return G_SOURCE_REMOVE;
}

static GtkTextMark *mark_at(GtkTextBuffer *buffer, int line)
{
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
  return gtk_text_buffer_create_mark(buffer, NULL, &iter, TRUE);
}

/* Finds the conflicts again: each "<<<<<<<" line with "=======" and then ">>>>>>>" after it, before
 * the next "<<<<<<<". */
static void conflicts_scan(Conflicts *c)
{
  GtkTextIter from, found;
  int lines = gtk_text_buffer_get_line_count(c->buffer);

  g_clear_handle_id(&c->scan_id, g_source_remove);
  conflicts_clear(c);
  if (c->tab->view->readonly)
    return;
  gtk_text_buffer_get_start_iter(c->buffer, &from);
  while (gtk_text_iter_forward_search(&from, "<<<<<<<", GTK_TEXT_SEARCH_TEXT_ONLY, &found, NULL, NULL)) {
    int start = gtk_text_iter_get_line(&found), base = -1, middle = -1, end = -1, line;
    if (!gtk_text_iter_starts_line(&found) || marker_at(c->buffer, start) != '<') {
      gtk_text_buffer_get_iter_at_line(c->buffer, &from, start + 1);
      if (start + 1 >= lines)
        break;
      continue;
    }
    for (line = start + 1; line < lines && end < 0; line++) {
      char kind = marker_at(c->buffer, line);
      if (kind == '<')
        break;
      if (kind == '|' && base < 0 && middle < 0)
        base = line;
      else if (kind == '=' && middle < 0)
        middle = line;
      else if (kind == '>' && middle >= 0)
        end = line;
    }
    if (end >= 0) {
      Conflict k = { mark_at(c->buffer, start),
                     base >= 0 ? mark_at(c->buffer, base) : NULL,
                     mark_at(c->buffer, middle),
                     mark_at(c->buffer, end),
                     actions_for(c, c->list->len),
                     0,
                     0 };
      actions_place(c, &k, &k.x, &k.y);
      gtk_text_view_move_overlay(GTK_TEXT_VIEW(c->tab->source), k.actions, k.x, k.y);
      gtk_widget_set_visible(k.actions, TRUE);
      g_array_append_val(c->list, k);
    }
    if (line >= lines)
      break;
    gtk_text_buffer_get_iter_at_line(c->buffer, &from, end >= 0 ? end + 1 : line);
  }
  gtk_widget_queue_draw(GTK_WIDGET(c->tab->source));
}

static gboolean conflicts_scan_later(gpointer data)
{
  Conflicts *c = data;

  c->scan_id = 0;
  if (!c->tab->view->loading)
    conflicts_scan(c);
  return G_SOURCE_REMOVE;
}

static void on_conflicts_changed(Conflicts *c)
{
  g_clear_handle_id(&c->scan_id, g_source_remove);
  c->scan_id = g_timeout_add(SCAN_MS, conflicts_scan_later, c);
}

void conflicts_attach(Tab *tab)
{
  Conflicts *c = g_new0(Conflicts, 1);

  c->tab = tab;
  c->buffer = GTK_TEXT_BUFFER(tab->buffer);
  c->list = g_array_new(FALSE, FALSE, sizeof(Conflict));
  c->links = g_ptr_array_new_with_free_func(g_object_unref);
  g_signal_connect_swapped(c->buffer, "changed", G_CALLBACK(on_conflicts_changed), c);
  tab->view->conflicts = c;
}

void conflicts_free(Conflicts *c)
{
  if (!c)
    return;
  g_clear_handle_id(&c->scan_id, g_source_remove);
  g_clear_handle_id(&c->place_id, g_source_remove);
  g_signal_handlers_disconnect_by_data(c->buffer, c);
  conflicts_clear(c);
  for (guint i = 0; i < c->links->len; i++)
    g_signal_handlers_disconnect_by_data(c->links->pdata[i], c);
  g_ptr_array_free(c->links, TRUE);
  g_array_free(c->list, TRUE);
  g_free(c);
}

/* Tints lines from up to (not with) to, the first one stronger when head. */
static void tint_lines(Conflicts *c, GtkTextView *view, GtkSnapshot *snapshot, const GdkRectangle *visible, int from,
                       int to, PalColor color, gboolean head)
{
  GdkRGBA fill = *palette_rgba(color);
  GtkTextIter iter;

  for (int line = from; line < to; line++) {
    int y, height;
    gtk_text_buffer_get_iter_at_line(c->buffer, &iter, line);
    gtk_text_view_get_line_yrange(view, &iter, &y, &height);
    if (y + height < visible->y || y > visible->y + visible->height)
      continue;
    fill.alpha = head && line == from ? 0.30f : 0.13f;
    gtk_snapshot_append_color(snapshot, &fill, &GRAPHENE_RECT_INIT(visible->x, y, visible->width, height));
  }
}

void conflicts_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot)
{
  Conflicts *c = tab->view->conflicts;
  GdkRectangle visible;

  if (!c || c->list->len == 0)
    return;
  gtk_text_view_get_visible_rect(view, &visible);
  for (guint i = 0; i < c->list->len; i++) {
    Conflict *k = &g_array_index(c->list, Conflict, i);
    int start = mark_line(c->buffer, k->start), middle = mark_line(c->buffer, k->middle);
    int end = mark_line(c->buffer, k->end), base = k->base ? mark_line(c->buffer, k->base) : middle;
    int x, y;
    tint_lines(c, view, snapshot, &visible, start, base, PAL_DIFF_ADDED, TRUE);
    tint_lines(c, view, snapshot, &visible, base, middle, PAL_FG_FAINT, TRUE);
    tint_lines(c, view, snapshot, &visible, middle + 1, end, PAL_DIFF_MODIFIED, FALSE);
    tint_lines(c, view, snapshot, &visible, end, end + 1, PAL_DIFF_MODIFIED, TRUE);
    actions_place(c, k, &x, &y); /* the text moved under the links (an edit, word wrap): they follow */
    if ((x != k->x || y != k->y) && !c->place_id)
      c->place_id = g_idle_add(conflicts_place, c);
  }
}
