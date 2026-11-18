/*
 * ▶ in the editors' gutter on the lines defining tests (see testing.h).
 */
#include "editor/editorview.h"
#include "testing/state.h"
#include "window/palette.h"
#include "window/window.h"

#include <string.h>

#define MARK_WIDTH 16
#define MARKS_DELAY_MS 300      /* after an edit, before the lines are found again */
#define MARKS_MAX_CHARS 2000000 /* bigger files are not where tests are defined */

/* A line with ▶: the tests it defines. */
typedef struct {
  int line; /* 0-based */
  TestState state;
  GPtrArray *ids;      /* char * */
  gboolean debuggable; /* one CTest test with a command */
} MarkLine;

struct TestMarks {
  Tab *tab;
  GtkWidget *renderer;
  GArray *lines; /* MarkLine, by line */
  guint idle;    /* an edit's update, waiting */
  GtkWidget *menu;
};

static GList *all_marks; /* TestMarks: the open editors' */

static void mark_line_clear(gpointer data)
{
  g_ptr_array_unref(((MarkLine *)data)->ids);
}

static MarkLine *mark_at(TestMarks *m, int line)
{
  for (guint i = 0; i < m->lines->len; i++)
    if (g_array_index(m->lines, MarkLine, i).line == line)
      return &g_array_index(m->lines, MarkLine, i);
  return NULL;
}

/* The state ▶ shows for several tests: running while one runs, failed when one failed, passed when
 * all passed. */
static TestState joined(TestState a, TestState b)
{
  if (a == TEST_RUNNING || b == TEST_RUNNING)
    return TEST_RUNNING;
  if (a == TEST_FAILED || b == TEST_FAILED)
    return TEST_FAILED;
  if (a == TEST_PASSED && b == TEST_PASSED)
    return TEST_PASSED;
  if ((a == TEST_SKIPPED && b == TEST_PASSED) || (a == TEST_PASSED && b == TEST_SKIPPED))
    return TEST_PASSED;
  return a == b ? a : TEST_UNKNOWN;
}

/* Finds the lines defining the listed tests in the text again. */
static void update(TestMarks *m)
{
  GPtrArray *tests = testing_list();
  Tab *tab = m->tab;

  g_array_set_size(m->lines, 0);
  if (tests && tests->len && tab->path && tab->view && !tab->view->loading && test_file_may_define(tab->path) &&
      gtk_text_buffer_get_char_count(GTK_TEXT_BUFFER(tab->buffer)) <= MARKS_MAX_CHARS) {
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
    char *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, FALSE);
    GArray *found = test_lines(tests, tab->app->project.root, tab->path, text);
    for (guint i = 0; i < found->len; i++) {
      TestLine *l = &g_array_index(found, TestLine, i);
      TestCase *test = tests->pdata[l->test];
      MarkLine *mark = mark_at(m, l->line - 1);
      if (!mark) {
        g_array_append_val(m->lines,
                           ((MarkLine){ l->line - 1, test->state, g_ptr_array_new_with_free_func(g_free), FALSE }));
        mark = &g_array_index(m->lines, MarkLine, m->lines->len - 1);
      } else {
        mark->state = joined(mark->state, test->state);
      }
      g_ptr_array_add(mark->ids, g_strdup(test->id));
      mark->debuggable = mark->ids->len == 1 && test->kind == TEST_CTEST && test->command && test->command[0];
    }
    g_array_free(found, TRUE);
    g_free(text);
  }
  gtk_widget_set_visible(m->renderer, m->lines->len > 0);
  gtk_widget_queue_draw(m->renderer);
}

void testing_marks_changed(void)
{
  for (GList *l = all_marks; l; l = l->next)
    update(l->data);
}

static gboolean update_later(gpointer data)
{
  TestMarks *m = data;

  m->idle = 0;
  update(m);
  return G_SOURCE_REMOVE;
}

static void on_changed(GtkTextBuffer *buffer G_GNUC_UNUSED, TestMarks *m)
{
  GPtrArray *tests = testing_list();

  if (!tests || !tests->len)
    return;
  g_clear_handle_id(&m->idle, g_source_remove);
  m->idle = g_timeout_add(MARKS_DELAY_MS, update_later, m);
}

