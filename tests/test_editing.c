/*
 * Tests for the editor's editing commands and typing help (src/editor/editing.c) and its find bar
 * (src/editor/findbar.c), on a real GtkSourceView. The editor page's files are included to reach
 * their static functions: keys are fed to the typing help the way the view's key controller would.
 */
#define _DEFAULT_SOURCE /* kill() under -std=c23 with musl */

#include "editor_page.h"
#include "broadway.h"

#include <glib/gstdio.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>

static App app;

/* An editor tab (not in a window) holding text as C, the cursor at the start. */
static Tab *new_tab(const char *text)
{
  Tab *tab = g_new0(Tab, 1);
  GtkTextIter start;

  tab->app = &app;
  tab->kind = TAB_EDITOR;
  tab->path = g_strdup("/test/a.c");
  tab->page = g_object_ref_sink(editor_page());
  editor_view_init(tab, FALSE);
  gtk_source_buffer_set_language(
      tab->buffer, gtk_source_language_manager_get_language(gtk_source_language_manager_get_default(), "c"));
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(tab->buffer), text, -1);
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(tab->buffer), &start);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &start);
  return tab;
}

static void free_tab(Tab *tab)
{
  editor_view_free(tab->view);
  g_object_unref(tab->page);
  g_free(tab->path);
  g_free(tab);
}

/* The whole text; the caller frees it. */
static char *text_of(Tab *tab)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
}

static void assert_text(Tab *tab, const char *expected)
{
  char *text = text_of(tab);

  g_assert_cmpstr(text, ==, expected);
  g_free(text);
}

static void select_lines(Tab *tab, int line, int column, int to_line, int to_column)
{
  GtkTextIter a, b;

  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &a, line, column);
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &b, to_line, to_column);
  gtk_text_buffer_select_range(GTK_TEXT_BUFFER(tab->buffer), &b, &a);
}

static void cursor_at(Tab *tab, int line, int column)
{
  select_lines(tab, line, column, line, column);
}

static void assert_cursor(Tab *tab, int line, int column)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
  g_assert_cmpint(gtk_text_iter_get_line(&iter), ==, line);
  g_assert_cmpint(gtk_text_iter_get_line_offset(&iter), ==, column);
}

/* A key as typed: the typing help first, then (unless it took the key) what the view would do. */
static void type_key(Tab *tab, guint key)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);

  if (on_typing_key(NULL, key, 0, 0, tab))
    return;
  if (key == GDK_KEY_BackSpace) {
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
    gtk_text_buffer_backspace(buffer, &iter, TRUE, TRUE);
  } else if (key == GDK_KEY_Return) {
    gtk_text_buffer_insert_at_cursor(buffer, "\n", -1);
  } else {
    char c[8] = { 0 };
    g_unichar_to_utf8(gdk_keyval_to_unicode(key), c);
    gtk_text_buffer_delete_selection(buffer, TRUE, TRUE);
    gtk_text_buffer_insert_at_cursor(buffer, c, -1);
  }
}

static void type_text(Tab *tab, const char *text)
{
  for (const char *p = text; *p; p++)
    type_key(tab, gdk_unicode_to_keyval((guchar)*p));
}

/* The selected text; the caller frees it. */
static char *selection_of(Tab *tab)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
}

/* Comments ----------------------------------------------------------------------------------- */

static void test_comment_lines(void)
{
  Tab *tab = new_tab("int a;\n  int b;\n\nint c;\n");

  select_lines(tab, 0, 0, 2, 0); /* ends at line 2's start: lines 0 and 1 */
  editor_toggle_comment(tab);
  assert_text(tab, "// int a;\n//   int b;\n\nint c;\n");
  editor_toggle_comment(tab);
  assert_text(tab, "int a;\n  int b;\n\nint c;\n");
  free_tab(tab);
}

/* Blank lines stay blank; the comment goes at the indentation. */
static void test_comment_indented(void)
{
  Tab *tab = new_tab("{\n    x();\n\n    y();\n}\n");

  select_lines(tab, 1, 0, 3, 3);
  editor_toggle_comment(tab);
  assert_text(tab, "{\n    // x();\n\n    // y();\n}\n");
  /* Partly commented: all of them get one. */
  cursor_at(tab, 1, 0);
  editor_toggle_comment(tab);
  select_lines(tab, 1, 0, 3, 3);
  editor_toggle_comment(tab);
  assert_text(tab, "{\n    // x();\n\n    // // y();\n}\n");
  free_tab(tab);
}

static void test_block_comment(void)
{
  Tab *tab = new_tab("a { color: red; }\n");

  gtk_source_buffer_set_language(
      tab->buffer, gtk_source_language_manager_get_language(gtk_source_language_manager_get_default(), "css"));
  editor_toggle_comment(tab);
  assert_text(tab, "/* a { color: red; } */\n");
  editor_toggle_comment(tab);
  assert_text(tab, "a { color: red; }\n");
  free_tab(tab);
}

/* Lines -------------------------------------------------------------------------------------- */

static void test_duplicate(void)
{
  Tab *tab = new_tab("a\nb\n");

  cursor_at(tab, 0, 1);
  editor_duplicate_lines(tab);
  assert_text(tab, "a\na\nb\n");
  assert_cursor(tab, 1, 1); /* on the copy */
  select_lines(tab, 1, 0, 2, 1);
  editor_duplicate_lines(tab);
  assert_text(tab, "a\na\nb\na\nb\n");
  free_tab(tab);
}

static void test_delete_line(void)
{
  Tab *tab = new_tab("a\nb\nc");

  cursor_at(tab, 1, 0);
  editor_delete_lines(tab);
  assert_text(tab, "a\nc");
  cursor_at(tab, 1, 1); /* the last line: the line break before it goes too */
  editor_delete_lines(tab);
  assert_text(tab, "a");
  free_tab(tab);
}

static void test_undo_redo(void)
{
  Tab *tab = new_tab("a\n");

  gtk_text_buffer_set_enable_undo(GTK_TEXT_BUFFER(tab->buffer), TRUE);
  editor_duplicate_lines(tab);
  assert_text(tab, "a\na\n");
  gtk_text_buffer_undo(GTK_TEXT_BUFFER(tab->buffer));
  assert_text(tab, "a\n");
  editor_redo(tab);
  assert_text(tab, "a\na\n");
  free_tab(tab);
}

/* Typing help -------------------------------------------------------------------------------- */

static void test_brackets(void)
{
  Tab *tab = new_tab("");

  type_text(tab, "f(");
  assert_text(tab, "f()");
  type_text(tab, "x[1");
  assert_text(tab, "f(x[1])");
  type_text(tab, "])"); /* steps over both */
  assert_text(tab, "f(x[1])");
  assert_cursor(tab, 0, 7);
  type_text(tab, ";");
  assert_text(tab, "f(x[1]);");
  free_tab(tab);
}

