/* Tests for the Testing panel (src/testing/testing.c): listing when it shows, Run All, a test's
 * output, Run Failed, listing again after a run, Stop, a project switch while a run goes on, debugging
 * a CTest test, going to a test's source, ▶ in an editor's gutter and a GoogleTest program's cases. The frameworks are a fake "go", "cmake" and "ctest" on
 * PATH (shell scripts answering what the panel asks), so no real tool is needed; needs a display. */
#include "testing/testing.h"

#include "debug/debugger.h"
#include "editor/editor.h"
#include "editor/editorview.h"
#include "plugins/plugins.h"
#include "project/toolchain.h"
#include "util.h"
#include "widgets.h"

#include <string.h>

/* What testing.c calls elsewhere in t-code. */
/* clang-format off */
void save_all(App *t, SavedFunc then, gpointer data) { then(t, data); }
char *toolchain_python(App *t G_GNUC_UNUSED, const char *root G_GNUC_UNUSED) { return NULL; }
char *toolchain_rust_features(App *t G_GNUC_UNUSED) { return NULL; }
char *toolchain_rust_target(App *t G_GNUC_UNUSED) { return NULL; }
gboolean debug_active(App *t G_GNUC_UNUSED) { return FALSE; }
static DebugAgain next_again; /* the last debug_next_again's */
void debug_next_again(App *t G_GNUC_UNUSED, DebugAgain again) { next_again = again; }
static char *shown_page; /* the last show_sidebar_page's */
void show_sidebar_page(App *t G_GNUC_UNUSED, const char *page) { g_set_str(&shown_page, page); }
/* clang-format on */

/* A plugin whose "debug" takes .java files (the Java plugin), while java_plugin_on. */
static gboolean java_plugin_on;
static Plugin java_plugin = { .id = "java", .name = "Java" };
static PluginCommand java_debug = { .id = "debug", .name = "Java: Debug", .plugin = &java_plugin };
static char *debugged_by_plugin; /* "file|test" of the last plugin_debug_test */
static int debugged_port;        /* ... and its port */
static char *opened_file;        /* the last open_file's */

PluginCommand *plugins_debug_command_for(App *t G_GNUC_UNUSED, const char *file)
{
  return java_plugin_on && g_str_has_suffix(file, ".java") ? &java_debug : NULL;
}
Plugin *plugin_find(App *t G_GNUC_UNUSED, const char *id)
{
  return g_strcmp0(id, "java") == 0 ? &java_plugin : NULL;
}
gboolean plugin_debug_test(App *t G_GNUC_UNUSED, PluginCommand *command, const char *file, const char *test, int port)
{
  g_assert_true(command == &java_debug);
  debugged_port = port;
  g_free(debugged_by_plugin);
  debugged_by_plugin = g_strdup_printf("%s|%s", file, test);
  return TRUE;
}
void open_file(App *t G_GNUC_UNUSED, const char *path)
{
  g_set_str(&opened_file, path);
}

static char *launched; /* "program|args|cwd" of the last debug_launch_test */
static char *opened;   /* "path:line" of the last open_file_at */

void debug_launch_test(App *t G_GNUC_UNUSED, const char *program, const char *const *args, const char *cwd,
                       DebugAgain again)
{
  char *joined = args ? g_strjoinv(" ", (char **)args) : g_strdup("");

  g_assert_true(again == testing_debug_again);
  g_free(launched);
  launched = g_strdup_printf("%s|%s|%s", program, joined, cwd ? cwd : "");
  g_free(joined);
}

void open_file_at(App *t G_GNUC_UNUSED, const char *path, int line, int column G_GNUC_UNUSED)
{
  g_free(opened);
  opened = g_strdup_printf("%s:%d", path, line);
}

static App app;
static char *tmp, *project;
static char *real_cmake, *real_ctest; /* the fakes run these in a build folder holding ".real" */

