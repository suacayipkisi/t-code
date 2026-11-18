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

/* CTest's listing gives what debugging needs: the command, its WORKING_DIRECTORY, the add_test line. */
static void test_ctest_debug_info(void)
{
  static const char list[] =
      "{\"backtraceGraph\":{\"commands\":[\"add_test\"],\"files\":[\"/p/CMakeLists.txt\"],"
      "\"nodes\":[{\"file\":0},{\"command\":0,\"file\":0,\"line\":7,\"parent\":0}]},\"kind\":\"ctestInfo\","
      "\"tests\":[{\"backtrace\":1,\"name\":\"Math.Adds\",\"command\":[\"/p/build/unit\",\"--gtest_filter=Math.Adds\"],"
      "\"properties\":[{\"name\":\"TIMEOUT\",\"value\":5},{\"name\":\"WORKING_DIRECTORY\",\"value\":\"/p/data\"}]},"
      "{\"name\":\"bare\"}]}";
  GPtrArray *tests = test_parse_list(TEST_CTEST, list);
  TestCase *adds = find(tests, "Math.Adds"), *bare = find(tests, "bare");
  TestSource source = { TEST_CTEST, "/p/build", NULL };
  char **argv = test_build_argv(&source);

  g_assert_cmpuint(g_strv_length(adds->command), ==, 2);
  g_assert_cmpstr(adds->command[1], ==, "--gtest_filter=Math.Adds");
  g_assert_cmpstr(adds->cwd, ==, "/p/data");
  g_assert_cmpstr(adds->file, ==, "/p/CMakeLists.txt");
  g_assert_cmpint(adds->line, ==, 7);
  TestCase *copy = test_case_copy(adds);
  g_assert_cmpstr(copy->command[0], ==, "/p/build/unit");
  g_assert_cmpstr(copy->file, ==, adds->file);
  test_case_free(copy);
  g_assert_null(bare->command);
  g_assert_null(bare->cwd);
  g_assert_null(bare->file);
  g_ptr_array_unref(tests);
  char *joined = g_strjoinv(" ", argv);
  g_assert_cmpstr(joined, ==, "cmake --build /p/build");
  g_free(joined);
  g_strfreev(argv);

  g_assert_false(test_is_program("/p/build/unit"));
  char *script = path_of("script.sh");
  g_assert_true(g_file_set_contents(script, "#!/bin/sh\n", -1, NULL));
  g_chmod(script, 0755);
  g_assert_false(test_is_program(script)); /* not ELF */
  g_free(script);
  char *sh = g_find_program_in_path("sh");
  g_assert_true(test_is_program(sh));
  g_free(sh);
}

/* The line defining a test, in each language. */
static void test_definition_lines(void)
{
  TestCase
      gtest = { TEST_CTEST, "Math.Adds", "Math", "Adds", 0, NULL, NULL, NULL, NULL, 0, NULL },
      param = { TEST_CTEST, "Big/Math.Adds/3", "Big/Math", "Adds/3", 0, NULL, NULL, NULL, NULL, 0, NULL },
      typed = { TEST_CTEST, "Math/0.Adds", "Math/0", "Adds", 0, NULL, NULL, NULL, NULL, 0, NULL },
      py = { TEST_PYTEST,
             "t/test_a.py::TestK::test_two[1]",
             "t/test_a.py",
             "TestK::test_two[1]",
             0,
             NULL,
             NULL,
             NULL,
             NULL,
             0,
             NULL },
      go = { TEST_GO, "example.com/m\tTestAdd/small", "example.com/m", "TestAdd/small", 0, NULL, NULL, NULL, NULL, 0,
             NULL },
      rs = { TEST_CARGO, "tests::adds", "tests", "adds", 0, NULL, NULL, NULL, NULL, 0, NULL };

  g_assert_cmpint(test_definition_line(&gtest, "#include <x>\nTEST(Math, AddsMore) {}\n TEST_F( Math ,Adds ) {\n"), ==,
                  3);
  g_assert_cmpint(test_definition_line(&param, "\nTEST_P(Math, Adds) {}\n"), ==, 2);
  g_assert_cmpint(test_definition_line(&typed, "TYPED_TEST(Math, Adds) {}\n"), ==, 1);
  g_assert_cmpint(test_definition_line(&gtest, "// TEST(Math, Adds)\n"), ==, 0);
  g_assert_cmpint(
      test_definition_line(&py, "class TestK:\n    def test_twos(self):\n    async def test_two(self, x):\n"), ==, 3);
  g_assert_cmpint(
      test_definition_line(&go, "package m\n\nfunc TestAddAll(t *testing.T) {}\nfunc TestAdd(t *testing.T) {\n"), ==,
      4);
  g_assert_cmpint(test_definition_line(&rs, "mod tests {\n    #[test]\n    fn adds() {}\n}\n"), ==, 3);
  g_assert_cmpint(test_definition_line(&rs, "pub(crate) async fn adds<T>() {}\n"), ==, 1);
  g_assert_cmpint(test_definition_line(&rs, "fn adds_more() {}\n"), ==, 0);
}

