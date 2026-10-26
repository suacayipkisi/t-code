/* Tests for src/search/searchmatch.c: command lines, match finding, parsing and snippets, and real
 * ripgrep / grep runs on a temporary folder. */
#include "search/searchmatch.h"
#include "util.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

static gboolean has_arg(char **argv, const char *arg)
{
  return g_strv_contains((const char *const *)argv, arg);
}

/* argv has a and b next to each other. */
static gboolean has_pair(char **argv, const char *a, const char *b)
{
  for (int i = 0; argv[i] && argv[i + 1]; i++)
    if (strcmp(argv[i], a) == 0 && strcmp(argv[i + 1], b) == 0)
      return TRUE;
  return FALSE;
}

static int count_arg(char **argv, const char *arg)
{
  int n = 0;

  for (int i = 0; argv[i]; i++)
    n += strcmp(argv[i], arg) == 0;
  return n;
}

static void test_argv_rg_defaults(void)
{
  SearchQuery q = { .text = "-foo" };
  char **argv = search_argv(&q, TRUE);
  int n = (int)g_strv_length(argv);

  g_assert_cmpstr(argv[0], ==, "rg");
  g_assert_true(has_arg(argv, "--ignore-case"));
  g_assert_true(has_arg(argv, "--fixed-strings"));
  g_assert_true(has_arg(argv, "--null"));
  g_assert_true(has_pair(argv, "--sort", "path"));
  g_assert_false(has_arg(argv, "--word-regexp"));
  g_assert_false(has_arg(argv, "--glob"));
  /* The text comes after "--", so one starting with '-' is not read as an option. */
  g_assert_cmpstr(argv[n - 3], ==, "--");
  g_assert_cmpstr(argv[n - 2], ==, "-foo");
  g_assert_cmpstr(argv[n - 1], ==, ".");
  g_strfreev(argv);
}

static void test_argv_rg_options(void)
{
  SearchQuery q = { .text = "a.b",
                    .match_case = TRUE,
                    .whole_word = TRUE,
                    .regex = TRUE,
                    .include = " src , *.c,,./doc/, lib/** ",
                    .exclude = "build, ./out/" };
  char **argv = search_argv(&q, TRUE);

  g_assert_true(has_arg(argv, "--case-sensitive"));
  g_assert_false(has_arg(argv, "--ignore-case"));
  g_assert_false(has_arg(argv, "--fixed-strings"));
  g_assert_true(has_arg(argv, "--word-regexp"));
  g_assert_true(has_pair(argv, "--glob", "src"));
  g_assert_true(has_pair(argv, "--glob", "src/**"));
  g_assert_true(has_pair(argv, "--glob", "*.c"));
  g_assert_false(has_arg(argv, "*.c/**"));
  g_assert_true(has_pair(argv, "--glob", "doc"));
  g_assert_true(has_pair(argv, "--glob", "doc/**"));
  g_assert_true(has_pair(argv, "--glob", "lib/**"));
  g_assert_false(has_arg(argv, "lib/**/**"));
  g_assert_true(has_pair(argv, "--glob", "!build"));
  g_assert_true(has_pair(argv, "--glob", "!out"));
  g_assert_cmpint(count_arg(argv, "--glob"), ==, 8);
  g_strfreev(argv);
}

static void test_argv_grep(void)
{
  SearchQuery q = { .text = "x", .include = "*.c, src/*.h", .exclude = "build" };
  char **argv = search_argv(&q, FALSE);

  g_assert_cmpstr(argv[0], ==, "grep");
  g_assert_true(has_arg(argv, "-rnIZ"));
  g_assert_true(has_arg(argv, "-i"));
  g_assert_true(has_arg(argv, "-F"));
  g_assert_false(has_arg(argv, "-w"));
  g_assert_true(has_arg(argv, "--include=*.c"));
  g_assert_false(has_arg(argv, "--include=src/*.h")); /* grep only matches names */
  g_assert_true(has_arg(argv, "--exclude=build"));
  g_assert_true(has_arg(argv, "--exclude-dir=build"));
  g_strfreev(argv);

  SearchQuery r = { .text = "x+", .match_case = TRUE, .whole_word = TRUE, .regex = TRUE };
  argv = search_argv(&r, FALSE);
  g_assert_false(has_arg(argv, "-i"));
  g_assert_true(has_arg(argv, "-E"));
  g_assert_true(has_arg(argv, "-w"));
  g_strfreev(argv);
}

