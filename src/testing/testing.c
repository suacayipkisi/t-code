/*
 * The Testing panel (see testing.h).
 */
#define _DEFAULT_SOURCE /* kill(), setpgid() */
#include "testing/testing.h"

#include "testing/state.h"

#include "debug/debugger.h"
#include "editor/editor.h"
#include "project/langenv.h"
#include "project/toolchain.h"
#include "testing/frameworks.h"
#include "window/palette.h"

#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INDENT_PX 16 /* a test under its group */

/* A command that lists or runs one framework's tests. */
typedef struct {
  TestKind kind;
  char *dir;
  char **argv;
  char *report; /* the JUnit file it writes, or NULL */
  gboolean listing;
  char *debug;        /* a build for debugging the test of this id, or NULL */
  GPtrArray *pending; /* a GoogleTest program's listing: CTest's list (TestCase), merged once each is listed */
  guint expand;       /* ... and the index of the test whose cases it lists */
  gboolean stopped;   /* by Stop: its tests have no result */
  guint generation;   /* testing.generation when it was made */
  GSubprocess *proc;
} Job;

static struct {
  App *app;
  GtkWidget *panel; /* NULL once the window is gone */
  GtkListBox *list;
  GtkLabel *summary, *note;
  GtkTextView *output;
  GtkWidget *run_all, *run_failed, *stop, *refresh;
  GPtrArray *sources;       /* TestSource; NULL until listed */
  GPtrArray *tests;         /* TestCase, in the frameworks' order */
  char *logs[TEST_KINDS];   /* each framework's last command's output */
  char *errors[TEST_KINDS]; /* why it listed nothing, or NULL */
  GHashTable *collapsed;    /* group keys of closed groups */
  char *selected;           /* the selected row's key */
  GQueue waiting;           /* Job */
  Job *job;                 /* the one running */
  guint generation;         /* the project changed: older jobs' answers are dropped */
  gboolean listed;          /* for this project */
  char *dir;                /* reports go here */
  guint reports;
  char *debugged; /* the id of the test last debugged: Restart debugs it again */
  guint finding;  /* looking for a test's source: its generation */
} testing;

/* Keys and marks --------------------------------------------------------------------------- */

static char *test_key(const TestCase *test)
{
  return g_strdup_printf("t%d\t%s", test->kind, test->id);
}

static char *group_key(TestKind kind, const char *group)
{
  return g_strdup_printf("g%d\t%s", kind, group);
}

static const char *state_mark(TestState state)
{
  switch (state) {
    case TEST_RUNNING: return "…";
    case TEST_PASSED: return "✓";
    case TEST_FAILED: return "✗";
    case TEST_SKIPPED: return "-";
    default: return "○";
  }
}

static const char *state_class(TestState state)
{
  switch (state) {
    case TEST_RUNNING: return "running";
    case TEST_PASSED: return "passed";
    case TEST_FAILED: return "failed";
    default: return "idle";
  }
}

/* A group's state: running while one runs, failed when one failed, passed when one passed and
 * none is left unrun. */
static TestState group_state(TestKind kind, const char *group)
{
  gboolean running = FALSE, failed = FALSE, passed = FALSE, unrun = FALSE;

  for (guint i = 0; i < testing.tests->len; i++) {
    TestCase *test = testing.tests->pdata[i];
    if (test->kind != kind || strcmp(test->group, group) != 0)
      continue;
    running |= test->state == TEST_RUNNING;
    failed |= test->state == TEST_FAILED;
    passed |= test->state == TEST_PASSED;
    unrun |= test->state == TEST_UNKNOWN;
  }
  return running ? TEST_RUNNING : failed ? TEST_FAILED : passed && !unrun ? TEST_PASSED : TEST_UNKNOWN;
}

static TestCase *find_test(const char *key)
{
  for (guint i = 0; key && i < testing.tests->len; i++) {
    char *own = test_key(testing.tests->pdata[i]);
    gboolean same = strcmp(own, key) == 0;
    g_free(own);
    if (same)
      return testing.tests->pdata[i];
  }
  return NULL;
}

static TestCase *find_test_id(const char *id)
{
  for (guint i = 0; testing.tests && i < testing.tests->len; i++)
    if (strcmp(((TestCase *)testing.tests->pdata[i])->id, id) == 0)
      return testing.tests->pdata[i];
  return NULL;
}

static const TestSource *source_of(TestKind kind)
{
  for (guint i = 0; testing.sources && i < testing.sources->len; i++)
    if (((TestSource *)testing.sources->pdata[i])->kind == kind)
      return testing.sources->pdata[i];
  return NULL;
}

gboolean testing_busy(void)
{
  return testing.job || !g_queue_is_empty(&testing.waiting);
}

/* The output view ------------------------------------------------------------------------------ */

/* What the selected row's output is. */
static char *output_of(const char *key)
{
  if (!key)
    return g_strdup(testing.tests && testing.tests->len ? "Select a test for its output." : "");
  if (key[0] == 't') {
    TestCase *test = find_test(key);
    if (!test)
      return g_strdup("");
    if (test->output && *test->output)
      return g_strdup(test->output);
    return g_strdup(test->state == TEST_RUNNING   ? "Running…"
                    : test->state == TEST_PASSED  ? "Passed (no output)."
                    : test->state == TEST_FAILED  ? "Failed (no output)."
                    : test->state == TEST_SKIPPED ? "Skipped."
                                                  : "Not run yet: ▶ runs it.");
  }
  int kind = atoi(key + 1);
  if (kind < 0 || kind >= TEST_KINDS)
    return g_strdup("");
  if (key[0] == 'e')
    return g_strconcat(testing.errors[kind] ? testing.errors[kind] : "", testing.logs[kind] ? "\n\n" : "",
                       testing.logs[kind] ? testing.logs[kind] : "", NULL);
  return g_strdup(testing.logs[kind] ? testing.logs[kind] : ""); /* a group: its framework's last command */
}