/* The lines of an open file defining the listed tests: by language, pytest by its file, go by its
 * package's folder, CTest's add_test line in a CMakeLists.txt; a line with several tests. */
static char *lines_text(GPtrArray *tests, const char *root, const char *path, const char *text)
{
  GArray *lines = test_lines(tests, root, path, text);
  GString *out = g_string_new(NULL);

  for (guint i = 0; i < lines->len; i++) {
    TestLine *l = &g_array_index(lines, TestLine, i);
    g_string_append_printf(out, "%s%d:%s", i ? " " : "", l->line, ((TestCase *)tests->pdata[l->test])->id);
  }
  g_array_free(lines, TRUE);
  return g_string_free(out, FALSE);
}

static void test_lines_in_file(void)
{
  GPtrArray *tests = g_ptr_array_new();
  TestCase gtest = { TEST_CTEST, "Math.Adds", "Math", "Adds", 0, NULL, NULL, NULL, NULL, 0, NULL },
           param0 = { TEST_CTEST, "Big/Math.Mul/0", "Big/Math", "Mul/0", 0, NULL, NULL, NULL, NULL, 0, NULL },
           param1 = { TEST_CTEST, "Big/Math.Mul/1", "Big/Math", "Mul/1", 0, NULL, NULL, NULL, NULL, 0, NULL },
           plain = { TEST_CTEST, "plain", "plain", "plain", 0, NULL, NULL, NULL, NULL, 0, NULL },
           py = { TEST_PYTEST,
                  "t/test_a.py::TestK::test_two[1]",
                  "t/test_a.py",
                  "TestK::test_two[1]",
                  0,
                  NULL,
                  NULL,
                  NULL,
                  NULL,
                  0,
                  NULL },
           other_py = { TEST_PYTEST, "t/test_b.py::test_two", "t/test_b.py", "test_two", 0, NULL, NULL, NULL, NULL, 0,
                        NULL },
           go = { TEST_GO, "example.com/m\tTestAdd", "example.com/m", "TestAdd", 0, NULL, NULL, NULL, NULL, 0, NULL },
           go_sub = { TEST_GO,
                      "example.com/m/sub\tTestAdd",
                      "example.com/m/sub",
                      "TestAdd",
                      0,
                      NULL,
                      NULL,
                      NULL,
                      NULL,
                      0,
                      NULL },
           rs = { TEST_CARGO, "tests::adds", "tests", "adds", 0, NULL, NULL, NULL, NULL, 0, NULL };
  char *root = path_of("lines"), *cmake = path_of("lines/CMakeLists.txt"), *text;

  put("lines/go.mod", "// the module\nmodule example.com/m\n\ngo 1.22\n");
  plain.file = cmake;
  plain.line = 4;
  g_ptr_array_add(tests, &gtest);
  g_ptr_array_add(tests, &param0);
  g_ptr_array_add(tests, &param1);
  g_ptr_array_add(tests, &plain);
  g_ptr_array_add(tests, &py);
  g_ptr_array_add(tests, &other_py);
  g_ptr_array_add(tests, &go);
  g_ptr_array_add(tests, &go_sub);
  g_ptr_array_add(tests, &rs);

  char *cc = path_of("lines/math_test.cc");
  text = lines_text(tests, root, cc, "TEST_P(Math, Mul) {}\n// TEST(Math, Adds)\n\nTEST_F(Math, Adds) {\n}\n");
  g_assert_cmpstr(text, ==, "1:Big/Math.Mul/0 1:Big/Math.Mul/1 4:Math.Adds");
  g_free(text);
  text = lines_text(tests, root, cmake, "project(m)\nenable_testing()\n\nadd_test(NAME plain COMMAND p)\n");
  g_assert_cmpstr(text, ==, "4:plain");
  g_free(text);
  char *pyfile = path_of("lines/t/test_a.py");
  text = lines_text(tests, root, pyfile, "class TestK:\n    def test_one(self):\n    async def test_two(self):\n");
  g_assert_cmpstr(text, ==, "1:t/test_a.py::TestK::test_two[1] 3:t/test_a.py::TestK::test_two[1]");
  g_free(text);
  char *gofile = path_of("lines/a_test.go"), *subfile = path_of("lines/sub/b_test.go");
  text = lines_text(tests, root, gofile, "package m\nfunc TestAdd(t *testing.T) {}\n");
  g_assert_cmpstr(text, ==, "2:example.com/m\tTestAdd");
  g_free(text);
  text = lines_text(tests, root, subfile, "package sub\n\nfunc TestAdd(t *testing.T) {}\n");
  g_assert_cmpstr(text, ==, "3:example.com/m/sub\tTestAdd");
  g_free(text);
  char *rsfile = path_of("lines/src/lib.rs");
  text = lines_text(tests, root, rsfile, "mod tests {\n    #[test]\n    fn adds() {}\n    fn adds_more() {}\n}\n");
  g_assert_cmpstr(text, ==, "3:tests::adds");
  g_free(text);
  text = lines_text(tests, root, cc, NULL);
  g_assert_cmpstr(text, ==, "");
  g_free(text);
  g_assert_true(test_file_may_define(cmake));
  g_assert_true(test_file_may_define(gofile));
  g_assert_false(test_file_may_define("/p/main.go"));
  g_assert_false(test_file_may_define("/p/README.md"));
  g_free(rsfile);
  g_free(subfile);
  g_free(gofile);
  g_free(pyfile);
  g_free(cc);
  g_free(cmake);
  g_free(root);
  g_ptr_array_free(tests, TRUE);
}