static gboolean matches(const SearchQuery *q, const char *text)
{
  GRegex *regex = search_regex(q, NULL);
  gboolean found;

  g_assert_nonnull(regex);
  found = g_regex_match(regex, text, 0, NULL);
  g_regex_unref(regex);
  return found;
}

static void test_regex(void)
{
  SearchQuery plain = { .text = "a.b(" };
  SearchQuery cased = { .text = "Foo", .match_case = TRUE };
  SearchQuery word = { .text = "foo", .whole_word = TRUE };
  SearchQuery regex = { .text = "fo+\\(", .regex = TRUE };
  SearchQuery bad = { .text = "foo(", .regex = TRUE };
  GError *error = NULL;

  g_assert_true(matches(&plain, "x = a.b(1)"));
  g_assert_false(matches(&plain, "x = axb(1)")); /* the dot is plain text */
  g_assert_true(matches(&cased, "Foo"));
  g_assert_false(matches(&cased, "foo"));
  g_assert_true(matches(&word, "int FOO;")); /* case is ignored by default */
  g_assert_true(matches(&word, "(foo)"));
  g_assert_false(matches(&word, "food"));
  g_assert_false(matches(&word, "a_foo"));
  g_assert_true(matches(&regex, "fooo(1)"));
  g_assert_false(matches(&regex, "f(1)"));
  g_assert_null(search_regex(&bad, &error));
  g_assert_nonnull(error);
  g_error_free(error);
}

typedef struct {
  char *path, *text;
  int line, column, start, end;
} Hit;

static void collect(const SearchHit *hit, gpointer data)
{
  Hit h = { g_strdup(hit->path), g_strdup(hit->text), hit->line, hit->column, hit->start, hit->end };

  g_array_append_val((GArray *)data, h);
}

static void clear_hit(gpointer data)
{
  Hit *h = data;

  g_free(h->path);
  g_free(h->text);
}

static GArray *hits_new(void)
{
  GArray *hits = g_array_new(FALSE, FALSE, sizeof(Hit));

  g_array_set_clear_func(hits, clear_hit);
  return hits;
}

static int parse(const char *line, gsize len, const char *text, GArray *hits)
{
  SearchQuery q = { .text = text };
  GRegex *regex = search_regex(&q, NULL);
  int n = search_parse_line(line, len, regex, collect, hits);

  g_regex_unref(regex);
  return n;
}

static void test_parse_line(void)
{
  static const char line[] = "./src/a.c\0"
                             "12:int foo = Foo(1);\r";
  GArray *hits = hits_new();
  Hit *h;

  g_assert_cmpint(parse(line, sizeof line - 1, "foo", hits), ==, 2);
  h = &g_array_index(hits, Hit, 0);
  g_assert_cmpstr(h->path, ==, "src/a.c");
  g_assert_cmpstr(h->text, ==, "int foo = Foo(1);"); /* without the '\r' */
  g_assert_cmpint(h->line, ==, 12);
  g_assert_cmpint(h->column, ==, 5);
  g_assert_cmpint(h->start, ==, 4);
  g_assert_cmpint(h->end, ==, 7);
  h = &g_array_index(hits, Hit, 1);
  g_assert_cmpint(h->column, ==, 11);
  g_assert_cmpint(h->start, ==, 10);
  g_array_free(hits, TRUE);
}

