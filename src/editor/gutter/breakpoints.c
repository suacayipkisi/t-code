/*
 * Breakpoints in the editor (see breakpoints.h). In an open file each breakpoint is a source mark at
 * the end of its line: Enter at the line's start takes it down with the text, Enter at its end leaves
 * it; its condition is the mark's data ("condition"). t->debug.breakpoints holds every file's
 * breakpoints; an open file's are read again from its marks after each change, and the debugger is
 * told when they moved.
 */
#include "editor/gutter/breakpoints.h"

#include "debug/debugger.h"
#include "debug/disassembly.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "window/palette.h"

#include <stdlib.h>
#include <string.h>

#define CATEGORY "tcode-breakpoint"
#define DOT_WIDTH 28 /* pixels: up to the line numbers, so a click left of them lands here */

struct Breakpoints {
  Tab *tab;
  GtkWidget *renderer;
  int hover;         /* the line under the pointer, or -1 */
  gboolean placed;   /* the marks are in (loaded): until then the text says nothing about the lines */
  GtkTextMark *stop; /* at the start of the line the program stopped at, or NULL */
  GtkWidget *ask;    /* the condition's popover (a right click), while it shows */
  int ask_line;
};

/* The table ---------------------------------------------------------------------------------- */

static void breakpoint_clear(gpointer data)
{
  g_free(((Breakpoint *)data)->condition);
}

static GArray *new_list(void)
{
  GArray *list = g_array_new(FALSE, FALSE, sizeof(Breakpoint));

  g_array_set_clear_func(list, breakpoint_clear);
  return list;
}

static GHashTable *table(App *t)
{
  if (!t->debug.breakpoints)
    t->debug.breakpoints = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
  return t->debug.breakpoints;
}

GArray *breakpoints_in(App *t, const char *path)
{
  return path && t->debug.breakpoints ? g_hash_table_lookup(t->debug.breakpoints, path) : NULL;
}

/* Whether two lists are the same. */
static gboolean same_list(GArray *a, GArray *b)
{
  guint na = a ? a->len : 0, nb = b ? b->len : 0;

  if (na != nb)
    return FALSE;
  for (guint i = 0; i < na; i++) {
    Breakpoint *x = &g_array_index(a, Breakpoint, i), *y = &g_array_index(b, Breakpoint, i);
    if (x->line != y->line || g_strcmp0(x->condition, y->condition) != 0)
      return FALSE;
  }
  return TRUE;
}

/* path's breakpoints are list now (empty or NULL: none); the debugger hears of a change. Takes
 * list. */
static void set_list(App *t, const char *path, GArray *list)
{
  GArray *old = breakpoints_in(t, path);

  if (list && list->len == 0)
    g_clear_pointer(&list, g_array_unref);
  if (same_list(old, list)) {
    if (list)
      g_array_unref(list);
    return;
  }
  if (list)
    g_hash_table_replace(table(t), g_strdup(path), list);
  else
    g_hash_table_remove(table(t), path);
  debug_breakpoints_changed(t, path);
}

static int by_line(gconstpointer a, gconstpointer b)
{
  return ((const Breakpoint *)a)->line - ((const Breakpoint *)b)->line;
}

/* Whether this editor takes breakpoints: not an old version (read-only, in a temporary folder). */
static gboolean can_break(Tab *tab)
{
  return tab->view && !tab->view->readonly && tab->path;
}

/* Whether a click in its column does something: a breakpoint, or one on an instruction in the
 * Disassembly View. */
static gboolean can_click(Tab *tab)
{
  return can_break(tab) || disassembly_is(tab->app, tab);
}

/* The marks ------------------------------------------------------------------------------------ */

/* The breakpoint marks on line, or NULL (free the list). */
static GSList *marks_on(Tab *tab, int line)
{
  return gtk_source_buffer_get_source_marks_at_line(tab->buffer, line, CATEGORY);
}

