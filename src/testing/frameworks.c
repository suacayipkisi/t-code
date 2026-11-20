/*
 * The test frameworks (see frameworks.h).
 */
#include "testing/frameworks.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

#define SOURCE_MAX_BYTES (2 * 1024 * 1024) /* bigger files are not where tests are defined */
#define SOURCE_MAX_FILES 50000             /* read when looking for a test's definition */
#define SOURCE_MAX_DEPTH 16

/* A Java method's declaration up to its name (then the group of the name and "("): annotations,
 * modifiers, type parameters and a return type before it, so a call or "return f(" is not one; it may
 * follow its class's "{" on the same line. */
#define JAVA_METHOD                                                                                                    \
  "^[ \\t]*(?:[^{}\\n]*\\{[ \\t]*)?(?:@[\\w.]+(?:\\([^)\\n]*\\))?[ \\t]+)*"                                            \
  "(?:(?:public|protected|private|static|final|abstract|synchronized|default)[ \\t]+)*"                                \
  "(?:<[^>\\n]*>[ \\t]*)?(?!return\\b|new\\b|throw\\b|else\\b|case\\b|yield\\b)"                                       \
  "[\\w.$]+(?:<[^=;()\\n]*>)?(?:\\[\\])*[ \\t]+"
#define JAVA_TEST_ANNOTATION "@(?:\\w+\\.)*(?:Test|ParameterizedTest|RepeatedTest|TestFactory|TestTemplate)\\b"

void test_case_free(gpointer data)
{
  TestCase *test = data;

  if (!test)
    return;
  g_free(test->id);
  g_free(test->group);
  g_free(test->name);
  g_free(test->output);
  g_strfreev(test->command);
  g_free(test->cwd);
  g_free(test->file);
  g_free(test->runner);
  g_free(test);
}

TestCase *test_case_copy(const TestCase *test)
{
  TestCase *copy = g_new0(TestCase, 1);

  *copy = *test;
  copy->id = g_strdup(test->id);
  copy->group = g_strdup(test->group);
  copy->name = g_strdup(test->name);
  copy->output = g_strdup(test->output);
  copy->command = g_strdupv(test->command);
  copy->cwd = g_strdup(test->cwd);
  copy->file = g_strdup(test->file);
  copy->runner = g_strdup(test->runner);
  return copy;
}

void test_source_free(gpointer data)
{
  TestSource *source = data;

  if (!source)
    return;
  g_free(source->dir);
  g_free(source->hint);
  g_free(source);
}

const char *test_kind_name(TestKind kind)
{
  static const char *const names[TEST_KINDS] = { "CTest", "pytest", "go test", "cargo test", "JUnit" };

  return kind < TEST_KINDS ? names[kind] : "";
}

/* Finding them ------------------------------------------------------------------------------- */

static gboolean exists_in(const char *dir, const char *name)
{
  char *path = g_build_filename(dir, name, NULL);
  gboolean found = g_file_test(path, G_FILE_TEST_EXISTS);

  g_free(path);
  return found;
}

/* Whether the file name in dir holds needle (a small configuration file). */
static gboolean file_holds(const char *dir, const char *name, const char *needle)
{
  char *path = g_build_filename(dir, name, NULL), *text = NULL;
  gboolean found = g_file_get_contents(path, &text, NULL, NULL) && strstr(text, needle);

  g_free(text);
  g_free(path);
  return found;
}

/* A folder CMake configured with tests: build/ first, else the first other one right in root. */
static char *ctest_dir(const char *root)
{
  char *build = g_build_filename(root, "build", NULL);
  GDir *dir;
  const char *name;
  GPtrArray *found;
  char *chosen = NULL;

  if (exists_in(build, "CTestTestfile.cmake"))
    return build;
  g_free(build);
  if (!(dir = g_dir_open(root, 0, NULL)))
    return NULL;
  found = g_ptr_array_new_with_free_func(g_free);
  while ((name = g_dir_read_name(dir))) {
    char *path = g_build_filename(root, name, NULL);
    if (*name != '.' && exists_in(path, "CTestTestfile.cmake") && exists_in(path, "CMakeCache.txt"))
      g_ptr_array_add(found, path);
    else
      g_free(path);
  }
  g_dir_close(dir);
  g_ptr_array_sort_values(found, (GCompareFunc)strcmp);
  if (found->len)
    chosen = g_strdup(found->pdata[0]);
  g_ptr_array_free(found, TRUE);
  return chosen;
}

/* Whether dir holds test_*.py or *_test.py. */
static gboolean has_test_files(const char *dir)
{
  GDir *d = g_dir_open(dir, 0, NULL);
  const char *name;
  gboolean found = FALSE;

  while (d && !found && (name = g_dir_read_name(d)))
    found = g_str_has_suffix(name, ".py") && (g_str_has_prefix(name, "test_") || g_str_has_suffix(name, "_test.py"));
  if (d)
    g_dir_close(d);
  return found;
}

static gboolean pytest_project(const char *root)
{
  char *tests = g_build_filename(root, "tests", NULL);
  gboolean found = exists_in(root, "pytest.ini") || exists_in(root, "conftest.py") ||
                   file_holds(root, "pyproject.toml", "[tool.pytest") ||
                   file_holds(root, "setup.cfg", "[tool:pytest]") || file_holds(root, "tox.ini", "[pytest]") ||
                   has_test_files(root) || has_test_files(tests);

  g_free(tests);
  return found;
}

static void add_source(GPtrArray *sources, TestKind kind, const char *dir, const char *hint)
{
  TestSource *source = g_new0(TestSource, 1);

  source->kind = kind;
  source->dir = g_strdup(dir);
  source->hint = g_strdup(hint);
  g_ptr_array_add(sources, source);
}

GPtrArray *test_sources(const char *root)
{
  GPtrArray *sources = g_ptr_array_new_with_free_func(test_source_free);
  char *build = ctest_dir(root);

  if (build)
    add_source(sources, TEST_CTEST, build, NULL);
  else if (exists_in(root, "CMakeLists.txt"))
    add_source(sources, TEST_CTEST, NULL, "Build the project first (Ctrl+Shift+B), then refresh");
  g_free(build);
  if (pytest_project(root))
    add_source(sources, TEST_PYTEST, root, NULL);
  if (exists_in(root, "go.mod"))
    add_source(sources, TEST_GO, root, NULL);
  if (exists_in(root, "Cargo.toml"))
    add_source(sources, TEST_CARGO, root, NULL);
  if (exists_in(root, "pom.xml") || exists_in(root, "build.gradle") || exists_in(root, "build.gradle.kts") ||
      exists_in(root, "settings.gradle") || exists_in(root, "settings.gradle.kts"))
    add_source(sources, TEST_JUNIT, root, NULL);
  return sources;
}

/* Commands ----------------------------------------------------------------------------------- */

static GPtrArray *argv_new(void)
{
  return g_ptr_array_new_with_free_func(g_free);
}

static void argv_add(GPtrArray *argv, const char *arg)
{
  g_ptr_array_add(argv, g_strdup(arg));
}

static char **argv_end(GPtrArray *argv)
{
  g_ptr_array_set_free_func(argv, NULL);
  g_ptr_array_add(argv, NULL);
  return (char **)g_ptr_array_free(argv, FALSE);
}

static void add_cargo_flags(GPtrArray *argv, const char *flags)
{
  char **words = NULL;

  if (flags && g_shell_parse_argv(flags, NULL, &words, NULL))
    for (int i = 0; words[i]; i++)
      argv_add(argv, words[i]);
  g_strfreev(words);
}

/* JUnit: the lines of the Java files holding a test annotation that the listing reads (parse_junit_list):
 * packages, classes, test annotations and what may be a method's declaration. grep exits with 1 when
 * nothing matches, xargs with 123 when one of its greps did. */