static void test_parse_line_utf8(void)
{
  static const char line[] = "dir:with:colons/ğ.c\0"
                             "3:ğüş foo";
  GArray *hits = hits_new();
  Hit *h;

  g_assert_cmpint(parse(line, sizeof line - 1, "foo", hits), ==, 1);
  h = &g_array_index(hits, Hit, 0);
  g_assert_cmpstr(h->path, ==, "dir:with:colons/ğ.c");
  g_assert_cmpint(h->line, ==, 3);
  g_assert_cmpint(h->column, ==, 5); /* characters, not bytes */
  g_assert_cmpint(h->start, ==, 7);
  g_array_free(hits, TRUE);
}

static void test_parse_line_unmatched(void)
{
  /* ripgrep's preview of a long line can end before the match. */
  static const char line[] = "a.c\0"
                             "7:xxxx [... 3 more matches]";
  GArray *hits = hits_new();
  Hit *h;

  g_assert_cmpint(parse(line, sizeof line - 1, "foo", hits), ==, 1);
  h = &g_array_index(hits, Hit, 0);
  g_assert_cmpint(h->line, ==, 7);
  g_assert_cmpint(h->column, ==, 1);
  g_assert_cmpint(h->start, ==, h->end);
  g_array_free(hits, TRUE);
}

static void test_parse_line_bad(void)
{
  static const char no_nul[] = "a.c:1:foo";
  static const char no_line[] = "a.c\0"
                                "foo";
  static const char not_number[] = "a.c\0"
                                   "1x:foo";
  static const char zero[] = "a.c\0"
                             "0:foo";
  static const char empty_number[] = "a.c\0"
                                     ":foo";
  GArray *hits = hits_new();

  g_assert_cmpint(parse(no_nul, sizeof no_nul - 1, "foo", hits), ==, 0);
  g_assert_cmpint(parse(no_line, sizeof no_line - 1, "foo", hits), ==, 0);
  g_assert_cmpint(parse(not_number, sizeof not_number - 1, "foo", hits), ==, 0);
  g_assert_cmpint(parse(zero, sizeof zero - 1, "foo", hits), ==, 0);
  g_assert_cmpint(parse(empty_number, sizeof empty_number - 1, "foo", hits), ==, 0);
  g_assert_cmpuint(hits->len, ==, 0);
  g_array_free(hits, TRUE);
}

static void test_snippet(void)
{
  char *match, *after, *before;
  const char *text = "\t    return\tfoo(x);";
  int start = (int)(strstr(text, "foo") - text);

  before = search_snippet(text, start, start + 3, &match, &after);
  g_assert_cmpstr(before, ==, "return ");
  g_assert_cmpstr(match, ==, "foo");
  g_assert_cmpstr(after, ==, "(x);");
  g_free(before);
  g_free(match);
  g_free(after);
}

static void test_snippet_long(void)
{
  GString *line = g_string_new(NULL);
  char *match, *after, *before;
  int start;

  for (int i = 0; i < 60; i++)
    g_string_append(line, "ğ");
  start = (int)line->len;
  g_string_append(line, "foo");
  for (int i = 0; i < 500; i++)
    g_string_append_c(line, 'z');

  before = search_snippet(line->str, start, start + 3, &match, &after);
  g_assert_true(g_str_has_prefix(before, "…"));
  g_assert_cmpint(g_utf8_strlen(before, -1), ==, 1 + 24);
  g_assert_true(g_utf8_validate(before, -1, NULL));
  g_assert_cmpstr(match, ==, "foo");
  g_assert_cmpint(strlen(after), ==, 200);
  g_free(before);
  g_free(match);
  g_free(after);
  g_string_free(line, TRUE);
}

/* Real searches ------------------------------------------------------------------------------- */

static char *tmp;

static void write_file(const char *rel, const char *contents, gssize len)
{
  g_free(write_at(tmp, rel, contents, len, 0));
}