/* What a line's tests do ------------------------------------------------------------------------ */

/* The line's ids, a copy: running them finds the lines again (testing_marks_changed). */
static char **ids_of(MarkLine *mark)
{
  char **ids = g_new0(char *, mark->ids->len + 1);

  for (guint i = 0; i < mark->ids->len; i++)
    ids[i] = g_strdup(mark->ids->pdata[i]);
  return ids;
}

static void run_line(MarkLine *mark)
{
  char **ids = ids_of(mark);

  testing_run_ids((const char *const *)ids);
  g_strfreev(ids);
}

static void close_menu(TestMarks *m)
{
  if (m->menu) {
    gtk_widget_unparent(m->menu);
    m->menu = NULL;
  }
}

static gboolean close_menu_later(gpointer data)
{
  close_menu(data);
  return G_SOURCE_REMOVE;
}

static void on_menu_closed(GtkPopover *popover G_GNUC_UNUSED, TestMarks *m)
{
  g_idle_add(close_menu_later, m); /* not while the popover is still handling its signal */
}

static void on_menu_item(GtkButton *button, TestMarks *m)
{
  const char *what = g_object_get_data(G_OBJECT(button), "what");
  MarkLine *mark = mark_at(m, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "line")));

  char **ids = mark ? ids_of(mark) : NULL;

  gtk_popover_popdown(GTK_POPOVER(m->menu));
  if (!ids)
    return;
  if (strcmp(what, "run") == 0) {
    testing_run_ids((const char *const *)ids);
  } else if (strcmp(what, "debug") == 0) {
    testing_debug_one(ids[0]);
  } else {
    show_sidebar_page(m->tab->app, "testing");
    testing_select(ids[0]);
  }
  g_strfreev(ids);
}

static void menu_item(TestMarks *m, GtkWidget *box, const char *label, const char *what, int line, gboolean sensitive)
{
  GtkWidget *button = gtk_button_new_with_label(label);

  gtk_widget_add_css_class(button, "flat");
  gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(button))), 0);
  gtk_widget_set_sensitive(button, sensitive);
  g_object_set_data(G_OBJECT(button), "what", (gpointer)what);
  g_object_set_data(G_OBJECT(button), "line", GINT_TO_POINTER(line));
  g_signal_connect(button, "clicked", G_CALLBACK(on_menu_item), m);
  gtk_box_append(GTK_BOX(box), button);
}

/* The line at y in the gutter. */
static int line_at_y(TestMarks *m, double y)
{
  GtkTextView *view = GTK_TEXT_VIEW(m->tab->source);
  GtkTextIter at;
  int buffer_y;

  gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_LEFT, 0, (int)y, NULL, &buffer_y);
  gtk_text_view_get_line_at_y(view, &at, buffer_y, NULL);
  return gtk_text_iter_get_line(&at);
}

/* A right click on ▶: Run, Debug (a CTest test), Show in Testing. */
static void on_right_click(GtkGestureClick *gesture G_GNUC_UNUSED, int presses G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                           double y, TestMarks *m)
{
  int line = line_at_y(m, y), top, height;
  MarkLine *mark = mark_at(m, line);
  GtkWidget *box;
  GtkTextIter start;

  if (!mark)
    return;
  close_menu(m);
  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  menu_item(m, box, mark->ids->len > 1 ? "Run Tests" : "Run Test", "run", line, !testing_busy());
  if (mark->debuggable)
    menu_item(m, box, "Debug Test", "debug", line, !testing_busy());
  menu_item(m, box, "Show in Testing", "show", line, TRUE);
  m->menu = gtk_popover_new();
  gtk_widget_add_css_class(m->menu, "menu");
  gtk_popover_set_child(GTK_POPOVER(m->menu), box);
  gtk_popover_set_has_arrow(GTK_POPOVER(m->menu), FALSE);
  gtk_popover_set_position(GTK_POPOVER(m->menu), GTK_POS_RIGHT);
  gtk_widget_set_parent(m->menu, m->renderer);
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(m->tab->buffer), &start, line);
  gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(m->tab->source), &start, &top, &height);
  gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(m->tab->source), GTK_TEXT_WINDOW_LEFT, 0, top, NULL, &top);
  gtk_popover_set_pointing_to(GTK_POPOVER(m->menu), &(GdkRectangle){ 0, top, MARK_WIDTH, height });
  g_signal_connect(m->menu, "closed", G_CALLBACK(on_menu_closed), m);
  gtk_popover_popup(GTK_POPOVER(m->menu));
}

