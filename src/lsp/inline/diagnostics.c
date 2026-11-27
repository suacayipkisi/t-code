/*
 * Diagnostics in an editor (see diagnostics.h). Each one keeps its range as two marks, so it moves
 * with the text typed before it until the server sends the next list; diagnostics_draw() draws the
 * wave under it from those marks. The language server's list and each plugin's (socket "diagnostics-set") replace only
 * their own; plugins' lists are kept per path, for files opened later too.
 */
#include "lsp/inline/diagnostics.h"

#include "editor/editorview.h"
#include "editor/tabs.h"
#include "lsp/client/client.h"
#include "window/statusbar.h"
#include "window/palette.h"
#include "lsp/problems.h"

/* LSP's severities. */
enum { SEVERITY_ERROR = 1, SEVERITY_WARNING, SEVERITY_INFO, SEVERITY_HINT };

struct Diagnostic {
  GtkTextMark *start, *end;
  int severity;
  char *message;
  char *json;  /* as the server sent it, for a quick fix request; NULL for a plugin's */
  char *owner; /* the plugin that gave it, or NULL: the language server */
};

/* The error, warning and info colors, from the color theme. */
static const PalColor colors[] = { PAL_ERROR, PAL_ERROR, PAL_WARNING, PAL_INFO, PAL_INFO };
static const char *const labels[] = { NULL, "error", "warning", "note", "hint" };

static void diagnostic_free(Diagnostic *d)
{
  GtkTextBuffer *buffer = gtk_text_mark_get_buffer(d->start);

  if (buffer) {
    gtk_text_buffer_delete_mark(buffer, d->start);
    gtk_text_buffer_delete_mark(buffer, d->end);
  }
  g_free(d->message);
  g_free(d->json);
  g_free(d->owner);
  g_free(d);
}

/* Forgets the list (diagnostics_draw() draws the waves from it). */
static void remove_all(Tab *tab)
{
  if (tab->view->diagnostics)
    g_ptr_array_set_size(tab->view->diagnostics, 0);
}

void diagnostics_clear(Tab *tab)
{
  remove_all(tab);
  g_clear_pointer(&tab->view->diagnostics, g_ptr_array_unref);
  problems_update(tab->app);
}

/* A range with nothing in it (a missing ";") gets the character after it, or else the one before:
 * a wave needs something to go under. */
static void widen(GtkTextIter *start, GtkTextIter *end)
{
  if (!gtk_text_iter_equal(start, end))
    return;
  if (!gtk_text_iter_ends_line(end))
    gtk_text_iter_forward_char(end);
  else if (!gtk_text_iter_starts_line(start))
    gtk_text_iter_backward_char(start);
}

/* Drops the diagnostics owner gave (NULL: the language server's); retag() then draws the rest. */
static void drop_owned(Tab *tab, const char *owner)
{
  GPtrArray *list = tab->view->diagnostics;

  for (guint i = list->len; i-- > 0;)
    if (g_strcmp0(((Diagnostic *)g_ptr_array_index(list, i))->owner, owner) == 0)
      g_ptr_array_remove_index(list, i);
}

/* The counts again, and everything that shows the diagnostics. */
static void retag(Tab *tab)
{
  tab->problems.errors = tab->problems.warnings = 0;
  for (guint i = 0; i < tab->view->diagnostics->len; i++) {
    Diagnostic *d = g_ptr_array_index(tab->view->diagnostics, i);
    tab->problems.errors += d->severity == SEVERITY_ERROR;
    tab->problems.warnings += d->severity == SEVERITY_WARNING;
  }
  editor_view_show_problems(tab);
  update_status(tab->app);
  problems_update(tab->app);
}

/* One more diagnostic over start..end (taken: message, json). */
static void add_one(Tab *tab, GtkTextIter *start, GtkTextIter *end, int severity, char *message, char *json,
                    const char *owner)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  Diagnostic *d = g_new0(Diagnostic, 1);

  widen(start, end);
  d->severity = severity;
  d->message = message;
  d->json = json;
  d->owner = g_strdup(owner);
  d->start = gtk_text_buffer_create_mark(buffer, NULL, start, TRUE);
  d->end = gtk_text_buffer_create_mark(buffer, NULL, end, TRUE); /* text typed right after is not in it */
  g_ptr_array_add(tab->view->diagnostics, d);
}

static void ensure_list(Tab *tab)
{
  if (!tab->view->diagnostics)
    tab->view->diagnostics = g_ptr_array_new_with_free_func((GDestroyNotify)diagnostic_free);
}