/* The fake go: "go test -list . ./..." lists TestOk and TestBad (and TestNew once the project has a
 * file "extra"); "go test -json [-run <regex> <package>]" passes TestOk, fails TestBad, each saying
 * its name, and sleeps first while the project has a file "slow" (ignoring SIGTERM while it also
 * has "stubborn"). Each call goes to the file "calls". */
static const char fake_go[] =
    "#!/bin/sh\n"
    "echo \"$@\" >> calls\n"
    "if [ \"$2\" = -list ]; then\n"
    "  printf 'TestOk\\nTestBad\\n'; [ -f extra ] && echo TestNew\n"
    "  printf 'ok  \\texample.com/m\\t0.01s\\n'; exit 0\n"
    "fi\n"
    "[ -f stubborn ] && trap '' TERM\n"
    "[ -f slow ] && sleep 30\n"
    "re=all; [ \"$3\" = -run ] && re=\"$4\"\n"
    "emit() { case \"$re\" in all|*\"$1\"*) printf '{\"Action\":\"output\",\"Package\":\"example.com/m\","
    "\"Test\":\"%s\",\"Output\":\"%s says\\\\n\"}\\n{\"Action\":\"%s\",\"Package\":\"example.com/m\","
    "\"Test\":\"%s\"}\\n' \"$1\" \"$1\" \"$2\" \"$1\";; esac; }\n"
    "emit TestOk pass; emit TestBad fail; [ -f extra ] && emit TestNew pass\n"
    "exit 1\n";

/* The fake ctest lists Math.Adds (a real program, run with GoogleTest's filter) and notelf (this
 * script); the fake cmake builds, failing while the build folder has a file "broken". %s: the program. */
static const char fake_ctest[] =
    "#!/bin/sh\n"
    "printf '{\"kind\":\"ctestInfo\",\"tests\":[{\"name\":\"Math.Adds\",\"command\":[\"%s\",\"--gtest_filter=Math.Adds\"]},"
    "{\"name\":\"notelf\",\"command\":[\"%%s\"]}]}' \"$0\"\n";
static const char fake_cmake[] = "#!/bin/sh\n"
                                 "[ -f broken ] && { echo 'math.cc:1: error: broken'; exit 1; }\n"
                                 "echo built; exit 0\n";

/* The fake mvn: run with maven.surefire.debug, it listens on that port (as a test JVM waiting for its
 * debugger) until the file "attached" appears in the project, then writes adds' passing report. */
static const char fake_mvn[] =
    "#!/bin/sh\n"
    "for a; do case \"$a\" in -Dmaven.surefire.debug=*) port=\"${a##*:}\";; esac; done\n"
    "[ -n \"$port\" ] && python3 -c 'import os, socket, sys, time\n"
    "s = socket.socket(); s.bind((\"127.0.0.1\", int(sys.argv[1]))); s.listen(1)\n"
    "end = time.time() + 10\n"
    "while not os.path.exists(\"attached\") and time.time() < end: time.sleep(0.05)' \"$port\"\n"
    "mkdir -p target/surefire-reports\n"
    "printf '<testsuite><testcase name=\"adds\" classname=\"com.example.CalcTest\"/></testsuite>\\n' "
    "> target/surefire-reports/TEST-com.example.CalcTest.xml\n";

static gboolean finding(gconstpointer data G_GNUC_UNUSED)
{
  return !testing_finding();
}

static gboolean idle(gconstpointer data G_GNUC_UNUSED)
{
  return !testing_busy();
}

static gboolean busy(gconstpointer data G_GNUC_UNUSED)
{
  return testing_busy();
}

static void assert_text(const char *expected)
{
  char *text = testing_text();

  g_assert_cmpstr(text, ==, expected);
  g_free(text);
}

/* A line of the fake go's "calls", back lines from its end (0: the last). */
static char *call_from_end(guint back)
{
  char *path = g_build_filename(project, "calls", NULL), *text = NULL;
  char **lines;
  char *last = NULL;

  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  lines = g_strsplit(g_strchomp(text), "\n", -1);
  g_assert_cmpuint(g_strv_length(lines), >, back);
  last = g_strdup(lines[g_strv_length(lines) - 1 - back]);
  g_strfreev(lines);
  g_free(text);
  g_free(path);
  return last;
}