/* Runs the query in tmp; returns "path:hits" for each file, sorted, like "a.c:1 sub/b.h:3". */
static char *run(const SearchQuery *q, gboolean ripgrep)
{
  char **argv = search_argv(q, ripgrep);
  GSubprocessLauncher *launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  GSubprocess *proc;
  GBytes *out = NULL;
  gsize size;
  const char *bytes;
  GRegex *regex = search_regex(q, NULL);
  GHashTable *files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GArray *hits = hits_new();
  GString *result = g_string_new(NULL);

  g_subprocess_launcher_set_cwd(launcher, tmp);
  proc = g_subprocess_launcher_spawnv(launcher, (const char *const *)argv, NULL);
  g_assert_nonnull(proc);
  g_assert_true(g_subprocess_communicate(proc, NULL, NULL, &out, NULL, NULL));
  bytes = g_bytes_get_data(out, &size);
  for (const char *start = bytes, *end = bytes + size; start && start < end;) {
    const char *newline = memchr(start, '\n', end - start);
    gsize len = (newline ? newline : end) - start;
    search_parse_line(start, len, regex, collect, hits);
    start = newline ? newline + 1 : end;
  }
  for (guint i = 0; i < hits->len; i++) {
    Hit *h = &g_array_index(hits, Hit, i);
    g_hash_table_insert(files, g_strdup(h->path),
                        GINT_TO_POINTER(GPOINTER_TO_INT(g_hash_table_lookup(files, h->path)) + 1));
  }
  GList *keys = g_list_sort(g_hash_table_get_keys(files), (GCompareFunc)strcmp);
  for (GList *k = keys; k; k = k->next)
    g_string_append_printf(result, "%s%s:%d", result->len ? " " : "", (char *)k->data,
                           GPOINTER_TO_INT(g_hash_table_lookup(files, k->data)));
  g_list_free(keys);
  g_hash_table_unref(files);
  g_array_free(hits, TRUE);
  g_regex_unref(regex);
  g_bytes_unref(out);
  g_object_unref(proc);
  g_object_unref(launcher);
  g_strfreev(argv);
  return g_string_free(result, FALSE);
}

static void check(const SearchQuery *q, gboolean ripgrep, const char *expected)
{
  char *result = run(q, ripgrep);

  g_assert_cmpstr(result, ==, expected);
  g_free(result);
}

static void test_real(gconstpointer data)
{
  gboolean ripgrep = GPOINTER_TO_INT(data);
  SearchQuery all = { .text = "foo" };
  SearchQuery cased = { .text = "foo", .match_case = TRUE };
  SearchQuery word = { .text = "fo", .whole_word = TRUE };
  SearchQuery regex = { .text = "fo+\\(", .regex = TRUE };
  SearchQuery headers = { .text = "foo", .include = "*.h" };
  SearchQuery folder = { .text = "foo", .include = "sub" };
  SearchQuery no_sub = { .text = "foo", .exclude = "sub" };
  SearchQuery dash = { .text = "-x" };
  char *rg = g_find_program_in_path("rg");
  gboolean missing = rg == NULL;

  g_free(rg);
  if (ripgrep && missing) {
    g_test_skip("ripgrep is not installed");
    return;
  }
  check(&all, ripgrep, "a.c:1 sub/b.h:3");
  check(&cased, ripgrep, "a.c:1 sub/b.h:1");
  check(&word, ripgrep, "");
  check(&regex, ripgrep, "a.c:1");
  check(&headers, ripgrep, "sub/b.h:3");
  check(&no_sub, ripgrep, "a.c:1");
  check(&dash, ripgrep, "c.txt:1");
  if (ripgrep) /* grep matches names only, not folders' contents */
    check(&folder, ripgrep, "sub/b.h:3");
}

/* Replacing: a literal replacement as it is, a regular expression's with its groups; whole words
 * and case as the search finds them; matches across lines are left. */
static char *replaced(const SearchQuery *q, const char *text, const char *replacement)
{
  GRegex *regex = search_regex(q, NULL);
  GArray *edits = search_edits(regex, text, replacement, q->regex, NULL);
  char *out = edits ? search_apply_edits(text, edits) : NULL;

  search_edits_free(edits);
  g_regex_unref(regex);
  return out;
}