static void show_output(void)
{
  char *text = output_of(testing.selected);

  if (testing.output)
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(testing.output), text, -1);
  g_free(text);
}

char *testing_output_text(void)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(testing.output);
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

/* The tree ------------------------------------------------------------------------------------- */

static void run_keys(GPtrArray *keys);

/* The debug button on a CTest test's row. */
static void on_row_debug(GtkButton *button, gpointer data G_GNUC_UNUSED)
{
  TestCase *test = find_test(g_object_get_data(G_OBJECT(button), "key"));

  if (test)
    testing_debug_one(test->id);
}

/* ▶ on a row: its test, or its group's tests. */
static void on_row_run(GtkButton *button, gpointer data G_GNUC_UNUSED)
{
  const char *key = g_object_get_data(G_OBJECT(button), "key");
  GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);

  if (key[0] == 't') {
    g_ptr_array_add(keys, g_strdup(key));
  } else {
    for (guint i = 0; i < testing.tests->len; i++) {
      TestCase *test = testing.tests->pdata[i];
      char *own = group_key(test->kind, test->group);
      if (strcmp(own, key) == 0)
        g_ptr_array_add(keys, test_key(test));
      g_free(own);
    }
  }
  run_keys(keys);
}

static GtkWidget *row_button(const char *key, const char *icon, const char *tooltip, GCallback clicked)
{
  GtkWidget *button = gtk_button_new_from_icon_name(icon);

  gtk_widget_add_css_class(button, "run");
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_widget_set_sensitive(button, !testing_busy());
  g_object_set_data_full(G_OBJECT(button), "key", g_strdup(key), g_free);
  g_signal_connect(button, "clicked", clicked, NULL);
  return button;
}

static GtkWidget *row_new(const char *key, const char *arrow, TestState state, const char *name, const char *dim,
                          gboolean runs, gboolean debugs)
{
  GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *mark = gtk_label_new(state_mark(state)), *label = gtk_label_new(name);

  if (arrow) {
    GtkWidget *expander = gtk_label_new(arrow);
    gtk_widget_add_css_class(expander, "arrow");
    gtk_box_append(GTK_BOX(box), expander);
    g_object_set_data(G_OBJECT(row), "arrow", expander);
  } else if (key[0] == 't') {
    gtk_widget_set_margin_start(box, INDENT_PX);
  }
  gtk_widget_add_css_class(mark, "mark");
  gtk_widget_add_css_class(mark, state_class(state));
  gtk_box_append(GTK_BOX(box), mark);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_tooltip_text(label, name);
  gtk_box_append(GTK_BOX(box), label);
  if (dim) {
    GtkWidget *extra = gtk_label_new(dim);
    gtk_widget_add_css_class(extra, "dim");
    gtk_label_set_ellipsize(GTK_LABEL(extra), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), extra);
  }
  gtk_widget_set_hexpand(label, TRUE);
  if (debugs)
    gtk_box_append(GTK_BOX(box), row_button(key, "tcode-debug-symbolic", "Debug Test", G_CALLBACK(on_row_debug)));
  if (runs)
    gtk_box_append(GTK_BOX(box), row_button(key, "tcode-run-symbolic", key[0] == 't' ? "Run Test" : "Run Tests",
                                            G_CALLBACK(on_row_run)));
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data_full(G_OBJECT(row), "key", g_strdup(key), g_free);
  return row;
}

/* A test row is hidden while its group is closed. */
static gboolean row_shown(GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  const char *group = g_object_get_data(G_OBJECT(row), "group");

  return !group || !g_hash_table_contains(testing.collapsed, group);
}

static void update_buttons(void)
{
  gboolean busy = testing_busy(), any = testing.tests && testing.tests->len, failed = FALSE;

  for (guint i = 0; any && i < testing.tests->len; i++)
    failed |= ((TestCase *)testing.tests->pdata[i])->state == TEST_FAILED;
  gtk_widget_set_sensitive(testing.run_all, !busy && testing.sources && testing.sources->len);
  gtk_widget_set_sensitive(testing.run_failed, !busy && failed);
  gtk_widget_set_sensitive(testing.refresh, !busy);
  gtk_widget_set_sensitive(testing.stop, busy);
}

static void update_summary(void)
{
  guint counts[TEST_SKIPPED + 1] = { 0 }, total = testing.tests ? testing.tests->len : 0;
  char *text, *tooltip = NULL;

  for (guint i = 0; i < total; i++)
    counts[((TestCase *)testing.tests->pdata[i])->state]++;
  if (testing.job && testing.job->listing)
    text = g_strdup_printf("Listing %s tests…", test_kind_name(testing.job->kind));
  else if (testing.job)
    text = g_strdup_printf("Running %s…", test_kind_name(testing.job->kind));
  else if (!total)
    text = g_strdup("");
  else if (counts[TEST_PASSED] + counts[TEST_FAILED] + counts[TEST_SKIPPED] == 0)
    text = g_strdup_printf("%u tests", total);
  else {
    text = g_strdup_printf("✓ %u  ✗ %u  - %u  of %u", counts[TEST_PASSED], counts[TEST_FAILED], counts[TEST_SKIPPED],
                           total);
    tooltip = g_strdup_printf("%u passed, %u failed, %u skipped, %u not run", counts[TEST_PASSED], counts[TEST_FAILED],
                              counts[TEST_SKIPPED], counts[TEST_UNKNOWN] + counts[TEST_RUNNING]);
  }
  gtk_label_set_text(testing.summary, text);
  gtk_widget_set_tooltip_text(GTK_WIDGET(testing.summary), tooltip);
  g_free(tooltip);
  g_free(text);
}