/* A breakpoint's mark on line (0-based); its condition (may be NULL) goes with it. */
static void add_mark(Tab *tab, int line, const char *condition)
{
  GtkTextIter end;
  GtkSourceMark *mark;

  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &end, line);
  if (!gtk_text_iter_ends_line(&end))
    gtk_text_iter_forward_to_line_end(&end);
  mark = gtk_source_buffer_create_source_mark(tab->buffer, NULL, CATEGORY, &end);
  if (condition && *condition)
    g_object_set_data_full(G_OBJECT(mark), "condition", g_strdup(condition), g_free);
}

/* The breakpoints of the marks in the text as it is now (ascending); two that came onto one line (the
 * line break between them was deleted) become one, the first one's condition kept. */
static GArray *list_of_marks(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GArray *list = new_list();
  GtkTextIter iter;

  gtk_text_buffer_get_start_iter(buffer, &iter);
  GSList *first = gtk_source_buffer_get_source_marks_at_iter(tab->buffer, &iter, CATEGORY);
  g_slist_free(first);
  if (!first && !gtk_source_buffer_forward_iter_to_source_mark(tab->buffer, &iter, CATEGORY))
    return list;
  do {
    int line = gtk_text_iter_get_line(&iter) + 1;
    GSList *here = gtk_source_buffer_get_source_marks_at_iter(tab->buffer, &iter, CATEGORY);
    if (list->len == 0 || g_array_index(list, Breakpoint, list->len - 1).line != line) {
      Breakpoint b = { line, g_strdup(here ? g_object_get_data(here->data, "condition") : NULL) };
      g_array_append_val(list, b);
    } else { /* a second mark on that line */
      for (GSList *m = here; m; m = m->next)
        gtk_text_buffer_delete_mark(buffer, m->data);
    }
    g_slist_free(here);
  } while (gtk_source_buffer_forward_iter_to_source_mark(tab->buffer, &iter, CATEGORY));
  return list;
}

/* The table follows the marks. */
static void sync_list(Tab *tab)
{
  set_list(tab->app, tab->path, list_of_marks(tab));
  gtk_widget_queue_draw(tab->view->breakpoints->renderer);
}

static void remove_marks(Tab *tab)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  gtk_source_buffer_remove_source_marks(tab->buffer, &start, &end, CATEGORY);
}

/* The marks follow the table (the file was loaded, the breakpoints came from the session). */
static void place_marks(Tab *tab)
{
  GArray *list = breakpoints_in(tab->app, tab->path);
  int count = gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(tab->buffer));

  remove_marks(tab);
  tab->view->breakpoints->placed = TRUE;
  for (guint i = 0; list && i < list->len; i++) {
    Breakpoint *b = &g_array_index(list, Breakpoint, i);
    if (b->line <= count)
      add_mark(tab, b->line - 1, b->condition);
  }
  gtk_widget_queue_draw(tab->view->breakpoints->renderer);
}

/* The editor showing path, if one is open and takes breakpoints. */
static Tab *editor_of(App *t, const char *path)
{
  Tab *tab = find_tab(t, TAB_EDITOR, path);

  return tab && tab->view && tab->view->breakpoints && !tab->view->loading && can_break(tab) ? tab : NULL;
}

/* path's breakpoints are list now, also in its open editor. Takes list. */
static void change_list(App *t, const char *path, GArray *list)
{
  Tab *tab;

  set_list(t, path, list);
  if ((tab = editor_of(t, path)))
    place_marks(tab);
}

/* A copy of path's breakpoints (empty when none). */
static GArray *copy_list(App *t, const char *path)
{
  GArray *old = breakpoints_in(t, path), *list = new_list();

  for (guint i = 0; old && i < old->len; i++) {
    Breakpoint b = g_array_index(old, Breakpoint, i);
    b.condition = g_strdup(b.condition);
    g_array_append_val(list, b);
  }
  return list;
}

void breakpoint_toggle(Tab *tab, int line)
{
  GSList *marks;

  if (!can_break(tab) || tab->view->loading)
    return;
  if (!tab->view->breakpoints->placed)
    place_marks(tab);
  marks = marks_on(tab, line);
  if (marks)
    for (GSList *m = marks; m; m = m->next)
      gtk_text_buffer_delete_mark(GTK_TEXT_BUFFER(tab->buffer), m->data);
  else
    add_mark(tab, line, NULL);
  g_slist_free(marks);
  sync_list(tab);
}