static void test_replace(void)
{
  SearchQuery literal = { .text = "a.b", .match_case = TRUE }, word = { .text = "foo", .whole_word = TRUE };
  SearchQuery groups = { .text = "(\\w+)=(?<value>\\d+)", .regex = TRUE, .match_case = TRUE };
  SearchQuery lines = { .text = "x\\s+y", .regex = TRUE }, start = { .text = "^#", .regex = TRUE };
  char *out;

  out = replaced(&literal, "a.b axb a.b", "$1\\n");
  g_assert_cmpstr(out, ==, "$1\\n axb $1\\n"); /* nothing special in a literal replacement */
  g_free(out);
  out = replaced(&word, "foo food FOO", "bar");
  g_assert_cmpstr(out, ==, "bar food bar");
  g_free(out);
  out = replaced(&groups, "a=1, bb=22", "${value}:$1 $$");
  g_assert_cmpstr(out, ==, "1:a $, 22:bb $");
  g_free(out);
  out = replaced(&lines, "x  y\nx\ny", "-");
  g_assert_cmpstr(out, ==, "-\nx\ny"); /* the search reads lines: no match across one */
  g_free(out);
  out = replaced(&start, "#a\n#b\n c#\n", "//");
  g_assert_cmpstr(out, ==, "//a\n//b\n c#\n"); /* ^ at each line */
  g_free(out);
  out = replaced(&groups, "a=1", "[$9]");
  g_assert_cmpstr(out, ==, "[]"); /* a group it does not have: nothing */
  g_free(out);
}

static void test_files_argv(void)
{
  SearchQuery q = { .text = "x" };
  char **rg = search_files_argv(&q, TRUE), **grep = search_files_argv(&q, FALSE);

  g_assert_cmpstr(rg[0], ==, "rg");
  g_assert_true(g_strv_contains((const char *const *)rg, "--files-with-matches"));
  g_assert_cmpstr(rg[g_strv_length(rg) - 1], ==, ".");
  g_assert_cmpstr(grep[1], ==, "-l");
  g_strfreev(grep);
  g_strfreev(rg);
}

int main(int argc, char **argv)
{
  int status;

  g_test_init(&argc, &argv, NULL);
  tmp = g_dir_make_tmp("t-code-search-XXXXXX", NULL);
  write_file("a.c", "int main(void) { return foo(); }\n", -1);
  write_file("sub/b.h", "Foo foo FOO\n", -1);
  write_file("c.txt", "nothing here\nbut a -x flag\n", -1);
  write_file("bin.dat", "foo\0\1\2foo\n", 9); /* binary: skipped */

  g_test_add_func("/search/argv/rg-defaults", test_argv_rg_defaults);
  g_test_add_func("/search/argv/rg-options", test_argv_rg_options);
  g_test_add_func("/search/argv/grep", test_argv_grep);
  g_test_add_func("/search/regex", test_regex);
  g_test_add_func("/search/parse/line", test_parse_line);
  g_test_add_func("/search/parse/utf8", test_parse_line_utf8);
  g_test_add_func("/search/parse/unmatched", test_parse_line_unmatched);
  g_test_add_func("/search/parse/bad", test_parse_line_bad);
  g_test_add_func("/search/snippet/short", test_snippet);
  g_test_add_func("/search/snippet/long", test_snippet_long);
  g_test_add_func("/search/replace", test_replace);
  g_test_add_func("/search/files-argv", test_files_argv);
  g_test_add_data_func("/search/real/ripgrep", GINT_TO_POINTER(TRUE), test_real);
  g_test_add_data_func("/search/real/grep", GINT_TO_POINTER(FALSE), test_real);
  status = g_test_run();

  char *rm[] = { "rm", "-rf", tmp, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(tmp);
  return status;
}