/* A ")" that was there already is typed, not stepped over. */
static void test_closer_not_ours(void)
{
  Tab *tab = new_tab("g(a)");

  cursor_at(tab, 0, 3);
  type_text(tab, ")");
  assert_text(tab, "g(a))");
  free_tab(tab);
}

/* Before a word, "(" is just "(". After a letter, "'" is an apostrophe. */
static void test_no_pair(void)
{
  Tab *tab = new_tab("abc");

  type_text(tab, "(");
  assert_text(tab, "(abc");
  cursor_at(tab, 0, 4);
  type_text(tab, " don't");
  assert_text(tab, "(abc don't");
  free_tab(tab);
}

static void test_backspace_pair(void)
{
  Tab *tab = new_tab("");

  type_text(tab, "x = \"");
  assert_text(tab, "x = \"\"");
  type_key(tab, GDK_KEY_BackSpace);
  assert_text(tab, "x = ");
  free_tab(tab);
}

static void test_wrap_selection(void)
{
  Tab *tab = new_tab("a + b");
  GtkTextIter start, end;
  char *selected;

  select_lines(tab, 0, 0, 0, 5);
  type_text(tab, "(");
  assert_text(tab, "(a + b)");
  g_assert_true(gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end));
  selected = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
  g_assert_cmpstr(selected, ==, "a + b");
  g_free(selected);
  free_tab(tab);
}

static void test_enter_between_braces(void)
{
  Tab *tab = new_tab("");

  app.settings.tab_size = 4;
  app.settings.tabs_to_spaces = TRUE;
  editor_view_apply_settings(tab);
  type_text(tab, "  if (x) {");
  type_key(tab, GDK_KEY_Return);
  assert_text(tab, "  if (x) {\n      \n  }");
  assert_cursor(tab, 1, 6);
  free_tab(tab);
}

/* Enter after any opening bracket goes one level deeper, blanks after the bracket dropped; not in a
 * comment. */
static void test_enter_after_open(void)
{
  Tab *tab = new_tab("  call(\n  int a[] = { \n  x = (a, b);\n// (\n");
  GtkTextIter start, end;

  app.settings.tab_size = 4;
  app.settings.tabs_to_spaces = TRUE;
  editor_view_apply_settings(tab);
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  gtk_source_buffer_ensure_highlight(tab->buffer, &start, &end);

  cursor_at(tab, 0, 7);
  type_key(tab, GDK_KEY_Return);
  assert_cursor(tab, 1, 6);
  cursor_at(tab, 2, 13); /* after "{ " */
  type_key(tab, GDK_KEY_Return);
  assert_cursor(tab, 3, 6);
  cursor_at(tab, 4, 7); /* "(|a, b)": the rest goes deeper */
  type_key(tab, GDK_KEY_Return);
  assert_cursor(tab, 5, 6);
  cursor_at(tab, 6, 4); /* in a comment: a plain line break */
  type_key(tab, GDK_KEY_Return);
  assert_text(tab, "  call(\n      \n  int a[] = {\n      \n  x = (\n      a, b);\n// (\n\n");
  free_tab(tab);
}

static void test_outdent_brace(void)
{
  Tab *tab = new_tab("{\n    x;\n    ");

  app.settings.tab_size = 4;
  app.settings.tabs_to_spaces = TRUE;
  editor_view_apply_settings(tab);
  cursor_at(tab, 2, 4);
  type_text(tab, "}");
  assert_text(tab, "{\n    x;\n}");
  free_tab(tab);
}

/* No brackets closed in a comment. */
static void test_no_pair_in_comment(void)
{
  Tab *tab = new_tab("// a\n");
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  gtk_source_buffer_ensure_highlight(tab->buffer, &start, &end);
  cursor_at(tab, 0, 4);
  type_text(tab, "(");
  assert_text(tab, "// a(\n");
  free_tab(tab);
}

/* Words ------------------------------------------------------------------------------------ */

/* What a double click on line, column selects. */
static char *double_click(Tab *tab, int line, int column)
{
  GtkTextIter at, start, end;
  gboolean stop = FALSE;

  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &at, line, column);
  start = end = at;
  g_signal_emit_by_name(tab->source, "extend-selection", GTK_TEXT_EXTEND_SELECTION_WORD, &at, &start, &end, &stop);
  g_assert_true(stop);
  return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
}

/* A double click selects the name only, without the brackets and punctuation around it. */
static void test_double_click(void)
{
  Tab *tab = new_tab("x = f(a_1, [b])->c<T>;\n");
  char *word;

  word = double_click(tab, 0, 4); /* on "f" */
  g_assert_cmpstr(word, ==, "f");
  g_free(word);
  word = double_click(tab, 0, 7); /* in "a_1" */
  g_assert_cmpstr(word, ==, "a_1");
  g_free(word);
  word = double_click(tab, 0, 12); /* on "b" in "[b]" */
  g_assert_cmpstr(word, ==, "b");
  g_free(word);
  word = double_click(tab, 0, 19); /* on "T" in "<T>" */
  g_assert_cmpstr(word, ==, "T");
  g_free(word);
  word = double_click(tab, 0, 15); /* on the "-" of "->": nothing */
  g_assert_cmpstr(word, ==, "");
  g_free(word);
  free_tab(tab);
}

/* Runs the main loop for ms. */
static void run_for(int ms)
{
  gint64 until = g_get_monotonic_time() + ms * 1000;

  while (g_get_monotonic_time() < until)
    g_main_context_iteration(NULL, FALSE);
}

/* A selected name's whole-word occurrences are highlighted and their lines marked; not a part of a
 * name, and an edit clears them. */
static void test_occurrences(void)
{
  Tab *tab = new_tab("int count;\nint counter;\ncount++;\n/* x.count */\n");
  GtkTextTag *tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(tab->buffer)), TAG);
  GArray *lines = g_array_new(FALSE, FALSE, sizeof(int));
  GtkTextIter iter;

  select_lines(tab, 0, 4, 0, 9);
  run_for(200);
  occurrences_lines(tab, lines);
  g_assert_cmpint(lines->len, ==, 3);
  g_assert_cmpint(g_array_index(lines, int, 0), ==, 0);
  g_assert_cmpint(g_array_index(lines, int, 1), ==, 2);
  g_assert_cmpint(g_array_index(lines, int, 2), ==, 3);
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &iter, 3, 5);
  g_assert_true(gtk_text_iter_has_tag(&iter, tag));
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &iter, 1, 4); /* "counter" */
  g_assert_false(gtk_text_iter_has_tag(&iter, tag));

  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "s", -1);
  g_array_set_size(lines, 0);
  occurrences_lines(tab, lines);
  g_assert_cmpint(lines->len, ==, 0);

  select_lines(tab, 0, 4, 0, 7); /* "cou": part of a name */
  run_for(200);
  occurrences_lines(tab, lines);
  g_assert_cmpint(lines->len, ==, 0);
  g_array_free(lines, TRUE);
  free_tab(tab);
}