void diagnostics_show(Tab *tab, JsonNode *list)
{
  JsonArray *array = list && JSON_NODE_HOLDS_ARRAY(list) ? json_node_get_array(list) : NULL;
  guint n = array ? json_array_get_length(array) : 0;

  ensure_list(tab);
  drop_owned(tab, NULL);
  for (guint i = 0; i < n; i++) {
    JsonNode *node = json_array_get_element(array, i);
    GtkTextIter start, end;

    lsp_iter_at(tab, json_at(node, "range.start"), &start);
    lsp_iter_at(tab, json_at(node, "range.end"), &end);
    add_one(tab, &start, &end, (int)CLAMP(json_int_at(node, "severity", SEVERITY_ERROR), SEVERITY_ERROR, SEVERITY_HINT),
            g_strdup(json_string_at(node, "message") ? json_string_at(node, "message") : ""),
            json_to_string(node, FALSE), NULL);
  }
  retag(tab);
}

/* Plugins' diagnostics ------------------------------------------------------------------------ */

/* The place at line and column (1-based, characters), kept inside the text. */
static void iter_at_place(GtkTextBuffer *buffer, int line, int column, GtkTextIter *iter)
{
  if (!gtk_text_buffer_get_iter_at_line(buffer, iter, MAX(line, 1) - 1)) {
    gtk_text_buffer_get_end_iter(buffer, iter);
    return;
  }
  GtkTextIter line_end = *iter;
  if (!gtk_text_iter_ends_line(&line_end))
    gtk_text_iter_forward_to_line_end(&line_end);
  gtk_text_iter_set_line_offset(iter, CLAMP(column - 1, 0, gtk_text_iter_get_line_offset(&line_end)));
}

/* An int in a JSON array, or fallback. */
static int int_in(JsonArray *array, guint index, int fallback)
{
  JsonNode *node = array && index < json_array_get_length(array) ? json_array_get_element(array, index) : NULL;

  return node && JSON_NODE_HOLDS_VALUE(node) ? (int)json_node_get_int(node) : fallback;
}

/* A plugin's list in an open editor, in place of its last one: {"range":[l1,c1,l2,c2],
 * "severity":"error|warning|info|hint","message":..,"code":..,"source":..}, lines and columns 1-based. */
static void show_owned(Tab *tab, const char *owner, JsonNode *items)
{
  static const char *const severities[] = { NULL, "error", "warning", "info", "hint" };
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  JsonArray *array = items && JSON_NODE_HOLDS_ARRAY(items) ? json_node_get_array(items) : NULL;

  ensure_list(tab);
  drop_owned(tab, owner);
  for (guint i = 0; array && i < json_array_get_length(array); i++) {
    JsonNode *node = json_array_get_element(array, i);
    JsonNode *range_node =
        JSON_NODE_HOLDS_OBJECT(node) ? json_object_get_member(json_node_get_object(node), "range") : NULL;
    JsonArray *range = range_node && JSON_NODE_HOLDS_ARRAY(range_node) ? json_node_get_array(range_node) : NULL;
    const char *severity_name = json_string_at(node, "severity"), *message = json_string_at(node, "message");
    const char *code = json_string_at(node, "code"), *source = json_string_at(node, "source");
    int severity = SEVERITY_ERROR;
    GtkTextIter start, end;

    if (!range || !message)
      continue;
    for (int s = SEVERITY_ERROR; s <= SEVERITY_HINT; s++)
      if (g_strcmp0(severity_name, severities[s]) == 0)
        severity = s;
    iter_at_place(buffer, int_in(range, 0, 1), int_in(range, 1, 1), &start);
    iter_at_place(buffer, int_in(range, 2, int_in(range, 0, 1)), int_in(range, 3, int_in(range, 1, 1)), &end);
    if (gtk_text_iter_compare(&end, &start) < 0)
      end = start;
    add_one(tab, &start, &end, severity,
            g_strdup_printf("%s (%s%s%s)", message, source ? source : owner, code ? " " : "", code ? code : ""), NULL,
            owner);
  }
  retag(tab);
}

/* The open editor of path that has its text, or NULL. */
static Tab *loaded_editor(App *t, const char *path)
{
  Tab *tab = find_tab(t, TAB_EDITOR, path);

  return tab && tab->view && tab->buffer && !tab->view->loading ? tab : NULL;
}

void diagnostics_set_owned(App *t, const char *path, const char *owner, JsonNode *items)
{
  GHashTable *owners;
  gboolean empty = !items || !JSON_NODE_HOLDS_ARRAY(items) || json_array_get_length(json_node_get_array(items)) == 0;
  Tab *tab = loaded_editor(t, path);

  if (!t->plugin_diagnostics)
    t->plugin_diagnostics = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_unref);
  owners = g_hash_table_lookup(t->plugin_diagnostics, path);
  if (!owners && !empty) {
    owners = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
    g_hash_table_insert(t->plugin_diagnostics, g_strdup(path), owners);
  }
  if (owners && empty)
    g_hash_table_remove(owners, owner);
  else if (owners)
    g_hash_table_insert(owners, g_strdup(owner), json_node_copy(items));
  if (owners && g_hash_table_size(owners) == 0)
    g_hash_table_remove(t->plugin_diagnostics, path);
  if (tab)
    show_owned(tab, owner, empty ? NULL : items);
}