static const char junit_list_script[] =
    "grep -rlZE --include='*.java' --exclude-dir=.git --exclude-dir=target --exclude-dir=build "
    "--exclude-dir=node_modules --exclude-dir=.gradle --exclude-dir=out -e \"$1\" . | "
    "xargs -0 -r grep -HnE -e '^[[:space:]]*package[[:space:]]' "
    "-e '(class|interface|enum|record)[[:space:]]+[[:alnum:]_$]' "
    "-e '[[:alnum:]_$][[:alnum:]_$<>.,?]*[[:space:]]+[[:alnum:]_$]+[[:space:]]*[(]' "
    "-e \"$1\"; s=$?; [ $s -le 1 ] || [ $s -eq 123 ]";

char **test_list_argv(const TestSource *source, const char *python, const char *cargo_flags)
{
  GPtrArray *argv = argv_new();

  switch (source->kind) {
    case TEST_JUNIT:
      for (const char *const *a = (const char *const[]){ "sh", "-c", junit_list_script, "sh",
                                                         "@([[:alnum:]_]+[.])*(Test|ParameterizedTest|RepeatedTest|"
                                                         "TestFactory|TestTemplate)([^[:alnum:]_]|$)",
                                                         NULL };
           *a; a++)
        argv_add(argv, *a);
      break;
    case TEST_CTEST:
      for (const char *const *a =
               (const char *const[]){ "ctest", "--test-dir", source->dir, "--show-only=json-v1", NULL };
           *a; a++)
        argv_add(argv, *a);
      break;
    case TEST_PYTEST:
      for (const char *const *a =
               (const char *const[]){ python ? python : "python3", "-m", "pytest", "--collect-only", "-q", NULL };
           *a; a++)
        argv_add(argv, *a);
      break;
    case TEST_GO:
      for (const char *const *a = (const char *const[]){ "go", "test", "-list", ".", "./...", NULL }; *a; a++)
        argv_add(argv, *a);
      break;
    default:
      argv_add(argv, "cargo");
      argv_add(argv, "test");
      add_cargo_flags(argv, cargo_flags);
      for (const char *const *a = (const char *const[]){ "--", "--list", "--format=terse", NULL }; *a; a++)
        argv_add(argv, *a);
  }
  return argv_end(argv);
}

/* text with the characters a regular expression gives a meaning to escaped. */
static void add_escaped(GString *regex, const char *text)
{
  for (const char *c = text; *c; c++) {
    if (strchr("^$.[]*+?|(){}\\", *c))
      g_string_append_c(regex, '\\');
    g_string_append_c(regex, *c);
  }
}

/* "^(a|b)$" for the tests' names (after the tab for go test's "package\tname"; a GoogleTest case's
 * CTest test: its runner's, once). */
static char *exact_regex(GPtrArray *tests)
{
  GString *regex = g_string_new("^(");
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);

  for (guint i = 0; i < tests->len; i++) {
    const TestCase *test = tests->pdata[i];
    const char *tab = strchr(test->id, '\t'), *name = test->runner ? test->runner : tab ? tab + 1 : test->id;
    if (!g_hash_table_add(seen, (gpointer)name))
      continue;
    if (regex->len > 2)
      g_string_append_c(regex, '|');
    add_escaped(regex, name);
  }
  g_hash_table_destroy(seen);
  g_string_append(regex, ")$");
  return g_string_free(regex, FALSE);
}

/* GoogleTest's filter for the cases among tests ("A.B:C.D"), or NULL when there are none. */
static char *gtest_filter(GPtrArray *tests)
{
  GString *filter = NULL;

  for (guint i = 0; tests && i < tests->len; i++) {
    const TestCase *test = tests->pdata[i];
    if (!test->runner)
      continue;
    filter = filter ? g_string_append_c(filter, ':') : g_string_new(NULL);
    g_string_append(filter, test->id);
  }
  return filter ? g_string_free(filter, FALSE) : NULL;
}

/* JUnit through Maven (mvnw when the project has it) or Gradle (gradlew): $1 the report, $2 Maven's -Dtest
 * ("a.B#c+d,e.F": empty for all), then Gradle's --tests arguments. The reports written by this run
 * (newer than a mark made first) become one report. Gradle's cleanTest: an up-to-date test task would
 * not run. */
static const char junit_run_script[] =
    "r=\"$1\"; t=\"$2\"; shift 2; : > \"$r.start\"; "
    "if [ -f pom.xml ]; then m=mvn; [ -x ./mvnw ] && m=./mvnw; "
    "\"$m\" -B test ${t:+-Dtest=\"$t\"} -Dsurefire.failIfNoSpecifiedTests=false -DfailIfNoTests=false; s=$?; "
    "p='*/surefire-reports/TEST-*.xml'; "
    "else g=gradle; [ -x ./gradlew ] && g=./gradlew; "
    "\"$g\" --console=plain cleanTest test \"$@\"; s=$?; p='*/test-results/*/TEST-*.xml'; fi; "
    "{ echo '<testsuites>'; find . -path \"$p\" -newer \"$r.start\" -exec sed '/^<?xml/d' {} +; "
    "echo '</testsuites>'; } > \"$r\"; rm -f \"$r.start\"; exit $s";

/* The JUnit tests as Maven's -Dtest ("a.B#c+d,e.F#g") and as Gradle's --tests arguments (added to argv). */
static char *junit_filters(GPtrArray *tests, GPtrArray *gradle)
{
  GString *filter = g_string_new(NULL);
  GHashTable *classes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *order = g_ptr_array_new();

  for (guint i = 0; tests && i < tests->len; i++) { /* the methods of each class, the classes in order */
    const TestCase *test = tests->pdata[i];
    const char *hash = strchr(test->id, '#');
    if (!hash)
      continue;
    char *cls = g_strndup(test->id, hash - test->id);
    GString *methods = g_hash_table_lookup(classes, cls);
    if (!methods) {
      methods = g_string_new(hash + 1);
      g_hash_table_insert(classes, cls, methods);
      g_ptr_array_add(order, methods);
      g_string_prepend_c(methods, '#');
      g_string_prepend(methods, cls);
    } else {
      g_string_append_printf(methods, "+%s", hash + 1);
      g_free(cls);
    }
    argv_add(gradle, "--tests");
    char *dotted = g_strdup(test->id);
    dotted[hash - test->id] = '.';
    g_ptr_array_add(gradle, dotted);
  }
  for (guint i = 0; i < order->len; i++) {
    if (filter->len)
      g_string_append_c(filter, ',');
    g_string_append(filter, ((GString *)order->pdata[i])->str);
    g_string_free(order->pdata[i], TRUE);
  }
  g_ptr_array_free(order, TRUE);
  g_hash_table_destroy(classes);
  return g_string_free(filter, FALSE);
}

