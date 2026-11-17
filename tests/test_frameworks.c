/* Tests for the test frameworks of the Testing panel (src/testing/frameworks.c): reading their
 * listings and results from recorded output, then real CTest, pytest, go and cargo projects (each
 * skipped when its tool is missing; pytest's interpreter is $TCODE_TEST_PYTHON or python3). */
#include "testing/frameworks.h"

#include <glib/gstdio.h>
#include <string.h>

static char *tmp;

static char *path_of(const char *relative)
{
  return g_build_filename(tmp, relative, NULL);
}

/* A file at relative (its folders made). */
static void put(const char *relative, const char *text)
{
  char *path = path_of(relative), *dir = g_path_get_dirname(path);

  g_mkdir_with_parents(dir, 0755);
  g_assert_true(g_file_set_contents(path, text, -1, NULL));
  g_free(dir);
  g_free(path);
}

static TestCase *find(GPtrArray *tests, const char *id)
{
  for (guint i = 0; i < tests->len; i++)
    if (strcmp(((TestCase *)tests->pdata[i])->id, id) == 0)
      return tests->pdata[i];
  g_assert_not_reached();
}

static void assert_test(GPtrArray *tests, const char *id, const char *group, const char *name)
{
  TestCase *test = find(tests, id);

  g_assert_cmpstr(test->group, ==, group);
  g_assert_cmpstr(test->name, ==, name);
}

static void assert_state(GPtrArray *tests, const char *id, TestState state, const char *output_part)
{
  TestCase *test = find(tests, id);

  g_assert_cmpint(test->state, ==, state);
  if (output_part) {
    g_assert_nonnull(test->output);
    g_assert_nonnull(strstr(test->output, output_part));
  }
}

/* Runs argv in dir; returns stdout and stderr together. */
static char *run(char **argv, const char *dir)
{
  char *out = NULL, *err = NULL, *both;

  g_assert_true(g_spawn_sync(dir, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, NULL, NULL));
  both = g_strconcat(out, err, NULL);
  g_free(out);
  g_free(err);
  return both;
}

static char *read_report(const char *path)
{
  char *text = NULL;

  g_file_get_contents(path, &text, NULL, NULL);
  g_unlink(path);
  return text;
}

/* Finding ------------------------------------------------------------------------------------ */

static void test_sources_found(void)
{
  char *root = path_of("find");
  GPtrArray *sources;

  put("find/CMakeLists.txt", "");
  put("find/tests/test_x.py", "");
  put("find/go.mod", "module m\n");
  sources = test_sources(root);
  g_assert_cmpuint(sources->len, ==, 3);
  g_assert_cmpint(((TestSource *)sources->pdata[0])->kind, ==, TEST_CTEST);
  g_assert_null(((TestSource *)sources->pdata[0])->dir); /* not built yet */
  g_assert_nonnull(((TestSource *)sources->pdata[0])->hint);
  g_assert_cmpint(((TestSource *)sources->pdata[1])->kind, ==, TEST_PYTEST);
  g_assert_cmpstr(((TestSource *)sources->pdata[1])->dir, ==, root);
  g_assert_cmpint(((TestSource *)sources->pdata[2])->kind, ==, TEST_GO);
  g_ptr_array_unref(sources);

  /* a configured build folder of another name; Cargo */
  put("find/out/CTestTestfile.cmake", "");
  put("find/out/CMakeCache.txt", "");
  put("find/Cargo.toml", "");
  sources = test_sources(root);
  char *out = path_of("find/out");
  g_assert_cmpuint(sources->len, ==, 4);
  g_assert_cmpstr(((TestSource *)sources->pdata[0])->dir, ==, out);
  g_assert_cmpint(((TestSource *)sources->pdata[3])->kind, ==, TEST_CARGO);
  g_ptr_array_unref(sources);
  g_free(out);

  /* nothing */
  char *empty = path_of("empty");
  g_mkdir_with_parents(empty, 0755);
  sources = test_sources(empty);
  g_assert_cmpuint(sources->len, ==, 0);
  g_ptr_array_unref(sources);
  g_free(empty);
  g_free(root);
}