static void touch(const char *name, gboolean present)
{
  char *path = g_build_filename(project, name, NULL);

  if (present)
    g_assert_true(g_file_set_contents(path, "", 0, NULL));
  else
    g_unlink(path);
  g_free(path);
}

static void switch_to(const char *root)
{
  g_set_str(&app.project.root, root);
  testing_reset(&app);
}

/* Listed when shown; Run All; a test's output; Run Failed runs only the failed one; a run lists
 * again (a new test comes, the results stay). */
static void test_list_and_run(void)
{
  switch_to(project);
  run_until(idle, NULL);
  assert_text("example.com/m\n  ○ TestOk\n  ○ TestBad\n");

  testing_run_all(&app);
  g_assert_true(testing_busy());
  run_until(idle, NULL);
  assert_text("example.com/m\n  ✓ TestOk\n  ✗ TestBad\n");
  g_assert_true(testing_select("example.com/m\tTestBad"));
  char *output = testing_output_text();
  g_assert_nonnull(strstr(output, "TestBad says"));
  g_free(output);

  touch("extra", TRUE);
  testing_run_failed(&app);
  run_until(idle, NULL);
  assert_text("example.com/m\n  ✓ TestOk\n  ✗ TestBad\n  ○ TestNew\n");
  char *call = call_from_end(1); /* the run, then the listing */
  g_assert_cmpstr(call, ==, "test -json -run ^(TestBad)$ example.com/m");
  g_free(call);
  call = call_from_end(0);
  g_assert_cmpstr(call, ==, "test -list . ./...");
  g_free(call);

  g_assert_true(testing_run_one("example.com/m\tTestNew"));
  run_until(idle, NULL);
  assert_text("example.com/m\n  ✓ TestOk\n  ✗ TestBad\n  ✓ TestNew\n");
  touch("extra", FALSE);
}

/* Stop ends the run (the script and its sleep): no result, no failure; one that ignores SIGTERM
 * is killed a moment later. */
static void test_stop(void)
{
  switch_to(project);
  run_until(idle, NULL);
  touch("slow", TRUE);
  for (int stubborn = 0; stubborn < 2; stubborn++) {
    touch("stubborn", stubborn);
    g_assert_true(testing_run_one("example.com/m\tTestOk"));
    run_until(busy, NULL);
    assert_text("example.com/m\n  … TestOk\n  ○ TestBad\n");
    gint64 start = g_get_monotonic_time();
    testing_stop();
    run_until(idle, NULL);
    g_assert_false(testing_busy());
    g_assert_cmpint(g_get_monotonic_time() - start, <, 4 * G_USEC_PER_SEC);
    assert_text("example.com/m\n  ○ TestOk\n  ○ TestBad\n");
  }
  touch("stubborn", FALSE);
  touch("slow", FALSE);
}

/* Another project while a run goes on: the run stops, its answer is dropped; a CMake project not
 * built yet says so; a folder without tests has none. */
static void test_switch(void)
{
  char *cmake = g_build_filename(tmp, "cmake", NULL), *empty = g_build_filename(tmp, "empty", NULL);

  g_free(write_at(cmake, "CMakeLists.txt", "", -1, 0));
  g_mkdir_with_parents(empty, 0755);
  switch_to(project);
  run_until(idle, NULL);
  touch("slow", TRUE);
  testing_run_all(&app);
  run_until(busy, NULL);
  switch_to(cmake);
  run_until(idle, NULL);
  run_for(200);
  assert_text("! CTest: Build the project first (Ctrl+Shift+B), then refresh\n");
  switch_to(empty);
  run_until(idle, NULL);
  assert_text("");
  touch("slow", FALSE);
  g_free(empty);
  g_free(cmake);
}

