/*
 * The test frameworks the Testing panel knows (GLib only): finding them in a project, the commands
 * that list and run their tests, and reading those commands' answers. CTest (GoogleTest cases come
 * through gtest_discover_tests), pytest, go test, cargo test and JUnit (through Maven or Gradle).
 */
#pragma once

#include <glib.h>

typedef enum { TEST_CTEST, TEST_PYTEST, TEST_GO, TEST_CARGO, TEST_JUNIT, TEST_KINDS } TestKind;

typedef enum { TEST_UNKNOWN, TEST_RUNNING, TEST_PASSED, TEST_FAILED, TEST_SKIPPED } TestState;

/* A test, as its framework listed it and its last run left it. */
typedef struct {
  TestKind kind;
  char *id;        /* what runs it: CTest's name, pytest's node id, "package\tTestName", cargo's path,
                      JUnit's "package.Class#method" ("Outer$Nested" for a nested class) */
  char *group;     /* the row it is under: a suite, file, package or module */
  char *name;      /* its own row */
  TestState state; /* TEST_UNKNOWN until it ran */
  char *output;    /* the last run's: its output and why it failed, or NULL */
  char **command;  /* CTest's: the command it runs (what gdb debugs), or NULL */
  char *cwd;       /* CTest's: where it runs (its WORKING_DIRECTORY), or NULL: the build folder */
  char *file;      /* where the listing says it is defined (CTest's add_test; pytest's and JUnit's file,
                      relative to the project), or NULL */
  int line;        /* in file (1-based), 0: unknown */
  char *runner;    /* a GoogleTest case of a CTest test running a whole GoogleTest program: that test's name
                      (CTest runs it, GoogleTest's filter chooses the cases), else NULL */
} TestCase;

/* A framework found in a project. */
typedef struct {
  TestKind kind;
  char *dir;  /* where its commands run: CTest's build folder, else the project's */
  char *hint; /* instead of dir when it cannot run yet ("build the project first"), else NULL */
} TestSource;

/* The frameworks of the project at root (TestSource, freed with the array), in TestKind order: CTest
 * for a build folder CMake configured with tests (build/, or another folder right in root; a
 * CMakeLists.txt without one: a hint), pytest for its configuration or test_*.py files (in root or
 * tests/), go for go.mod, cargo for Cargo.toml, JUnit for a Maven or Gradle build file. Reads only small
 * files. */
GPtrArray *test_sources(const char *root);

/* "CTest", "pytest", "go test", "cargo test", "JUnit". */
const char *test_kind_name(TestKind kind);

/* The command that lists source's tests (NULL-terminated, g_strfreev). python: pytest's interpreter
 * (NULL: python3); cargo_flags: Cargo's feature and target flags as a shell string (NULL: none). */
char **test_list_argv(const TestSource *source, const char *python, const char *cargo_flags);

/* The tests in a listing command's output (TestCase, freed with the array), in its order, each once. */
GPtrArray *test_parse_list(TestKind kind, const char *out);

/* The command that runs tests (TestCase of source's kind; NULL: all of them), its report going to
 * the file report (CTest, pytest, JUnit: JUnit XML, Surefire's or Gradle's reports put together for JUnit;
 * go test and cargo test answer on stdout). CTest builds
 * first, and its GoogleTest programs also write JSON reports into the folder "<report>.gtest/" (see
 * test_apply_gtest); the GoogleTest cases among tests go into their filter. */
char **test_run_argv(const TestSource *source, const char *python, const char *cargo_flags, GPtrArray *tests,
                     const char *report);

/* Puts a run's results into tests (those of kind among them; others are left alone): out is the
 * command's output, report the report file's text (NULL: none). Returns how many got a result. */
guint test_apply_results(TestKind kind, GPtrArray *tests, const char *out, const char *report);

/* The command that builds a CTest project before gdb debugs one of its tests. */
char **test_build_argv(const TestSource *source);

/* Whether path is a program gdb can debug (an ELF file). */
gboolean test_is_program(const char *path);

/* The line (1-based) of text, a source file's, where test is defined (GoogleTest's TEST(Suite, Name),
 * pytest's def, go's func, Rust's fn, a JUnit test's method), 0: none. */
int test_definition_line(const TestCase *test, const char *text);

/* Where test is defined in the project at root: the file (absolute; g_free) and *line (0: unknown), or
 * NULL. Reads the source files of its language when the listing did not say: slow on a big project,
 * call it in a thread. */
char *test_find_source(const char *root, const TestCase *test, int *line);

/* A line defining a test (test_lines). */
typedef struct {
  int line;   /* 1-based */
  guint test; /* its index in the tests given */
} TestLine;

/* The lines of text, the file at path (absolute) in the project at root, where tests (TestCase) are
 * defined: GoogleTest's TEST(Suite, Name), pytest's def or class (a class: each of its tests), go's func
 * in its package's folder, Rust's fn, a JUnit test's method in its file, and for CTest the add_test / gtest_discover_tests line of a
 * CMakeLists.txt. TestLine, sorted by line then test: a line may define several (a parameterized test's
 * instances, a class's tests). Reads only text (and go.mod for a go file). */
GArray *test_lines(GPtrArray *tests, const char *root, const char *path, const char *text);

/* Whether the file at path may define or add tests (a source file of a known kind, a CMakeLists.txt). */
gboolean test_file_may_define(const char *path);

/* A CTest test that runs a whole program without a --gtest_ argument: the command that lists its
 * GoogleTest cases when it is linked with GoogleTest (it exits non-zero, saying nothing, when it is not;
 * run it in the test's folder), else NULL. */
char **test_gtest_list_argv(const TestCase *test);

/* The cases (TestCase, freed with the array) of the CTest test runner in its program's
 * --gtest_list_tests output: "Suite.Name" under their suite, each debugged as runner's command with
 * --gtest_filter, run through runner (their runner field). */
GPtrArray *test_parse_gtest_list(const TestCase *runner, const char *out);

/* Puts the results of a GoogleTest JSON report (--gtest_output=json) into the cases among tests (those
 * with a runner). Returns how many got a result. */
guint test_apply_gtest(GPtrArray *tests, const char *json);

/* Copies / frees a TestCase; frees a TestSource. */
TestCase *test_case_copy(const TestCase *test);
void test_case_free(gpointer test);
void test_source_free(gpointer source);