/* Fills the tree again from testing.tests (after a listing or a run). */
static void rebuild(void)
{
  GtkListBoxRow *selected = NULL;
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean several = FALSE;

  if (!testing.list)
    return;
  for (guint i = 1; i < testing.tests->len; i++)
    several |= ((TestCase *)testing.tests->pdata[i])->kind != ((TestCase *)testing.tests->pdata[0])->kind;
  gtk_list_box_remove_all(testing.list);
  /* a group's tests may be apart in the list (pytest's order): each group once, its tests together */
  for (guint i = 0; i < testing.tests->len; i++) {
    TestCase *first = testing.tests->pdata[i];
    char *gkey = group_key(first->kind, first->group);
    if (!g_hash_table_add(seen, gkey))
      continue;
    gboolean closed = g_hash_table_contains(testing.collapsed, gkey);
    GtkWidget *row = row_new(gkey, closed ? "▸" : "▾", group_state(first->kind, first->group), first->group,
                             several ? test_kind_name(first->kind) : NULL, TRUE, FALSE);
    gtk_widget_add_css_class(row, "group");
    gtk_list_box_append(testing.list, row);
    if (g_strcmp0(testing.selected, gkey) == 0)
      selected = GTK_LIST_BOX_ROW(row);
    for (guint j = i; j < testing.tests->len; j++) {
      TestCase *test = testing.tests->pdata[j];
      if (test->kind != first->kind || strcmp(test->group, first->group) != 0)
        continue;
      char *key = test_key(test);
      GtkWidget *child = row_new(key, NULL, test->state, test->name, NULL, TRUE,
                                 test->kind == TEST_CTEST && test->command && test->command[0]);
      g_object_set_data_full(G_OBJECT(child), "group", g_strdup(gkey), g_free);
      gtk_list_box_append(testing.list, child);
      if (g_strcmp0(testing.selected, key) == 0)
        selected = GTK_LIST_BOX_ROW(child);
      g_free(key);
    }
  }
  for (int kind = 0; kind < TEST_KINDS; kind++) {
    if (!testing.errors[kind])
      continue;
    char *key = g_strdup_printf("e%d", kind),
         *name = g_strdup_printf("%s: %s", test_kind_name(kind), testing.errors[kind]);
    GtkWidget *row = row_new(key, NULL, TEST_UNKNOWN, name, NULL, FALSE, FALSE);
    gtk_widget_add_css_class(row, "error");
    gtk_list_box_append(testing.list, row);
    if (g_strcmp0(testing.selected, key) == 0)
      selected = GTK_LIST_BOX_ROW(row);
    g_free(name);
    g_free(key);
  }
  g_hash_table_destroy(seen);
  gtk_list_box_invalidate_filter(testing.list);
  if (selected)
    gtk_list_box_select_row(testing.list, selected);
  show_output();

  gboolean empty = testing.listed && !testing_busy() && !testing.tests->len;
  gboolean errors = FALSE;
  for (int kind = 0; kind < TEST_KINDS; kind++)
    errors |= testing.errors[kind] != NULL;
  gtk_label_set_text(testing.note, testing.sources && !testing.sources->len
                                       ? "No tests found. The panel looks for CTest (a CMake build folder), pytest, "
                                         "go test and cargo test in the project folder."
                                       : "No tests listed.");
  gtk_widget_set_visible(GTK_WIDGET(testing.note), empty && !errors);
  update_summary();
  update_buttons();
  testing_marks_changed();
}

GPtrArray *testing_list(void)
{
  return testing.panel ? testing.tests : NULL;
}

static void on_row_selected(GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  if (!row)
    return;
  g_set_str(&testing.selected, g_object_get_data(G_OBJECT(row), "key"));
  show_output();
}

/* A double click on a test goes to its source. */
static void on_list_pressed(GtkGestureClick *gesture G_GNUC_UNUSED, int presses, double x G_GNUC_UNUSED, double y,
                            gpointer data G_GNUC_UNUSED)
{
  GtkListBoxRow *row = presses == 2 ? gtk_list_box_get_row_at_y(testing.list, (int)y) : NULL;
  TestCase *test = row ? find_test(g_object_get_data(G_OBJECT(row), "key")) : NULL;

  if (test)
    testing_open_source(test->id);
}

/* A click on a group opens or closes it. */
static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  GtkLabel *arrow = g_object_get_data(G_OBJECT(row), "arrow");
  const char *key = g_object_get_data(G_OBJECT(row), "key");

  if (!arrow)
    return;
  if (!g_hash_table_remove(testing.collapsed, key))
    g_hash_table_add(testing.collapsed, g_strdup(key));
  gtk_label_set_text(arrow, g_hash_table_contains(testing.collapsed, key) ? "▸" : "▾");
  gtk_list_box_invalidate_filter(list);
}

/* Running commands ----------------------------------------------------------------------------- */

/* Removes the folder of a CTest run's GoogleTest reports, reading each into the tests first (read). */
static void gtest_reports(const char *report, gboolean read)
{
  char *folder = g_strconcat(report, ".gtest", NULL);
  GDir *dir = g_dir_open(folder, 0, NULL);
  const char *name;

  while (dir && (name = g_dir_read_name(dir))) {
    char *path = g_build_filename(folder, name, NULL), *json = NULL;
    if (read && g_file_get_contents(path, &json, NULL, NULL))
      test_apply_gtest(testing.tests, json);
    g_free(json);
    g_unlink(path);
    g_free(path);
  }
  if (dir)
    g_dir_close(dir);
  g_rmdir(folder);
  g_free(folder);
}