/* The strip's marks sit beside their lines in the minimap, also once the minimap scrolled; lines it
 * does not show go to its edges; a click there goes to the line beside it. */
static void test_strip_follows_map(void)
{
  GString *text = g_string_new(NULL);
  GtkWidget *window = gtk_window_new();
  GtkTextView *map;
  Tab *tab;

  for (int i = 0; i < 2000; i++)
    g_string_append_printf(text, "line %d\n", i);
  app.settings.minimap = TRUE;
  tab = new_tab(text->str);
  map = GTK_TEXT_VIEW(tab->view->map);
  gtk_window_set_default_size(GTK_WINDOW(window), 600, 300);
  gtk_window_set_child(GTK_WINDOW(window), tab->page);
  gtk_window_present(GTK_WINDOW(window));
  for (gint64 until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
       !gtk_widget_get_mapped(GTK_WIDGET(map)) && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, TRUE);
  run_for(300);
  if (!gtk_widget_get_mapped(GTK_WIDGET(map))) {
    g_test_skip("the window was not shown");
  } else {
    int height = gtk_widget_get_height(tab->view->overview);
    for (int round = 0; round < 2; round++) {
      GdkRectangle shown;
      GtkTextIter iter;
      int top, bottom, middle, y, h, window_y;
      gtk_text_view_get_visible_rect(map, &shown);
      gtk_text_view_get_line_at_y(map, &iter, shown.y, NULL);
      top = gtk_text_iter_get_line(&iter);
      gtk_text_view_get_line_at_y(map, &iter, shown.y + shown.height - 1, NULL);
      bottom = gtk_text_iter_get_line(&iter);
      middle = (top + bottom) / 2;
      g_assert_cmpint(bottom, <, 1999); /* the minimap shows a part only */

      gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &iter, middle);
      gtk_text_view_get_line_yrange(map, &iter, &y, &h);
      gtk_text_view_buffer_to_window_coords(map, GTK_TEXT_WINDOW_WIDGET, 0, y, NULL, &window_y);
      g_assert_cmpfloat(fabs(strip_y(tab, middle, height) + MARK_HEIGHT / 2 - (window_y + h / 2.0)), <=, 1);
      g_assert_cmpint(abs(strip_line(tab, window_y + h / 2.0, height) - middle), <=, 1);
      if (top > 0)
        g_assert_cmpfloat(strip_y(tab, 0, height), ==, 0);
      g_assert_cmpfloat(strip_y(tab, 1999, height), ==, height - MARK_HEIGHT);

      /* Far down: the minimap scrolls with the view. */
      gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &iter, 1500);
      gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &iter);
      gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(tab->source), &iter, 0, TRUE, 0, 0.5);
      run_for(300);
    }
  }
  gtk_window_set_child(GTK_WINDOW(window), NULL); /* the tab keeps the page */
  gtk_window_destroy(GTK_WINDOW(window));
  free_tab(tab);
  g_string_free(text, TRUE);
  app.settings.minimap = FALSE;
}

/* Snippets ----------------------------------------------------------------------------------- */

/* A completed function's snippet: its first parameter selected and typed over. Tab then moves to
 * the next one only with enable-snippets on (GtkSourceView's key handler checks it even for pushed
 * snippets); the key itself cannot be fed here, the handler wants a real GdkEvent. */
static void test_snippet_tab(void)
{
  Tab *tab = new_tab("");
  GtkSourceSnippet *snippet = gtk_source_snippet_new_parsed("f(${1:int a}, ${2:int b})$0", NULL);
  GtkTextIter start;
  char *selected;

  g_assert_true(gtk_source_view_get_enable_snippets(tab->source));
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(tab->buffer), &start);
  gtk_source_view_push_snippet(tab->source, snippet, &start);
  selected = selection_of(tab);
  g_assert_cmpstr(selected, ==, "int a");
  g_free(selected);
  type_text(tab, "x");
  assert_text(tab, "f(x, int b)");
  g_assert_cmpint(gtk_source_snippet_get_focus_position(snippet), ==, 1); /* still in the snippet */
  g_object_unref(snippet);
  free_tab(tab);
}

/* Find bar ----------------------------------------------------------------------------------- */

/* Runs the main loop until the find bar has counted the matches. */
static void wait_for_count(FindBar *f)
{
  gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

  while (gtk_source_search_context_get_occurrences_count(f->context) < 0 && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  update_count(f);
}

static void test_find(void)
{
  Tab *tab = new_tab("foo bar foo baz foo\n");
  FindBar *f;

  findbar_show(tab, FALSE);
  f = tab->view->find;
  gtk_editable_set_text(GTK_EDITABLE(f->find), "foo");
  wait_for_count(f);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "1 of 3");
  findbar_next(tab, FALSE);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "2 of 3");
  findbar_next(tab, TRUE);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "1 of 3");
  findbar_next(tab, TRUE); /* wraps around */
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "3 of 3");

  gtk_editable_set_text(GTK_EDITABLE(f->find), "nothing");
  wait_for_count(f);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "No results");
  free_tab(tab);
}

static void test_replace(void)
{
  Tab *tab = new_tab("foo bar foo baz foo\n");
  FindBar *f;

  findbar_show(tab, TRUE);
  f = tab->view->find;
  g_assert_true(gtk_widget_get_visible(f->replace_row));
  gtk_editable_set_text(GTK_EDITABLE(f->find), "foo");
  gtk_editable_set_text(GTK_EDITABLE(f->replace), "x");
  wait_for_count(f);
  on_replace(f); /* the selected first match, then on to the next */
  assert_text(tab, "x bar foo baz foo\n");
  on_replace_all(f);
  assert_text(tab, "x bar x baz x\n");
  free_tab(tab);
}

/* A big file is counted in the background: "x of N" once it is. */
static void test_find_big_file(void)
{
  GString *text = g_string_new(NULL);
  Tab *tab;
  FindBar *f;

  for (int i = 0; i < 5000; i++)
    g_string_append_printf(text, "line %d with a tab\n", i);
  tab = new_tab(text->str);
  cursor_at(tab, 2500, 0);
  findbar_show(tab, FALSE);
  f = tab->view->find;
  gtk_editable_set_text(GTK_EDITABLE(f->find), "tab");
  wait_for_count(f);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "2501 of 5000");
  g_string_free(text, TRUE);
  free_tab(tab);
}

/* Ctrl+H on the match the bar selected keeps it: "1 of 3", and Replace takes that one. */
static void test_find_again(void)
{
  Tab *tab = new_tab("foo bar foo baz foo\n");
  FindBar *f;

  findbar_show(tab, FALSE);
  f = tab->view->find;
  gtk_editable_set_text(GTK_EDITABLE(f->find), "foo");
  wait_for_count(f);
  findbar_show(tab, TRUE);
  wait_for_count(f);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "1 of 3");
  gtk_editable_set_text(GTK_EDITABLE(f->replace), "x");
  on_replace(f);
  assert_text(tab, "x bar foo baz foo\n");
  free_tab(tab);
}