void breakpoint_toggle_at_cursor(Tab *tab)
{
  GtkTextIter cursor;

  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &cursor,
                                   gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer)));
  breakpoint_toggle(tab, gtk_text_iter_get_line(&cursor));
}

void breakpoint_set_condition(Tab *tab, int line, const char *condition)
{
  GSList *marks;

  if (!can_break(tab) || tab->view->loading)
    return;
  if (!tab->view->breakpoints->placed)
    place_marks(tab);
  marks = marks_on(tab, line);
  for (GSList *m = marks; m; m = m->next)
    gtk_text_buffer_delete_mark(GTK_TEXT_BUFFER(tab->buffer), m->data);
  g_slist_free(marks);
  add_mark(tab, line, condition);
  sync_list(tab);
}

const char *breakpoint_condition(Tab *tab, int line)
{
  GSList *marks = tab->view && tab->view->breakpoints ? marks_on(tab, line) : NULL;
  const char *condition = marks ? g_object_get_data(marks->data, "condition") : NULL;

  g_slist_free(marks);
  return condition;
}

void breakpoint_remove(App *t, const char *path, int line)
{
  GArray *list = copy_list(t, path);

  for (guint i = list->len; i-- > 0;)
    if (g_array_index(list, Breakpoint, i).line == line)
      g_array_remove_index(list, i);
  change_list(t, path, list);
}

void breakpoint_move(App *t, const char *path, int from, int to)
{
  GArray *list = copy_list(t, path);
  gboolean taken = FALSE;

  for (guint i = 0; i < list->len; i++)
    taken |= g_array_index(list, Breakpoint, i).line == to;
  for (guint i = list->len; i-- > 0;) {
    Breakpoint *b = &g_array_index(list, Breakpoint, i);
    if (b->line != from)
      continue;
    if (taken) /* one is there already */
      g_array_remove_index(list, i);
    else
      b->line = to;
  }
  g_array_sort(list, by_line);
  change_list(t, path, list);
}

/* The text changed: the breakpoints may be on other lines now. Not while the loader replaces it:
 * once loaded, the marks are put back from the table. */
static void on_changed(GtkTextBuffer *buffer G_GNUC_UNUSED, Breakpoints *b)
{
  if (b->placed && !b->tab->view->loading && can_break(b->tab) && breakpoints_in(b->tab->app, b->tab->path))
    sync_list(b->tab);
}

/* The stopped line ----------------------------------------------------------------------------- */

/* The stopped line's mark goes where t->debug says (or away). */
static void place_stop(Tab *tab)
{
  Breakpoints *b = tab->view->breakpoints;
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  App *t = tab->app;
  int line = 0;

  if (tab->view->loading)
    line = 0;
  else if (disassembly_is(t, tab))
    line = t->debug.disassembly_line;
  else if (t->debug.stop_path && g_strcmp0(t->debug.stop_path, tab->path) == 0)
    line = t->debug.stop_line;
  if (b->stop) {
    gtk_text_buffer_delete_mark(buffer, b->stop);
    b->stop = NULL;
  }
  if (line >= 1 && line <= gtk_text_buffer_get_line_count(buffer)) {
    GtkTextIter start;
    gtk_text_buffer_get_iter_at_line(buffer, &start, line - 1);
    b->stop = gtk_text_buffer_create_mark(buffer, NULL, &start, FALSE);
  }
  gtk_widget_queue_draw(b->renderer);
  gtk_widget_queue_draw(GTK_WIDGET(tab->source));
}

/* The stopped line in tab, or -1. */
static int stop_line(Tab *tab)
{
  Breakpoints *b = tab->view ? tab->view->breakpoints : NULL;
  GtkTextIter at;

  if (!b || !b->stop)
    return -1;
  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &at, b->stop);
  return gtk_text_iter_get_line(&at);
}