static void job_free(Job *job)
{
  g_strfreev(job->argv);
  g_free(job->debug);
  g_free(job->dir);
  if (job->pending)
    g_ptr_array_unref(job->pending);
  if (job->report && job->kind == TEST_CTEST)
    gtest_reports(job->report, FALSE);
  if (job->report)
    g_unlink(job->report);
  g_free(job->report);
  g_clear_object(&job->proc);
  g_free(job);
}

static char *report_path(void)
{
  if (!testing.dir)
    testing.dir = g_dir_make_tmp("t-code-tests-XXXXXX", NULL);
  return testing.dir ? g_strdup_printf("%s/report-%u.xml", testing.dir, ++testing.reports) : NULL;
}

/* The project's Python for pytest and Cargo's flags for cargo (NULL otherwise). */
static void tools_of(TestKind kind, char **python, char **flags)
{
  *python = kind == TEST_PYTEST ? toolchain_python(testing.app, NULL) : NULL;
  *flags = NULL;
  if (kind == TEST_CARGO) {
    char *features = toolchain_rust_features(testing.app), *target = toolchain_rust_target(testing.app);
    *flags = langenv_cargo_flags(features, target);
    g_free(target);
    g_free(features);
  }
}

/* A command for source's tests: the listing, or a run of tests (TestCase; NULL: all). */
static Job *job_new(const TestSource *source, gboolean listing, GPtrArray *tests)
{
  Job *job = g_new0(Job, 1);
  char *python, *flags;

  tools_of(source->kind, &python, &flags);
  job->kind = source->kind;
  job->dir = g_strdup(source->dir);
  job->listing = listing;
  job->generation = testing.generation;
  if (listing) {
    job->argv = test_list_argv(source, python, flags);
  } else {
    job->report = source->kind == TEST_CTEST || source->kind == TEST_PYTEST ? report_path() : NULL;
    job->argv = test_run_argv(source, python, flags, tests, job->report);
  }
  g_free(flags);
  g_free(python);
  return job;
}

/* Its own process group, so Stop ends what it started too (ctest's tests, cargo's programs). */
static void child_setup(gpointer data G_GNUC_UNUSED)
{
  setpgid(0, 0);
}

static void start_next(void);

/* The tests listed again: their last results stay. */
static void merge_listing(TestKind kind, GPtrArray *listed)
{
  GPtrArray *merged = g_ptr_array_new_with_free_func(test_case_free);
  gboolean placed = FALSE;

  for (guint i = 0; i < listed->len; i++) {
    TestCase *test = listed->pdata[i];
    for (guint j = 0; j < testing.tests->len; j++) {
      TestCase *old = testing.tests->pdata[j];
      if (old->kind == kind && strcmp(old->id, test->id) == 0) {
        test->state = old->state == TEST_RUNNING ? TEST_UNKNOWN : old->state;
        test->output = g_steal_pointer(&old->output);
        break;
      }
    }
  }
  /* in the frameworks' order: this kind's tests where they were, or before the next kind's */
  for (guint j = 0; j <= testing.tests->len; j++) {
    TestCase *old = j < testing.tests->len ? testing.tests->pdata[j] : NULL;
    if (!placed && (!old || old->kind >= kind)) {
      for (guint i = 0; i < listed->len; i++)
        g_ptr_array_add(merged, listed->pdata[i]);
      placed = TRUE;
    }
    if (old && old->kind != kind) {
      g_ptr_array_add(merged, old);
      testing.tests->pdata[j] = NULL;
    }
  }
  g_ptr_array_set_free_func(listed, NULL);
  g_ptr_array_free(listed, TRUE);
  g_ptr_array_free(testing.tests, TRUE);
  testing.tests = merged;
}

static void debug_built(Job *job, const char *out, gboolean ok, gboolean stopped);

/* Lists the GoogleTest cases of the first test of CTest's list from index on that runs a whole
 * GoogleTest program (next, before what waits); FALSE when none is left. */
static gboolean list_gtest_cases(GPtrArray *listed, guint from)
{
  const TestSource *source = source_of(TEST_CTEST);

  for (guint i = from; source && i < listed->len; i++) {
    TestCase *test = listed->pdata[i];
    char **argv = test_gtest_list_argv(test);
    if (!argv)
      continue;
    Job *job = g_new0(Job, 1);
    job->kind = TEST_CTEST;
    job->dir = g_strdup(test->cwd ? test->cwd : source->dir);
    job->argv = argv;
    job->listing = TRUE;
    job->pending = g_ptr_array_ref(listed);
    job->expand = i;
    job->generation = testing.generation;
    g_queue_push_head(&testing.waiting, job);
    return TRUE;
  }
  return FALSE;
}

/* A GoogleTest program's cases listed: they replace its CTest test in the list. */
static void gtest_listed(Job *job, const char *out, gboolean ok, gboolean stopped)
{
  GPtrArray *listed = g_steal_pointer(&job->pending);
  guint next = job->expand + 1;

  if (ok && job->expand < listed->len) {
    GPtrArray *cases = test_parse_gtest_list(listed->pdata[job->expand], out);
    if (cases->len) {
      g_ptr_array_remove_index(listed, job->expand);
      for (guint i = 0; i < cases->len; i++)
        g_ptr_array_insert(listed, job->expand + i, g_steal_pointer(&cases->pdata[i]));
      next = job->expand + cases->len;
    }
    g_ptr_array_unref(cases);
  }
  if (stopped || !list_gtest_cases(listed, next))
    merge_listing(TEST_CTEST, listed);
  else
    g_ptr_array_unref(listed); /* the next listing holds it */
}