/* Match Case on: the selection moves to a match that still is one. */
static void test_find_option(void)
{
  Tab *tab = new_tab("FOO foo\n");
  FindBar *f;
  GtkTextIter start, end;
  char *selected;

  findbar_show(tab, FALSE);
  f = tab->view->find;
  gtk_editable_set_text(GTK_EDITABLE(f->find), "foo");
  wait_for_count(f);
  gtk_source_search_settings_set_case_sensitive(f->settings, TRUE);
  on_option(NULL, f);
  g_assert_true(gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end));
  selected = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
  g_assert_cmpstr(selected, ==, "foo");
  g_free(selected);
  free_tab(tab);
}

/* The selection on one line is what is looked for. */
static void test_find_selection(void)
{
  Tab *tab = new_tab("alpha beta alpha\n");

  select_lines(tab, 0, 6, 0, 10);
  findbar_show(tab, FALSE);
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(tab->view->find->find)), ==, "beta");
  wait_for_count(tab->view->find);
  g_assert_cmpstr(gtk_label_get_text(tab->view->find->count), ==, "1 of 1");
  free_tab(tab);
}

/* The buffer keeps a file's last line break (an empty last line): an unchanged file that ends in a
 * blank line has no marks, and a line break added at the end is an added line. */
static void test_trailing_newline(void)
{
  char *base = g_build_filename(g_get_tmp_dir(), "t-code-test-trailing.c", NULL);
  Tab *tab = new_tab("a\nb\n\n");
  gint64 until;

  g_assert_false(gtk_source_buffer_get_implicit_trailing_newline(tab->buffer));
  g_file_set_contents(base, "a\nb\n\n", -1, NULL);
  diff_gutter_compare_file(tab, base);
  for (until = g_get_monotonic_time() + G_USEC_PER_SEC; g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  for (guint i = 0; i < tab->view->diff->marks->len; i++)
    g_assert_cmpuint(g_array_index(tab->view->diff->marks, guint8, i), ==, LINE_SAME);
  g_remove(base);
  g_free(base);
  free_tab(tab);
}

/* Indent guides ------------------------------------------------------------------------------ */

/* Brackets in strings, characters and comments do not count. */
static void test_guide_brackets(void)
{
  GArray *found = brackets_of("f(\"(\", ')'); // {\n/* [ */ g[1]\n");
  GString *text = g_string_new(NULL);

  for (guint i = 0; i < found->len; i++)
    g_string_append_printf(text, "%c%d", g_array_index(found, Bracket, i).c, g_array_index(found, Bracket, i).line);
  g_assert_cmpstr(text->str, ==, "(0)0[1]1");
  g_string_free(text, TRUE);
  g_array_free(found, TRUE);
}

/* The block around offset in text (see enclosing_pair): "open-close", or "-" for none. */
static char *pair_at(const char *text, int offset)
{
  GArray *brackets = brackets_of(text);
  int open, close;
  char *pair = enclosing_pair(brackets, offset, &open, &close) ? g_strdup_printf("%d-%d", open, close) : g_strdup("-");

  g_array_free(brackets, TRUE);
  return pair;
}

static void check_pair(const char *text, const char *before_cursor, const char *expected)
{
  const char *at = strstr(text, before_cursor);
  char *pair;

  g_assert_nonnull(at);
  pair = pair_at(text, (int)g_utf8_pointer_to_offset(text, at + strlen(before_cursor)));
  g_assert_cmpstr(pair, ==, expected);
  g_free(pair);
}

/* The block around the cursor: the innermost one with lines between its brackets. */
static void test_guide_pair(void)
{
  const char *text = "void f(void)\n{\n  if (x) {\n    a();\n  }\n  b();\n}\n";

  check_pair(text, "  b", "1-6");
  check_pair(text, "    a", "2-4"); /* inside the if */
  check_pair(text, "vo", "-");      /* outside everything */
}

/* The cursor inside a comment or a string: the brackets in them still do not count, after it too. */
static void test_guide_pair_in_comment(void)
{
  const char *text = "int f()\n{\n  /* ( { */\n  s = \"(\";\n  x;\n}\n";

  check_pair(text, "/* (", "1-5");
  check_pair(text, "s = \"", "1-5");
  check_pair(text, "  x", "1-5");
}

/* Offsets count characters: a "ğ" before the cursor is one. */
static void test_guide_pair_utf8(void)
{
  check_pair("{ // ğğğ\n  a;\n\n}\n", "  a", "0-3");
}

/* A blank line gets the guides of the lines around it. */
static void test_guide_blank_lines(void)
{
  Tab *tab = new_tab("{\n    a;\n\n    b;\n        c;\n\n}\n");
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);

  g_assert_cmpint(guide_count(buffer, 1, 4), ==, 1);
  g_assert_cmpint(guide_count(buffer, 2, 4), ==, 1); /* between two lines of one level */
  g_assert_cmpint(guide_count(buffer, 4, 4), ==, 2);
  g_assert_cmpint(guide_count(buffer, 5, 4), ==, 1); /* where a block closes: the smaller */
  g_assert_cmpint(guide_count(buffer, 0, 4), ==, 0);
  free_tab(tab);
}

/* Diff gutter -------------------------------------------------------------------------------- */

/* The gutter compares with a file; typing changes the marks a moment later. */
static void test_diff_gutter(void)
{
  char *base = g_build_filename(g_get_tmp_dir(), "t-code-test-base.c", NULL);
  Tab *tab = new_tab("a\nb\nc\n");
  gint64 until;
  GtkTextIter start, end;

  g_file_set_contents(base, "a\nb\nc\n", -1, NULL);
  diff_gutter_compare_file(tab, base);
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &start, 1);
  end = start;
  gtk_text_iter_forward_char(&end);
  gtk_text_buffer_delete(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &start, "B", -1);       /* b -> B */
  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "new\n", -1); /* a line at the start */
  for (until = g_get_monotonic_time() + G_USEC_PER_SEC; g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_nonnull(tab->view->diff->marks);
  g_assert_cmpuint(g_array_index(tab->view->diff->marks, guint8, 0), ==, LINE_ADDED);
  g_assert_cmpuint(g_array_index(tab->view->diff->marks, guint8, 1), ==, LINE_SAME);
  g_assert_cmpuint(g_array_index(tab->view->diff->marks, guint8, 2), ==, LINE_MODIFIED);
  PalColor color; /* the strip beside the minimap: the same marks, in the gutter's colors */
  g_assert_cmpint(diff_gutter_line_total(tab), >=, 4);
  g_assert_true(diff_gutter_mark(tab, 0, &color));
  g_assert_cmpint(color, ==, PAL_DIFF_ADDED);
  g_assert_false(diff_gutter_mark(tab, 1, &color));
  g_assert_true(diff_gutter_mark(tab, 2, &color));
  g_assert_cmpint(color, ==, PAL_DIFF_MODIFIED);
  g_remove(base);
  g_free(base);
  free_tab(tab);
}

