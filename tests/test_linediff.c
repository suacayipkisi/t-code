/*
 * Tests for src/git/linediff.c: the diff gutter's marks.
 */
#include "git/linediff.h"

#include <stdlib.h>
#include <string.h>

/* The marks as a string, one letter per line: . same, A added, M modified, D deleted above. */
static char *marks_of(const char *old, const char *new)
{
  GArray *marks = line_diff(old, new);
  GString *text = g_string_new(NULL);

  for (guint i = 0; i < marks->len; i++)
    g_string_append_c(text, ".AMD"[g_array_index(marks, guint8, i)]);
  g_array_free(marks, TRUE);
  return g_string_free(text, FALSE);
}

static void check(const char *old, const char *new, const char *expected)
{
  char *marks = marks_of(old, new);

  g_assert_cmpstr(marks, ==, expected);
  g_free(marks);
}

static void test_same(void)
{
  check("a\nb\nc\n", "a\nb\nc", "...");
  check("", "", ".");
}

static void test_added(void)
{
  check("a\nc\n", "a\nb\nc", ".A.");
  check("a\n", "a\nb\nc", ".AA");
  check("b\n", "a\nb", "A.");
}

static void test_modified(void)
{
  check("a\nb\nc\n", "a\nB\nc", ".M.");
  /* Two lines for one: the first is modified, the second added. */
  check("a\nb\nc\n", "a\nB1\nB2\nc", ".MA.");
}

static void test_deleted(void)
{
  check("a\nb\nc\n", "a\nc", ".D");
  check("a\nb\nc\n", "b\nc", "D.");
  check("a\nb\nc\n", "a\nb", ".D"); /* at the end: the last line carries it */
}

/* Several hunks apart from each other, lined up by what they share. */
static void test_hunks(void)
{
  check("1\n2\n3\n4\n5\n6\n7\n", "1\nX\n3\n4\n6\n7\nY", ".M..D.A");
}

/* A file rewritten past the table's size: every line in the middle counts as modified. */
static void test_big(void)
{
  GString *old = g_string_new(NULL), *new = g_string_new(NULL);
  GArray *marks;

  for (int i = 0; i < 3000; i++) {
    g_string_append_printf(old, "old %d\n", i);
    g_string_append_printf(new, "new %d\n", i);
  }
  marks = line_diff(old->str, new->str);
  g_assert_cmpuint(marks->len, ==, 3000);
  g_assert_cmpuint(g_array_index(marks, guint8, 0), ==, LINE_MODIFIED);
  g_assert_cmpuint(g_array_index(marks, guint8, 2999), ==, LINE_MODIFIED);
  g_array_free(marks, TRUE);
  g_string_free(new, TRUE);
  g_string_free(old, TRUE);
}

/* The gaps as a string: "<line>:<count>" (below that line) or "^:<count>" (above line 0); rows:
 * the screen rows of each line with word wrap (NULL: one each). */
static char *wrapped_gaps_of(const char *old, const char *new, const int *old_rows, const int *new_rows,
                             gboolean old_side)
{
  GArray *gaps = line_diff_gaps(old, new, NULL, old_rows, new_rows, old_side);
  GString *text = g_string_new(NULL);

  for (guint i = 0; i < gaps->len; i++) {
    DiffGap *gap = &g_array_index(gaps, DiffGap, i);
    if (gap->above)
      g_string_append_printf(text, "%s^:%d", text->len ? " " : "", gap->count);
    else
      g_string_append_printf(text, "%s%d:%d", text->len ? " " : "", gap->line, gap->count);
    if (gap->pad)
      g_string_append_printf(text, "+%d", gap->pad);
  }
  g_array_free(gaps, TRUE);
  return g_string_free(text, FALSE);
}

static void check_wrapped_gaps(const char *old, const char *new, const int *old_rows, const int *new_rows,
                               const char *new_side, const char *old_side)
{
  char *a = wrapped_gaps_of(old, new, old_rows, new_rows, FALSE),
       *b = wrapped_gaps_of(old, new, old_rows, new_rows, TRUE);

  g_assert_cmpstr(a, ==, new_side);
  g_assert_cmpstr(b, ==, old_side);
  g_free(b);
  g_free(a);
}

static void check_gaps(const char *old, const char *new, const char *new_side, const char *old_side)
{
  check_wrapped_gaps(old, new, NULL, NULL, new_side, old_side);
}

/* Side by side, the side with fewer lines in a stretch gets the difference as a gap. */
static void test_gaps(void)
{
  check_gaps("a\nb\nc\n", "a\nc", "0:1", "");       /* b deleted: a gap under a on the new side */
  check_gaps("a\nc\n", "a\nb1\nb2\nc", "", "0:2");  /* two added: a gap under a on the old side */
  check_gaps("x\ny\na\n", "a", "^:2", "");          /* deleted at the start: above line 0 */
  check_gaps("a\nb\nc\n", "a\nB\nc", "", "");       /* modified: same size, no gap */
  check_gaps("a\nb\n", "a\nB1\nB2\nB3", "", "1:2"); /* one for three: two rows */
}

/* With word wrap, rows line up rather than lines: a line both sides have that wraps to more rows on
 * one side gives the other a gap; a missing line counts all its rows; gaps on one line add up. */