static void job_finished(Job *job, const char *out, gboolean ok, gboolean stopped)
{
  if (job->pending) {
    gtest_listed(job, out, ok, stopped);
    return;
  }
  g_set_str(&testing.logs[job->kind], out);
  if (job->debug) {
    debug_built(job, out, ok, stopped);
    return;
  }
  if (job->listing) {
    GPtrArray *listed = test_parse_list(job->kind, out);
    g_clear_pointer(&testing.errors[job->kind], g_free);
    if (!listed->len && !ok && !stopped)
      testing.errors[job->kind] = g_strdup("could not list the tests (select for the output)");
    if (job->kind == TEST_CTEST && !stopped && list_gtest_cases(listed, 0)) {
      g_ptr_array_unref(listed); /* the GoogleTest listings hold it */
      return;
    }
    merge_listing(job->kind, listed);
    return;
  }
  char *report = NULL;
  if (job->report)
    g_file_get_contents(job->report, &report, NULL, NULL);
  test_apply_results(job->kind, testing.tests, out, report);
  g_free(report);
  if (job->report && job->kind == TEST_CTEST)
    gtest_reports(job->report, TRUE);
  /* No result: the build failed, or the command was stopped. */
  for (guint i = 0; i < testing.tests->len; i++) {
    TestCase *test = testing.tests->pdata[i];
    if (test->kind != job->kind || test->state != TEST_RUNNING)
      continue;
    test->state = ok || stopped ? TEST_UNKNOWN : TEST_FAILED;
    if (test->state == TEST_FAILED)
      g_set_str(&test->output, out);
  }
  /* listed again: a run may follow a change that added tests */
  const TestSource *source = source_of(job->kind);
  if (source && !stopped)
    g_queue_push_tail(&testing.waiting, job_new(source, TRUE, NULL));
}

static void on_job_done(GObject *object, GAsyncResult *result, gpointer data)
{
  Job *job = data;
  GSubprocess *proc = G_SUBPROCESS(object);
  GBytes *bytes = NULL;
  gboolean ok = g_subprocess_communicate_finish(proc, result, &bytes, NULL, NULL);
  gboolean stopped = job->stopped;

  ok = ok && g_subprocess_get_if_exited(proc) && g_subprocess_get_exit_status(proc) == 0;
  if (testing.job == job)
    testing.job = NULL;
  if (testing.panel && job->generation == testing.generation) {
    gsize size = 0;
    const char *raw = bytes ? g_bytes_get_data(bytes, &size) : "";
    char *out = g_utf8_make_valid(raw ? raw : "", size);
    job_finished(job, out, ok, stopped);
    g_free(out);
    start_next();
    rebuild();
  }
  if (bytes)
    g_bytes_unref(bytes);
  job_free(job);
}

/* Starts the next waiting command when none runs. */
static void start_next(void)
{
  Job *job;

  while (!testing.job && (job = g_queue_pop_head(&testing.waiting))) {
    GSubprocessLauncher *launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
    GError *error = NULL;

    g_subprocess_launcher_set_cwd(launcher, job->dir);
    g_subprocess_launcher_set_child_setup(launcher, child_setup, NULL, NULL);
    /* plain text for the parsers and the output view */
    g_subprocess_launcher_setenv(launcher, "CARGO_TERM_COLOR", "never", TRUE);
    g_subprocess_launcher_setenv(launcher, "NO_COLOR", "1", TRUE);
    g_subprocess_launcher_setenv(launcher, "PY_COLORS", "0", TRUE);
    g_subprocess_launcher_setenv(launcher, "GTEST_COLOR", "no", TRUE);
    job->proc = g_subprocess_launcher_spawnv(launcher, (const char *const *)job->argv, &error);
    g_object_unref(launcher);
    if (!job->proc) {
      g_free(testing.errors[job->kind]);
      testing.errors[job->kind] = g_strdup(error->message);
      g_error_free(error);
      for (guint i = 0; i < testing.tests->len; i++) {
        TestCase *test = testing.tests->pdata[i];
        if (test->kind == job->kind && test->state == TEST_RUNNING)
          test->state = TEST_UNKNOWN;
      }
      job_free(job);
      continue;
    }
    testing.job = job;
    g_subprocess_communicate_async(job->proc, NULL, NULL, on_job_done, job);
  }
}

/* Stop's second step: a run that is still going two seconds after SIGTERM (ignored or blocked by
 * what it started) is killed. The identifier is NULL once the process has been reaped. */
static gboolean kill_if_running(gpointer data)
{
  const char *id = g_subprocess_get_identifier(G_SUBPROCESS(data));

  if (id)
    kill(-(pid_t)atoi(id), SIGKILL);
  return G_SOURCE_REMOVE;
}

void testing_stop(void)
{
  Job *job;

  while ((job = g_queue_pop_head(&testing.waiting)))
    job_free(job);
  for (guint i = 0; testing.tests && i < testing.tests->len; i++) {
    TestCase *test = testing.tests->pdata[i];
    if (test->state == TEST_RUNNING && (!testing.job || test->kind != testing.job->kind))
      test->state = TEST_UNKNOWN;
  }
  if (testing.job && testing.job->proc) {
    testing.job->stopped = TRUE;
    const char *id = g_subprocess_get_identifier(testing.job->proc);
    if (id) {
      kill(-(pid_t)atoi(id), SIGTERM);
      g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 2, kill_if_running, g_object_ref(testing.job->proc),
                                 g_object_unref);
    }
  }
  if (testing.panel)
    rebuild();
}

void testing_refresh(App *t)
{
  if (testing_busy() || !t->project.root)
    return;
  testing.listed = TRUE;
  if (testing.sources)
    g_ptr_array_unref(testing.sources);
  testing.sources = test_sources(t->project.root);
  for (int kind = 0; kind < TEST_KINDS; kind++)
    g_clear_pointer(&testing.errors[kind], g_free);
  /* frameworks no longer there take their tests with them */
  for (guint i = testing.tests->len; i-- > 0;)
    if (!source_of(((TestCase *)testing.tests->pdata[i])->kind))
      g_ptr_array_remove_index(testing.tests, i);
  for (guint i = 0; i < testing.sources->len; i++) {
    TestSource *source = testing.sources->pdata[i];
    if (source->hint)
      testing.errors[source->kind] = g_strdup(source->hint);
    else
      g_queue_push_tail(&testing.waiting, job_new(source, TRUE, NULL));
  }
  start_next();
  rebuild();
}