static const char *state_word(TestState state)
{
  switch (state) {
    case TEST_RUNNING: return "running";
    case TEST_PASSED: return "passed";
    case TEST_FAILED: return "failed";
    case TEST_SKIPPED: return "skipped";
    default: return "not run";
  }
}

static gboolean on_query_tooltip(GtkWidget *widget G_GNUC_UNUSED, int x G_GNUC_UNUSED, int y,
                                 gboolean keyboard G_GNUC_UNUSED, GtkTooltip *tooltip, TestMarks *m)
{
  MarkLine *mark = mark_at(m, line_at_y(m, y));
  char *text;

  if (!mark)
    return FALSE;
  if (mark->ids->len == 1)
    text = g_strdup_printf("Run %s (%s)\nRight click: more", (char *)mark->ids->pdata[0], state_word(mark->state));
  else
    text = g_strdup_printf("Run %u tests (%s)\nRight click: more", mark->ids->len, state_word(mark->state));
  gtk_tooltip_set_text(tooltip, text);
  g_free(text);
  return TRUE;
}

/* The renderer ---------------------------------------------------------------------------------- */

struct _TcodeTestRenderer {
  GtkSourceGutterRenderer parent;
  TestMarks *marks;
};

#define TCODE_TYPE_TEST_RENDERER (tcode_test_renderer_get_type())
G_DECLARE_FINAL_TYPE(TcodeTestRenderer, tcode_test_renderer, TCODE, TEST_RENDERER, GtkSourceGutterRenderer)
G_DEFINE_FINAL_TYPE(TcodeTestRenderer, tcode_test_renderer, GTK_SOURCE_TYPE_GUTTER_RENDERER)

static void draw_play(GtkSnapshot *snapshot, double y, double height, const GdkRGBA *color)
{
  GskPathBuilder *builder = gsk_path_builder_new();
  double h = MIN(10, height - 4), top = y + (height - h) / 2, left = (MARK_WIDTH - h * 0.85) / 2;
  GskPath *path;

  gsk_path_builder_move_to(builder, left, top);
  gsk_path_builder_line_to(builder, left + h * 0.85, top + h / 2);
  gsk_path_builder_line_to(builder, left, top + h);
  gsk_path_builder_close(builder);
  path = gsk_path_builder_free_to_path(builder);
  gtk_snapshot_append_fill(snapshot, path, GSK_FILL_RULE_WINDING, color);
  gsk_path_unref(path);
}

static void snapshot_mark(GtkSourceGutterRenderer *renderer, GtkSnapshot *snapshot, GtkSourceGutterLines *lines,
                          guint line)
{
  TestMarks *m = TCODE_TEST_RENDERER(renderer)->marks;
  MarkLine *mark = m ? mark_at(m, (int)line) : NULL;
  double y, height;

  if (!mark)
    return;
  gtk_source_gutter_lines_get_line_extent(lines, line, GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_FIRST, &y, &height);
  draw_play(snapshot, y, height,
            palette_rgba(mark->state == TEST_PASSED    ? PAL_GIT_ADDED
                         : mark->state == TEST_FAILED  ? PAL_ERROR
                         : mark->state == TEST_RUNNING ? PAL_ACCENT
                                                       : PAL_FG_DIM));
}

static gboolean query_mark(GtkSourceGutterRenderer *renderer, GtkTextIter *iter, GdkRectangle *area G_GNUC_UNUSED)
{
  TestMarks *m = TCODE_TEST_RENDERER(renderer)->marks;

  return m && mark_at(m, gtk_text_iter_get_line(iter)) && !testing_busy();
}