void breakpoints_show_stop(App *t, const char *path, int line)
{
  char *old = t->debug.stop_path;

  t->debug.stop_path = g_strdup(path);
  t->debug.stop_line = line;
  for (int i = 0; i < tab_count(t); i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR && tab->view && tab->view->breakpoints &&
        (g_strcmp0(tab->path, old) == 0 || g_strcmp0(tab->path, path) == 0))
      place_stop(tab);
  }
  g_free(old);
}

void breakpoints_show_instruction(App *t, int line)
{
  Tab *tab = t->debug.disassembly ? find_tab(t, TAB_EDITOR, t->debug.disassembly) : NULL;

  t->debug.disassembly_line = line;
  if (tab && tab->view && tab->view->breakpoints)
    place_stop(tab);
}

void breakpoints_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot)
{
  int line = stop_line(tab);
  GtkTextIter start;
  GdkRectangle visible;
  GdkRGBA color;
  int y, height;

  if (line < 0)
    return;
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &start, line);
  gtk_text_view_get_line_yrange(view, &start, &y, &height);
  gtk_text_view_get_visible_rect(view, &visible);
  color = *palette_rgba(PAL_WARNING);
  color.alpha = 0.22f;
  gtk_snapshot_append_color(snapshot, &color, &GRAPHENE_RECT_INIT(visible.x, y, visible.width, height));
}

/* Loading, saving ------------------------------------------------------------------------------ */

void breakpoints_loaded(Tab *tab)
{
  if (!tab->view || !tab->view->breakpoints)
    return;
  if (can_break(tab))
    place_marks(tab);
  place_stop(tab);
}

/* Every open editor puts its marks where the table says. */
static void place_everywhere(App *t)
{
  for (int i = 0; i < tab_count(t); i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR && tab->view && tab->view->breakpoints && !tab->view->loading && can_break(tab))
      place_marks(tab);
  }
}

void breakpoints_remove_all(App *t)
{
  GList *paths = t->debug.breakpoints ? g_hash_table_get_keys(t->debug.breakpoints) : NULL;
  char **copy = g_new0(char *, g_list_length(paths) + 1);
  int n = 0;

  for (GList *p = paths; p; p = p->next)
    copy[n++] = g_strdup(p->data);
  g_list_free(paths);
  for (int i = 0; i < n; i++)
    set_list(t, copy[i], NULL);
  g_strfreev(copy);
  place_everywhere(t);
}

char **breakpoints_save(App *t, char ***conditions)
{
  GPtrArray *list = g_ptr_array_new(), *ifs = g_ptr_array_new();
  GHashTableIter iter;
  gpointer path, items;

  if (t->debug.breakpoints) {
    g_hash_table_iter_init(&iter, t->debug.breakpoints);
    while (g_hash_table_iter_next(&iter, &path, &items))
      for (guint i = 0; i < ((GArray *)items)->len; i++) {
        Breakpoint *b = &g_array_index((GArray *)items, Breakpoint, i);
        g_ptr_array_add(list, g_strdup_printf("%d:%s", b->line, (char *)path));
        g_ptr_array_add(ifs, g_strdup(b->condition ? b->condition : ""));
      }
  }
  g_ptr_array_add(ifs, NULL);
  *conditions = (char **)g_ptr_array_free(ifs, FALSE);
  if (list->len == 0) {
    g_ptr_array_free(list, TRUE);
    return NULL;
  }
  g_ptr_array_add(list, NULL);
  return (char **)g_ptr_array_free(list, FALSE);
}

void breakpoints_restore(App *t, char **saved, char **conditions)
{
  guint n_conditions = conditions ? g_strv_length(conditions) : 0;

  g_clear_pointer(&t->debug.breakpoints, g_hash_table_destroy);
  for (guint i = 0; saved && saved[i]; i++) {
    char *end;
    long line = strtol(saved[i], &end, 10);
    if (*end != ':' || line < 1 || !g_path_is_absolute(end + 1))
      continue;
    GArray *list = g_hash_table_lookup(table(t), end + 1);
    if (!list) {
      list = new_list();
      g_hash_table_insert(table(t), g_strdup(end + 1), list);
    }
    Breakpoint b = { (int)line, i < n_conditions && *conditions[i] ? g_strdup(conditions[i]) : NULL };
    g_array_append_val(list, b);
  }
  if (t->debug.breakpoints) {
    GHashTableIter iter;
    gpointer list;
    g_hash_table_iter_init(&iter, t->debug.breakpoints);
    while (g_hash_table_iter_next(&iter, NULL, &list))
      g_array_sort(list, by_line);
  }
  place_everywhere(t);
}