void diagnostics_clear_owned(App *t, const char *path, const char *owner)
{
  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter iter;
  gpointer key, value;

  if (path)
    g_ptr_array_add(paths, g_strdup(path));
  else if (t->plugin_diagnostics) {
    g_hash_table_iter_init(&iter, t->plugin_diagnostics);
    while (g_hash_table_iter_next(&iter, &key, &value))
      if (g_hash_table_contains(value, owner))
        g_ptr_array_add(paths, g_strdup(key));
  }
  for (guint i = 0; i < paths->len; i++)
    diagnostics_set_owned(t, g_ptr_array_index(paths, i), owner, NULL);
  g_ptr_array_unref(paths);
}

void diagnostics_restore(Tab *tab)
{
  GHashTable *owners =
      tab->app->plugin_diagnostics && tab->path ? g_hash_table_lookup(tab->app->plugin_diagnostics, tab->path) : NULL;
  GHashTableIter iter;
  gpointer owner, items;

  if (!owners)
    return;
  g_hash_table_iter_init(&iter, owners);
  while (g_hash_table_iter_next(&iter, &owner, &items))
    show_owned(tab, owner, items);
}

/* Whether d's range holds iter; start and end are set to it. */
static gboolean covers(Tab *tab, const Diagnostic *d, const GtkTextIter *iter, GtkTextIter *start, GtkTextIter *end)
{
  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), start, d->start);
  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), end, d->end);
  return gtk_text_iter_compare(iter, start) >= 0 && gtk_text_iter_compare(iter, end) <= 0;
}

char *diagnostics_at(Tab *tab, const GtkTextIter *iter, gboolean markup)
{
  GString *text = g_string_new(NULL);

  for (guint i = 0; tab->view->diagnostics && i < tab->view->diagnostics->len; i++) {
    Diagnostic *d = g_ptr_array_index(tab->view->diagnostics, i);
    GtkTextIter start, end;
    if (!covers(tab, d, iter, &start, &end))
      continue;
    if (text->len)
      g_string_append_c(text, '\n');
    if (markup) {
      char *escaped = g_markup_escape_text(d->message, -1);
      g_string_append_printf(text, "<span foreground=\"%s\">%s:</span> %s", palette_hex(colors[d->severity]),
                             labels[d->severity], escaped);
      g_free(escaped);
    } else {
      g_string_append_printf(text, "%s: %s", labels[d->severity], d->message);
    }
  }
  return g_string_free(text, text->len == 0);
}

void diagnostics_lines(Tab *tab, GArray *errors, GArray *warnings)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);

  for (guint i = 0; tab->view->diagnostics && i < tab->view->diagnostics->len; i++) {
    Diagnostic *d = g_ptr_array_index(tab->view->diagnostics, i);
    GtkTextIter start;
    int line;
    gtk_text_buffer_get_iter_at_mark(buffer, &start, d->start);
    line = gtk_text_iter_get_line(&start);
    if (d->severity == SEVERITY_ERROR)
      g_array_append_val(errors, line);
    else if (d->severity == SEVERITY_WARNING)
      g_array_append_val(warnings, line);
  }
}

static void draw_wave(cairo_t *cr, double x1, double x2, double y)
{
  double p = 3.6;
  double amp = 0.8;
  double x = x1;

  cairo_move_to(cr, x, y);
  while (x + p <= x2) {
    cairo_curve_to(cr, x + p * 0.25, y - amp, x + p * 0.25, y - amp, x + p * 0.5, y);
    cairo_curve_to(cr, x + p * 0.75, y + amp, x + p * 0.75, y + amp, x + p, y);
    x += p;
  }
  if (x + p * 0.5 <= x2) {
    cairo_curve_to(cr, x + p * 0.25, y - amp, x + p * 0.25, y - amp, x + p * 0.5, y);
    x += p * 0.5;
  }
  if (x < x2)
    cairo_line_to(cr, x2, y);
}