/* A test's file found in the project: build folders and dependencies skipped; go prefers its package's
 * folder; pytest's file comes from its id; CTest falls back to add_test. */
static void test_find_sources(void)
{
  TestCase gtest = { TEST_CTEST, "Math.Adds", "Math", "Adds", 0, NULL, NULL, NULL, NULL, 0, NULL },
           plain = { TEST_CTEST, "plain", "plain", "plain", 0, NULL, NULL, NULL, NULL, 0, NULL },
           py = { TEST_PYTEST, "tests/test_a.py::test_one", "tests/test_a.py", "test_one", 0, NULL, NULL, NULL, NULL, 0,
                  NULL },
           go = { TEST_GO, "example.com/m/sub\tTestAdd", "example.com/m/sub", "TestAdd", 0, NULL, NULL, NULL, NULL, 0,
                  NULL },
           rs = { TEST_CARGO, "tests::adds", "tests", "adds", 0, NULL, NULL, NULL, NULL, 0, NULL };
  char *found, *expected;
  int line = -1;

  put("find/build/CMakeCache.txt", "");
  put("find/build/gen.cc", "TEST(Math, Adds) {}\n");
  put("find/node_modules/x.py", "def test_one():\n");
  put("find/.hidden/a.cc", "TEST(Math, Adds) {}\n");
  put("find/src/math_test.cc", "\n\nTEST(Math, Adds) {}\n");
  put("find/tests/test_a.py", "import os\n\ndef test_one():\n    pass\n");
  put("find/a_test.go", "package m\nfunc TestAdd(t *testing.T) {}\n");
  put("find/sub/b_test.go", "package sub\n\nfunc TestAdd(t *testing.T) {}\n");
  put("find/target/debug/x.rs", "fn adds() {}\n");
  put("find/src/lib.rs", "#[cfg(test)]\nmod tests {\n    #[test]\n    fn adds() {}\n}\n");
  char *root = path_of("find");

  found = test_find_source(root, &gtest, &line);
  expected = path_of("find/src/math_test.cc");
  g_assert_cmpstr(found, ==, expected);
  g_assert_cmpint(line, ==, 3);
  g_free(expected);
  g_free(found);

  found = test_find_source(root, &py, &line);
  expected = path_of("find/tests/test_a.py");
  py.file = (char *)"tests/test_a.py";
  g_free(found);
  found = test_find_source(root, &py, &line);
  g_assert_cmpstr(found, ==, expected);
  g_assert_cmpint(line, ==, 3);
  g_free(expected);
  g_free(found);

  found = test_find_source(root, &go, &line);
  expected = path_of("find/sub/b_test.go");
  g_assert_cmpstr(found, ==, expected);
  g_assert_cmpint(line, ==, 3);
  g_free(expected);
  g_free(found);

  found = test_find_source(root, &rs, &line);
  expected = path_of("find/src/lib.rs");
  g_assert_cmpstr(found, ==, expected);
  g_assert_cmpint(line, ==, 4);
  g_free(expected);
  g_free(found);

  g_assert_null(test_find_source(root, &plain, &line)); /* no add_test known, no pattern */
  plain.file = (char *)"/p/CMakeLists.txt";
  plain.line = 9;
  found = test_find_source(root, &plain, &line);
  g_assert_cmpstr(found, ==, "/p/CMakeLists.txt");
  g_assert_cmpint(line, ==, 9);
  g_free(found);
  gtest.id = (char *)"Math.Missing";
  g_assert_null(test_find_source(root, &gtest, &line));
  g_free(root);
}

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
  TestCase a = { TEST_CTEST, "Math.Adds(1)", "Math", "Adds(1)", 0, NULL, NULL, NULL, NULL, 0, NULL },
           b = { TEST_CTEST, "ok", "ok", "ok", 0, NULL, NULL, NULL, NULL, 0, NULL };
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

  TestCase c = { TEST_GO, "m/a\tTestA", "m/a", "TestA", 0, NULL, NULL, NULL, NULL, 0, NULL },
           d = { TEST_GO, "m/a\tTestB", "m/a", "TestB", 0, NULL, NULL, NULL, NULL, 0, NULL },
           e = { TEST_GO, "m/b\tTestC", "m/b", "TestC", 0, NULL, NULL, NULL, NULL, 0, NULL };
  g_ptr_array_set_size(tests, 0);
  g_ptr_array_add(tests, &c);
  g_ptr_array_add(tests, &d);
  g_ptr_array_add(tests, &e);
  argv = test_run_argv(&go, NULL, NULL, tests, NULL);
  char *joined = g_strjoinv(" ", argv);
  g_assert_cmpstr(joined, ==, "go test -json -run ^(TestA|TestB|TestC)$ m/a m/b");
  g_free(joined);
  g_strfreev(argv);

  TestCase f = { TEST_CARGO, "tests::ok", "tests", "ok", 0, NULL, NULL, NULL, NULL, 0, NULL };
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
  /* what debugging it needs: the program (built now), where add_test is */
  TestCase *ok = find(tests, "ok");
  char *program = path_of("c/build/ok"), *lists = path_of("c/CMakeLists.txt");
  g_assert_cmpstr(ok->command[0], ==, program);
  g_assert_true(test_is_program(ok->command[0]));
  g_assert_cmpstr(ok->file, ==, lists);
  g_assert_cmpint(ok->line, ==, 6);
  g_free(lists);
  g_free(program);
  g_ptr_array_unref(tests);
  g_ptr_array_unref(sources);
  g_free(build);
  g_free(src);
}