char **test_run_argv(const TestSource *source, const char *python, const char *cargo_flags, GPtrArray *tests,
                     const char *report)
{
  GPtrArray *argv = argv_new();

  switch (source->kind) {
    case TEST_JUNIT: {
      GPtrArray *gradle = argv_new();
      for (const char *const *a = (const char *const[]){ "sh", "-c", junit_run_script, "sh", report, NULL }; *a; a++)
        argv_add(argv, *a);
      g_ptr_array_add(argv, junit_filters(tests, gradle));
      for (guint i = 0; i < gradle->len; i++)
        argv_add(argv, gradle->pdata[i]);
      g_ptr_array_free(gradle, TRUE);
      break;
    }
    case TEST_CTEST:
      /* What runs is what is built: build first, as Build would. */
      argv_add(argv, "sh");
      argv_add(argv, "-c");
      /* GoogleTest programs write their results to report.gtest/ too (a CTest test running many cases);
       * the cases chosen among those go in its filter */
      argv_add(argv, "cmake --build \"$1\" && export GTEST_OUTPUT=\"json:$2.gtest/\" && "
                     "{ [ -z \"$4\" ] || export GTEST_FILTER=\"$4\"; } && "
                     "exec ctest --test-dir \"$1\" --output-junit \"$2\" ${3:+-R \"$3\"}");
      argv_add(argv, "sh");
      argv_add(argv, source->dir);
      argv_add(argv, report);
      if (tests) {
        char *filter = gtest_filter(tests);
        g_ptr_array_add(argv, exact_regex(tests));
        if (filter)
          g_ptr_array_add(argv, filter);
      }
      break;
    case TEST_PYTEST: {
      char *junit = g_strconcat("--junitxml=", report, NULL);
      for (const char *const *a = (const char *const[]){ python ? python : "python3", "-m", "pytest", "-q", "-o",
                                                         "junit_logging=all", junit, NULL };
           *a; a++)
        argv_add(argv, *a);
      g_free(junit);
      for (guint i = 0; tests && i < tests->len; i++)
        argv_add(argv, ((TestCase *)tests->pdata[i])->id);
      break;
    }
    case TEST_GO:
      argv_add(argv, "go");
      argv_add(argv, "test");
      argv_add(argv, "-json");
      if (!tests) {
        argv_add(argv, "./...");
        break;
      }
      argv_add(argv, "-run");
      g_ptr_array_add(argv, exact_regex(tests));
      for (guint i = 0; i < tests->len; i++) { /* each package once */
        const TestCase *test = tests->pdata[i];
        gboolean seen = FALSE;
        for (guint j = 0; j < i && !seen; j++)
          seen = strcmp(((TestCase *)tests->pdata[j])->group, test->group) == 0;
        if (!seen)
          argv_add(argv, test->group);
      }
      break;
    default:
      argv_add(argv, "cargo");
      argv_add(argv, "test");
      argv_add(argv, "--no-fail-fast");
      add_cargo_flags(argv, cargo_flags);
      argv_add(argv, "--");
      if (tests) {
        argv_add(argv, "--exact");
        for (guint i = 0; i < tests->len; i++)
          argv_add(argv, ((TestCase *)tests->pdata[i])->id);
      }
  }
  return argv_end(argv);
}

/* Listing ------------------------------------------------------------------------------------ */

static TestCase *test_new(TestKind kind, const char *id, const char *group, const char *name)
{
  TestCase *test = g_new0(TestCase, 1);

  test->kind = kind;
  test->id = g_strdup(id);
  test->group = g_strdup(group);
  test->name = g_strdup(name);
  return test;
}

/* Where CTest's backtrace node index says a test was added (its add_test line). */
static void ctest_origin(TestCase *test, JsonObject *graph, gint64 index)
{
  JsonArray *nodes = json_object_has_member(graph, "nodes") ? json_object_get_array_member(graph, "nodes") : NULL;
  JsonArray *files = json_object_has_member(graph, "files") ? json_object_get_array_member(graph, "files") : NULL;
  JsonNode *node;
  JsonObject *object;

  if (!nodes || !files || index < 0 || index >= json_array_get_length(nodes) ||
      !JSON_NODE_HOLDS_OBJECT(node = json_array_get_element(nodes, index)))
    return;
  object = json_node_get_object(node);
  gint64 file = json_object_has_member(object, "file") ? json_object_get_int_member(object, "file") : -1;
  if (file < 0 || file >= json_array_get_length(files))
    return;
  test->file = g_strdup(json_array_get_string_element(files, file));
  test->line = json_object_has_member(object, "line") ? (int)json_object_get_int_member(object, "line") : 0;
}

/* A test of CTest's: GoogleTest's "Suite.Name" goes under its suite, others under their program. */
static void add_ctest(GPtrArray *tests, JsonObject *entry, JsonObject *graph)
{
  const char *name = json_object_has_member(entry, "name") ? json_object_get_string_member(entry, "name") : NULL;
  JsonArray *command = json_object_has_member(entry, "command") ? json_object_get_array_member(entry, "command") : NULL;
  JsonArray *properties =
      json_object_has_member(entry, "properties") ? json_object_get_array_member(entry, "properties") : NULL;
  const char *dot;
  TestCase *test;

  if (!name || !*name)
    return;
  if ((dot = strchr(name, '.')) && dot != name && dot[1]) {
    char *suite = g_strndup(name, dot - name);
    test = test_new(TEST_CTEST, name, suite, dot + 1);
    g_free(suite);
  } else {
    char *program = command && json_array_get_length(command)
                        ? g_path_get_basename(json_array_get_string_element(command, 0))
                        : g_strdup("CTest");
    test = test_new(TEST_CTEST, name, program, name);
    g_free(program);
  }
  if (command && json_array_get_length(command)) {
    guint n = json_array_get_length(command);
    test->command = g_new0(char *, n + 1);
    for (guint i = 0; i < n; i++)
      test->command[i] = g_strdup(json_array_get_string_element(command, i));
  }
  for (guint i = 0; properties && i < json_array_get_length(properties); i++) {
    JsonNode *node = json_array_get_element(properties, i);
    JsonObject *property = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    if (property && json_object_has_member(property, "name") && json_object_has_member(property, "value") &&
        g_strcmp0(json_object_get_string_member(property, "name"), "WORKING_DIRECTORY") == 0)
      g_set_str(&test->cwd, json_object_get_string_member(property, "value"));
  }
  if (graph && json_object_has_member(entry, "backtrace"))
    ctest_origin(test, graph, json_object_get_int_member(entry, "backtrace"));
  g_ptr_array_add(tests, test);
}

static void parse_ctest(GPtrArray *tests, const char *out)
{
  JsonParser *parser = json_parser_new();
  JsonNode *root;

  if (json_parser_load_from_data(parser, out, -1, NULL) && (root = json_parser_get_root(parser)) &&
      JSON_NODE_HOLDS_OBJECT(root)) {
    JsonObject *object = json_node_get_object(root);
    JsonArray *list = json_object_has_member(object, "tests") ? json_object_get_array_member(object, "tests") : NULL;
    JsonObject *graph = json_object_has_member(object, "backtraceGraph")
                            ? json_object_get_object_member(object, "backtraceGraph")
                            : NULL;
    for (guint i = 0; list && i < json_array_get_length(list); i++) {
      JsonNode *entry = json_array_get_element(list, i);
      if (JSON_NODE_HOLDS_OBJECT(entry))
        add_ctest(tests, json_node_get_object(entry), graph);
    }
  }
  g_object_unref(parser);
}

/* "tests/test_a.py::TestK::test_two[1]": under its file. */
static void parse_pytest(GPtrArray *tests, char **lines)
{
  for (int i = 0; lines[i]; i++) {
    char *line = g_strchomp(lines[i]), *sep = strstr(line, "::");
    if (!sep || sep == line || g_ascii_isspace(*line) || memchr(line, ' ', sep - line))
      continue;
    char *file = g_strndup(line, sep - line);
    TestCase *test = test_new(TEST_PYTEST, line, file, sep + 2);
    test->file = file;
    g_ptr_array_add(tests, test);
  }
}

static gboolean go_test_name(const char *line)
{
  if (!g_str_has_prefix(line, "Test") && !g_str_has_prefix(line, "Example") && !g_str_has_prefix(line, "Fuzz"))
    return FALSE;
  for (const char *c = line; *c; c++)
    if (!g_ascii_isalnum(*c) && *c != '_')
      return FALSE;
  return TRUE;
}

/* go test -list prints a package's tests, then "ok  <package> <time>". */
static void parse_go(GPtrArray *tests, char **lines)
{
  GPtrArray *names = g_ptr_array_new();

  for (int i = 0; lines[i]; i++) {
    char *line = g_strstrip(lines[i]);
    if (go_test_name(line)) {
      g_ptr_array_add(names, line);
    } else if (g_str_has_prefix(line, "ok ") || g_str_has_prefix(line, "ok\t")) {
      char **words = g_strsplit_set(line, " \t", -1);
      const char *package = NULL;
      for (int w = 1; words[w] && !package; w++)
        if (*words[w])
          package = words[w];
      for (guint n = 0; package && n < names->len; n++) {
        char *id = g_strconcat(package, "\t", names->pdata[n], NULL);
        g_ptr_array_add(tests, test_new(TEST_GO, id, package, names->pdata[n]));
        g_free(id);
      }
      g_strfreev(words);
      g_ptr_array_set_size(names, 0);
    } else if (g_str_has_prefix(line, "FAIL") || g_str_has_prefix(line, "?")) {
      g_ptr_array_set_size(names, 0);
    }
  }
  g_ptr_array_free(names, TRUE);
}

