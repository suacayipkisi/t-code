/* Tests for the Search panel (src/search/search.c) as the user drives it: typing, the options, results
 * grouped by file, folding, opening a result, the result limit and switching projects. Needs a
 * display; skipped without one. */
#include "search/search.h"
#include "util.h"
#include "widgets.h"

#include <glib/gstdio.h>
#include <string.h>

/* What the panel opened last (open_file_at stands in for editor.c). */
static struct {
  char *path;
  int line, column;
} opened;

void open_file_at(App *t G_GNUC_UNUSED, const char *path, int line, int column)
{
  g_free(opened.path);
  opened.path = g_strdup(path);
  opened.line = line;
  opened.column = column;
}

/* clang-format off */
void open_file(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
void open_to_side(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
void show_sidebar_page(App *t G_GNUC_UNUSED, const char *page G_GNUC_UNUSED) {}
void files_reveal(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
Tab *find_tab(App *t G_GNUC_UNUSED, TabKind kind G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) { return NULL; }
/* clang-format on */

static char *said; /* status_say's last message */

void status_say(App *t G_GNUC_UNUSED, const char *message)
{
  g_free(said);
  said = g_strdup(message);
}

static App app;
static GtkWidget *panel;
static char *tmp, *other;

static GtkEditable *entry(void)
{
  return GTK_EDITABLE(find_widget(panel, GTK_TYPE_ENTRY, NULL, NULL));
}

static GtkListBox *list(void)
{
  return GTK_LIST_BOX(find_widget(panel, GTK_TYPE_LIST_BOX, NULL, NULL));
}

static const char *summary(void)
{
  return gtk_label_get_text(GTK_LABEL(find_widget(panel, GTK_TYPE_LABEL, NULL, "summary")));
}

static GtkToggleButton *option(const char *tooltip)
{
  return GTK_TOGGLE_BUTTON(find_widget(panel, GTK_TYPE_TOGGLE_BUTTON, tooltip, NULL));
}

/* Runs the main loop until the summary is final (not empty, not "Searching…") or 10s pass. */
static void wait_for_results(void)
{
  gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

  /* The debounce first: a search starts once typing pauses. */
  for (gint64 pause = g_get_monotonic_time() + 400 * 1000; g_get_monotonic_time() < pause;)
    g_main_context_iteration(NULL, FALSE);
  while (g_get_monotonic_time() < until && (!*summary() || strcmp(summary(), "Searching…") == 0))
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpint(g_get_monotonic_time(), <, until);
}

/* Types the text and presses Enter. */
static void search_for(const char *text)
{
  gtk_editable_set_text(entry(), text);
  g_signal_emit_by_name(entry(), "activate");
  wait_for_results();
}

static int rows(gboolean headings, gboolean visible_only)
{
  int n = 0;

  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(list())); row; row = gtk_widget_get_next_sibling(row))
    if (gtk_widget_has_css_class(row, "heading") == headings && (!visible_only || gtk_widget_get_visible(row)))
      n++;
  return n;
}

static GtkListBoxRow *nth_row(int index)
{
  return gtk_list_box_get_row_at_index(list(), index);
}

/* The text of a row's labels, joined with spaces. */
static void row_text(GtkWidget *widget, GString *out)
{
  if (GTK_IS_LABEL(widget) && *gtk_label_get_text(GTK_LABEL(widget)))
    g_string_append_printf(out, "%s%s", out->len ? " " : "", gtk_label_get_text(GTK_LABEL(widget)));
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    row_text(child, out);
}

static char *text_of(GtkListBoxRow *row)
{
  GString *out = g_string_new(NULL);

  row_text(GTK_WIDGET(row), out);
  return g_string_free(out, FALSE);
}

static void reset_options(void)
{
  gtk_toggle_button_set_active(option("Match Case"), FALSE);
  gtk_toggle_button_set_active(option("Match Whole Word"), FALSE);
  gtk_toggle_button_set_active(option("Use Regular Expression"), FALSE);
}

static void test_results(void)
{
  char *text;

  reset_options();
  search_for("foo");
  g_assert_cmpstr(summary(), ==, "4 results in 2 files");
  g_assert_cmpint(rows(TRUE, FALSE), ==, 2);
  g_assert_cmpint(rows(FALSE, FALSE), ==, 4);

  /* Sorted by path: a.c first, with its folder empty and one result. */
  text = text_of(nth_row(0));
  g_assert_cmpstr(text, ==, "▾ a.c 1");
  g_free(text);
  text = text_of(nth_row(1));
  g_assert_cmpstr(text, ==, "return foo();");
  g_free(text);
  text = text_of(nth_row(2));
  g_assert_cmpstr(text, ==, "▾ b.h sub 3");
  g_free(text);
}

static void test_open(void)
{
  char *expected = g_build_filename(tmp, "a.c", NULL);

  reset_options();
  search_for("foo");
  g_signal_emit_by_name(list(), "row-activated", nth_row(1));
  g_assert_cmpstr(opened.path, ==, expected);
  g_assert_cmpint(opened.line, ==, 3);
  g_assert_cmpint(opened.column, ==, 12);

  /* sub/b.h "Foo foo FOO": the third match. */
  g_signal_emit_by_name(list(), "row-activated", nth_row(5));
  g_assert_true(g_str_has_suffix(opened.path, "/sub/b.h"));
  g_assert_cmpint(opened.line, ==, 1);
  g_assert_cmpint(opened.column, ==, 9);
  g_free(expected);
}

static void test_fold(void)
{
  reset_options();
  search_for("foo");
  g_signal_emit_by_name(list(), "row-activated", nth_row(2)); /* sub/b.h */
  g_assert_cmpint(rows(FALSE, TRUE), ==, 1);
  g_signal_emit_by_name(list(), "row-activated", nth_row(2));
  g_assert_cmpint(rows(FALSE, TRUE), ==, 4);

  /* Collapse All folds both files, pressed again unfolds them. */
  GtkWidget *fold = find_widget(panel, GTK_TYPE_BUTTON, "Collapse / Expand All", NULL);
  g_signal_emit_by_name(fold, "clicked");
  g_assert_cmpint(rows(FALSE, TRUE), ==, 0);
  g_signal_emit_by_name(fold, "clicked");
  g_assert_cmpint(rows(FALSE, TRUE), ==, 4);
}

static void test_options(void)
{
  reset_options();
  gtk_toggle_button_set_active(option("Match Case"), TRUE);
  search_for("foo");
  g_assert_cmpstr(summary(), ==, "2 results in 2 files");

  reset_options();
  gtk_toggle_button_set_active(option("Match Whole Word"), TRUE);
  search_for("fo");
  g_assert_cmpstr(summary(), ==, "No results found.");

  reset_options();
  gtk_toggle_button_set_active(option("Use Regular Expression"), TRUE);
  search_for("F[o]+ f");
  g_assert_cmpstr(summary(), ==, "1 result in 1 file");

  search_for("foo(");
  g_assert_cmpstr(summary(), ==, "Invalid regular expression");
  g_assert_cmpint(rows(TRUE, FALSE), ==, 0);
}

static void test_include_exclude(void)
{
  GtkWidget *details = find_widget(panel, GTK_TYPE_TOGGLE_BUTTON, "Toggle Search Details", NULL);
  GtkWidget *include, *exclude;

  reset_options();
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(details), TRUE);
  /* The include and exclude entries follow the row of ⇄ and ⋯. */
  GtkWidget *box = gtk_widget_get_next_sibling(gtk_widget_get_parent(details));
  include = find_widget(box, GTK_TYPE_ENTRY, NULL, NULL);
  exclude = find_widget(gtk_widget_get_last_child(box), GTK_TYPE_ENTRY, NULL, NULL);
  g_assert_true(include != exclude);

  gtk_editable_set_text(GTK_EDITABLE(include), "*.h");
  search_for("foo");
  g_assert_cmpstr(summary(), ==, "3 results in 1 file");
  gtk_editable_set_text(GTK_EDITABLE(include), "");
  gtk_editable_set_text(GTK_EDITABLE(exclude), "sub");
  wait_for_results();
  g_assert_cmpstr(summary(), ==, "1 result in 1 file");
  gtk_editable_set_text(GTK_EDITABLE(exclude), "");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(details), FALSE);
}