/* Parameter hint ----------------------------------------------------------------------------- */

/* The view's adjustments can outlive the editor (a comparison's other side scrolls them): once
 * the editor goes, scrolling them must not reach its freed hint. */
static void test_signature_outlived(void)
{
  Tab *tab = new_tab("int a;\n");
  GtkAdjustment *vertical = g_object_ref(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->source)));
  GtkAdjustment *horizontal = g_object_ref(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(tab->source)));

  g_assert_cmpuint(g_signal_handler_find(vertical, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_scrolled, NULL), !=, 0);
  free_tab(tab);
  g_assert_cmpuint(g_signal_handler_find(vertical, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_scrolled, NULL), ==, 0);
  g_assert_cmpuint(g_signal_handler_find(horizontal, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_scrolled, NULL), ==, 0);
  gtk_adjustment_set_value(vertical, 10);
  g_object_unref(vertical);
  g_object_unref(horizontal);
}

/* Side by side, the sides line up with gap rows: the new side's where lines were deleted (drawn
 * red), the old side's where lines were added (drawn black); changed lines and words are colored. */
static void test_comparison_gaps(void)
{
  Tab *new_side = new_tab("a\nadded\nb\n"), *old_side = new_tab("a\nb\nremoved\n");
  int new_px = 0, old_px = 0, above, below;
  gboolean new_red = FALSE, old_red = FALSE;

  diff_gutter_pair(new_side, old_side);
  for (int line = 0; line < 4; line++) {
    if (diff_gutter_line_gaps(new_side, line, &above, &below) && above + below > 0)
      new_red = TRUE;
    new_px += above + below;
    if (diff_gutter_line_gaps(old_side, line, &above, &below) && above + below > 0)
      old_red = TRUE;
    old_px += above + below;
  }
  g_assert_cmpint(new_px, >, 0); /* for "removed" */
  g_assert_cmpint(old_px, >, 0); /* for "added" */
  g_assert_cmpint(new_px, ==, old_px);
  g_assert_true(new_red);
  g_assert_false(old_red);

  /* Changed lines are tinted and their words colored (a tag); the same lines are not. */
  GtkTextIter at;
  GtkTextTag *words = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(new_side->buffer)),
                                                "tcode-changed-words");
  g_assert_false(diff_gutter_line_changed(new_side, 0));
  g_assert_true(diff_gutter_line_changed(new_side, 1)); /* "added" */
  g_assert_true(diff_gutter_line_changed(old_side, 2)); /* "removed" */
  g_assert_nonnull(words);
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(new_side->buffer), &at, 1, 2);
  g_assert_true(gtk_text_iter_has_tag(&at, words));
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(new_side->buffer), &at, 0);
  g_assert_false(gtk_text_iter_has_tag(&at, words));

  diff_gutter_pair(new_side, NULL); /* no comparison: no colors */
  g_assert_false(diff_gutter_line_changed(new_side, 1));
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(new_side->buffer), &at, 1, 2);
  g_assert_false(gtk_text_iter_has_tag(&at, words));
  free_tab(old_side);
  free_tab(new_side);
}

/* With word wrap, the gaps are worked out from each line's height as the view lays it out: also for
 * lines that just fit, or just do not (the view keeps a pixel for the cursor), else every line below
 * one of them was out of step with the other side. */
static void test_wrapped_heights(void)
{
  GString *text = g_string_new(NULL);
  GtkWidget *window = gtk_window_new();
  Tab *tab;

  for (int length = 1; length <= 60; length++) { /* lines of every length, wrapped anywhere */
    for (int i = 0; i < length; i++)
      g_string_append_c(text, 'x');
    g_string_append_c(text, '\n');
  }
  app.settings.word_wrap = TRUE;
  tab = new_tab(text->str);
  editor_view_apply_settings(tab);
  gtk_window_set_default_size(GTK_WINDOW(window), 400, 300);
  gtk_window_set_child(GTK_WINDOW(window), tab->page);
  gtk_window_present(GTK_WINDOW(window));
  for (gint64 until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
       !gtk_widget_get_mapped(GTK_WIDGET(tab->source)) && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, TRUE);
  run_for(300);
  if (!gtk_widget_get_mapped(GTK_WIDGET(tab->source)))
    g_test_skip("the window was not shown");
  /* A letter is ~9 pixels wide: one of ten widths in a row is where a line just fits, or not. */
  for (int margin = 2; margin < 12 && gtk_widget_get_mapped(GTK_WIDGET(tab->source)); margin++) {
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tab->source), margin);
    run_for(100);
    int wrap, *heights = line_heights(tab, text->str, &wrap);
    GtkAdjustment *vertical = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->source));
    for (int line = 0; line < 60; line++) {
      GtkTextIter iter;
      int y, h;
      gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &iter, line);
      gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(tab->source), &iter, &y, &h);
      gtk_adjustment_set_value(vertical, y); /* laid out for real, not estimated */
      run_for(10);
      gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(tab->source), &iter, &y, &h);
      g_assert_cmpint(h, ==, heights[line]);
    }
    g_assert_cmpint(heights[59], >, heights[0]); /* the longest line wraps */
    g_free(heights);
  }
  gtk_window_set_child(GTK_WINDOW(window), NULL); /* the tab keeps the page */
  gtk_window_destroy(GTK_WINDOW(window));
  free_tab(tab);
  app.settings.word_wrap = FALSE;
  g_string_free(text, TRUE);
}

static void line_yrange(Tab *tab, int line, int *y, int *height)
{
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &iter, line);
  gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(tab->source), &iter, y, height);
}

/* "h" for each line a comparison hides, "." for each shown one. */
static char *compared_lines(Tab *tab, int lines)
{
  GString *out = g_string_new(NULL);

  for (int line = 0; line < lines; line++)
    g_string_append_c(out, diff_gutter_line_hidden(tab, line) ? 'h' : '.');
  return g_string_free(out, FALSE);
}

static void check_compared_lines(Tab *tab, const char *expected)
{
  char *lines = compared_lines(tab, (int)strlen(expected));

  g_assert_cmpstr(lines, ==, expected);
  g_free(lines);
}

/* A comparison shows its changes with three lines around each: the other unchanged lines are hidden
 * on both sides alike (but the last), a row standing for each stretch, and the sides stay lined up.
 * The toggle, or the cursor going into hidden lines, shows them all. */