/* Recorded output ---------------------------------------------------------------------------- */

static void test_ctest_recorded(void)
{
  static const char list[] = "{\"kind\":\"ctestInfo\",\"tests\":[{\"name\":\"ok\",\"command\":[\"/p/build/ok\"]},"
                             "{\"name\":\"Math.Adds\",\"command\":[\"/p/build/unit\",\"--gtest_filter=Math.Adds\"]},"
                             "{\"name\":\"Math.Fails\",\"command\":[\"/p/build/unit\"]},{\"name\":\"lonely\"}]}";
  static const char report[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"(empty)\" tests=\"3\">\n"
                               "<testcase name=\"ok\" classname=\"ok\" time=\"0.001\" status=\"run\">\n"
                               "<properties/><system-out>fine\n</system-out></testcase>\n"
                               "<testcase name=\"Math.Fails\" classname=\"Math.Fails\" status=\"fail\">\n"
                               "<failure message=\"Failed\"/><properties/><system-out>a &lt; b\n</system-out>"
                               "</testcase>\n<testcase name=\"lonely\" classname=\"lonely\" status=\"notrun\">"
                               "<skipped message=\"Disabled\"/></testcase></testsuite>";
  GPtrArray *tests = test_parse_list(TEST_CTEST, list);

  g_assert_cmpuint(tests->len, ==, 4);
  assert_test(tests, "ok", "ok", "ok");
  assert_test(tests, "Math.Adds", "Math", "Adds");
  assert_test(tests, "lonely", "CTest", "lonely");
  g_assert_cmpuint(test_apply_results(TEST_CTEST, tests, "", report), ==, 3);
  assert_state(tests, "ok", TEST_PASSED, "fine");
  assert_state(tests, "Math.Fails", TEST_FAILED, "a < b");
  g_assert_null(strstr(find(tests, "Math.Fails")->output, "Failed"));
  assert_state(tests, "lonely", TEST_SKIPPED, "Disabled");
  assert_state(tests, "Math.Adds", TEST_UNKNOWN, NULL);
  g_ptr_array_unref(tests);

  /* not JSON, a broken report */
  tests = test_parse_list(TEST_CTEST, "CMake Error: no tests");
  g_assert_cmpuint(tests->len, ==, 0);
  g_ptr_array_unref(tests);
  tests = test_parse_list(TEST_CTEST, list);
  g_assert_cmpuint(test_apply_results(TEST_CTEST, tests, "", "<testsuite><testcase name=\"ok\"></testcase><test"), ==,
                   1);
  g_ptr_array_unref(tests);
}

static void test_pytest_recorded(void)
{
  static const char list[] = "tests/test_a.py::test_one\ntests/test_a.py::TestK::test_two[1]\n"
                             "tests/test_a.py::TestK::test_two[a b]\nsub/test_b.py::test_three\n\n"
                             "4 tests collected in 0.01s\n";
  static const char report[] =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?><testsuites name=\"pytest tests\"><testsuite name=\"pytest\">"
      "<testcase classname=\"tests.test_a\" name=\"test_one\" time=\"0.000\"><system-out>said\n</system-out>"
      "</testcase><testcase classname=\"tests.test_a.TestK\" name=\"test_two[1]\"><failure message=\"assert 1 == 2\">"
      "def test_two():\n&gt;  assert 1 == 2</failure></testcase>"
      "<testcase classname=\"tests.test_a.TestK\" name=\"test_two[a b]\"><skipped type=\"pytest.skip\" "
      "message=\"later\">t.py:3: later</skipped></testcase></testsuite></testsuites>";
  GPtrArray *tests = test_parse_list(TEST_PYTEST, list);

  g_assert_cmpuint(tests->len, ==, 4);
  assert_test(tests, "tests/test_a.py::TestK::test_two[1]", "tests/test_a.py", "TestK::test_two[1]");
  assert_test(tests, "sub/test_b.py::test_three", "sub/test_b.py", "test_three");
  g_assert_cmpuint(test_apply_results(TEST_PYTEST, tests, "", report), ==, 3);
  assert_state(tests, "tests/test_a.py::test_one", TEST_PASSED, "said");
  assert_state(tests, "tests/test_a.py::TestK::test_two[1]", TEST_FAILED, ">  assert 1 == 2");
  assert_state(tests, "tests/test_a.py::TestK::test_two[a b]", TEST_SKIPPED, "later");
  assert_state(tests, "sub/test_b.py::test_three", TEST_UNKNOWN, NULL);
  g_ptr_array_unref(tests);
}

static void test_go_recorded(void)
{
  static const char list[] = "TestTop\nok  \texample.com/g\t0.002s\nTestOk\nTestBad\nBenchmarkX\nExampleY\n"
                             "ok  \texample.com/g/pkg\t0.003s\n?   \texample.com/g/none\t[no test files]\n";
  static const char out[] =
      "{\"Action\":\"run\",\"Package\":\"example.com/g/pkg\",\"Test\":\"TestOk\"}\n"
      "{\"Action\":\"output\",\"Package\":\"example.com/g/pkg\",\"Test\":\"TestOk\",\"Output\":\"hello\\n\"}\n"
      "{\"Action\":\"pass\",\"Package\":\"example.com/g/pkg\",\"Test\":\"TestOk\"}\n"
      "{\"Action\":\"output\",\"Package\":\"example.com/g/pkg\",\"Test\":\"TestBad/case\",\"Output\":\"inner\\n\"}\n"
      "{\"Action\":\"pass\",\"Package\":\"example.com/g/pkg\",\"Test\":\"TestBad/case\"}\n"
      "{\"Action\":\"output\",\"Package\":\"example.com/g/pkg\",\"Test\":\"TestBad\",\"Output\":\"p_test.go:4: nope\\n\"}\n"
      "{\"Action\":\"fail\",\"Package\":\"example.com/g/pkg\",\"Test\":\"TestBad\"}\n"
      "{\"Action\":\"fail\",\"Package\":\"example.com/g/pkg\"}\nnot json\n";
  GPtrArray *tests = test_parse_list(TEST_GO, list);

  g_assert_cmpuint(tests->len, ==, 4);
  assert_test(tests, "example.com/g\tTestTop", "example.com/g", "TestTop");
  assert_test(tests, "example.com/g/pkg\tExampleY", "example.com/g/pkg", "ExampleY");
  g_assert_cmpuint(test_apply_results(TEST_GO, tests, out, NULL), ==, 2);
  assert_state(tests, "example.com/g/pkg\tTestOk", TEST_PASSED, "hello");
  assert_state(tests, "example.com/g/pkg\tTestBad", TEST_FAILED, "nope");
  assert_state(tests, "example.com/g/pkg\tTestBad", TEST_FAILED, "inner");
  assert_state(tests, "example.com/g\tTestTop", TEST_UNKNOWN, NULL);
  g_ptr_array_unref(tests);
}

static void test_cargo_recorded(void)
{
  static const char list[] = "tests::bad: test\ntests::ok: test\nbench_x: benchmark\noutside: test\ntests::ok: test\n";
  static const char out[] = "\nrunning 3 tests\ntest tests::later ... ignored\ntest tests::ok ... ok\n"
                            "test tests::bad ... FAILED\n\nfailures:\n\n---- tests::bad stdout ----\nbefore\n\n"
                            "thread 'tests::bad' panicked at src/lib.rs:5:44:\n\nfailures:\n    tests::bad\n\n"
                            "test result: FAILED. 1 passed; 1 failed\n\nrunning 1 test\ntest outside ... ok\n";
  GPtrArray *tests = test_parse_list(TEST_CARGO, list);

  g_assert_cmpuint(tests->len, ==, 3); /* listed twice (two programs): once */
  assert_test(tests, "tests::bad", "tests", "bad");
  assert_test(tests, "outside", "crate", "outside");
  g_assert_cmpuint(test_apply_results(TEST_CARGO, tests, out, NULL), ==, 3);
  assert_state(tests, "tests::ok", TEST_PASSED, NULL);
  assert_state(tests, "tests::bad", TEST_FAILED, "panicked at src/lib.rs:5:44");
  g_assert_null(strstr(find(tests, "tests::bad")->output, "failures:"));
  assert_state(tests, "outside", TEST_PASSED, NULL);
  g_ptr_array_unref(tests);
}

static void test_commands(void)
{
  TestSource ctest = { TEST_CTEST, "/p/build", NULL }, go = { TEST_GO, "/p", NULL }, cargo = { TEST_CARGO, "/p", NULL };
  GPtrArray *tests = g_ptr_array_new();
  TestCase a = { TEST_CTEST, "Math.Adds(1)", "Math", "Adds(1)", 0, NULL },
           b = { TEST_CTEST, "ok", "ok", "ok", 0, NULL };
  char **argv;

  g_ptr_array_add(tests, &a);
  g_ptr_array_add(tests, &b);
  argv = test_run_argv(&ctest, NULL, NULL, tests, "/r.xml");
  g_assert_cmpstr(argv[4], ==, "/p/build");
  g_assert_cmpstr(argv[5], ==, "/r.xml");
  g_assert_cmpstr(argv[6], ==, "^(Math\\.Adds\\(1\\)|ok)$");
  g_strfreev(argv);
  argv = test_run_argv(&ctest, NULL, NULL, NULL, "/r.xml");
  g_assert_null(argv[6]);
  g_strfreev(argv);

  TestCase c = { TEST_GO, "m/a\tTestA", "m/a", "TestA", 0, NULL },
           d = { TEST_GO, "m/a\tTestB", "m/a", "TestB", 0, NULL },
           e = { TEST_GO, "m/b\tTestC", "m/b", "TestC", 0, NULL };
  g_ptr_array_set_size(tests, 0);
  g_ptr_array_add(tests, &c);
  g_ptr_array_add(tests, &d);
  g_ptr_array_add(tests, &e);
  argv = test_run_argv(&go, NULL, NULL, tests, NULL);
  char *joined = g_strjoinv(" ", argv);
  g_assert_cmpstr(joined, ==, "go test -json -run ^(TestA|TestB|TestC)$ m/a m/b");
  g_free(joined);
  g_strfreev(argv);

  TestCase f = { TEST_CARGO, "tests::ok", "tests", "ok", 0, NULL };
  g_ptr_array_set_size(tests, 0);
  g_ptr_array_add(tests, &f);
  argv = test_run_argv(&cargo, NULL, " --features 'a b'", tests, NULL);
  joined = g_strjoinv("|", argv);
  g_assert_cmpstr(joined, ==, "cargo|test|--no-fail-fast|--features|a b|--|--exact|tests::ok");
  g_free(joined);
  g_strfreev(argv);
  argv = test_list_argv(&cargo, NULL, NULL);
  joined = g_strjoinv(" ", argv);
  g_assert_cmpstr(joined, ==, "cargo test -- --list --format=terse");
  g_free(joined);
  g_strfreev(argv);
  g_ptr_array_unref(tests);
}

/* Real projects ------------------------------------------------------------------------------ */

/* Lists source's tests, runs the given ones (NULL: all) and applies the results. */
static GPtrArray *list_and_run(const TestSource *source, const char *python, const char *const *only)
{
  char **argv = test_list_argv(source, python, NULL), *out = run(argv, source->dir), *report = path_of("report.xml");
  GPtrArray *tests = test_parse_list(source->kind, out), *chosen = NULL;

  g_strfreev(argv);
  g_free(out);
  if (only) {
    chosen = g_ptr_array_new();
    for (int i = 0; only[i]; i++)
      g_ptr_array_add(chosen, find(tests, only[i]));
  }
  argv = test_run_argv(source, python, NULL, chosen, report);
  out = run(argv, source->dir);
  char *text = read_report(report);
  test_apply_results(source->kind, tests, out, text);
  g_free(text);
  g_free(out);
  g_strfreev(argv);
  g_free(report);
  if (chosen)
    g_ptr_array_unref(chosen);
  return tests;
}

static gboolean have(const char *program)
{
  char *path = g_find_program_in_path(program);

  g_free(path);
  if (!path)
    g_test_skip(program);
  return path != NULL;
}

static void test_ctest_real(void)
{
  if (!have("cmake") || !have("ctest") || !have("cc"))
    return;
  put("c/CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(s C)\nenable_testing()\n"
                          "add_executable(ok ok.c)\nadd_executable(bad bad.c)\n"
                          "add_test(NAME ok COMMAND ok)\nadd_test(NAME math.bad COMMAND bad)\n");
  put("c/ok.c", "#include <stdio.h>\nint main(void) { puts(\"fine\"); return 0; }\n");
  put("c/bad.c", "#include <stdio.h>\nint main(void) { puts(\"broken here\"); return 1; }\n");
  char *src = path_of("c"), *build = path_of("c/build");
  char *configure[] = { "cmake", "-S", src, "-B", build, NULL };
  g_free(run(configure, tmp));

  GPtrArray *sources = test_sources(src);
  TestSource *source = sources->pdata[0];
  g_assert_cmpstr(source->dir, ==, build);
  /* the run builds first: nothing was built yet */
  GPtrArray *tests = list_and_run(source, NULL, (const char *const[]){ "math.bad", NULL });
  g_assert_cmpuint(tests->len, ==, 2);
  assert_test(tests, "math.bad", "math", "bad");
  assert_state(tests, "math.bad", TEST_FAILED, "broken here");
  assert_state(tests, "ok", TEST_UNKNOWN, NULL); /* not chosen: "^(math\.bad)$" is exact */
  g_ptr_array_unref(tests);
  tests = list_and_run(source, NULL, NULL);
  assert_state(tests, "ok", TEST_PASSED, "fine");
  g_ptr_array_unref(tests);
  g_ptr_array_unref(sources);
  g_free(build);
  g_free(src);
}

static void test_pytest_real(void)
{
  const char *python = g_getenv("TCODE_TEST_PYTHON") ? g_getenv("TCODE_TEST_PYTHON") : "python3";
  char *check[] = { (char *)python, "-m", "pytest", "--version", NULL };
  int status = 1;

  g_spawn_sync(NULL, check, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL,
               NULL, NULL, NULL, &status, NULL);
  if (status != 0) {
    g_test_skip("pytest is not installed");
    return;
  }
  put("p/tests/test_a.py", "import pytest\ndef test_one():\n    print('out one')\ndef test_bad():\n    assert 1 == 2\n"
                           "class TestK:\n    @pytest.mark.parametrize('x', [1, 2])\n    def test_two(self, x):\n"
                           "        assert x\n");
  char *root = path_of("p");
  GPtrArray *sources = test_sources(root);
  GPtrArray *tests = list_and_run(sources->pdata[0], python, NULL);
  g_assert_cmpuint(tests->len, ==, 4);
  assert_state(tests, "tests/test_a.py::test_one", TEST_PASSED, "out one");
  assert_state(tests, "tests/test_a.py::test_bad", TEST_FAILED, "assert 1 == 2");
  assert_state(tests, "tests/test_a.py::TestK::test_two[2]", TEST_PASSED, NULL);
  g_ptr_array_unref(tests);
  g_ptr_array_unref(sources);
  g_free(root);
}

static void test_go_real(void)
{
  if (!have("go"))
    return;
  put("g/go.mod", "module example.com/g\ngo 1.21\n");
  put("g/a_test.go", "package g\nimport \"testing\"\nfunc TestTop(t *testing.T) {}\n");
  put("g/pkg/p_test.go", "package pkg\nimport (\"testing\"; \"fmt\")\n"
                         "func TestOk(t *testing.T) { fmt.Println(\"hello\") }\n"
                         "func TestBad(t *testing.T) { t.Fatal(\"nope\") }\n");
  char *root = path_of("g");
  GPtrArray *sources = test_sources(root);
  GPtrArray *tests = list_and_run(sources->pdata[0], NULL, (const char *const[]){ "example.com/g/pkg\tTestBad", NULL });
  g_assert_cmpuint(tests->len, ==, 3);
  assert_state(tests, "example.com/g/pkg\tTestBad", TEST_FAILED, "nope");
  assert_state(tests, "example.com/g/pkg\tTestOk", TEST_UNKNOWN, NULL);
  g_ptr_array_unref(tests);
  tests = list_and_run(sources->pdata[0], NULL, NULL);
  assert_state(tests, "example.com/g\tTestTop", TEST_PASSED, NULL);
  assert_state(tests, "example.com/g/pkg\tTestOk", TEST_PASSED, "hello");
  g_ptr_array_unref(tests);
  g_ptr_array_unref(sources);
  g_free(root);
}

static void test_cargo_real(void)
{
  if (!have("cargo"))
    return;
  put("r/Cargo.toml", "[package]\nname = \"r\"\nversion = \"0.1.0\"\nedition = \"2021\"\n");
  put("r/src/lib.rs", "#[cfg(test)]\nmod tests {\n    #[test] fn ok() {}\n"
                      "    #[test] fn bad() { println!(\"before\"); assert_eq!(1, 2); }\n"
                      "    #[test] #[ignore] fn later() {}\n}\n");
  put("r/tests/it.rs", "#[test] fn outside() {}\n");
  char *root = path_of("r");
  GPtrArray *sources = test_sources(root);
  GPtrArray *tests = list_and_run(sources->pdata[0], NULL, NULL);
  g_assert_cmpuint(tests->len, ==, 4);
  assert_state(tests, "tests::ok", TEST_PASSED, NULL);
  assert_state(tests, "tests::bad", TEST_FAILED, "before");
  assert_state(tests, "tests::later", TEST_SKIPPED, NULL);
  assert_state(tests, "outside", TEST_PASSED, NULL);
  g_ptr_array_unref(tests);
  g_ptr_array_unref(sources);
  g_free(root);
}

int main(int argc, char **argv)
{
  int status;

  g_test_init(&argc, &argv, NULL);
  tmp = g_dir_make_tmp("t-code-frameworks-XXXXXX", NULL);
  g_setenv("CARGO_TARGET_DIR", tmp, TRUE); /* not in the test's folder: removed with it anyway */
  g_test_add_func("/frameworks/sources", test_sources_found);
  g_test_add_func("/frameworks/ctest-recorded", test_ctest_recorded);
  g_test_add_func("/frameworks/pytest-recorded", test_pytest_recorded);
  g_test_add_func("/frameworks/go-recorded", test_go_recorded);
  g_test_add_func("/frameworks/cargo-recorded", test_cargo_recorded);
  g_test_add_func("/frameworks/commands", test_commands);
  g_test_add_func("/frameworks/ctest", test_ctest_real);
  g_test_add_func("/frameworks/pytest", test_pytest_real);
  g_test_add_func("/frameworks/go", test_go_real);
  g_test_add_func("/frameworks/cargo", test_cargo_real);
  status = g_test_run();

  char *rm[] = { "rm", "-rf", tmp, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(tmp);
  return status;
}