/* The dots ------------------------------------------------------------------------------------- */

struct _TcodeBreakpointRenderer {
  GtkSourceGutterRenderer parent;
  Breakpoints *breakpoints;
};

#define TCODE_TYPE_BREAKPOINT_RENDERER (tcode_breakpoint_renderer_get_type())
G_DECLARE_FINAL_TYPE(TcodeBreakpointRenderer, tcode_breakpoint_renderer, TCODE, BREAKPOINT_RENDERER,
                     GtkSourceGutterRenderer)
G_DEFINE_FINAL_TYPE(TcodeBreakpointRenderer, tcode_breakpoint_renderer, GTK_SOURCE_TYPE_GUTTER_RENDERER)

/* A dot; a conditional breakpoint's has "=" in it. */
static void draw_dot(GtkSnapshot *snapshot, double y, double height, GdkRGBA color, gboolean conditional)
{
  double size = MIN(10, height - 4), left = (DOT_WIDTH - size) / 2, top = y + (height - size) / 2;
  GskRoundedRect dot;

  gsk_rounded_rect_init_from_rect(&dot, &GRAPHENE_RECT_INIT(left, top, size, size), size / 2);
  gtk_snapshot_push_rounded_clip(snapshot, &dot);
  gtk_snapshot_append_color(snapshot, &color, &dot.bounds);
  gtk_snapshot_pop(snapshot);
  if (conditional) {
    const GdkRGBA *ink = palette_rgba(PAL_PANEL);
    gtk_snapshot_append_color(snapshot, ink, &GRAPHENE_RECT_INIT(left + 2.5, top + size / 2 - 2.5, size - 5, 1.5));
    gtk_snapshot_append_color(snapshot, ink, &GRAPHENE_RECT_INIT(left + 2.5, top + size / 2 + 1, size - 5, 1.5));
  }
}

/* The arrow at the stopped line: a box pointing right. */
static void draw_arrow(GtkSnapshot *snapshot, double y, double height)
{
  GskPathBuilder *builder = gsk_path_builder_new();
  double h = MIN(10, height - 4), top = y + (height - h) / 2, left = DOT_WIDTH / 2.0 - 7, tip = DOT_WIDTH / 2.0 + 7;
  GskPath *path;

  gsk_path_builder_move_to(builder, left, top);
  gsk_path_builder_line_to(builder, tip - h / 2, top);
  gsk_path_builder_line_to(builder, tip, top + h / 2);
  gsk_path_builder_line_to(builder, tip - h / 2, top + h);
  gsk_path_builder_line_to(builder, left, top + h);
  gsk_path_builder_close(builder);
  path = gsk_path_builder_free_to_path(builder);
  gtk_snapshot_append_fill(snapshot, path, GSK_FILL_RULE_WINDING, palette_rgba(PAL_WARNING));
  gsk_path_unref(path);
}

static void snapshot_dot(GtkSourceGutterRenderer *renderer, GtkSnapshot *snapshot, GtkSourceGutterLines *lines,
                         guint line)
{
  Breakpoints *b = TCODE_BREAKPOINT_RENDERER(renderer)->breakpoints;
  GSList *marks;
  double y, height;
  GdkRGBA red;

  if (!b || !b->tab->view || b->tab->view->loading)
    return;
  gtk_source_gutter_lines_get_line_extent(lines, line, GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_FIRST, &y, &height);
  red = *palette_rgba(PAL_ERROR);
  marks = marks_on(b->tab, (int)line);
  if (marks)
    draw_dot(snapshot, y, height, red, g_object_get_data(marks->data, "condition") != NULL);
  else if (disassembly_is(b->tab->app, b->tab) && disassembly_breakpoint_at(b->tab->app, (int)line))
    draw_dot(snapshot, y, height, red, FALSE);
  else if (b->hover == (int)line && can_click(b->tab)) {
    red.alpha = 0.4f;
    draw_dot(snapshot, y, height, red, FALSE);
  }
  g_slist_free(marks);
  if (stop_line(b->tab) == (int)line)
    draw_arrow(snapshot, y, height);
}