/* "tests::ok: test": under its module. */
static void parse_cargo(GPtrArray *tests, char **lines)
{
  for (int i = 0; lines[i]; i++) {
    char *line = g_strstrip(lines[i]);
    if (!g_str_has_suffix(line, ": test"))
      continue;
    line[strlen(line) - strlen(": test")] = '\0';
    if (!*line || strchr(line, ' '))
      continue;
    char *last = g_strrstr(line, "::");
    char *module = last ? g_strndup(line, last - line) : g_strdup("crate");
    g_ptr_array_add(tests, test_new(TEST_CARGO, line, module, last ? last + 2 : line));
    g_free(module);
  }
}

/* A class the JUnit listing is in. */
typedef struct {
  int indent;
  char *name;
  gboolean abstract; /* or an interface: its tests run in other classes */
} JavaClass;

static void java_class_clear(gpointer data)
{
  g_free(((JavaClass *)data)->name);
}

/* The classes a line indented by indent is no longer in. */
static void java_classes_leave(GArray *classes, int indent)
{
  while (classes->len && g_array_index(classes, JavaClass, classes->len - 1).indent >= indent)
    g_array_remove_index(classes, classes->len - 1);
}

/* grep's "./path/File.java:12:text" lines of the Java files holding a test annotation (in the order of
 * the files' lines): a test is a method declared after a test annotation, in a class that is not
 * abstract. A nested class (indented more) is "Outer$Nested"; a line indented as much as a class, or
 * less, has left it. */
static void parse_junit_list(GPtrArray *tests, char **lines)
{
  GRegex *line_re = g_regex_new("^(?:\\./)?(.+?):(\\d+):(.*)$", G_REGEX_RAW, 0, NULL);
  GRegex *package_re = g_regex_new("^package\\s+([\\w.]+)\\s*;", G_REGEX_RAW, 0, NULL);
  GRegex *class_re = g_regex_new("^(?:@[\\w.]+(?:\\([^)]*\\))?\\s+)*((?:(?:public|protected|private|static|final|"
                                 "abstract|sealed|non-sealed|strictfp)\\s+)*)(class|interface|enum|record)\\s+"
                                 "([\\w$]+)",
                                 G_REGEX_RAW, 0, NULL);
  GRegex *annotation_re = g_regex_new(JAVA_TEST_ANNOTATION, G_REGEX_RAW, 0, NULL);
  GRegex *method_re = g_regex_new(JAVA_METHOD "(\\w+)\\s*\\(", G_REGEX_RAW, 0, NULL);
  GArray *classes = g_array_new(FALSE, TRUE, sizeof(JavaClass));
  char *file = NULL, *package = NULL;
  gboolean pending = FALSE;

  g_array_set_clear_func(classes, java_class_clear);
  for (int i = 0; lines[i]; i++) {
    GMatchInfo *m = NULL;
    if (!g_regex_match(line_re, lines[i], 0, &m)) {
      g_match_info_free(m);
      continue;
    }
    char *path = g_match_info_fetch(m, 1), *number = g_match_info_fetch(m, 2), *text = g_match_info_fetch(m, 3);
    g_match_info_free(m);
    if (g_strcmp0(path, file) != 0) {
      g_set_str(&file, path);
      g_clear_pointer(&package, g_free);
      g_array_set_size(classes, 0);
      pending = FALSE;
    }
    int indent = 0;
    for (const char *c = text; *c == ' ' || *c == '\t'; c++)
      indent += *c == '\t' ? 4 : 1;
    const char *code = text + strspn(text, " \t");
    GMatchInfo *pm = NULL, *cm = NULL, *mm = NULL;
    const char *member = NULL; /* where a test method may be declared: the line, or after its class's "{" */
    int member_indent = indent;
    if (g_str_has_prefix(code, "//") || g_str_has_prefix(code, "*") || g_str_has_prefix(code, "/*")) {
      /* a comment */
    } else if (g_regex_match(package_re, code, 0, &pm)) {
      g_free(package);
      package = g_match_info_fetch(pm, 1);
    } else if (g_regex_match(class_re, code, 0, &cm)) {
      char *modifiers = g_match_info_fetch(cm, 1), *what = g_match_info_fetch(cm, 2);
      JavaClass cls = { indent, g_match_info_fetch(cm, 3),
                        strstr(modifiers, "abstract") != NULL || strcmp(what, "interface") == 0 };
      java_classes_leave(classes, indent);
      g_array_append_val(classes, cls);
      pending = FALSE;
      g_free(what);
      g_free(modifiers);
      if ((member = strchr(code, '{'))) { /* "class Inner { @Test void deep() {} }" */
        member++;
        member_indent = indent + 1;
      }
    } else {
      member = text;
    }
    if (member && (pending |= g_regex_match(annotation_re, member, 0, NULL)) &&
        g_regex_match(method_re, member, 0, &mm)) {
      pending = FALSE;
      java_classes_leave(classes, member_indent);
      if (classes->len && !g_array_index(classes, JavaClass, classes->len - 1).abstract) {
        char *method = g_match_info_fetch(mm, 1);
        GString *cls = g_string_new(package);
        if (cls->len)
          g_string_append_c(cls, '.');
        for (guint c = 0; c < classes->len; c++)
          g_string_append_printf(cls, c ? "$%s" : "%s", g_array_index(classes, JavaClass, c).name);
        char *id = g_strdup_printf("%s#%s", cls->str, method);
        TestCase *test = test_new(TEST_JUNIT, id, cls->str, method);
        test->file = g_strdup(path);
        test->line = (int)g_ascii_strtoll(number, NULL, 10);
        g_ptr_array_add(tests, test);
        g_free(id);
        g_string_free(cls, TRUE);
        g_free(method);
      }
    }
    g_match_info_free(mm);
    g_match_info_free(cm);
    g_match_info_free(pm);
    g_free(text);
    g_free(number);
    g_free(path);
  }
  g_array_unref(classes);
  g_free(package);
  g_free(file);
  g_regex_unref(method_re);
  g_regex_unref(annotation_re);
  g_regex_unref(class_re);
  g_regex_unref(package_re);
  g_regex_unref(line_re);
}

GPtrArray *test_parse_list(TestKind kind, const char *out)
{
  GPtrArray *tests = g_ptr_array_new_with_free_func(test_case_free), *unique;
  GHashTable *seen;
  char **lines;

  if (!out)
    return tests;
  if (kind == TEST_CTEST) {
    parse_ctest(tests, out);
  } else {
    lines = g_strsplit(out, "\n", -1);
    if (kind == TEST_PYTEST)
      parse_pytest(tests, lines);
    else if (kind == TEST_GO)
      parse_go(tests, lines);
    else if (kind == TEST_JUNIT)
      parse_junit_list(tests, lines);
    else
      parse_cargo(tests, lines);
    g_strfreev(lines);
  }
  /* cargo lists a test once per program it is in; a JUnit method may be overloaded */
  seen = g_hash_table_new(g_str_hash, g_str_equal);
  unique = g_ptr_array_new_with_free_func(test_case_free);
  for (guint i = 0; i < tests->len; i++) {
    TestCase *test = tests->pdata[i];
    if (g_hash_table_add(seen, test->id)) {
      g_ptr_array_add(unique, test);
      tests->pdata[i] = NULL;
    }
  }
  g_hash_table_destroy(seen);
  g_ptr_array_free(tests, TRUE);
  return unique;
}

/* Results ------------------------------------------------------------------------------------ */

typedef struct {
  TestState state;
  GString *output;
} Result;

static void result_free(gpointer data)
{
  Result *result = data;

  g_string_free(result->output, TRUE);
  g_free(result);
}

/* The result kept under key (made, and taken, when it is new). */
static Result *result_at(GHashTable *results, char *key)
{
  Result *result = g_hash_table_lookup(results, key);

  if (result) {
    g_free(key);
    return result;
  }
  result = g_new0(Result, 1);
  result->output = g_string_new(NULL);
  g_hash_table_insert(results, key, result);
  return result;
}