static void assert_output_has(const char *id, const char *needle)
{
  g_assert_true(testing_select(id));
  char *output = testing_output_text();
  if (!strstr(output, needle))
    g_error("no \"%s\" in:\n%s", needle, output);
  g_free(output);
}

/* A CTest test is built, then debugged with its command (the program, GoogleTest's filter) in the build
 * folder; Restart debugs it again; a failed build or a command that is no program says why; a double
 * click's job finds TEST(Math, Adds). */
static void test_debug(void)
{
  char *cproject = g_build_filename(tmp, "cproject", NULL), *build = g_build_filename(cproject, "build", NULL);
  char *program = g_find_program_in_path("true"), *expected, *source;

  g_free(write_at(build, "CTestTestfile.cmake", "", -1, 0));
  g_free(write_at(build, "CMakeCache.txt", "", -1, 0));
  source = write_at(cproject, "math_test.cc", "#include <gtest/gtest.h>\nTEST(Math, Adds) {}\n", -1, 0);
  switch_to(cproject);
  run_until(idle, NULL);
  assert_text("Math\n  ○ Adds\nctest\n  ○ notelf\n");

  g_assert_true(testing_debug_one("Math.Adds"));
  run_until(idle, NULL);
  expected = g_strdup_printf("%s|--gtest_filter=Math.Adds|%s", program, build);
  g_assert_cmpstr(launched, ==, expected);
  g_clear_pointer(&launched, g_free);
  testing_debug_again(&app);
  run_until(idle, NULL);
  g_assert_cmpstr(launched, ==, expected);
  g_clear_pointer(&launched, g_free);

  g_assert_true(testing_debug_one("notelf"));
  run_until(idle, NULL);
  g_assert_null(launched);
  assert_output_has("notelf", "is not a program gdb can debug");

  g_free(write_at(build, "broken", "", -1, 0));
  g_assert_true(testing_debug_one("Math.Adds"));
  run_until(idle, NULL);
  g_assert_null(launched);
  assert_output_has("Math.Adds", "The build for debugging failed");
  assert_output_has("Math.Adds", "error: broken");
  assert_text("Math\n  ○ Adds\nctest\n  ○ notelf\n"); /* no result: not run */

  g_assert_true(testing_open_source("Math.Adds"));
  run_until(finding, NULL);
  char *at = g_strdup_printf("%s:2", source);
  g_assert_cmpstr(opened, ==, at);
  g_clear_pointer(&opened, g_free);
  g_assert_true(testing_open_source("notelf")); /* no definition, no add_test line */
  run_until(finding, NULL);
  g_assert_null(opened);
  g_assert_false(testing_debug_one("nothing"));

  g_free(at);
  g_free(expected);
  g_free(source);
  g_free(program);
  g_free(build);
  g_free(cproject);
}

/* Runs the main loop until the plugin was asked to attach (or 20 seconds pass). */
static void wait_plugin_asked(void)
{
  gint64 end = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;

  while (!debugged_by_plugin && g_get_monotonic_time() < end)
    g_main_context_iteration(NULL, FALSE) || (g_usleep(10000), TRUE);
}

/* A JUnit test is debugged by the plugin whose "debug" takes its file: its file opens (for its language
 * server), the panel runs the test with its JVM waiting on a port, and once something listens there the
 * plugin gets the file, the test's id and the port (Restart: the same again). The run's result shows as a
 * Run's would. Without such a plugin it cannot be debugged. */
