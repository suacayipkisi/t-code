/*
 * The test frameworks the Testing panel knows (GLib only): finding them in a project, the commands
 * that list and run their tests, and reading those commands' answers. CTest (GoogleTest cases come
 * through gtest_discover_tests), pytest, go test and cargo test.
 */
#pragma once

#include <glib.h>

typedef enum { TEST_CTEST, TEST_PYTEST, TEST_GO, TEST_CARGO, TEST_KINDS } TestKind;

typedef enum { TEST_UNKNOWN, TEST_RUNNING, TEST_PASSED, TEST_FAILED, TEST_SKIPPED } TestState;

/* A test, as its framework listed it and its last run left it. */
typedef struct {
  TestKind kind;
  char *id;        /* what runs it: CTest's name, pytest's node id, "package\tTestName", cargo's path */
  char *group;     /* the row it is under: a suite, file, package or module */
  char *name;      /* its own row */
  TestState state; /* TEST_UNKNOWN until it ran */
  char *output;    /* the last run's: its output and why it failed, or NULL */
  char **command;  /* CTest's: the command it runs (what gdb debugs), or NULL */
  char *cwd;       /* CTest's: where it runs (its WORKING_DIRECTORY), or NULL: the build folder */
  char *file;      /* where the listing says it is defined (CTest's add_test, pytest's file: relative to the
                      project), or NULL */
  int line;        /* in file (1-based), 0: unknown */
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
 * tests/), go for go.mod, cargo for Cargo.toml. Reads only small files. */
GPtrArray *test_sources(const char *root);

/* "CTest", "pytest", "go test", "cargo test". */
const char *test_kind_name(TestKind kind);

/* The command that lists source's tests (NULL-terminated, g_strfreev). python: pytest's interpreter
 * (NULL: python3); cargo_flags: Cargo's feature and target flags as a shell string (NULL: none). */
char **test_list_argv(const TestSource *source, const char *python, const char *cargo_flags);

/* The tests in a listing command's output (TestCase, freed with the array), in its order, each once. */
GPtrArray *test_parse_list(TestKind kind, const char *out);

/* The command that runs tests (TestCase of source's kind; NULL: all of them), its report going to
 * the file report (CTest, pytest: JUnit XML; go test and cargo test answer on stdout). CTest builds
 * first. */
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
 * pytest's def, go's func, Rust's fn), 0: none. */
int test_definition_line(const TestCase *test, const char *text);

/* Where test is defined in the project at root: the file (absolute; g_free) and *line (0: unknown), or
 * NULL. Reads the source files of its language when the listing did not say: slow on a big project,
 * call it in a thread. */
char *test_find_source(const char *root, const TestCase *test, int *line);

/* Copies / frees a TestCase; frees a TestSource. */
TestCase *test_case_copy(const TestCase *test);
void test_case_free(gpointer test);
void test_source_free(gpointer source);
