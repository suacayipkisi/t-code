/* Tests for the Testing panel (src/testing/testing.c): listing when it shows, Run All, a test's
 * output, Run Failed, listing again after a run, Stop, a project switch while a run goes on, debugging
 * a CTest test and going to a test's source. The frameworks are a fake "go", "cmake" and "ctest" on
 * PATH (shell scripts answering what the panel asks), so no real tool is needed; needs a display. */
#include "testing/testing.h"

#include "debug/debugger.h"
#include "editor/editor.h"
#include "project/toolchain.h"
#include "util.h"

#include <string.h>

/* What testing.c calls elsewhere in t-code. */
/* clang-format off */
void save_all(App *t, SavedFunc then, gpointer data) { then(t, data); }
char *toolchain_python(App *t G_GNUC_UNUSED, const char *root G_GNUC_UNUSED) { return NULL; }
char *toolchain_rust_features(App *t G_GNUC_UNUSED) { return NULL; }
char *toolchain_rust_target(App *t G_GNUC_UNUSED) { return NULL; }
gboolean debug_active(App *t G_GNUC_UNUSED) { return FALSE; }
/* clang-format on */

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

/* The fake go: "go test -list . ./..." lists TestOk and TestBad (and TestNew once the project has a
 * file "extra"); "go test -json [-run <regex> <package>]" passes TestOk, fails TestBad, each saying
 * its name, and sleeps first while the project has a file "slow". Each call goes to the file
 * "calls". */
static const char fake_go[] =
    "#!/bin/sh\n"
    "echo \"$@\" >> calls\n"
    "if [ \"$2\" = -list ]; then\n"
    "  printf 'TestOk\\nTestBad\\n'; [ -f extra ] && echo TestNew\n"
    "  printf 'ok  \\texample.com/m\\t0.01s\\n'; exit 0\n"
    "fi\n"
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

/* Stop ends the run (the script and its sleep): no result, no failure. */
static void test_stop(void)
{
  switch_to(project);
  run_until(idle, NULL);
  touch("slow", TRUE);
  g_assert_true(testing_run_one("example.com/m\tTestOk"));
  run_until(busy, NULL);
  assert_text("example.com/m\n  … TestOk\n  ○ TestBad\n");
  gint64 start = g_get_monotonic_time();
  testing_stop();
  run_until(idle, NULL);
  g_assert_false(testing_busy());
  g_assert_cmpint(g_get_monotonic_time() - start, <, 4 * G_USEC_PER_SEC);
  assert_text("example.com/m\n  ○ TestOk\n  ○ TestBad\n");
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

int main(int argc, char **argv)
{
  GtkWidget *window;
  int status;

  g_test_init(&argc, &argv, NULL);
  tmp = g_dir_make_tmp("t-code-testing-XXXXXX", NULL);
  project = g_build_filename(tmp, "project", NULL);
  g_free(write_at(project, "go.mod", "module example.com/m\n", -1, 0));
  char *bin = g_build_filename(tmp, "bin", NULL), *path = g_strconcat(bin, ":", g_getenv("PATH"), NULL);
  g_free(write_at(bin, "go", fake_go, -1, 0755));
  char *truth = g_find_program_in_path("true"), *ctest = g_strdup_printf(fake_ctest, truth);
  g_free(write_at(bin, "ctest", ctest, -1, 0755));
  g_free(write_at(bin, "cmake", fake_cmake, -1, 0755));
  g_free(ctest);
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
  status = g_test_run();

  gtk_window_destroy(GTK_WINDOW(window));
  run_for(50);
  char *rm[] = { "rm", "-rf", tmp, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(project);
  g_free(tmp);
  return status;
}