static gboolean query_dot(GtkSourceGutterRenderer *renderer, GtkTextIter *iter G_GNUC_UNUSED,
                          GdkRectangle *area G_GNUC_UNUSED)
{
  Breakpoints *b = TCODE_BREAKPOINT_RENDERER(renderer)->breakpoints;

  return b && can_click(b->tab);
}

static void activate_dot(GtkSourceGutterRenderer *renderer, GtkTextIter *iter, GdkRectangle *area G_GNUC_UNUSED,
                         guint button G_GNUC_UNUSED, GdkModifierType state G_GNUC_UNUSED, int n_presses G_GNUC_UNUSED)
{
  Breakpoints *b = TCODE_BREAKPOINT_RENDERER(renderer)->breakpoints;

  if (b && disassembly_is(b->tab->app, b->tab)) {
    disassembly_toggle_breakpoint(b->tab->app, gtk_text_iter_get_line(iter));
    gtk_widget_queue_draw(b->renderer);
  } else if (b) {
    breakpoint_toggle(b->tab, gtk_text_iter_get_line(iter));
  }
}

static void tcode_breakpoint_renderer_class_init(TcodeBreakpointRendererClass *klass)
{
  GtkSourceGutterRendererClass *renderer = GTK_SOURCE_GUTTER_RENDERER_CLASS(klass);

  renderer->snapshot_line = snapshot_dot;
  renderer->query_activatable = query_dot;
  renderer->activate = activate_dot;
}

static void tcode_breakpoint_renderer_init(TcodeBreakpointRenderer *self)
{
  gtk_widget_set_size_request(GTK_WIDGET(self), DOT_WIDTH, -1);
}

/* The line at y in the gutter. */
static int line_at_y(Breakpoints *b, double y)
{
  GtkTextView *view = GTK_TEXT_VIEW(b->tab->source);
  GtkTextIter at;
  int buffer_y;

  gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_LEFT, 0, (int)y, NULL, &buffer_y);
  gtk_text_view_get_line_at_y(view, &at, buffer_y, NULL);
  return gtk_text_iter_get_line(&at);
}

/* The faint dot follows the pointer. */
static void on_motion(GtkEventControllerMotion *motion G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y, Breakpoints *b)
{
  int line = line_at_y(b, y);

  if (line != b->hover) {
    b->hover = line;
    gtk_widget_queue_draw(b->renderer);
  }
}

static void on_leave(GtkEventControllerMotion *motion G_GNUC_UNUSED, Breakpoints *b)
{
  b->hover = -1;
  gtk_widget_queue_draw(b->renderer);
}

/* The condition's popover --------------------------------------------------------------------- */

static void close_ask(Breakpoints *b)
{
  if (b->ask) {
    gtk_widget_unparent(b->ask);
    b->ask = NULL;
  }
}

static gboolean close_ask_later(gpointer data)
{
  close_ask(data);
  return G_SOURCE_REMOVE;
}

static void on_ask_closed(GtkPopover *popover G_GNUC_UNUSED, Breakpoints *b)
{
  g_idle_add(close_ask_later, b); /* not inside its own signal */
}

/* Enter: the condition is set (a breakpoint is made for it); empty: the breakpoint stops always. */
static void on_ask_activate(GtkEntry *entry, Breakpoints *b)
{
  char *condition = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
  GSList *marks = marks_on(b->tab, b->ask_line);

  if (*condition || marks)
    breakpoint_set_condition(b->tab, b->ask_line, *condition ? condition : NULL);
  g_slist_free(marks);
  g_free(condition);
  gtk_popover_popdown(GTK_POPOVER(b->ask));
}