static void test_debug_junit(void)
{
  char *jproject = g_build_filename(tmp, "jproject", NULL), *source;
  GPtrArray *commands = g_ptr_array_new();

  g_free(write_at(jproject, "pom.xml", "<project/>\n", -1, 0));
  source = write_at(jproject, "src/test/java/com/example/CalcTest.java",
                    "package com.example;\nclass CalcTest {\n  @Test void adds() {}\n}\n", -1, 0);
  g_ptr_array_add(commands, &java_debug);
  java_plugin.commands = commands;
  java_plugin.enabled = TRUE;
  switch_to(jproject);
  run_until(idle, NULL);
  assert_text("com.example.CalcTest\n  ○ adds\n");

  g_assert_false(testing_debug_one("com.example.CalcTest#adds")); /* no plugin debugs Java */
  char *python = g_find_program_in_path("python3");
  if (!python) {
    g_test_skip("no python3 for the fake mvn");
    g_ptr_array_unref(commands);
    java_plugin.commands = NULL;
    g_free(source);
    g_free(jproject);
    return;
  }
  g_free(python);
  java_plugin_on = TRUE;
  char *attached = g_build_filename(jproject, "attached", NULL),
       *expected = g_strdup_printf("%s|com.example.CalcTest#adds", source);
  for (int round = 0; round < 2; round++) { /* the second: Restart */
    g_clear_pointer(&debugged_by_plugin, g_free);
    g_unlink(attached);
    if (round == 0)
      g_assert_true(testing_debug_one("com.example.CalcTest#adds"));
    else
      next_again(&app);
    g_assert_cmpstr(opened_file, ==, source);
    wait_plugin_asked(); /* once the JVM listens */
    g_assert_cmpstr(debugged_by_plugin, ==, expected);
    g_assert_cmpint(debugged_port, >, 0);
    g_assert_true(next_again == testing_debug_again);
    g_assert_true(g_file_set_contents(attached, "", 0, NULL)); /* the debugger let it go on */
    run_until(idle, NULL);
    assert_text("com.example.CalcTest\n  ✓ adds\n");
  }
  /* Restart while the test still runs (the debugger detached, the JVM goes on): it is stopped, then run
   * again, the plugin asked again */
  g_unlink(attached);
  g_clear_pointer(&debugged_by_plugin, g_free);
  g_assert_true(testing_debug_one("com.example.CalcTest#adds"));
  wait_plugin_asked();
  g_assert_nonnull(debugged_by_plugin);
  g_clear_pointer(&debugged_by_plugin, g_free);
  next_again(&app);
  wait_plugin_asked();
  g_assert_cmpstr(debugged_by_plugin, ==, expected);
  g_assert_true(g_file_set_contents(attached, "", 0, NULL));
  run_until(idle, NULL);
  assert_text("com.example.CalcTest\n  ✓ adds\n");
  g_unlink(attached);
  g_free(attached);

  java_plugin_on = FALSE;
  java_plugin.commands = NULL;
  g_ptr_array_unref(commands);
  g_clear_pointer(&debugged_by_plugin, g_free);
  g_clear_pointer(&opened_file, g_free);
  g_free(expected);
  g_free(source);
  g_free(jproject);
}

static void assert_marks(Tab *tab, const char *expected)
{
  char *text = testing_marks_text(tab);

  g_assert_cmpstr(text, ==, expected);
  g_free(text);
}

static GtkWidget *test_renderer(Tab *tab)
{
  GtkWidget *gutter = GTK_WIDGET(gtk_source_view_get_gutter(tab->source, GTK_TEXT_WINDOW_LEFT));

  for (GtkWidget *child = gtk_widget_get_first_child(gutter); child; child = gtk_widget_get_next_sibling(child))
    if (strcmp(G_OBJECT_TYPE_NAME(child), "TcodeTestRenderer") == 0)
      return child;
  return NULL;
}

/* ▶ on the lines of an open file defining listed tests, in their results' colors; a click on one runs
 * its test; an edit moves them; the column hides in a file without tests. */