/* Running tests -------------------------------------------------------------------------------- */

/* After the save: a command per framework for the tests of keys (NULL: all of them). */
static void run_saved(App *t, gpointer data)
{
  GPtrArray *keys = data;

  if (t->closing || !testing.panel || testing_busy() || !testing.sources) {
    if (keys)
      g_ptr_array_unref(keys);
    return;
  }
  for (guint s = 0; s < testing.sources->len; s++) {
    const TestSource *source = testing.sources->pdata[s];
    GPtrArray *chosen = g_ptr_array_new();
    if (source->hint) {
      g_ptr_array_free(chosen, TRUE);
      continue;
    }
    for (guint i = 0; i < testing.tests->len; i++) {
      TestCase *test = testing.tests->pdata[i];
      gboolean wanted = test->kind == source->kind;
      if (wanted && keys) {
        char *key = test_key(test);
        wanted = g_ptr_array_find_with_equal_func(keys, key, g_str_equal, NULL);
        g_free(key);
      }
      if (wanted)
        g_ptr_array_add(chosen, test);
    }
    if (keys && !chosen->len) {
      g_ptr_array_free(chosen, TRUE);
      continue;
    }
    Job *job = job_new(source, FALSE, keys ? chosen : NULL);
    for (guint i = 0; i < chosen->len; i++)
      ((TestCase *)chosen->pdata[i])->state = TEST_RUNNING;
    g_queue_push_tail(&testing.waiting, job);
    g_ptr_array_free(chosen, TRUE);
  }
  if (keys)
    g_ptr_array_unref(keys);
  start_next();
  rebuild();
}

/* Runs the tests of keys (NULL: all), once the changed editors are saved. */
static void run_keys(GPtrArray *keys)
{
  if (testing_busy() || !testing.sources || (keys && !keys->len)) {
    if (keys)
      g_ptr_array_unref(keys);
    return;
  }
  save_all(testing.app, run_saved, keys);
}

void testing_run_all(App *t G_GNUC_UNUSED)
{
  run_keys(NULL);
}

void testing_run_failed(App *t G_GNUC_UNUSED)
{
  GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; testing.tests && i < testing.tests->len; i++)
    if (((TestCase *)testing.tests->pdata[i])->state == TEST_FAILED)
      g_ptr_array_add(keys, test_key(testing.tests->pdata[i]));
  run_keys(keys);
}

gboolean testing_run_ids(const char *const *ids)
{
  GPtrArray *keys;

  if (testing_busy() || !ids)
    return FALSE;
  keys = g_ptr_array_new_with_free_func(g_free);
  for (int i = 0; ids[i]; i++) {
    TestCase *test = find_test_id(ids[i]);
    if (test)
      g_ptr_array_add(keys, test_key(test));
  }
  if (!keys->len) {
    g_ptr_array_unref(keys);
    return FALSE;
  }
  run_keys(keys);
  return TRUE;
}

gboolean testing_run_one(const char *id)
{
  TestCase *test = find_test_id(id);
  GPtrArray *keys;

  if (!test || testing_busy())
    return FALSE;
  keys = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(keys, test_key(test));
  run_keys(keys);
  return TRUE;
}

/* Debugging a test ------------------------------------------------------------------------------ */

/* The build for debugging is done: gdb starts on the test's command. */
static void debug_built(Job *job, const char *out, gboolean ok, gboolean stopped)
{
  TestCase *test = find_test_id(job->debug);
  char *why = NULL;

  if (stopped || !test || !test->command)
    return;
  if (!ok)
    why = g_strdup_printf("The build for debugging failed:\n\n%s", out);
  else if (!test_is_program(test->command[0]))
    why = g_strdup_printf("%s is not a program gdb can debug.", test->command[0]);
  if (why) {
    g_free(test->output);
    test->output = why;
    g_set_str(&testing.selected, NULL);
    testing_select(test->id);
    return;
  }
  g_set_str(&testing.debugged, test->id);
  debug_launch_test(testing.app, test->command[0], (const char *const *)test->command + 1,
                    test->cwd ? test->cwd : job->dir, testing_debug_again);
}

/* After the save: the build, then gdb (debug_built). */
static void debug_saved(App *t, gpointer data)
{
  char *id = data;
  TestCase *test = find_test_id(id);
  const TestSource *source = source_of(TEST_CTEST);

  if (!t->closing && testing.panel && !testing_busy() && test && source && !source->hint) {
    Job *job = g_new0(Job, 1);
    job->kind = TEST_CTEST;
    job->dir = g_strdup(source->dir);
    job->argv = test_build_argv(source);
    job->debug = g_strdup(id);
    job->generation = testing.generation;
    g_queue_push_tail(&testing.waiting, job);
    start_next();
    rebuild();
  }
  g_free(id);
}

gboolean testing_debug_one(const char *id)
{
  TestCase *test = find_test_id(id);

  if (!test || test->kind != TEST_CTEST || !test->command || !test->command[0] || testing_busy())
    return FALSE;
  if (debug_active(testing.app)) { /* says so */
    debug_launch_test(testing.app, test->command[0], NULL, NULL, NULL);
    return FALSE;
  }
  save_all(testing.app, debug_saved, g_strdup(id));
  return TRUE;
}

void testing_debug_again(App *t G_GNUC_UNUSED)
{
  if (testing.debugged)
    testing_debug_one(testing.debugged);
}