/* A right click in the column: a popover asks for the condition the breakpoint on that line stops on. */
static void on_right_click(GtkGestureClick *click, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y,
                           Breakpoints *b)
{
  GtkWidget *box, *label, *entry, *hint;
  const char *condition;
  int line = line_at_y(b, y), top, height;
  GtkTextIter start;

  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  if (!can_break(b->tab) || b->tab->view->loading)
    return;
  close_ask(b);
  condition = breakpoint_condition(b->tab, line);
  b->ask_line = line;
  b->ask = gtk_popover_new();
  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  label = gtk_label_new(NULL);
  char *title = g_strdup_printf("Line %d: stop only when this is true", line + 1);
  gtk_label_set_text(GTK_LABEL(label), title);
  g_free(title);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "e.g. i == 3");
  gtk_editable_set_text(GTK_EDITABLE(entry), condition ? condition : "");
  gtk_editable_set_width_chars(GTK_EDITABLE(entry), 32);
  hint = gtk_label_new("Enter sets it; an empty one stops every time. Escape closes.");
  gtk_label_set_xalign(GTK_LABEL(hint), 0);
  gtk_widget_add_css_class(hint, "dim-label");
  gtk_box_append(GTK_BOX(box), label);
  gtk_box_append(GTK_BOX(box), entry);
  gtk_box_append(GTK_BOX(box), hint);
  gtk_popover_set_child(GTK_POPOVER(b->ask), box);
  gtk_popover_set_position(GTK_POPOVER(b->ask), GTK_POS_RIGHT);
  gtk_widget_set_parent(b->ask, b->renderer);
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(b->tab->buffer), &start, line);
  gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(b->tab->source), &start, &top, &height);
  gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(b->tab->source), GTK_TEXT_WINDOW_LEFT, 0, top, NULL, &top);
  gtk_popover_set_pointing_to(GTK_POPOVER(b->ask), &(GdkRectangle){ 0, top, DOT_WIDTH, height });
  g_signal_connect(entry, "activate", G_CALLBACK(on_ask_activate), b);
  g_signal_connect(b->ask, "closed", G_CALLBACK(on_ask_closed), b);
  gtk_popover_popup(GTK_POPOVER(b->ask));
  gtk_widget_grab_focus(entry);
}

void breakpoints_attach(Tab *tab)
{
  Breakpoints *b = g_new0(Breakpoints, 1);
  GtkSourceGutter *gutter = gtk_source_view_get_gutter(tab->source, GTK_TEXT_WINDOW_LEFT);
  GtkEventController *motion = gtk_event_controller_motion_new();

  b->tab = tab;
  b->hover = -1;
  b->renderer = g_object_ref_sink(g_object_new(TCODE_TYPE_BREAKPOINT_RENDERER, NULL)); /* ours too: see free */
  TCODE_BREAKPOINT_RENDERER(b->renderer)->breakpoints = b;
  gtk_source_gutter_insert(gutter, GTK_SOURCE_GUTTER_RENDERER(b->renderer), -40); /* left of the line numbers */
  g_signal_connect(motion, "motion", G_CALLBACK(on_motion), b);
  g_signal_connect(motion, "leave", G_CALLBACK(on_leave), b);
  gtk_widget_add_controller(b->renderer, motion);
  GtkGesture *right = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), GDK_BUTTON_SECONDARY);
  g_signal_connect(right, "pressed", G_CALLBACK(on_right_click), b);
  gtk_widget_add_controller(b->renderer, GTK_EVENT_CONTROLLER(right));
  g_signal_connect(tab->buffer, "changed", G_CALLBACK(on_changed), b);
  tab->view->breakpoints = b;
}

void breakpoints_free(Breakpoints *b)
{
  if (!b)
    return;
  g_signal_handlers_disconnect_by_data(b->tab->buffer, b);
  g_idle_remove_by_data(b);
  close_ask(b);
  /* The page went first: the renderer lives on with our reference, and must not reach us. */
  TCODE_BREAKPOINT_RENDERER(b->renderer)->breakpoints = NULL;
  g_object_unref(b->renderer);
  g_free(b); /* the marks go with the buffer */
}