/* A worse result wins: a test cargo runs in two programs. */
static void set_state(Result *result, TestState state)
{
  if (result->state != TEST_FAILED)
    result->state = state;
}

/* JUnit XML (CTest, pytest, JUnit): one testcase element per test (a parameterized JUnit test: one per
 * case, the worst wins). */
typedef struct {
  GHashTable *results;
  TestKind kind;
  char *classname, *name, *message;
  GString *why, *out; /* a failure's or skip's text; system-out and system-err */
  GString *collect;   /* where text goes now, or NULL */
  TestState state;
  gboolean in_case;
} Junit;

static const char *attribute(const char **names, const char **values, const char *wanted)
{
  for (int i = 0; names[i]; i++)
    if (strcmp(names[i], wanted) == 0)
      return values[i];
  return NULL;
}

static void junit_start(GMarkupParseContext *context G_GNUC_UNUSED, const char *element, const char **names,
                        const char **values, gpointer data, GError **error G_GNUC_UNUSED)
{
  Junit *junit = data;

  if (strcmp(element, "testcase") == 0) {
    junit->in_case = TRUE;
    junit->state = TEST_PASSED;
    g_set_str(&junit->classname, attribute(names, values, "classname"));
    g_set_str(&junit->name, attribute(names, values, "name"));
    g_clear_pointer(&junit->message, g_free);
    g_string_truncate(junit->why, 0);
    g_string_truncate(junit->out, 0);
    return;
  }
  if (!junit->in_case)
    return;
  if (strcmp(element, "failure") == 0 || strcmp(element, "error") == 0 || strcmp(element, "skipped") == 0) {
    junit->state = element[0] == 's' ? TEST_SKIPPED : TEST_FAILED;
    g_set_str(&junit->message, attribute(names, values, "message"));
    junit->collect = junit->why;
  } else if (strcmp(element, "system-out") == 0 || strcmp(element, "system-err") == 0) {
    junit->collect = junit->out;
  }
}

/* pytest's "tests/test_a.py::TestK::test_two[1]" as its JUnit classname and name:
 * "tests.test_a.TestK::test_two[1]". */
static char *pytest_key(const char *id)
{
  const char *sep = strstr(id, "::");
  GString *key;
  char **parts;
  guint n;

  if (!sep)
    return g_strdup(id);
  key = g_string_new_len(id, sep - id);
  if (g_str_has_suffix(key->str, ".py"))
    g_string_truncate(key, key->len - 3);
  g_string_replace(key, "/", ".", 0);
  parts = g_strsplit(sep + 2, "::", -1);
  n = g_strv_length(parts);
  for (guint i = 0; i + 1 < n; i++)
    g_string_append_printf(key, ".%s", parts[i]);
  g_string_append_printf(key, "::%s", n ? parts[n - 1] : "");
  g_strfreev(parts);
  return g_string_free(key, FALSE);
}

static void junit_end(GMarkupParseContext *context G_GNUC_UNUSED, const char *element, gpointer data,
                      GError **error G_GNUC_UNUSED)
{
  Junit *junit = data;

  if (strcmp(element, "testcase") != 0) {
    if (strcmp(element, "failure") == 0 || strcmp(element, "error") == 0 || strcmp(element, "skipped") == 0 ||
        strcmp(element, "system-out") == 0 || strcmp(element, "system-err") == 0)
      junit->collect = NULL;
    return;
  }
  junit->in_case = FALSE;
  if (!junit->name)
    return;
  char *key;
  if (junit->kind == TEST_PYTEST) {
    key = g_strconcat(junit->classname ? junit->classname : "", "::", junit->name, NULL);
  } else if (junit->kind == TEST_JUNIT) { /* "method", Gradle's "method()", a parameterized "method(int)[1]" */
    gsize len = strcspn(junit->name, "([");
    key = g_strdup_printf("%s#%.*s", junit->classname ? junit->classname : "", (int)len, junit->name);
  } else {
    key = g_strdup(junit->name);
  }
  Result *result = result_at(junit->results, key);
  set_state(result, junit->state);
  /* CTest's message is only "Failed": the text says more */
  if (junit->why->len)
    g_string_append(result->output, junit->why->str);
  else if (junit->message && junit->state != TEST_PASSED && strcmp(junit->message, "Failed") != 0)
    g_string_append(result->output, junit->message);
  if (result->output->len && result->output->str[result->output->len - 1] != '\n')
    g_string_append_c(result->output, '\n');
  g_string_append(result->output, junit->out->str);
}

static void junit_text(GMarkupParseContext *context G_GNUC_UNUSED, const char *text, gsize len, gpointer data,
                       GError **error G_GNUC_UNUSED)
{
  Junit *junit = data;

  if (junit->collect)
    g_string_append_len(junit->collect, text, len);
}

static void parse_junit(GHashTable *results, TestKind kind, const char *report)
{
  static const GMarkupParser parser = { junit_start, junit_end, junit_text, NULL, NULL };
  Junit junit = { .results = results, .kind = kind, .why = g_string_new(NULL), .out = g_string_new(NULL) };
  GMarkupParseContext *context = g_markup_parse_context_new(&parser, 0, &junit, NULL);

  /* A broken report keeps what came before the break. */
  if (g_markup_parse_context_parse(context, report, -1, NULL))
    g_markup_parse_context_end_parse(context, NULL);
  g_markup_parse_context_free(context);
  g_free(junit.classname);
  g_free(junit.name);
  g_free(junit.message);
  g_string_free(junit.why, TRUE);
  g_string_free(junit.out, TRUE);
}

/* go test -json: an event per line; a subtest's ("TestA/case") belongs to its test. */
static void parse_go_events(GHashTable *results, const char *out)
{
  char **lines = g_strsplit(out, "\n", -1);
  JsonParser *parser = json_parser_new();

  for (int i = 0; lines[i]; i++) {
    JsonNode *root;
    if (lines[i][0] != '{' || !json_parser_load_from_data(parser, lines[i], -1, NULL) ||
        !(root = json_parser_get_root(parser)) || !JSON_NODE_HOLDS_OBJECT(root))
      continue;
    JsonObject *event = json_node_get_object(root);
    const char *action = json_object_get_string_member_with_default(event, "Action", "");
    const char *package = json_object_get_string_member_with_default(event, "Package", "");
    const char *test = json_object_get_string_member_with_default(event, "Test", NULL);
    if (!test)
      continue;
    const char *slash = strchr(test, '/');
    char *key = g_strdup_printf("%s\t%.*s", package, slash ? (int)(slash - test) : (int)strlen(test), test);
    Result *result = result_at(results, key);
    if (strcmp(action, "output") == 0)
      g_string_append(result->output, json_object_get_string_member_with_default(event, "Output", ""));
    else if (!slash && strcmp(action, "pass") == 0)
      set_state(result, TEST_PASSED);
    else if (!slash && strcmp(action, "fail") == 0)
      set_state(result, TEST_FAILED);
    else if (!slash && strcmp(action, "skip") == 0)
      set_state(result, TEST_SKIPPED);
  }
  g_object_unref(parser);
  g_strfreev(lines);
}

/* cargo test: "test tests::ok ... ok", then each failure's "---- tests::bad stdout ----" and its output. */
static void parse_cargo_results(GHashTable *results, const char *out)
{
  char **lines = g_strsplit(out, "\n", -1);
  Result *block = NULL;

  for (int i = 0; lines[i]; i++) {
    char *line = lines[i], *dots;
    if (g_str_has_prefix(line, "---- ") && g_str_has_suffix(line, " ----")) {
      char *name = g_strndup(line + 5, strlen(line) - 10), *space = strrchr(name, ' ');
      if (space) /* " stdout" */
        *space = '\0';
      block = result_at(results, name);
      continue;
    }
    if (block && (strcmp(line, "failures:") == 0 || g_str_has_prefix(line, "test result:"))) {
      block = NULL;
      continue;
    }
    if (block) {
      g_string_append(block->output, line);
      g_string_append_c(block->output, '\n');
      continue;
    }
    if (!g_str_has_prefix(line, "test ") || !(dots = strstr(line, " ... ")))
      continue;
    Result *result = result_at(results, g_strndup(line + 5, dots - line - 5));
    const char *word = dots + 5;
    if (g_str_has_prefix(word, "ok"))
      set_state(result, TEST_PASSED);
    else if (g_str_has_prefix(word, "FAILED"))
      set_state(result, TEST_FAILED);
    else if (g_str_has_prefix(word, "ignored"))
      set_state(result, TEST_SKIPPED);
  }
  g_strfreev(lines);
}