static void test_clear(void)
{
  reset_options();
  search_for("foo");
  g_signal_emit_by_name(find_widget(panel, GTK_TYPE_BUTTON, "Clear Search Results", NULL), "clicked");
  g_assert_cmpstr(gtk_editable_get_text(entry()), ==, "");
  g_assert_cmpstr(summary(), ==, "");
  g_assert_null(nth_row(0));
}

static void test_limit(void)
{
  reset_options();
  gtk_editable_set_text(entry(), "many");
  g_signal_emit_by_name(entry(), "activate");

  gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (!strstr(summary(), "search stopped there") && g_get_monotonic_time() < until) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
  g_assert_cmpstr(summary(), ==, "2000 results in 1 file (search stopped there)");
  g_assert_cmpint(rows(FALSE, FALSE), ==, 2000);
}

static void test_project_switch(void)
{
  reset_options();
  search_for("foo");
  g_assert_cmpstr(summary(), ==, "4 results in 2 files");
  app.project.root = other;
  search_reset();
  wait_for_results();
  g_assert_cmpstr(summary(), ==, "1 result in 1 file");
  app.project.root = tmp;
  search_reset();
  wait_for_results();
}

/* A search started while another runs replaces it: only the last one's results stay. */
static void test_replace_running(void)
{
  reset_options();
  gtk_editable_set_text(entry(), "many");
  g_signal_emit_by_name(entry(), "activate");
  search_for("foo");
  g_assert_cmpstr(summary(), ==, "4 results in 2 files");
  g_assert_cmpint(rows(FALSE, FALSE), ==, 4);
}