static void activate_mark(GtkSourceGutterRenderer *renderer, GtkTextIter *iter, GdkRectangle *area G_GNUC_UNUSED,
                          guint button, GdkModifierType state G_GNUC_UNUSED, int n_presses G_GNUC_UNUSED)
{
  TestMarks *m = TCODE_TEST_RENDERER(renderer)->marks;
  MarkLine *mark = m && button == GDK_BUTTON_PRIMARY ? mark_at(m, gtk_text_iter_get_line(iter)) : NULL;

  if (mark)
    run_line(mark);
}

static void tcode_test_renderer_class_init(TcodeTestRendererClass *klass)
{
  GtkSourceGutterRendererClass *renderer = GTK_SOURCE_GUTTER_RENDERER_CLASS(klass);

  renderer->snapshot_line = snapshot_mark;
  renderer->query_activatable = query_mark;
  renderer->activate = activate_mark;
}

static void tcode_test_renderer_init(TcodeTestRenderer *self)
{
  gtk_widget_set_size_request(GTK_WIDGET(self), MARK_WIDTH, -1);
}

/* An editor's marks --------------------------------------------------------------------------------- */

void testing_marks_attach(Tab *tab)
{
  TestMarks *m = g_new0(TestMarks, 1);
  GtkSourceGutter *gutter = gtk_source_view_get_gutter(tab->source, GTK_TEXT_WINDOW_LEFT);
  GtkGesture *right = gtk_gesture_click_new();

  m->tab = tab;
  m->lines = g_array_new(FALSE, FALSE, sizeof(MarkLine));
  g_array_set_clear_func(m->lines, mark_line_clear);
  m->renderer = g_object_ref_sink(g_object_new(TCODE_TYPE_TEST_RENDERER, NULL)); /* ours too: see free */
  TCODE_TEST_RENDERER(m->renderer)->marks = m;
  gtk_widget_set_visible(m->renderer, FALSE);
  gtk_widget_set_has_tooltip(m->renderer, TRUE);
  g_signal_connect(m->renderer, "query-tooltip", G_CALLBACK(on_query_tooltip), m);
  gtk_source_gutter_insert(gutter, GTK_SOURCE_GUTTER_RENDERER(m->renderer), -50); /* left of the breakpoints */
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), GDK_BUTTON_SECONDARY);
  g_signal_connect(right, "pressed", G_CALLBACK(on_right_click), m);
  gtk_widget_add_controller(m->renderer, GTK_EVENT_CONTROLLER(right));
  g_signal_connect(tab->buffer, "changed", G_CALLBACK(on_changed), m);
  tab->view->test_marks = m;
  all_marks = g_list_prepend(all_marks, m);
}

void testing_marks_free(TestMarks *m)
{
  if (!m)
    return;
  all_marks = g_list_remove(all_marks, m);
  g_signal_handlers_disconnect_by_data(m->tab->buffer, m);
  g_clear_handle_id(&m->idle, g_source_remove);
  g_idle_remove_by_data(m);
  close_menu(m);
  /* The page went first: the renderer lives on with our reference, and must not reach us. */
  TCODE_TEST_RENDERER(m->renderer)->marks = NULL;
  g_object_unref(m->renderer);
  g_array_free(m->lines, TRUE);
  g_free(m);
}

char *testing_marks_text(Tab *tab)
{
  TestMarks *m = tab->view ? tab->view->test_marks : NULL;
  GString *text = g_string_new(NULL);

  for (guint i = 0; m && i < m->lines->len; i++) {
    MarkLine *mark = &g_array_index(m->lines, MarkLine, i);
    g_string_append_printf(text, "%s%d %s", i ? "\n" : "", mark->line + 1, state_word(mark->state));
    for (guint j = 0; j < mark->ids->len; j++)
      g_string_append_printf(text, " %s", (char *)mark->ids->pdata[j]);
  }
  return g_string_free(text, FALSE);
}