guint test_apply_results(TestKind kind, GPtrArray *tests, const char *out, const char *report)
{
  GHashTable *results = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, result_free);
  guint applied = 0;

  if (kind == TEST_CTEST || kind == TEST_PYTEST || kind == TEST_JUNIT) {
    if (report)
      parse_junit(results, kind, report);
  } else if (kind == TEST_GO) {
    parse_go_events(results, out ? out : "");
  } else {
    parse_cargo_results(results, out ? out : "");
  }
  for (guint i = 0; i < tests->len; i++) {
    TestCase *test = tests->pdata[i];
    if (test->kind != kind)
      continue;
    char *key = kind == TEST_PYTEST ? pytest_key(test->id) : g_strdup(test->id);
    Result *result = g_hash_table_lookup(results, key);
    g_free(key);
    if (!result || result->state == TEST_UNKNOWN)
      continue;
    test->state = result->state;
    g_free(test->output);
    test->output = result->output->len ? g_strdup(result->output->str) : NULL;
    applied++;
  }
  g_hash_table_destroy(results);
  return applied;
}

/* Debugging ---------------------------------------------------------------------------------- */

char **test_build_argv(const TestSource *source)
{
  GPtrArray *argv = argv_new();

  argv_add(argv, "cmake");
  argv_add(argv, "--build");
  argv_add(argv, source->dir);
  return argv_end(argv);
}

gboolean test_is_program(const char *path)
{
  char magic[4] = { 0 };
  FILE *file;
  gboolean elf;

  if (!path || !g_path_is_absolute(path) || !g_file_test(path, G_FILE_TEST_IS_EXECUTABLE) ||
      g_file_test(path, G_FILE_TEST_IS_DIR) || !(file = fopen(path, "rb")))
    return FALSE;
  elf = fread(magic, 1, sizeof magic, file) == sizeof magic && memcmp(magic,
                                                                      "\x7f"
                                                                      "ELF",
                                                                      4) == 0;
  fclose(file);
  return elf;
}

/* Where a test is defined -------------------------------------------------------------------- */

/* GoogleTest's suite and test of a CTest name: "Prefix/Suite.Name/0" or "Suite/0.Name" (parameterized
 * and typed tests) give Suite and Name; FALSE for a name without a dot. */
static gboolean gtest_parts(const char *id, char **suite, char **name)
{
  const char *dot = strchr(id, '.'), *start = id, *end;

  if (!dot || dot == id || !dot[1])
    return FALSE;
  for (const char *c = id; c < dot; c++) /* a prefix of instantiation */
    if (*c == '/' && c + 1 < dot && !g_ascii_isdigit(c[1]))
      start = c + 1;
  for (end = start; end < dot && *end != '/'; end++)
    ;
  *suite = g_strndup(start, end - start);
  end = strchr(dot + 1, '/');
  *name = end ? g_strndup(dot + 1, end - dot - 1) : g_strdup(dot + 1);
  return **suite && **name;
}

/* The pattern of the line that defines test, or NULL. */
static char *definition_pattern(const TestCase *test)
{
  char *suite = NULL, *name = NULL, *pattern = NULL;
  const char *start, *end;

  switch (test->kind) {
    case TEST_CTEST:
      if (gtest_parts(test->id, &suite, &name)) {
        char *s = g_regex_escape_string(suite, -1), *n = g_regex_escape_string(name, -1);
        pattern = g_strdup_printf("^[ \\t]*(TYPED_)?TEST(_F|_P)?\\s*\\(\\s*%s\\s*,\\s*%s\\s*\\)", s, n);
        g_free(n);
        g_free(s);
      }
      break;
    case TEST_PYTEST: /* "file.py::Class::test_a[1]" */
      start = g_strrstr(test->id, "::");
      start = start ? start + 2 : test->id;
      end = strchr(start, '[');
      name = g_strndup(start, end ? (gsize)(end - start) : strlen(start));
      break;
    case TEST_GO: /* "package\tTestName/subtest" */
      start = strchr(test->id, '\t');
      start = start ? start + 1 : test->id;
      end = strchr(start, '/');
      name = g_strndup(start, end ? (gsize)(end - start) : strlen(start));
      break;
    case TEST_CARGO: /* "module::name" */
      start = g_strrstr(test->id, "::");
      name = g_strdup(start ? start + 2 : test->id);
      break;
    case TEST_JUNIT: /* "package.Class#method" */
      start = strchr(test->id, '#');
      name = g_strdup(start ? start + 1 : test->id);
      break;
    default: break;
  }
  if (!pattern && name && *name) {
    char *n = g_regex_escape_string(name, -1);
    if (test->kind == TEST_PYTEST)
      pattern = g_strdup_printf("^[ \\t]*(async\\s+)?def\\s+%s\\s*\\(", n);
    else if (test->kind == TEST_GO)
      pattern = g_strdup_printf("^func\\s+%s\\s*\\(", n);
    else if (test->kind == TEST_CARGO)
      pattern = g_strdup_printf("^[ \\t]*(pub(\\([^)]*\\))?\\s+)?(async\\s+)?fn\\s+%s\\s*[<(]", n);
    else if (test->kind == TEST_JUNIT)
      pattern = g_strdup_printf(JAVA_METHOD "%s\\s*\\(", n);
    g_free(n);
  }
  g_free(suite);
  g_free(name);
  return pattern;
}

static int line_in(GRegex *regex, const char *text)
{
  GMatchInfo *match = NULL;
  int line = 0, start = 0;

  if (g_regex_match(regex, text, 0, &match) && g_match_info_fetch_pos(match, 0, &start, NULL)) {
    line = 1;
    for (const char *c = text; c < text + start; c++)
      line += *c == '\n';
  }
  g_match_info_free(match);
  return line;
}

static GRegex *definition_regex(const TestCase *test)
{
  char *pattern = definition_pattern(test);
  GRegex *regex = pattern ? g_regex_new(pattern, G_REGEX_MULTILINE | G_REGEX_RAW, 0, NULL) : NULL;

  g_free(pattern);
  return regex;
}

int test_definition_line(const TestCase *test, const char *text)
{
  GRegex *regex = definition_regex(test);
  int line = regex && text ? line_in(regex, text) : 0;

  if (regex)
    g_regex_unref(regex);
  return line;
}

/* Whether a file of this name may define a test of kind. */
static gboolean source_of_kind(TestKind kind, const char *name)
{
  static const char *const cpp[] = { ".cc", ".cpp", ".cxx", ".c++", ".C", ".c", NULL };

  switch (kind) {
    case TEST_CTEST:
      for (int i = 0; cpp[i]; i++)
        if (g_str_has_suffix(name, cpp[i]))
          return TRUE;
      return FALSE;
    case TEST_PYTEST: return g_str_has_suffix(name, ".py");
    case TEST_GO: return g_str_has_suffix(name, "_test.go");
    case TEST_CARGO: return g_str_has_suffix(name, ".rs");
    case TEST_JUNIT: return g_str_has_suffix(name, ".java");
    default: return FALSE;
  }
}

/* Folders where no test is defined: hidden ones, dependencies, build output (a CMake build folder). */
static gboolean skipped_folder(const char *path, const char *name)
{
  static const char *const names[] = { "node_modules", "target", "__pycache__", "venv", "vendor", NULL };

  if (name[0] == '.' || g_strv_contains(names, name))
    return TRUE;
  char *cache = g_build_filename(path, "CMakeCache.txt", NULL);
  gboolean build = g_file_test(cache, G_FILE_TEST_EXISTS);
  g_free(cache);
  return build;
}