/* The entry with this placeholder under root. */
static GtkEditable *entry_with(GtkWidget *root, const char *placeholder)
{
  if (GTK_IS_ENTRY(root) && g_strcmp0(gtk_entry_get_placeholder_text(GTK_ENTRY(root)), placeholder) == 0)
    return GTK_EDITABLE(root);
  for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
    GtkEditable *found = entry_with(child, placeholder);
    if (found)
      return found;
  }
  return NULL;
}

/* ⇄: the results show each match struck out with its replacement after it; Replace All writes the
 * files (keeping their permissions), says how many, and the search runs again. */
static void test_replace_all(void)
{
  char *path = g_build_filename(tmp, "r", "rep.sh", NULL), *text, *row;
  gint64 until;
  GStatBuf info;

  reset_options();
  gtk_toggle_button_set_active(option("Use Regular Expression"), TRUE);
  search_for("ze(ta)");
  g_assert_cmpstr(summary(), ==, "2 results in 1 file");
  gtk_toggle_button_set_active(option("Toggle Replace"), TRUE);
  gtk_editable_set_text(entry_with(panel, "Replace"), "<$1>");
  row = text_of(nth_row(1));
  g_assert_nonnull(strstr(row, "zeta<ta>")); /* the match, struck out, then what replaces it */
  g_free(row);

  search_replace_run();
  until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (g_get_monotonic_time() < until && g_strcmp0(said, "Replaced 2 occurrences in 1 file") != 0)
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpstr(said, ==, "Replaced 2 occurrences in 1 file");
  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  g_assert_cmpstr(text, ==, "echo <ta> beta <ta>\n");
  g_assert_cmpint(g_stat(path, &info), ==, 0);
  g_assert_true(info.st_mode & S_IXUSR); /* still executable */
  wait_for_results();
  g_assert_cmpstr(summary(), ==, "No results found.");
  gtk_toggle_button_set_active(option("Toggle Replace"), FALSE);
  g_free(text);
  g_free(path);
}

static void write_file(const char *dir, const char *rel, const char *contents)
{
  g_free(write_at(dir, rel, contents, -1, 0));
}

int main(int argc, char **argv)
{
  GString *many = g_string_new(NULL);
  GtkWidget *window;
  int status;

  g_test_init(&argc, &argv, NULL);
  if (!gtk_init_check()) {
    g_test_skip("no display");
    return g_test_run();
  }
  tmp = g_dir_make_tmp("t-code-panel-XXXXXX", NULL);
  other = g_dir_make_tmp("t-code-panel-XXXXXX", NULL);
  write_file(tmp, "a.c", "int main(void)\n{\n    return foo();\n}\n");
  write_file(tmp, "sub/b.h", "Foo foo FOO\n");
  for (int i = 0; i < 2500; i++)
    g_string_append(many, "many\n");
  write_file(tmp, "z/many.txt", many->str);
  write_file(other, "x.c", "foo\n");
  write_file(tmp, "r/rep.sh", "echo zeta beta zeta\n");
  char *script = g_build_filename(tmp, "r", "rep.sh", NULL);
  g_chmod(script, 0755);
  g_free(script);
  g_string_free(many, TRUE);

  app.project.root = tmp;
  panel = search_panel(&app);
  window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), panel);
  gtk_window_present(GTK_WINDOW(window));

  g_test_add_func("/search-panel/results", test_results);
  g_test_add_func("/search-panel/open", test_open);
  g_test_add_func("/search-panel/fold", test_fold);
  g_test_add_func("/search-panel/options", test_options);
  g_test_add_func("/search-panel/include-exclude", test_include_exclude);
  g_test_add_func("/search-panel/clear", test_clear);
  g_test_add_func("/search-panel/limit", test_limit);
  g_test_add_func("/search-panel/project-switch", test_project_switch);
  g_test_add_func("/search-panel/replace-running", test_replace_running);
  g_test_add_func("/search-panel/replace-all", test_replace_all);
  status = g_test_run();

  gtk_window_destroy(GTK_WINDOW(window));
  char *rm[] = { "rm", "-rf", tmp, other, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(tmp);
  g_free(other);
  return status;
}