/* Going to a test's source -------------------------------------------------------------------- */

typedef struct {
  char *root;
  TestCase *test;
  guint generation;
  char *path; /* the answer */
  int line;
} Finding;

static void finding_free(gpointer data)
{
  Finding *finding = data;

  g_free(finding->root);
  test_case_free(finding->test);
  g_free(finding->path);
  g_free(finding);
}

static void find_in_thread(GTask *task, gpointer object G_GNUC_UNUSED, gpointer data,
                           GCancellable *cancellable G_GNUC_UNUSED)
{
  Finding *finding = data;

  finding->path = test_find_source(finding->root, finding->test, &finding->line);
  g_task_return_boolean(task, TRUE);
}

static void on_found(GObject *object G_GNUC_UNUSED, GAsyncResult *result, gpointer data G_GNUC_UNUSED)
{
  Finding *finding = g_task_get_task_data(G_TASK(result));

  if (testing.finding != finding->generation || !testing.panel || testing.app->closing)
    return; /* the project changed, or another test was asked for */
  testing.finding = 0;
  if (finding->path) {
    open_file_at(testing.app, finding->path, finding->line, 0);
  } else {
    char *note = g_strdup_printf("Could not find where %s is defined.", finding->test->name);
    gtk_label_set_text(testing.note, note);
    gtk_widget_set_visible(GTK_WIDGET(testing.note), TRUE);
    g_free(note);
  }
}

static guint finding_generation;

gboolean testing_open_source(const char *id)
{
  TestCase *test = find_test_id(id);
  Finding *finding;
  GTask *task;

  if (!test || !testing.app->project.root)
    return FALSE;
  finding = g_new0(Finding, 1);
  finding->root = g_strdup(testing.app->project.root);
  finding->test = test_case_copy(test);
  finding->generation = testing.finding = ++finding_generation;
  task = g_task_new(NULL, NULL, on_found, NULL);
  g_task_set_task_data(task, finding, finding_free);
  g_task_run_in_thread(task, find_in_thread);
  g_object_unref(task);
  return TRUE;
}

gboolean testing_finding(void)
{
  return testing.finding != 0;
}

gboolean testing_select(const char *id)
{
  TestCase *test = find_test_id(id);
  char *key;
  gboolean found = FALSE;

  if (!test || !testing.list)
    return FALSE;
  key = test_key(test);
  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(testing.list)); row && !found;
       row = gtk_widget_get_next_sibling(row))
    if (GTK_IS_LIST_BOX_ROW(row) && g_strcmp0(g_object_get_data(G_OBJECT(row), "key"), key) == 0) {
      gtk_list_box_select_row(testing.list, GTK_LIST_BOX_ROW(row));
      found = TRUE;
    }
  g_free(key);
  return found;
}

char *testing_text(void)
{
  GString *text = g_string_new(NULL);
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; testing.tests && i < testing.tests->len; i++) {
    TestCase *first = testing.tests->pdata[i];
    if (!g_hash_table_add(seen, group_key(first->kind, first->group)))
      continue;
    g_string_append_printf(text, "%s\n", first->group);
    for (guint j = i; j < testing.tests->len; j++) {
      TestCase *test = testing.tests->pdata[j];
      if (test->kind == first->kind && strcmp(test->group, first->group) == 0)
        g_string_append_printf(text, "  %s %s\n", state_mark(test->state), test->name);
    }
  }
  for (int kind = 0; kind < TEST_KINDS; kind++)
    if (testing.errors[kind])
      g_string_append_printf(text, "! %s: %s\n", test_kind_name(kind), testing.errors[kind]);
  g_hash_table_destroy(seen);
  return g_string_free(text, FALSE);
}

/* The project ---------------------------------------------------------------------------------- */

static void forget(void)
{
  testing_stop();
  testing.generation++; /* the stopped command's answer is the old project's */
  testing.job = NULL;   /* its callback frees it */
  g_ptr_array_set_size(testing.tests, 0);
  g_clear_pointer(&testing.sources, g_ptr_array_unref);
  for (int kind = 0; kind < TEST_KINDS; kind++) {
    g_clear_pointer(&testing.logs[kind], g_free);
    g_clear_pointer(&testing.errors[kind], g_free);
  }
  g_hash_table_remove_all(testing.collapsed);
  g_clear_pointer(&testing.selected, g_free);
  g_clear_pointer(&testing.debugged, g_free);
  testing.finding = 0; /* its answer is dropped */
  testing.listed = FALSE;
}

void testing_reset(App *t)
{
  if (!testing.panel)
    return;
  forget();
  rebuild();
  if (gtk_widget_get_mapped(testing.panel))
    testing_refresh(t);
}

/* Listed the first time the panel shows. */
static void on_map(GtkWidget *panel G_GNUC_UNUSED, gpointer data)
{
  if (!testing.listed)
    testing_refresh(data);
}

static void on_destroy(GtkWidget *panel G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  testing.panel = NULL;
  testing.list = NULL;
  testing.output = NULL;
  forget();
  g_clear_pointer(&testing.tests, g_ptr_array_unref);
  g_clear_pointer(&testing.collapsed, g_hash_table_destroy);
  testing_marks_changed();
  if (testing.dir) {
    g_rmdir(testing.dir); /* each report is removed with its command */
    g_clear_pointer(&testing.dir, g_free);
  }
}

static void on_run_all(GtkButton *button G_GNUC_UNUSED, gpointer data)
{
  testing_run_all(data);
}

static void on_run_failed(GtkButton *button G_GNUC_UNUSED, gpointer data)
{
  testing_run_failed(data);
}

static void on_stop(GtkButton *button G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  testing_stop();
}

static void on_refresh(GtkButton *button G_GNUC_UNUSED, gpointer data)
{
  testing_refresh(data);
}

