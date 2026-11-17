/*
 * The test frameworks (see frameworks.h).
 */
#include "testing/frameworks.h"

#include <json-glib/json-glib.h>
#include <string.h>

void test_case_free(gpointer data)
{
  TestCase *test = data;

  if (!test)
    return;
  g_free(test->id);
  g_free(test->group);
  g_free(test->name);
  g_free(test->output);
  g_free(test);
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
  static const char *const names[TEST_KINDS] = { "CTest", "pytest", "go test", "cargo test" };

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

char **test_list_argv(const TestSource *source, const char *python, const char *cargo_flags)
{
  GPtrArray *argv = argv_new();

  switch (source->kind) {
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

/* "^(a|b)$" for the tests' names (after the tab for go test's "package\tname"). */
static char *exact_regex(GPtrArray *tests)
{
  GString *regex = g_string_new("^(");

  for (guint i = 0; i < tests->len; i++) {
    const TestCase *test = tests->pdata[i];
    const char *tab = strchr(test->id, '\t');
    if (i)
      g_string_append_c(regex, '|');
    add_escaped(regex, tab ? tab + 1 : test->id);
  }
  g_string_append(regex, ")$");
  return g_string_free(regex, FALSE);
}

char **test_run_argv(const TestSource *source, const char *python, const char *cargo_flags, GPtrArray *tests,
                     const char *report)
{
  GPtrArray *argv = argv_new();

  switch (source->kind) {
    case TEST_CTEST:
      /* What runs is what is built: build first, as Build would. */
      argv_add(argv, "sh");
      argv_add(argv, "-c");
      argv_add(argv, "cmake --build \"$1\" && exec ctest --test-dir \"$1\" --output-junit \"$2\" ${3:+-R \"$3\"}");
      argv_add(argv, "sh");
      argv_add(argv, source->dir);
      argv_add(argv, report);
      if (tests)
        g_ptr_array_add(argv, exact_regex(tests));
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

/* A test of CTest's: GoogleTest's "Suite.Name" goes under its suite, others under their program. */
static void add_ctest(GPtrArray *tests, JsonObject *entry)
{
  const char *name = json_object_has_member(entry, "name") ? json_object_get_string_member(entry, "name") : NULL;
  JsonArray *command = json_object_has_member(entry, "command") ? json_object_get_array_member(entry, "command") : NULL;
  const char *dot;

  if (!name || !*name)
    return;
  if ((dot = strchr(name, '.')) && dot != name && dot[1]) {
    char *suite = g_strndup(name, dot - name);
    g_ptr_array_add(tests, test_new(TEST_CTEST, name, suite, dot + 1));
    g_free(suite);
  } else {
    char *program = command && json_array_get_length(command)
                        ? g_path_get_basename(json_array_get_string_element(command, 0))
                        : g_strdup("CTest");
    g_ptr_array_add(tests, test_new(TEST_CTEST, name, program, name));
    g_free(program);
  }
}

static void parse_ctest(GPtrArray *tests, const char *out)
{
  JsonParser *parser = json_parser_new();
  JsonNode *root;

  if (json_parser_load_from_data(parser, out, -1, NULL) && (root = json_parser_get_root(parser)) &&
      JSON_NODE_HOLDS_OBJECT(root)) {
    JsonObject *object = json_node_get_object(root);
    JsonArray *list = json_object_has_member(object, "tests") ? json_object_get_array_member(object, "tests") : NULL;
    for (guint i = 0; list && i < json_array_get_length(list); i++) {
      JsonNode *entry = json_array_get_element(list, i);
      if (JSON_NODE_HOLDS_OBJECT(entry))
        add_ctest(tests, json_node_get_object(entry));
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
    g_ptr_array_add(tests, test_new(TEST_PYTEST, line, file, sep + 2));
    g_free(file);
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
    else
      parse_cargo(tests, lines);
    g_strfreev(lines);
  }
  /* cargo lists a test once per program it is in */
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

/* JUnit XML (CTest, pytest): one testcase element per test. */
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
  char *key = junit->kind == TEST_PYTEST
                  ? g_strconcat(junit->classname ? junit->classname : "", "::", junit->name, NULL)
                  : g_strdup(junit->name);
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

  if (kind == TEST_CTEST || kind == TEST_PYTEST) {
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