static void test_marks(void)
{
  EditorView view = { 0 };
  Tab tab = { 0 };
  GtkTextIter at;
  GdkRectangle area = { 0 };

  switch_to(project);
  run_until(idle, NULL);
  tab.app = &app;
  tab.view = &view;
  tab.path = g_build_filename(project, "a_test.go", NULL);
  tab.buffer = gtk_source_buffer_new(NULL);
  tab.source = GTK_SOURCE_VIEW(g_object_ref_sink(gtk_source_view_new_with_buffer(tab.buffer)));
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(tab.buffer),
                           "package m\n\nfunc TestOk(t *testing.T) {}\n\nfunc TestBad(t *testing.T) {}\n", -1);
  testing_marks_attach(&tab);
  GtkWidget *renderer = test_renderer(&tab);
  g_assert_nonnull(renderer);
  g_assert_false(gtk_widget_get_visible(renderer));

  testing_refresh(&app); /* the list changes: the lines are found */
  run_until(idle, NULL);
  assert_marks(&tab, "3 not run example.com/m\tTestOk\n5 not run example.com/m\tTestBad");
  g_assert_true(gtk_widget_get_visible(renderer));

  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab.buffer), &at, 4);
  g_assert_true(gtk_source_gutter_renderer_query_activatable(GTK_SOURCE_GUTTER_RENDERER(renderer), &at, &area));
  gtk_source_gutter_renderer_activate(GTK_SOURCE_GUTTER_RENDERER(renderer), &at, &area, GDK_BUTTON_PRIMARY, 0, 1);
  g_assert_true(testing_busy());
  run_until(idle, NULL);
  char *call = call_from_end(1);
  g_assert_cmpstr(call, ==, "test -json -run ^(TestBad)$ example.com/m");
  g_free(call);
  assert_marks(&tab, "3 not run example.com/m\tTestOk\n5 failed example.com/m\tTestBad");
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab.buffer), &at, 1);
  g_assert_false(gtk_source_gutter_renderer_query_activatable(GTK_SOURCE_GUTTER_RENDERER(renderer), &at, &area));

  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(tab.buffer), &at);
  gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab.buffer), &at, "// tests\n", -1);
  run_for(500);
  assert_marks(&tab, "4 not run example.com/m\tTestOk\n6 failed example.com/m\tTestBad");

  g_set_str(&tab.path, project); /* not a go test file */
  testing_refresh(&app);
  run_until(idle, NULL);
  assert_marks(&tab, "");
  g_assert_false(gtk_widget_get_visible(renderer));

  testing_marks_free(view.test_marks);
  g_object_unref(tab.source);
  g_object_unref(tab.buffer);
  g_free(tab.path);
  g_clear_pointer(&shown_page, g_free);
}

/* A GoogleTest program CTest runs as one test (no gtest_discover_tests): its cases are rows, a run of
 * one keeps the others' results (and the list made again after it keeps the cases), Run All runs them
 * all through the program, a case is debugged with its filter. Real CMake, CTest and GoogleTest. */
static void test_gtest_cases(void)
{
  char *header = g_strdup("/usr/include/gtest/gtest.h"), *cxx = g_find_program_in_path("c++");
  gboolean ready = real_cmake && real_ctest && cxx && g_file_test(header, G_FILE_TEST_EXISTS);

  g_free(cxx);
  g_free(header);
  if (!ready) {
    g_test_skip("needs CMake, a C++ compiler and GoogleTest");
    return;
  }
  char *gproject = g_build_filename(tmp, "gproject", NULL), *build = g_build_filename(gproject, "build", NULL);
  g_free(
      write_at(gproject, "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.21)\nproject(g CXX)\nenable_testing()\nfind_package(GTest REQUIRED)\n"
               "add_executable(math math.cc)\ntarget_link_libraries(math GTest::gtest_main)\n"
               "add_test(NAME math COMMAND math)\n",
               -1, 0));
  g_free(write_at(gproject, "math.cc",
                  "#include <gtest/gtest.h>\nTEST(Math, Adds) { EXPECT_EQ(2, 1 + 1); }\n"
                  "TEST(Math, Breaks) { EXPECT_EQ(3, 1 + 1) << \"wrong sum\"; }\n",
                  -1, 0));
  char *configure[] = { real_cmake, "-S", gproject, "-B", build, NULL },
       *make[] = { real_cmake, "--build", build, NULL };
  g_assert_true(g_spawn_sync(tmp, configure, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL));
  g_assert_true(g_spawn_sync(tmp, make, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL));
  g_free(write_at(build, ".real", "", -1, 0));

  switch_to(gproject);
  run_until(idle, NULL);
  assert_text("Math\n  ○ Adds\n  ○ Breaks\n");

  g_assert_true(testing_run_one("Math.Breaks"));
  run_until(idle, NULL);
  assert_text("Math\n  ○ Adds\n  ✗ Breaks\n");
  assert_output_has("Math.Breaks", "wrong sum");

  testing_run_all(&app);
  run_until(idle, NULL);
  assert_text("Math\n  ✓ Adds\n  ✗ Breaks\n");

  g_assert_true(testing_debug_one("Math.Adds"));
  run_until(idle, NULL);
  char *program = g_build_filename(build, "math", NULL),
       *expected = g_strdup_printf("%s|--gtest_filter=Math.Adds|%s", program, build);
  g_assert_cmpstr(launched, ==, expected);
  g_clear_pointer(&launched, g_free);

  g_free(expected);
  g_free(program);
  g_free(build);
  g_free(gproject);
}