static void test_comparison_folds(void)
{
  GString *old_text = g_string_new(NULL), *new_text = g_string_new(NULL);
  const char *folded = "hhhhhhhhhhhh.......hhhhhhhhhh.", *shown = "..............................";
  GtkWidget *window = gtk_window_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  int row_px;

  for (int n = 0; n < 30; n++) {
    g_string_append_printf(old_text, "l%d\n", n);
    g_string_append_printf(new_text, n == 15 ? "changed\n" : "l%d\n", n);
  }
  Tab *new_side = new_tab(new_text->str), *old_side = new_tab(old_text->str);
  GtkTextIter at;
  diff_gutter_pair(new_side, old_side);
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(new_side->buffer), &at, 15); /* editor_goto is a stub here */
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(new_side->buffer), &at);
  check_compared_lines(new_side, folded);
  check_compared_lines(old_side, folded);
  g_assert_cmpint(diff_gutter_line_folded(new_side, 12, &row_px), ==, 12);
  g_assert_cmpint(row_px, >, 0);
  g_assert_cmpint(diff_gutter_line_folded(old_side, 29, &row_px), ==, 10);
  g_assert_cmpint(diff_gutter_line_folded(new_side, 13, &row_px), ==, 0);

  /* Laid out: hidden lines take no height, the row stands above the next line. */
  gtk_box_append(GTK_BOX(box), new_side->page);
  gtk_box_append(GTK_BOX(box), old_side->page);
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
  gtk_window_set_child(GTK_WINDOW(window), box);
  gtk_window_present(GTK_WINDOW(window));
  for (gint64 until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
       !gtk_widget_get_mapped(GTK_WIDGET(new_side->source)) && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, TRUE);
  run_for(400); /* the marks are made again too: nothing hidden changes */
  check_compared_lines(new_side, folded);
  if (gtk_widget_get_mapped(GTK_WIDGET(new_side->source))) {
    int y, height, line_px, new_y, old_y;
    diff_gutter_line_folded(new_side, 12, &row_px);
    line_yrange(new_side, 13, &y, &line_px);
    line_yrange(new_side, 12, &y, &height);
    g_assert_cmpint(y, ==, 0);
    g_assert_cmpint(height, ==, row_px + line_px);
    line_yrange(new_side, 15, &new_y, &height);
    line_yrange(old_side, 15, &old_y, &height);
    g_assert_cmpint(new_y, ==, row_px + 3 * line_px);
    g_assert_cmpint(old_y, ==, new_y);
    line_yrange(new_side, 29, &new_y, &height);
    line_yrange(old_side, 29, &old_y, &height);
    g_assert_cmpint(new_y, ==, row_px + 7 * line_px); /* its row starts there */
    g_assert_cmpint(old_y, ==, new_y);
  }

  diff_gutter_toggle_unchanged(old_side); /* either side's tab */
  check_compared_lines(new_side, shown);
  check_compared_lines(old_side, shown);
  diff_gutter_toggle_unchanged(new_side);
  check_compared_lines(old_side, folded);
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(old_side->buffer), &at, 3);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(old_side->buffer), &at);
  check_compared_lines(new_side, shown);

  diff_gutter_pair(new_side, NULL); /* no comparison: nothing hidden */
  diff_gutter_toggle_unchanged(new_side);
  check_compared_lines(new_side, shown);
  gtk_window_set_child(GTK_WINDOW(window), NULL); /* the tabs keep their pages */
  gtk_window_destroy(GTK_WINDOW(window));
  free_tab(old_side);
  free_tab(new_side);
  g_string_free(old_text, TRUE);
  g_string_free(new_text, TRUE);
}

/* Folding ---------------------------------------------------------------------------------- */

static void place_cursor(Tab *tab, int line)
{
  GtkTextIter at;

  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &at, line);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &at);
}

/* "h" for each folded-away line, "." for each shown one: "..hh.". */
static char *hidden_lines(Tab *tab)
{
  GString *out = g_string_new(NULL);

  for (int line = 0; line < gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(tab->buffer)); line++)
    g_string_append_c(out, line_hidden(tab->view->folding, line) ? 'h' : '.');
  return g_string_free(out, FALSE);
}

static void assert_hidden(Tab *tab, const char *want)
{
  char *now = hidden_lines(tab);

  g_assert_cmpstr(now, ==, want);
  g_free(now);
}

/* A block folds to its head and closing line, the head of an Allman block being the line above its
 * brace; the cursor going into it unfolds it, and so does deleting its brace. */
static void test_folding(void)
{
  Tab *tab = new_tab("void f()\n{\n  a();\n}\nint g() {\n  if (x) {\n    y();\n  }\n}\n");
  Folding *f = tab->view->folding;

  g_assert_nonnull(block_at(tab, 0));
  g_assert_null(block_at(tab, 1)); /* f's arrow is on its head, "void f()" */
  toggle(f, 0);
  assert_hidden(tab, ".hh.......");
  toggle(f, 0);
  assert_hidden(tab, "..........");

  place_cursor(tab, 6); /* in the if */
  fold_at_cursor(tab);
  assert_hidden(tab, "......h...");
  place_cursor(tab, 6); /* into it: it opens */
  assert_hidden(tab, "..........");

  fold_all(tab);
  assert_hidden(tab, ".hh..hhh..");
  unfold_all(tab);
  assert_hidden(tab, "..........");

  /* g's brace goes: its fold opens a moment later. */
  toggle(f, 4);
  assert_hidden(tab, ".....hhh..");
  GtkTextIter brace, after;
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &brace, 4, 8);
  after = brace;
  gtk_text_iter_forward_char(&after);
  gtk_text_buffer_delete(GTK_TEXT_BUFFER(tab->buffer), &brace, &after);
  tab->view->changes++; /* editor.c counts them in t-code */
  run_for(CHECK_MS + 200);
  assert_hidden(tab, "..........");
  free_tab(tab);
}

/* Sticky scroll: the heads of the blocks around the top line stay at the top, outermost first; a
 * click on one goes to it; the setting turns them off. */