/* A GoogleTest program CTest runs as one test (no gtest_discover_tests): its cases listed with
 * --gtest_list_tests, one of them run through CTest with GoogleTest's filter, its result read from the
 * JSON report; a program without GoogleTest is not asked. Needs GoogleTest installed (CMake's GTest). */
static void test_gtest_cases_real(void)
{
  char *header = g_strdup("/usr/include/gtest/gtest.h");
  gboolean installed = g_file_test(header, G_FILE_TEST_EXISTS);

  g_free(header);
  if (!have("cmake") || !have("ctest") || !have("c++"))
    return;
  if (!installed) {
    g_test_skip("GoogleTest is not installed");
    return;
  }
  put("gt/CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(g CXX)\nenable_testing()\n"
                          "find_package(GTest REQUIRED)\nadd_executable(math math.cc)\n"
                          "target_link_libraries(math GTest::gtest_main)\nadd_test(NAME math COMMAND math)\n"
                          "add_executable(plain plain.cc)\nadd_test(NAME plain COMMAND plain)\n");
  put("gt/math.cc",
      "#include <gtest/gtest.h>\nTEST(Math, Adds) { EXPECT_EQ(2, 1 + 1); }\n"
      "TEST(Math, Breaks) { EXPECT_EQ(3, 1 + 1) << \"wrong sum\"; }\n"
      "class Big : public testing::TestWithParam<int> {};\nTEST_P(Big, Works) { EXPECT_GT(GetParam(), 0); }\n"
      "INSTANTIATE_TEST_SUITE_P(Some, Big, testing::Values(1, 2));\n");
  put("gt/plain.cc", "int main() { return 0; }\n");
  char *src = path_of("gt"), *build = path_of("gt/build"), *report = path_of("gt/report.xml");
  char *configure[] = { "cmake", "-S", src, "-B", build, NULL }, *make[] = { "cmake", "--build", build, NULL };
  g_free(run(configure, tmp));
  g_free(run(make, tmp));

  GPtrArray *sources = test_sources(src);
  TestSource *source = sources->pdata[0];
  char **argv = test_list_argv(source, NULL, NULL), *out = run(argv, build);
  GPtrArray *tests = test_parse_list(TEST_CTEST, out);
  g_strfreev(argv);
  g_free(out);
  g_assert_cmpuint(tests->len, ==, 2);
  TestCase *math = find(tests, "math");
  char **plain = test_gtest_list_argv(find(tests, "plain"));
  g_assert_nonnull(plain); /* a program: asked, it says nothing and fails */
  int status = 0;
  g_assert_true(g_spawn_sync(build, plain, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, NULL, &status, NULL));
  g_assert_false(g_spawn_check_wait_status(status, NULL));
  g_assert_cmpstr(out, ==, "");
  g_free(out);
  g_strfreev(plain);

  argv = test_gtest_list_argv(math);
  out = run(argv, build);
  g_strfreev(argv);
  GPtrArray *cases = test_parse_gtest_list(math, out);
  g_free(out);
  GString *ids = g_string_new(NULL);
  for (guint i = 0; i < cases->len; i++) {
    TestCase *one = cases->pdata[i];
    g_string_append_printf(ids, "%s%s|%s|%s", i ? " " : "", one->id, one->group, one->name);
    g_assert_cmpstr(one->runner, ==, "math");
    g_assert_cmpstr(one->command[0], ==, math->command[0]);
  }
  g_assert_cmpstr(ids->str, ==,
                  "Math.Adds|Math|Adds Math.Breaks|Math|Breaks Some/Big.Works/0|Some/Big|Works/0 "
                  "Some/Big.Works/1|Some/Big|Works/1");
  g_string_free(ids, TRUE);
  TestCase *breaks = find(cases, "Math.Breaks");
  g_assert_cmpstr(breaks->command[1], ==, "--gtest_filter=Math.Breaks");
  g_assert_null(test_gtest_list_argv(breaks)); /* a case is not listed again */

  GPtrArray *chosen = g_ptr_array_new();
  g_ptr_array_add(chosen, breaks);
  g_ptr_array_add(chosen, find(cases, "Some/Big.Works/1"));
  argv = test_run_argv(source, NULL, NULL, chosen, report);
  g_assert_cmpstr(argv[6], ==, "^(math)$");
  g_assert_cmpstr(argv[7], ==, "Math.Breaks:Some/Big.Works/1");
  g_free(run(argv, build));
  g_strfreev(argv);
  char *folder = g_strconcat(report, ".gtest", NULL), *json = g_build_filename(folder, "math.json", NULL);
  char *text = read_report(json);
  g_assert_cmpuint(test_apply_gtest(cases, text), ==, 2);
  assert_state(cases, "Math.Breaks", TEST_FAILED, "wrong sum");
  assert_state(cases, "Some/Big.Works/1", TEST_PASSED, NULL);
  assert_state(cases, "Math.Adds", TEST_UNKNOWN, NULL); /* filtered out */
  g_free(text);
  g_free(json);
  g_free(folder);
  g_ptr_array_unref(chosen);
  g_ptr_array_unref(cases);
  g_ptr_array_unref(tests);
  g_ptr_array_unref(sources);
  g_free(report);
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
  g_test_add_func("/frameworks/ctest-debug-info", test_ctest_debug_info);
  g_test_add_func("/frameworks/definition-lines", test_definition_lines);
  g_test_add_func("/frameworks/find-source", test_find_sources);
  g_test_add_func("/frameworks/lines", test_lines_in_file);
  g_test_add_func("/frameworks/pytest-recorded", test_pytest_recorded);
  g_test_add_func("/frameworks/go-recorded", test_go_recorded);
  g_test_add_func("/frameworks/cargo-recorded", test_cargo_recorded);
  g_test_add_func("/frameworks/commands", test_commands);
  g_test_add_func("/frameworks/ctest", test_ctest_real);
  g_test_add_func("/frameworks/gtest-cases", test_gtest_cases_real);
  g_test_add_func("/frameworks/pytest", test_pytest_real);
  g_test_add_func("/frameworks/go", test_go_real);
  g_test_add_func("/frameworks/cargo", test_cargo_real);
  status = g_test_run();

  char *rm[] = { "rm", "-rf", tmp, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(tmp);
  return status;
}