int main(int argc, char **argv)
{
  GtkWidget *window;
  int status;

  g_test_init(&argc, &argv, NULL);
  ignore_gtk_noise();
  tmp = g_dir_make_tmp("t-code-testing-XXXXXX", NULL);
  project = g_build_filename(tmp, "project", NULL);
  g_free(write_at(project, "go.mod", "module example.com/m\n", -1, 0));
  char *bin = g_build_filename(tmp, "bin", NULL), *path = g_strconcat(bin, ":", g_getenv("PATH"), NULL);
  g_free(write_at(bin, "go", fake_go, -1, 0755));
  g_free(write_at(bin, "mvn", fake_mvn, -1, 0755));
  real_cmake = g_find_program_in_path("cmake");
  real_ctest = g_find_program_in_path("ctest");
  char *truth = g_find_program_in_path("true"), *body = g_strdup_printf(fake_ctest, truth);
  char *ctest = g_strdup_printf("#!/bin/sh\n[ -f .real ] && exec '%s' \"$@\"\n%s", real_ctest ? real_ctest : "false",
                                body + strlen("#!/bin/sh\n"));
  char *cmake = g_strdup_printf("#!/bin/sh\n[ -f .real ] && exec '%s' \"$@\"\n%s", real_cmake ? real_cmake : "false",
                                fake_cmake + strlen("#!/bin/sh\n"));
  g_free(write_at(bin, "ctest", ctest, -1, 0755));
  g_free(write_at(bin, "cmake", cmake, -1, 0755));
  g_free(cmake);
  g_free(ctest);
  g_free(body);
  g_free(truth);
  g_setenv("PATH", path, TRUE);
  g_free(path);
  g_free(bin);
  if (!gtk_init_check()) { /* PATH is set before GTK starts its threads */
    g_test_skip("no display");
    return g_test_run();
  }

  app.project.root = g_strdup(tmp); /* none: listed once the project is set */
  window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), testing_panel(&app));
  gtk_window_present(GTK_WINDOW(window));
  run_until(idle, NULL);

  g_test_add_func("/testing/list-and-run", test_list_and_run);
  g_test_add_func("/testing/stop", test_stop);
  g_test_add_func("/testing/switch", test_switch);
  g_test_add_func("/testing/debug", test_debug);
  g_test_add_func("/testing/debug-junit", test_debug_junit);
  g_test_add_func("/testing/marks", test_marks);
  g_test_add_func("/testing/gtest-cases", test_gtest_cases);
  status = g_test_run();

  gtk_window_destroy(GTK_WINDOW(window));
  run_for(50);
  char *rm[] = { "rm", "-rf", tmp, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(project);
  g_free(tmp);
  g_free(real_ctest);
  g_free(real_cmake);
  return status;
}