typedef struct {
  const TestCase *test;
  GRegex *regex;
  const char *root;
  guint files; /* read so far */
  char *found; /* the first file defining it */
  int line;
  gboolean done; /* go: found in the folder of its package */
} Search;

/* Whether the folder at path (relative rel) is that of the go package of id ("example.com/m/sub"). */
static gboolean go_package_folder(const char *id, const char *rel)
{
  const char *tab = strchr(id, '\t');
  char *package = tab ? g_strndup(id, tab - id) : g_strdup(id);
  gboolean same = *rel ? g_str_has_suffix(package, rel) &&
                             (strlen(package) == strlen(rel) || package[strlen(package) - strlen(rel) - 1] == '/')
                       : FALSE;

  g_free(package);
  return same;
}

static void search_folder(Search *search, const char *path, int depth)
{
  GDir *dir = depth <= SOURCE_MAX_DEPTH ? g_dir_open(path, 0, NULL) : NULL;
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  const char *name;

  if (!dir) {
    g_ptr_array_free(names, TRUE);
    return;
  }
  while ((name = g_dir_read_name(dir)))
    g_ptr_array_add(names, g_strdup(name));
  g_dir_close(dir);
  g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0); /* the same answer each time */
  for (guint pass = 0; pass < 2; pass++)                   /* the folder's files, then its folders */
    for (guint i = 0; i < names->len && !search->done && search->files < SOURCE_MAX_FILES; i++) {
      char *child = g_build_filename(path, names->pdata[i], NULL);
      gboolean folder = g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK);
      if (pass == 0 && !folder && source_of_kind(search->test->kind, names->pdata[i])) {
        char *text = NULL;
        gsize size = 0;
        search->files++;
        GStatBuf st;
        if (g_stat(child, &st) == 0 && st.st_size <= SOURCE_MAX_BYTES &&
            g_file_get_contents(child, &text, &size, NULL)) {
          int line = line_in(search->regex, text);
          const char *rel = path + strlen(search->root) + (path[strlen(search->root)] == '/');
          if (line && search->test->kind == TEST_GO && go_package_folder(search->test->id, rel)) {
            g_set_str(&search->found, child);
            search->line = line;
            search->done = TRUE;
          } else if (line && !search->found) {
            search->found = g_strdup(child);
            search->line = line;
            search->done = search->test->kind != TEST_GO;
          }
        }
        g_free(text);
      } else if (pass == 1 && folder && !skipped_folder(child, names->pdata[i])) {
        search_folder(search, child, depth + 1);
      }
      g_free(child);
    }
  g_ptr_array_free(names, TRUE);
}

char *test_find_source(const char *root, const TestCase *test, int *line)
{
  Search search = { test, NULL, root, 0, NULL, 0, FALSE };
  char *listed = NULL;

  *line = 0;
  if (test->file) /* pytest's and JUnit's file is relative to the project; CTest's add_test absolute */
    listed = g_path_is_absolute(test->file) ? g_strdup(test->file) : g_build_filename(root, test->file, NULL);
  if (listed && (test->kind == TEST_PYTEST || test->kind == TEST_JUNIT)) {
    char *text = NULL;
    if (g_file_get_contents(listed, &text, NULL, NULL))
      *line = test_definition_line(test, text);
    g_free(text);
    return listed;
  }
  if ((search.regex = definition_regex(test))) {
    search_folder(&search, root, 0);
    g_regex_unref(search.regex);
  }
  if (search.found) {
    g_free(listed);
    *line = search.line;
    return search.found;
  }
  if (listed) /* CTest: where add_test is */
    *line = test->line;
  return listed;
}

/* The lines defining tests in a file ----------------------------------------------------------- */

static void add_key(GHashTable *keys, char *key, guint index)
{
  GArray *indices = g_hash_table_lookup(keys, key);

  if (!indices) {
    indices = g_array_new(FALSE, FALSE, sizeof(guint));
    g_hash_table_insert(keys, key, indices);
  } else {
    g_free(key);
  }
  g_array_append_val(indices, index);
}

/* The go module of the project at root (go.mod's "module" line), or NULL. */
static char *go_module(const char *root)
{
  char *path = g_build_filename(root, "go.mod", NULL), *text = NULL, *module = NULL;

  if (g_file_get_contents(path, &text, NULL, NULL)) {
    char **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i] && !module; i++) {
      char *line = g_strstrip(lines[i]);
      if (g_str_has_prefix(line, "module") && g_ascii_isspace(line[6]))
        module = g_strdup(g_strstrip(line + 7));
    }
    g_strfreev(lines);
  }
  g_free(text);
  g_free(path);
  return module;
}

/* Adds the names test is defined under in the file rel (relative to the project) to keys, for kind's
 * definitions in it: GoogleTest's "Suite.Name", pytest's function and class names (when its id is of
 * this file), go's function (when its package is the file's folder: package), Rust's function, JUnit's
 * method (when the listing found it in this file). */
static void add_test_keys(GHashTable *keys, const TestCase *test, guint index, const char *rel, const char *package)
{
  char *suite = NULL, *name = NULL;
  const char *start, *end;

  switch (test->kind) {
    case TEST_CTEST:
      if (gtest_parts(test->id, &suite, &name))
        add_key(keys, g_strdup_printf("%s.%s", suite, name), index);
      break;
    case TEST_PYTEST: { /* "file.py::Class::test_a[1]" */
      char **parts = g_strsplit(test->id, "::", -1);
      if (parts[0] && strcmp(parts[0], rel) == 0)
        for (int i = 1; parts[i]; i++) {
          end = strchr(parts[i], '[');
          add_key(keys, g_strndup(parts[i], end ? (gsize)(end - parts[i]) : strlen(parts[i])), index);
        }
      g_strfreev(parts);
      break;
    }
    case TEST_GO: /* "package\tTestName" */
      start = strchr(test->id, '\t');
      if (start && package && strlen(package) == (gsize)(start - test->id) &&
          strncmp(test->id, package, start - test->id) == 0) {
        end = strchr(start + 1, '/');
        add_key(keys, g_strndup(start + 1, end ? (gsize)(end - start - 1) : strlen(start + 1)), index);
      }
      break;
    case TEST_CARGO: /* "module::name" */
      start = g_strrstr(test->id, "::");
      add_key(keys, g_strdup(start ? start + 2 : test->id), index);
      break;
    case TEST_JUNIT: /* "package.Class#method", when the listing found it in this file */
      start = strchr(test->id, '#');
      if (start && g_strcmp0(test->file, rel) == 0)
        add_key(keys, g_strdup(start + 1), index);
      break;
    default: break;
  }
  g_free(suite);
  g_free(name);
}

/* The definitions kind's files hold: the names are the pattern's groups (two: "1.2"). */
static const char *const definition_patterns[TEST_KINDS] = {
  [TEST_CTEST] = "^[ \\t]*(?:TYPED_)?TEST(?:_F|_P)?\\s*\\(\\s*(\\w+)\\s*,\\s*(\\w+)\\s*\\)",
  [TEST_PYTEST] = "^[ \\t]*(?:(?:async\\s+)?def|class)\\s+(\\w+)\\s*[(:]",
  [TEST_GO] = "^func\\s+(\\w+)\\s*\\(",
  [TEST_CARGO] = "^[ \\t]*(?:pub(?:\\([^)]*\\))?\\s+)?(?:async\\s+)?fn\\s+(\\w+)\\s*[<(]",
  [TEST_JUNIT] = JAVA_METHOD "(\\w+)\\s*\\(",
};