static void test_wrapped_gaps(void)
{
  static const int one_three_one[] = { 1, 3, 1 }, ones[] = { 1, 1, 1, 1 }, one_two_one[] = { 1, 2, 1 };
  static const int px_17[] = { 17, 17, 17 }, px_52[] = { 17, 52, 17 };

  /* "long" wraps to 3 rows in the new version only: the old one is padded (no block). */
  check_wrapped_gaps("a\nlong\nb", "a\nlong\nb", ones, one_three_one, "", "1:0+2");
  /* a deleted line that took 2 rows: a 2-row block on the new side. */
  check_wrapped_gaps("a\nx\nb", "a\nb", one_two_one, ones, "0:2", "");
  /* line 1 wraps to 3 rows on the new side and line 2 was added: one gap, padded 2, a block of 1. */
  check_wrapped_gaps("a\nlong", "a\nlong\nadded", ones, one_three_one, "", "1:1+2");
  /* in pixels: a paragraph of 3 rows is not always 3 times one row (each one is rounded alone) */
  check_wrapped_gaps("a\nlong\nb", "a\nlong\nb", px_17, px_52, "", "1:0+35");
}

/* The spans as a string: "<line>" for a changed line, "<line>:<text>" for its changed words. */
static char *spans_of(const char *old, const char *new, gboolean old_side)
{
  const char *text = old_side ? old : new;
  char **lines = g_strsplit(text, "\n", -1);
  GArray *spans = line_diff_spans(old, new, NULL, old_side);
  GString *out = g_string_new(NULL);

  for (guint i = 0; i < spans->len; i++) {
    DiffSpan *span = &g_array_index(spans, DiffSpan, i);
    if (out->len)
      g_string_append_c(out, ' ');
    if (span->end < 0)
      g_string_append_printf(out, "%d", span->line);
    else
      g_string_append_printf(out, "%d:%.*s", span->line, span->end - span->start, lines[span->line] + span->start);
  }
  g_array_free(spans, TRUE);
  g_strfreev(lines);
  return g_string_free(out, FALSE);
}

static void check_spans(const char *old, const char *new, const char *new_side, const char *old_side)
{
  char *a = spans_of(old, new, FALSE), *b = spans_of(old, new, TRUE);

  g_assert_cmpstr(a, ==, new_side);
  g_assert_cmpstr(b, ==, old_side);
  g_free(b);
  g_free(a);
}

/* Changed words: only what differs in a changed line; a line without a pair is all
 * changed; words with only spaces between them make one span. */
static void test_spans(void)
{
  check_spans("a\n{ \"Close a tab\", \"x\" },\nb\n", "a\n{ \"Close a tab (too)\", \"x\" },\nb\n", "1 1:(too)", "1");
  check_spans("int count = 1;\n", "int total = 1;\n", "0 0:total", "0 0:count");
  check_spans("x\n", "x\nint added;\n", "1 1:int added;", "");
  check_spans("keep\ngone line\n", "keep\n", "", "1 1:gone line");
  check_spans("same\n", "same\n", "", "");
  check_spans("a b c\n", "a x y c\n", "0 0:x y", "0 0:b"); /* "x y": one span over the space */
}

/* lines lines "l<n>", but those in changed ("|"-separated "n=text", text "-" deletes the line). */
static char *numbered(int lines, const char *changed)
{
  GString *text = g_string_new(NULL);
  char **changes = g_strsplit(changed, "|", -1);

  for (int n = 0; n < lines; n++) {
    const char *with = NULL;
    for (char **c = changes; *c && **c; c++)
      if (atoi(*c) == n)
        with = strchr(*c, '=') + 1;
    if (!with)
      g_string_append_printf(text, "l%d\n", n);
    else if (strcmp(with, "-") != 0)
      g_string_append_printf(text, "%s\n", with);
  }
  g_strfreev(changes);
  return g_string_free(text, FALSE);
}

/* The folds as a string: "<old line>:<new line>+<count>", context 3 and at least 4 lines. */
static void check_folds(const char *old, const char *new, const char *expected)
{
  GArray *hunks = line_diff_hunks(old, new), *folds = line_diff_folds(hunks, line_count(new), 3, 4);
  GString *text = g_string_new(NULL);

  for (guint i = 0; i < folds->len; i++) {
    DiffFold *fold = &g_array_index(folds, DiffFold, i);
    g_string_append_printf(text, "%s%u:%u+%u", text->len ? " " : "", fold->old_line, fold->new_line, fold->count);
  }
  g_assert_cmpstr(text->str, ==, expected);
  g_string_free(text, TRUE);
  g_array_unref(folds);
  g_array_unref(hunks);
}

/* A comparison folds away what did not change, but three lines around each change and the last
 * line; the two sides' folds line up (the same count of lines). */
static void test_folds(void)
{
  char *same = numbered(30, ""), *changed = numbered(30, "15=new"), *deleted = numbered(30, "10=-|11=-");
  char *close = numbered(12, "2=x|9=y"), *same_close = numbered(12, "");

  check_folds(same, changed, "0:0+12 19:19+10");
  check_folds(same, deleted, "0:0+7 15:13+14");
  check_folds(deleted, same, "0:0+7 13:15+14");
  check_folds(same, same, "");        /* nothing changed: nothing folded */
  check_folds(same_close, close, ""); /* too few lines between the changes */
  g_free(same);
  g_free(changed);
  g_free(deleted);
  g_free(close);
  g_free(same_close);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/linediff/same", test_same);
  g_test_add_func("/linediff/added", test_added);
  g_test_add_func("/linediff/modified", test_modified);
  g_test_add_func("/linediff/deleted", test_deleted);
  g_test_add_func("/linediff/hunks", test_hunks);
  g_test_add_func("/linediff/big", test_big);
  g_test_add_func("/linediff/gaps", test_gaps);
  g_test_add_func("/linediff/wrapped-gaps", test_wrapped_gaps);
  g_test_add_func("/linediff/spans", test_spans);
  g_test_add_func("/linediff/folds", test_folds);
  return g_test_run();
}