static GtkWidget *header_button(const char *icon, const char *tooltip, GCallback clicked, App *t)
{
  GtkWidget *button = gtk_button_new_from_icon_name(icon);

  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_tooltip_text(button, tooltip);
  g_signal_connect(button, "clicked", clicked, t);
  return button;
}

static const char *const testing_css =
    ".tcode-testing { background: var(--tc-panel); }"
    ".tcode-testing list { background: transparent; }"
    ".tcode-testing .header { margin: 4px 6px 2px 10px; }"
    ".tcode-testing .header button { min-height: 22px; min-width: 22px; padding: 1px 2px; }"
    ".tcode-testing .summary { color: var(--tc-fg-dim); }"
    ".tcode-testing .note { color: var(--tc-fg-dim); margin: 8px 10px; }"
    ".tcode-testing row { padding: 0 4px; margin: 0 4px; min-height: 22px; border-radius: 6px; color: var(--tc-fg); }"
    ".tcode-testing row:hover { background: var(--tc-hover); }"
    ".tcode-testing row:selected { background: var(--tc-selection); }"
    ".tcode-testing row.group { color: var(--tc-fg-strong); }"
    ".tcode-testing row.error { color: var(--tc-warning); }"
    ".tcode-testing .arrow { color: var(--tc-fg-dim); min-width: 10px; }"
    ".tcode-testing .mark { min-width: 14px; }"
    ".tcode-testing .mark.passed { color: var(--tc-git-added); }"
    ".tcode-testing .mark.failed { color: var(--tc-error); }"
    ".tcode-testing .mark.running { color: var(--tc-accent); }"
    ".tcode-testing .mark.idle { color: var(--tc-fg-faint); }"
    ".tcode-testing .dim { color: var(--tc-fg-faint); font-size: 0.9em; }"
    ".tcode-testing row .run { min-height: 18px; min-width: 18px; padding: 0 2px; opacity: 0; }"
    ".tcode-testing row:hover .run, .tcode-testing row:selected .run { opacity: 1; }"
    ".tcode-testing textview { font-family: monospace; font-size: 0.9em; }"
    ".tcode-testing textview text { background: var(--tc-panel); color: var(--tc-fg); }";

GtkWidget *testing_panel(App *t)
{
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL), *top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *list_scroll = gtk_scrolled_window_new(), *output_scroll = gtk_scrolled_window_new();

  palette_add_css(testing_css);
  testing.app = t;
  testing.panel = panel;
  testing.tests = g_ptr_array_new_with_free_func(test_case_free);
  testing.collapsed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gtk_widget_add_css_class(panel, "tcode-testing");

  gtk_widget_add_css_class(header, "header");
  testing.summary = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(testing.summary), "summary");
  gtk_label_set_xalign(testing.summary, 0);
  gtk_label_set_ellipsize(testing.summary, PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(GTK_WIDGET(testing.summary), TRUE);
  gtk_box_append(GTK_BOX(header), GTK_WIDGET(testing.summary));
  testing.run_all = header_button("tcode-run-symbolic", "Run All Tests", G_CALLBACK(on_run_all), t);
  testing.run_failed = header_button("tcode-redo-symbolic", "Run Failed Tests", G_CALLBACK(on_run_failed), t);
  testing.stop = header_button("tcode-stop-symbolic", "Stop", G_CALLBACK(on_stop), t);
  testing.refresh = header_button("tcode-refresh-symbolic", "Refresh Tests", G_CALLBACK(on_refresh), t);
  gtk_box_append(GTK_BOX(header), testing.run_all);
  gtk_box_append(GTK_BOX(header), testing.run_failed);
  gtk_box_append(GTK_BOX(header), testing.stop);
  gtk_box_append(GTK_BOX(header), testing.refresh);
  gtk_box_append(GTK_BOX(panel), header);

  testing.note = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(testing.note), "note");
  gtk_label_set_wrap(testing.note, TRUE);
  gtk_label_set_xalign(testing.note, 0);
  gtk_widget_set_visible(GTK_WIDGET(testing.note), FALSE);
  gtk_box_append(GTK_BOX(top), GTK_WIDGET(testing.note));

  testing.list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(testing.list, GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(testing.list, TRUE);
  gtk_list_box_set_filter_func(testing.list, row_shown, NULL, NULL);
  g_signal_connect(testing.list, "row-selected", G_CALLBACK(on_row_selected), NULL);
  g_signal_connect(testing.list, "row-activated", G_CALLBACK(on_row_activated), NULL);
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_list_pressed), NULL);
  gtk_widget_add_controller(GTK_WIDGET(testing.list), GTK_EVENT_CONTROLLER(click));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll), GTK_WIDGET(testing.list));
  gtk_widget_set_vexpand(list_scroll, TRUE);
  gtk_box_append(GTK_BOX(top), list_scroll);

  testing.output = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_editable(testing.output, FALSE);
  gtk_text_view_set_cursor_visible(testing.output, FALSE);
  gtk_text_view_set_monospace(testing.output, TRUE);
  gtk_text_view_set_left_margin(testing.output, 8);
  gtk_text_view_set_top_margin(testing.output, 4);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(output_scroll), GTK_WIDGET(testing.output));

  gtk_paned_set_start_child(GTK_PANED(paned), top);
  gtk_paned_set_end_child(GTK_PANED(paned), output_scroll);
  gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(paned), TRUE);
  gtk_paned_set_position(GTK_PANED(paned), 320);
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_box_append(GTK_BOX(panel), paned);

  g_signal_connect(panel, "map", G_CALLBACK(on_map), t);
  g_signal_connect(panel, "destroy", G_CALLBACK(on_destroy), NULL);
  rebuild();
  return panel;
}