static void test_sticky(void)
{
  GString *text = g_string_new("namespace app {\nint f()\n{\n  for (;;) {\n");
  GtkWidget *window = gtk_window_new();
  Tab *tab;

  for (int i = 0; i < 300; i++)
    g_string_append_printf(text, "    step(%d);\n", i);
  g_string_append(text, "  }\n}\n}\n");
  app.settings.sticky_scroll = TRUE;
  tab = new_tab(text->str);
  gtk_window_set_default_size(GTK_WINDOW(window), 600, 300);
  gtk_window_set_child(GTK_WINDOW(window), tab->page);
  gtk_window_present(GTK_WINDOW(window));
  for (gint64 until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
       !gtk_widget_get_mapped(GTK_WIDGET(tab->source)) && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, TRUE);
  run_for(300);
  if (!gtk_widget_get_mapped(GTK_WIDGET(tab->source))) {
    g_test_skip("the window was not shown");
  } else {
    Sticky *st = tab->view->sticky;
    GtkAdjustment *vertical = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->source));
    g_assert_cmpint(st->count, ==, 0); /* at the top: nothing is above */
    gtk_adjustment_set_value(vertical, 100 * st->row);
    run_for(200);
    g_assert_cmpint(st->count, ==, 3);
    g_assert_cmpint(st->lines[0], ==, 0); /* namespace app { */
    g_assert_cmpint(st->lines[1], ==, 1); /* int f(), its brace on the next line */
    g_assert_cmpint(st->lines[2], ==, 3); /* for (;;) { */
    g_assert_true(gtk_widget_get_visible(st->area));

    on_sticky_click(NULL, 1, 10, st->row * 1.5, st); /* the second one */
    run_for(200);
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &cursor,
                                     gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer)));
    g_assert_cmpint(gtk_text_iter_get_line(&cursor), ==, 1);

    gtk_adjustment_set_value(vertical, 100 * st->row);
    app.settings.sticky_scroll = FALSE;
    editor_view_apply_settings(tab);
    run_for(200);
    g_assert_cmpint(st->count, ==, 0);
    g_assert_false(gtk_widget_get_visible(st->area));
  }
  gtk_window_set_child(GTK_WINDOW(window), NULL); /* the tab keeps the page */
  gtk_window_destroy(GTK_WINDOW(window));
  free_tab(tab);
  g_string_free(text, TRUE);
}

/* The ruler: a line after the chosen column, none at 0. */
static void test_ruler(void)
{
  Tab *tab = new_tab("int a;\n");

  app.settings.ruler = 80;
  editor_view_apply_settings(tab);
  g_assert_true(gtk_source_view_get_show_right_margin(tab->source));
  g_assert_cmpuint(gtk_source_view_get_right_margin_position(tab->source), ==, 80);

  app.settings.ruler = 0;
  editor_view_apply_settings(tab);
  g_assert_false(gtk_source_view_get_show_right_margin(tab->source));
  free_tab(tab);
}

/* The indentation in use: the settings, then the file's own, the config files, the project's
 * choice; a Makefile keeps tabs. */
static void test_indent_order(void)
{
  Tab *tab = new_tab("int a;\n");
  EditorView *v = tab->view;

  editor_view_apply_settings(tab);
  g_assert_false(v->tabs);
  g_assert_cmpint(v->width, ==, 4);
  g_assert_cmpstr(v->indent_from, ==, "Settings");

  v->own_indent = TRUE;
  v->own_tabs = FALSE;
  v->own_width = 2;
  editor_view_apply_settings(tab);
  g_assert_cmpint(v->width, ==, 2);

  v->config = (IndentStyle){ 1, 0 }; /* .editorconfig says tabs only: the file's width stays */
  v->config_from = ".editorconfig";
  editor_view_apply_settings(tab);
  g_assert_true(v->tabs);
  g_assert_cmpint(v->width, ==, 2);
  g_assert_cmpstr(v->indent_from, ==, ".editorconfig");
  g_assert_false(gtk_source_view_get_insert_spaces_instead_of_tabs(tab->source));

  app.project.indent.tabs = FALSE;
  app.project.indent.width = 8;
  editor_view_apply_settings(tab);
  g_assert_false(v->tabs);
  g_assert_cmpint(gtk_source_view_get_tab_width(tab->source), ==, 8);
  g_assert_true(gtk_source_view_get_insert_spaces_instead_of_tabs(tab->source));

  gtk_source_buffer_set_language(
      tab->buffer, gtk_source_language_manager_get_language(gtk_source_language_manager_get_default(), "makefile"));
  editor_view_apply_settings(tab);
  g_assert_true(v->tabs);
  g_assert_cmpint(v->width, ==, 8);

  app.project.indent.width = 0;
  free_tab(tab);
}

/* The breakpoints in path as "3,5 if i == 2" ("" for none). */
static char *breakpoints_of(const char *path)
{
  GArray *list = breakpoints_in(&app, path);
  GString *text = g_string_new("");

  for (guint i = 0; list && i < list->len; i++) {
    Breakpoint *b = &g_array_index(list, Breakpoint, i);
    g_string_append_printf(text, "%s%d", i ? "," : "", b->line);
    if (b->condition)
      g_string_append_printf(text, " if %s", b->condition);
  }
  return g_string_free(text, FALSE);
}

static void assert_breakpoints(const char *expected)
{
  char *lines = breakpoints_of("/test/a.c");

  g_assert_cmpstr(lines, ==, expected);
  g_free(lines);
}

/* Inserts text at the start (column 0) or the end of line (0-based). */
static void insert_at_line(Tab *tab, int line, gboolean at_end, const char *text)
{
  GtkTextIter at;

  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &at, line);
  if (at_end)
    gtk_text_iter_forward_to_line_end(&at);
  gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &at, text, -1);
}

/* A breakpoint moves with its line's text, two that come onto one line become one, and the table
 * every file's breakpoints are kept in follows. */
static void test_breakpoints(void)
{
  Tab *tab = new_tab("int a;\nint b;\nint c;\nint d;\n");

  breakpoint_toggle(tab, 1);
  breakpoint_toggle(tab, 3);
  assert_breakpoints("2,4");
  insert_at_line(tab, 1, FALSE, "\n"); /* Enter at the start of line 2: it goes down with its text */
  assert_breakpoints("3,5");
  insert_at_line(tab, 2, TRUE, "\n"); /* Enter at its end: it stays */
  assert_breakpoints("3,6");
  breakpoint_toggle(tab, 2); /* off again */
  assert_breakpoints("6");
  breakpoint_toggle(tab, 4);
  assert_breakpoints("5,6");
  GtkTextIter start, end; /* the line break between 5 and 6 goes: one breakpoint is left */
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &start, 4);
  gtk_text_iter_forward_to_line_end(&start);
  end = start;
  gtk_text_iter_forward_char(&end);
  gtk_text_buffer_delete(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  assert_breakpoints("5");
  breakpoint_toggle(tab, 4);
  assert_breakpoints("");
  g_assert_null(breakpoints_in(&app, "/test/a.c"));
  free_tab(tab);
}

/* The session keeps them as "line:path"; a file's breakpoints come back when it is opened (loaded). */
static void test_breakpoints_saved(void)
{
  Tab *tab = new_tab("one\ntwo\nthree\n");
  char **saved;

  char **conditions = NULL;

  breakpoint_toggle(tab, 2);
  breakpoint_set_condition(tab, 0, "i == 2");
  saved = breakpoints_save(&app, &conditions);
  g_assert_cmpstrv(saved, ((const char *[]){ "1:/test/a.c", "3:/test/a.c", NULL }));
  g_assert_cmpstrv(conditions, ((const char *[]){ "i == 2", "", NULL }));
  g_strfreev(conditions);
  breakpoints_remove_all(&app);
  assert_breakpoints("");
  g_assert_null(breakpoints_save(&app, &conditions));
  g_strfreev(conditions);
  free_tab(tab);

  char *again[] = { "2:/test/a.c", "1:/test/a.c", "x:/test/a.c", "4:relative.c", NULL };
  char *ifs[] = { "", "n > 1", "", "", NULL };
  breakpoints_restore(&app, again, ifs);
  assert_breakpoints("1 if n > 1,2");
  tab = new_tab("one\ntwo\nthree\n");
  breakpoints_loaded(tab); /* as editor.c does once the file is in */
  GSList *marks = marks_on(tab, 1);
  g_assert_nonnull(marks);
  g_slist_free(marks);
  g_assert_cmpstr(breakpoint_condition(tab, 0), ==, "n > 1");
  insert_at_line(tab, 0, FALSE, "zero\n");
  assert_breakpoints("2 if n > 1,3"); /* the condition goes with its line */
  breakpoints_remove_all(&app);
  g_strfreev(saved);
  free_tab(tab);
}