void diagnostics_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot)
{
  if (!tab || !tab->view || !tab->view->diagnostics || tab->view->diagnostics->len == 0)
    return;

  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GdkRectangle visible;
  gtk_text_view_get_visible_rect(view, &visible);

  GtkTextIter line_iter;
  gtk_text_view_get_line_at_y(view, &line_iter, visible.y, NULL);
  int first_line = gtk_text_iter_get_line(&line_iter);
  gtk_text_view_get_line_at_y(view, &line_iter, visible.y + visible.height, NULL);
  int last_line = gtk_text_iter_get_line(&line_iter);

  graphene_rect_t bounds =
      GRAPHENE_RECT_INIT((float)visible.x, (float)visible.y, (float)visible.width, (float)visible.height);
  cairo_t *cr = NULL;

  for (guint i = 0; i < tab->view->diagnostics->len; i++) {
    Diagnostic *d = g_ptr_array_index(tab->view->diagnostics, i);
    if (d->severity < SEVERITY_ERROR || d->severity > SEVERITY_INFO)
      continue;

    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_mark(buffer, &start, d->start);
    gtk_text_buffer_get_iter_at_mark(buffer, &end, d->end);

    int s_line = gtk_text_iter_get_line(&start);
    int e_line = gtk_text_iter_get_line(&end);
    if (e_line < first_line || s_line > last_line)
      continue;

    if (!cr) {
      cr = gtk_snapshot_append_cairo(snapshot, &bounds);
      cairo_set_line_width(cr, 1.1);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    }

    gdk_cairo_set_source_rgba(cr, palette_rgba(colors[d->severity]));

    for (int line = MAX(s_line, first_line); line <= MIN(e_line, last_line); line++) {
      GtkTextIter line_start, line_end;
      gtk_text_buffer_get_iter_at_line(buffer, &line_start, line);
      line_end = line_start;
      if (!gtk_text_iter_ends_line(&line_end))
        gtk_text_iter_forward_to_line_end(&line_end);

      GtkTextIter seg_start = (line == s_line) ? start : line_start;
      GtkTextIter seg_end = (line == e_line) ? end : line_end;

      if (gtk_text_iter_compare(&seg_start, &seg_end) >= 0)
        continue;

      GdkRectangle r1, r2;
      gtk_text_view_get_iter_location(view, &seg_start, &r1);
      gtk_text_view_get_iter_location(view, &seg_end, &r2);

      double x1 = r1.x;
      double x2 = r2.x;
      if (x2 <= x1)
        x2 = x1 + MAX(r1.width, 8);
      double y = r1.y + r1.height - 1.2;

      draw_wave(cr, x1, x2, y);
      cairo_stroke(cr);
    }
  }

  if (cr)
    cairo_destroy(cr);
}

char *diagnostics_json(Tab *tab, int first, int last)
{
  GString *list = g_string_new("[");
  GtkTextIter start, end;

  for (guint i = 0; tab->view->diagnostics && i < tab->view->diagnostics->len; i++) {
    Diagnostic *d = g_ptr_array_index(tab->view->diagnostics, i);
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &start, d->start);
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &end, d->end);
    if (d->json && gtk_text_iter_get_line(&start) <= last && gtk_text_iter_get_line(&end) >= first)
      g_string_append_printf(list, "%s%s", list->len > 1 ? "," : "", d->json);
  }
  g_string_append_c(list, ']');
  return g_string_free(list, FALSE);
}

char *diagnostics_json_at(Tab *tab, const GtkTextIter *iter, GtkTextIter *start, GtkTextIter *end)
{
  GString *list = g_string_new("[");

  for (guint i = 0; tab->view->diagnostics && i < tab->view->diagnostics->len; i++) {
    Diagnostic *d = g_ptr_array_index(tab->view->diagnostics, i);
    GtkTextIter from, to;
    if (!d->json || !covers(tab, d, iter, &from, &to)) /* a plugin's: not the server's to fix */
      continue;
    if (list->len == 1 || gtk_text_iter_compare(&from, start) < 0)
      *start = from;
    if (list->len == 1 || gtk_text_iter_compare(&to, end) > 0)
      *end = to;
    g_string_append_printf(list, "%s%s", list->len > 1 ? "," : "", d->json);
  }
  if (list->len == 1)
    return g_string_free(list, TRUE);
  g_string_append_c(list, ']');
  return g_string_free(list, FALSE);
}

GPtrArray *diagnostics_of(Tab *tab)
{
  return (tab && tab->view) ? tab->view->diagnostics : NULL;
}

void diagnostic_position(Tab *tab, const Diagnostic *d, int *line, int *column)
{
  if (line)
    *line = 1;
  if (column)
    *column = 1;
  if (!tab || !tab->buffer || !d || !d->start)
    return;
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_mark(buffer, &iter, d->start);
  if (line)
    *line = gtk_text_iter_get_line(&iter) + 1;
  if (column)
    *column = gtk_text_iter_get_line_offset(&iter) + 1;
}

int diagnostic_severity(const Diagnostic *d)
{
  return d ? d->severity : 1;
}

const char *diagnostic_message(const Diagnostic *d)
{
  return (d && d->message) ? d->message : "";
}