/* Adds the lines of text where kind's definitions in keys are. */
static void add_definitions(GArray *lines, TestKind kind, GHashTable *keys, const char *text)
{
  GRegex *regex = g_regex_new(definition_patterns[kind], G_REGEX_MULTILINE | G_REGEX_RAW, 0, NULL);
  GMatchInfo *match = NULL;
  const char *counted = text;
  int line = 1;

  for (g_regex_match(regex, text, 0, &match); g_match_info_matches(match); g_match_info_next(match, NULL)) {
    int start = 0;
    char *first = g_match_info_fetch(match, 1), *second = g_match_info_fetch(match, 2), *key;
    key = second && *second ? g_strdup_printf("%s.%s", first, second) : g_strdup(first);
    GArray *indices = g_hash_table_lookup(keys, key);
    g_match_info_fetch_pos(match, 0, &start, NULL);
    for (; counted < text + start; counted++)
      line += *counted == '\n';
    for (guint i = 0; indices && i < indices->len; i++)
      g_array_append_val(lines, ((TestLine){ line, g_array_index(indices, guint, i) }));
    g_free(key);
    g_free(second);
    g_free(first);
  }
  g_match_info_free(match);
  g_regex_unref(regex);
}

static int by_line(gconstpointer a, gconstpointer b)
{
  const TestLine *x = a, *y = b;

  return x->line != y->line ? (x->line > y->line) - (x->line < y->line) : (x->test > y->test) - (x->test < y->test);
}

GArray *test_lines(GPtrArray *tests, const char *root, const char *path, const char *text)
{
  GArray *lines = g_array_new(FALSE, FALSE, sizeof(TestLine));
  const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
  gsize root_len = root ? strlen(root) : 0;
  const char *rel = root && strncmp(path, root, root_len) == 0 && path[root_len] == '/' ? path + root_len + 1 : path;
  char *package = NULL;

  for (TestKind kind = 0; kind < TEST_KINDS && text; kind++) {
    GHashTable *keys = NULL;
    for (guint i = 0; i < tests->len; i++) {
      const TestCase *test = tests->pdata[i];
      if (test->kind != kind)
        continue;
      if (kind == TEST_CTEST && test->file && test->line > 0 && strcmp(test->file, path) == 0) /* add_test */
        g_array_append_val(lines, ((TestLine){ test->line, i }));
      if (!source_of_kind(kind, name))
        continue;
      if (kind == TEST_GO && !package && root) {
        char *module = go_module(root), *folder = g_path_get_dirname(rel);
        package = !module                    ? g_strdup("")
                  : strcmp(folder, ".") == 0 ? g_strdup(module)
                                             : g_strdup_printf("%s/%s", module, folder);
        g_free(folder);
        g_free(module);
      }
      if (!keys)
        keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
      add_test_keys(keys, test, i, rel, package);
    }
    if (keys && g_hash_table_size(keys))
      add_definitions(lines, kind, keys, text);
    if (keys)
      g_hash_table_destroy(keys);
  }
  g_free(package);
  g_array_sort(lines, by_line);
  return lines;
}

gboolean test_file_may_define(const char *path)
{
  const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;

  if (strcmp(name, "CMakeLists.txt") == 0)
    return TRUE;
  for (TestKind kind = 0; kind < TEST_KINDS; kind++)
    if (source_of_kind(kind, name))
      return TRUE;
  return FALSE;
}

/* GoogleTest programs that CTest runs as one test ------------------------------------------------ */

char **test_gtest_list_argv(const TestCase *test)
{
  GPtrArray *argv;

  if (test->kind != TEST_CTEST || test->runner || !test->command || !test->command[0] ||
      !test_is_program(test->command[0]))
    return NULL;
  for (int i = 1; test->command[i]; i++)
    if (g_str_has_prefix(test->command[i], "--gtest_"))
      return NULL; /* gtest_discover_tests' (one case) or the project's own choice */
  argv = argv_new();
  argv_add(argv, "sh");
  argv_add(argv, "-c");
  /* a program linked with GoogleTest: its help text, its library or its mangled names */
  argv_add(argv, "grep -qaE 'gtest_list_tests|libgtest|_ZN7testing8internal' \"$1\" && exec \"$@\" --gtest_list_tests");
  argv_add(argv, "sh");
  for (int i = 0; test->command[i]; i++)
    argv_add(argv, test->command[i]);
  return argv_end(argv);
}

GPtrArray *test_parse_gtest_list(const TestCase *runner, const char *out)
{
  GPtrArray *cases = g_ptr_array_new_with_free_func(test_case_free);
  char **lines = g_strsplit(out ? out : "", "\n", -1);
  char *suite = NULL;

  for (int i = 0; lines[i]; i++) {
    char *line = lines[i], *comment = strstr(line, "  #");
    if (comment)
      *comment = '\0';
    g_strchomp(line);
    if (!*line)
      continue;
    if (!g_ascii_isspace(*line)) { /* "Suite." */
      g_free(suite);
      suite = g_str_has_suffix(line, ".") && !strchr(line, ' ') ? g_strndup(line, strlen(line) - 1) : NULL;
      continue;
    }
    char *name = g_strchug(line);
    if (!suite || !*name || strchr(name, ' '))
      continue;
    char *id = g_strdup_printf("%s.%s", suite, name), *filter = g_strconcat("--gtest_filter=", id, NULL);
    TestCase *test = test_new(TEST_CTEST, id, suite, name);
    guint n = g_strv_length(runner->command);
    test->command = g_new0(char *, n + 2);
    for (guint j = 0; j < n; j++)
      test->command[j] = g_strdup(runner->command[j]);
    test->command[n] = filter;
    test->cwd = g_strdup(runner->cwd);
    test->file = g_strdup(runner->file);
    test->line = runner->line;
    test->runner = g_strdup(runner->id);
    g_ptr_array_add(cases, test);
    g_free(id);
  }
  g_free(suite);
  g_strfreev(lines);
  return cases;
}

static const char *member_string(JsonObject *object, const char *name)
{
  JsonNode *node = json_object_get_member(object, name);

  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
             ? json_node_get_string(node)
             : NULL;
}

static JsonArray *member_array(JsonObject *object, const char *name)
{
  JsonNode *node = json_object_get_member(object, name);

  return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

guint test_apply_gtest(GPtrArray *tests, const char *json)
{
  JsonParser *parser = json_parser_new();
  JsonNode *root;
  guint applied = 0;

  if (!json || !json_parser_load_from_data(parser, json, -1, NULL) || !(root = json_parser_get_root(parser)) ||
      !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return 0;
  }
  JsonArray *suites = member_array(json_node_get_object(root), "testsuites");
  for (guint s = 0; suites && s < json_array_get_length(suites); s++) {
    JsonNode *snode = json_array_get_element(suites, s);
    JsonObject *suite = JSON_NODE_HOLDS_OBJECT(snode) ? json_node_get_object(snode) : NULL;
    JsonArray *cases = suite ? member_array(suite, "testsuite") : NULL;
    const char *suite_name = suite ? member_string(suite, "name") : NULL;
    for (guint c = 0; suite_name && cases && c < json_array_get_length(cases); c++) {
      JsonNode *cnode = json_array_get_element(cases, c);
      JsonObject *one = JSON_NODE_HOLDS_OBJECT(cnode) ? json_node_get_object(cnode) : NULL;
      const char *name = one ? member_string(one, "name") : NULL;
      if (!name)
        continue;
      char *id = g_strdup_printf("%s.%s", suite_name, name);
      JsonArray *failures = member_array(one, "failures");
      GString *why = g_string_new(NULL);
      for (guint f = 0; failures && f < json_array_get_length(failures); f++) {
        JsonNode *fnode = json_array_get_element(failures, f);
        const char *text = JSON_NODE_HOLDS_OBJECT(fnode) ? member_string(json_node_get_object(fnode), "failure") : NULL;
        if (text)
          g_string_append_printf(why, "%s%s", why->len ? "\n\n" : "", text);
      }
      TestState state = failures && json_array_get_length(failures)               ? TEST_FAILED
                        : g_strcmp0(member_string(one, "result"), "SKIPPED") == 0 ? TEST_SKIPPED
                                                                                  : TEST_PASSED;
      for (guint i = 0; i < tests->len; i++) {
        TestCase *test = tests->pdata[i];
        if (!test->runner || strcmp(test->id, id) != 0)
          continue;
        test->state = state;
        g_free(test->output);
        test->output = why->len ? g_strdup(why->str) : NULL;
        applied++;
      }
      g_string_free(why, TRUE);
      g_free(id);
    }
  }
  g_object_unref(parser);
  return applied;
}