/* A condition is set on a line with no breakpoint (one is made) or taken off (it stays, stopping
 * always); the debugger moves one to the line it really stops at, or takes one away, also in a file
 * that is not open. */
static void test_breakpoint_conditions(void)
{
  Tab *tab = new_tab("int a;\nint b;\nint c;\n");

  breakpoint_set_condition(tab, 1, "a > 0");
  assert_breakpoints("2 if a > 0");
  breakpoint_set_condition(tab, 1, NULL);
  assert_breakpoints("2");
  g_assert_null(breakpoint_condition(tab, 1));
  breakpoint_toggle(tab, 1);
  assert_breakpoints("");
  free_tab(tab);

  char *saved[] = { "3:/test/b.c", "5:/test/b.c", NULL };
  char *ifs[] = { "x", "", NULL };
  breakpoints_restore(&app, saved, ifs);
  breakpoint_move(&app, "/test/b.c", 3, 4); /* gdb: line 3 has no code, it stops at 4 */
  char *now = breakpoints_of("/test/b.c");
  g_assert_cmpstr(now, ==, "4 if x,5");
  g_free(now);
  breakpoint_move(&app, "/test/b.c", 4, 5); /* onto another one: one is left */
  now = breakpoints_of("/test/b.c");
  g_assert_cmpstr(now, ==, "5");
  g_free(now);
  breakpoint_remove(&app, "/test/b.c", 5);
  g_assert_null(breakpoints_in(&app, "/test/b.c"));
}

/* The line the program stopped at is marked once the file is in; read-only old versions take no
 * breakpoints. */
static void test_breakpoints_stop(void)
{
  Tab *tab = new_tab("one\ntwo\nthree\n");

  app.debug.stop_path = g_strdup("/test/a.c");
  app.debug.stop_line = 2;
  breakpoints_loaded(tab);
  g_assert_cmpint(stop_line(tab), ==, 1);
  insert_at_line(tab, 0, FALSE, "zero\n"); /* it moves with the text */
  g_assert_cmpint(stop_line(tab), ==, 2);
  breakpoints_show_stop(&app, NULL, 0); /* running again: no tab gets it (none is in a group here) */
  g_assert_null(app.debug.stop_path);
  breakpoints_loaded(tab);
  g_assert_cmpint(stop_line(tab), ==, -1);

  tab->view->readonly = TRUE;
  breakpoint_toggle(tab, 0);
  assert_breakpoints("");
  free_tab(tab);
}

int main(int argc, char **argv)
{
  int status;

  g_test_init(&argc, &argv, NULL);
  start_broadway();
  if (!gtk_init_check()) {
    g_test_skip("no display");
    return g_test_run();
  }
  gtk_source_init();
  app.settings.tab_size = 4;
  app.settings.tabs_to_spaces = TRUE;
  app.font_scale = 1.0;
  app.paths.styles = g_canonicalize_filename("styles", NULL); /* run from the repository (make test) */
  g_test_add_func("/editing/comment-lines", test_comment_lines);
  g_test_add_func("/editing/breakpoints", test_breakpoints);
  g_test_add_func("/editing/breakpoints-saved", test_breakpoints_saved);
  g_test_add_func("/editing/breakpoints-stop", test_breakpoints_stop);
  g_test_add_func("/editing/breakpoint-conditions", test_breakpoint_conditions);
  g_test_add_func("/editing/enter-after-open", test_enter_after_open);
  g_test_add_func("/editing/double-click", test_double_click);
  g_test_add_func("/editing/occurrences", test_occurrences);
  g_test_add_func("/editing/strip-follows-map", test_strip_follows_map);
  g_test_add_func("/editing/indent-order", test_indent_order);
  g_test_add_func("/editing/comment-indented", test_comment_indented);
  g_test_add_func("/editing/block-comment", test_block_comment);
  g_test_add_func("/editing/duplicate", test_duplicate);
  g_test_add_func("/editing/delete-line", test_delete_line);
  g_test_add_func("/editing/undo-redo", test_undo_redo);
  g_test_add_func("/editing/brackets", test_brackets);
  g_test_add_func("/editing/closer-not-ours", test_closer_not_ours);
  g_test_add_func("/editing/no-pair", test_no_pair);
  g_test_add_func("/editing/backspace-pair", test_backspace_pair);
  g_test_add_func("/editing/wrap-selection", test_wrap_selection);
  g_test_add_func("/editing/enter-between-braces", test_enter_between_braces);
  g_test_add_func("/editing/outdent-brace", test_outdent_brace);
  g_test_add_func("/editing/no-pair-in-comment", test_no_pair_in_comment);
  g_test_add_func("/editing/snippet-tab", test_snippet_tab);
  g_test_add_func("/find/next-previous", test_find);
  g_test_add_func("/find/replace", test_replace);
  g_test_add_func("/find/selection", test_find_selection);
  g_test_add_func("/find/again", test_find_again);
  g_test_add_func("/find/option", test_find_option);
  g_test_add_func("/find/big-file", test_find_big_file);
  g_test_add_func("/diff-gutter/marks", test_diff_gutter);
  g_test_add_func("/diff-gutter/trailing-newline", test_trailing_newline);
  g_test_add_func("/diff-gutter/comparison-gaps", test_comparison_gaps);
  g_test_add_func("/diff-gutter/comparison-folds", test_comparison_folds);
  g_test_add_func("/diff-gutter/wrapped-heights", test_wrapped_heights);
  g_test_add_func("/folding/fold-unfold", test_folding);
  g_test_add_func("/sticky/heads", test_sticky);
  g_test_add_func("/settings/ruler", test_ruler);
  g_test_add_func("/guides/brackets", test_guide_brackets);
  g_test_add_func("/guides/pair", test_guide_pair);
  g_test_add_func("/guides/pair-in-comment", test_guide_pair_in_comment);
  g_test_add_func("/guides/pair-utf8", test_guide_pair_utf8);
  g_test_add_func("/guides/blank-lines", test_guide_blank_lines);
  g_test_add_func("/signature/outlived", test_signature_outlived);
  status = g_test_run();
  stop_broadway();
  return status;
}
