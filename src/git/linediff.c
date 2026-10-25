/*
 * Line differences (see linediff.h). The lines both versions start and end with are left out
 * first; what remains is lined up by Myers' algorithm, whose cost grows with the lines added and
 * deleted, not with the file: two small edits far apart in a long file stay two small hunks. Past
 * MAX_EDITS the middle just counts as modified: a gutter needs no perfect diff of a file that was
 * rewritten.
 */
#include "git/linediff.h"

#include <string.h>

#define MAX_EDITS 1000 /* lines added plus deleted; the search keeps up to (MAX_EDITS + 1)^2 ints */

/* A file's lines (no line breaks), sharing its text. */
typedef struct {
  char *text;
  GPtrArray *lines;
} Lines;

static void split(Lines *l, const char *text)
{
  l->text = g_strdup(text ? text : "");
  l->lines = g_ptr_array_new();
  for (char *p = l->text;;) {
    char *nl = strchr(p, '\n');
    g_ptr_array_add(l->lines, p);
    if (!nl)
      break;
    *nl = '\0';
    p = nl + 1;
  }
  /* A last line break ends the last line: no empty line after it. */
  if (l->lines->len > 1 && !*(char *)g_ptr_array_index(l->lines, l->lines->len - 1))
    g_ptr_array_set_size(l->lines, l->lines->len - 1);
}

static void lines_free(Lines *l)
{
  g_ptr_array_free(l->lines, TRUE);
  g_free(l->text);
}

static const char *line(Lines *l, guint i)
{
  return g_ptr_array_index(l->lines, i);
}

/* A run of lines both versions share: old's from x, new's from y. */
typedef struct {
  int x, y, length;
} Snake;

static void add_hunk(GArray *hunks, guint old_start, guint deleted, guint new_start, guint added)
{
  DiffHunk hunk = { old_start, deleted, new_start, added };

  g_array_append_val(hunks, hunk);
}

/* The stretches where old and new differ, in order. */
static GArray *hunks_of(Lines *old, Lines *new)
{
  GArray *hunks = g_array_new(FALSE, FALSE, sizeof(DiffHunk));
  guint head = 0, tail = 0, n, m;

  while (head < old->lines->len && head < new->lines->len && strcmp(line(old, head), line(new, head)) == 0)
    head++;
  while (tail < old->lines->len - head && tail < new->lines->len - head &&
         strcmp(line(old, old->lines->len - 1 - tail), line(new, new->lines->len - 1 - tail)) == 0)
    tail++;
  n = old->lines->len - head - tail;
  m = new->lines->len - head - tail;

  if (n == 0 || m == 0) {
    if (n || m)
      add_hunk(hunks, head, n, head, m);
    return hunks;
  }
  /* Lines are compared by their hashes first: most pairs differ, and a strcmp costs more. */
  guint *old_hash = g_new(guint, n), *new_hash = g_new(guint, m);
  for (guint k = 0; k < n; k++)
    old_hash[k] = g_str_hash(line(old, head + k));
  for (guint k = 0; k < m; k++)
    new_hash[k] = g_str_hash(line(new, head + k));
#define SAME(a, b) (old_hash[a] == new_hash[b] && strcmp(line(old, head + (a)), line(new, head + (b))) == 0)
  /* v[k]: how far into old the furthest path with d edits gets on diagonal k (= x - y), at
   * v[k + offset]; trace keeps v of each d (its 2d + 1 diagonals from -d, at d * d) to walk back,
   * growing with the edits found. */
  int max = (int)MIN(n + m, MAX_EDITS), offset = max + 1, edits = -1;
  int *v = g_new0(int, 2 * max + 3);
  GArray *trace = g_array_new(FALSE, FALSE, sizeof(int));
  for (int d = 0; d <= max && edits < 0; d++) {
    for (int k = -d; k <= d; k += 2) {
      int x = k == -d || (k != d && v[k - 1 + offset] < v[k + 1 + offset]) ? v[k + 1 + offset] : v[k - 1 + offset] + 1;
      int y = x - k;
      while (x < (int)n && y < (int)m && SAME(x, y))
        x++, y++;
      v[k + offset] = x;
      if (x >= (int)n && y >= (int)m)
        edits = d;
    }
    g_array_append_vals(trace, v + offset - d, 2 * d + 1);
  }
  if (edits < 0) {
    add_hunk(hunks, head, n, head, m);
  } else {
    /* Walk back from the end: each edit is followed by a run of common lines (a snake). */
    GArray *snakes = g_array_new(FALSE, FALSE, sizeof(Snake));
    int x = (int)n, y = (int)m;
    for (int d = edits; d >= 0; d--) {
      int k = x - y, prev_x = 0, prev_y = 0, start_x = 0;
      if (d > 0) {
        int *prev = (int *)trace->data + (gsize)(d - 1) * (d - 1) + (d - 1); /* prev[k]: v of d - 1 */
        int down = k == -d || (k != d && prev[k - 1] < prev[k + 1]);         /* an addition, else a deletion */
        int prev_k = down ? k + 1 : k - 1;
        prev_x = prev[prev_k];
        prev_y = prev_x - prev_k;
        start_x = down ? prev_x : prev_x + 1;
      }
      Snake snake = { start_x, start_x - k, x - start_x };
      if (snake.length > 0)
        g_array_prepend_val(snakes, snake);
      x = prev_x, y = prev_y;
    }
    /* Between two runs of common lines: one hunk of the deletions and additions there. */
    int i = 0, j = 0;
    for (guint e = 0; e < snakes->len; e++) {
      Snake *snake = &g_array_index(snakes, Snake, e);
      if (snake->x > i || snake->y > j)
        add_hunk(hunks, head + i, snake->x - i, head + j, snake->y - j);
      i = snake->x + snake->length, j = snake->y + snake->length;
    }
    if (i < (int)n || j < (int)m)
      add_hunk(hunks, head + i, n - i, head + j, m - j);
    g_array_free(snakes, TRUE);
  }
#undef SAME
  g_array_free(trace, TRUE);
  g_free(v);
  g_free(old_hash);
  g_free(new_hash);
  return hunks;
}

GArray *line_diff_hunks(const char *old_text, const char *new_text)
{
  Lines old, new;
  GArray *hunks;

  split(&old, old_text);
  split(&new, new_text);
  hunks = hunks_of(&old, &new);
  lines_free(&old);
  lines_free(&new);
  return hunks;
}

typedef void (*HunkFunc)(guint old_start, guint deleted, guint new_start, guint added, gpointer data);

/* Calls hunk for each stretch where old and new differ, in order: those of hunks, or else worked out. */
static void walk_hunks(Lines *old, Lines *new, const GArray *hunks, HunkFunc hunk, gpointer data)
{
  GArray *own = hunks ? NULL : hunks_of(old, new);

  if (own)
    hunks = own;
  for (guint i = 0; i < hunks->len; i++) {
    const DiffHunk *h = &g_array_index(hunks, DiffHunk, i);
    hunk(h->old_start, h->deleted, h->new_start, h->added, data);
  }
  if (own)
    g_array_unref(own);
}

/* HunkFunc for line_diff(): where both, the first added lines are modified ones; deleted lines
 * alone mark the line after them. */
static void mark_hunk(guint old_start G_GNUC_UNUSED, guint deleted, guint new_start, guint added, gpointer data)
{
  GArray *marks = data;

  for (guint i = 0; i < added; i++)
    g_array_index(marks, guint8, new_start + i) = i < deleted ? LINE_MODIFIED : LINE_ADDED;
  if (added == 0 && deleted > 0 && marks->len > 0)
    g_array_index(marks, guint8, MIN(new_start, marks->len - 1)) = LINE_DELETED_ABOVE;
}

GArray *line_diff(const char *old_text, const char *new_text)
{
  Lines old, new;
  GArray *marks = g_array_new(FALSE, TRUE, sizeof(guint8));

  split(&old, old_text);
  split(&new, new_text);
  g_array_set_size(marks, new.lines->len);
  walk_hunks(&old, &new, NULL, mark_hunk, marks);
  lines_free(&old);
  lines_free(&new);
  return marks;
}

typedef struct {
  GArray *gaps;
  gboolean old_side;
  guint lines;                          /* in the side the gaps are for */
  const int *old_heights, *new_heights; /* per line (word wrap, pixels); NULL: 1 each */
  guint old_at, new_at;                 /* the first lines not lined up yet */
} Gaps;

static int height_of(const int *heights, guint line)
{
  return heights ? MAX(heights[line], 0) : 1;
}

/* Adds space below line (above line 0 when above): count for missing lines, pad for a shorter line.
 * A line has one gap (a text view takes one pixels-below per line, not their sum). */
static void add_gap(Gaps *g, guint line, gboolean above, int count, int pad)
{
  DiffGap *last = g->gaps->len ? &g_array_index(g->gaps, DiffGap, g->gaps->len - 1) : NULL;
  DiffGap gap = { (int)MIN(line, g->lines - 1), MAX(count, 0), MAX(pad, 0), above };

  if (gap.count + gap.pad == 0 || g->lines == 0)
    return;
  if (last && last->line == gap.line && last->above == above) {
    last->count += gap.count;
    last->pad += gap.pad;
  } else {
    g_array_append_val(g->gaps, gap);
  }
}

/* Lines both sides have, from old_at / new_at on: the shorter of each pair (it wraps less) is padded
 * to the other's height. */
static void line_up_same(Gaps *g, guint count)
{
  for (guint k = 0; k < count; k++) {
    guint old_line = g->old_at + k, new_line = g->new_at + k;
    int old_height = height_of(g->old_heights, old_line), new_height = height_of(g->new_heights, new_line);
    int mine = g->old_side ? old_height : new_height, theirs = g->old_side ? new_height : old_height;
    add_gap(g, g->old_side ? old_line : new_line, FALSE, 0, theirs - mine);
  }
  g->old_at += count;
  g->new_at += count;
}

/* HunkFunc for line_diff_gaps(): the same lines before the stretch are lined up one by one; in the
 * stretch, the side whose lines take less height gets a gap of the difference, below its last line
 * there (or the line before; above line 0 at the start). */
static void gap_hunk(guint old_start, guint deleted, guint new_start, guint added, gpointer data)
{
  Gaps *g = data;
  int old_height = 0, new_height = 0;

  line_up_same(g, old_start - g->old_at);
  for (guint i = 0; i < deleted; i++)
    old_height += height_of(g->old_heights, old_start + i);
  for (guint i = 0; i < added; i++)
    new_height += height_of(g->new_heights, new_start + i);
  guint at = (g->old_side ? old_start + deleted : new_start + added);
  add_gap(g, at > 0 ? at - 1 : 0, at == 0, g->old_side ? new_height - old_height : old_height - new_height, 0);
  g->old_at = old_start + deleted;
  g->new_at = new_start + added;
}

GArray *line_diff_gaps(const char *old_text, const char *new_text, const GArray *hunks, const int *old_heights,
                       const int *new_heights, gboolean for_old_side)
{
  Lines old, new;
  Gaps g = { g_array_new(FALSE, FALSE, sizeof(DiffGap)), for_old_side, 0, old_heights, new_heights, 0, 0 };

  split(&old, old_text);
  split(&new, new_text);
  g.lines = for_old_side ? old.lines->len : new.lines->len;
  walk_hunks(&old, &new, hunks, gap_hunk, &g);
  line_up_same(&g, old.lines->len - g.old_at); /* the same lines at the end */
  lines_free(&old);
  lines_free(&new);
  return g.gaps;
}

guint line_count(const char *text)
{
  Lines l;
  guint n;

  split(&l, text);
  n = l.lines->len;
  lines_free(&l);
  return n;
}

GArray *line_diff_folds(const GArray *hunks, guint new_lines, guint context, guint min_lines)
{
  GArray *folds = g_array_new(FALSE, FALSE, sizeof(DiffFold));
  guint old_at = 0, new_at = 0;

  for (guint i = 0; hunks->len && i <= hunks->len; i++) {
    const DiffHunk *h = i < hunks->len ? &g_array_index(hunks, DiffHunk, i) : NULL;
    guint same = (h ? h->new_start : new_lines) - new_at;               /* lines both have, up to the next change */
    guint keep_head = i > 0 ? context : 0, keep_tail = h ? context : 1; /* the file's last line stays */
    if (same >= keep_head + keep_tail + MAX(min_lines, 1)) {
      DiffFold fold = { old_at + keep_head, new_at + keep_head, same - keep_head - keep_tail };
      g_array_append_val(folds, fold);
    }
    if (h) {
      old_at = h->old_start + h->deleted;
      new_at = h->new_start + h->added;
    }
  }
  return folds;
}

/* Changed words ------------------------------------------------------------------------------ */

#define MAX_TOKEN_CELLS (400 * 400) /* past it, a line pair is just changed as a whole */

/* A line's tokens: words (letters, digits, _ and any non-ASCII byte), runs of spaces, or one other
 * character each; as byte offsets, the last one the line's end. */
static GArray *tokens(const char *text)
{
  GArray *starts = g_array_new(FALSE, FALSE, sizeof(int));
  int i = 0, n = (int)strlen(text);

#define WORD(c) (g_ascii_isalnum(c) || (c) == '_' || (guchar)(c) >= 0x80)
  while (i < n) {
    g_array_append_val(starts, i);
    if (WORD(text[i]))
      while (i < n && WORD(text[i]))
        i++;
    else if (text[i] == ' ' || text[i] == '\t')
      while (i < n && (text[i] == ' ' || text[i] == '\t'))
        i++;
    else
      i++;
  }
#undef WORD
  g_array_append_val(starts, n);
  return starts;
}

static gboolean same_token(const char *a, const GArray *ta, guint i, const char *b, const GArray *tb, guint j)
{
  int la = g_array_index(ta, int, i + 1) - g_array_index(ta, int, i);
  int lb = g_array_index(tb, int, j + 1) - g_array_index(tb, int, j);

  return la == lb && memcmp(a + g_array_index(ta, int, i), b + g_array_index(tb, int, j), la) == 0;
}

/* Adds a span of line; one that starts where the last one ended, or only spaces after it, joins it.
 * Spaces alone are no span (they only bridge two). */
static void add_span(GArray *spans, const char *text, int line, int start, int end)
{
  DiffSpan *last = spans->len ? &g_array_index(spans, DiffSpan, spans->len - 1) : NULL;
  DiffSpan span = { line, start, end };

  if (start >= end || strspn(text + start, " \t") >= (size_t)(end - start))
    return;
  if (last && last->line == line && last->end >= 0 && strspn(text + last->end, " \t") >= (size_t)(start - last->end)) {
    last->end = end;
    return;
  }
  g_array_append_val(spans, span);
}

/* The words of mine (line line) that theirs does not have, as spans. */
static void changed_words(GArray *spans, int line, const char *mine, const char *theirs)
{
  GArray *tm = tokens(mine), *tt = tokens(theirs);
  guint n = tm->len - 1, m = tt->len - 1;

  if ((gsize)n * m > MAX_TOKEN_CELLS) {
    add_span(spans, mine, line, 0, (int)strlen(mine));
  } else {
    guint16 *lcs = g_new0(guint16, (gsize)(n + 1) * (m + 1));
    guint i = 0, j = 0;
#define LCS(a, b) lcs[(gsize)(a) * (m + 1) + (b)]
    for (int a = (int)n - 1; a >= 0; a--)
      for (int b = (int)m - 1; b >= 0; b--)
        LCS(a, b) = same_token(mine, tm, a, theirs, tt, b) ? LCS(a + 1, b + 1) + 1 : MAX(LCS(a + 1, b), LCS(a, b + 1));
    while (i < n) {
      if (j < m && same_token(mine, tm, i, theirs, tt, j)) {
        i++, j++;
      } else if (j < m && LCS(i, j + 1) >= LCS(i + 1, j)) {
        j++;
      } else { /* a token of mine theirs does not have */
        add_span(spans, mine, line, g_array_index(tm, int, i), g_array_index(tm, int, i + 1));
        i++;
      }
    }
#undef LCS
    g_free(lcs);
  }
  g_array_free(tm, TRUE);
  g_array_free(tt, TRUE);
}

typedef struct {
  GArray *spans;
  gboolean old_side;
  Lines *old, *new;
} Spans;

/* HunkFunc for line_diff_spans(): each changed line of this side is marked; the lines of a stretch
 * that both sides have (paired in order) mark their changed words, the others all their text. */
static void span_hunk(guint old_start, guint deleted, guint new_start, guint added, gpointer data)
{
  Spans *s = data;
  guint mine = s->old_side ? deleted : added, start = s->old_side ? old_start : new_start;
  Lines *my_lines = s->old_side ? s->old : s->new, *their_lines = s->old_side ? s->new : s->old;
  guint their_start = s->old_side ? new_start : old_start, theirs = s->old_side ? added : deleted;

  for (guint k = 0; k < mine; k++) {
    const char *text = line(my_lines, start + k);
    DiffSpan whole = { (int)(start + k), 0, -1 };
    g_array_append_val(s->spans, whole);
    if (k < theirs)
      changed_words(s->spans, (int)(start + k), text, line(their_lines, their_start + k));
    else
      add_span(s->spans, text, (int)(start + k), 0, (int)strlen(text));
  }
}

GArray *line_diff_spans(const char *old_text, const char *new_text, const GArray *hunks, gboolean for_old_side)
{
  Lines old, new;
  Spans s = { g_array_new(FALSE, FALSE, sizeof(DiffSpan)), for_old_side, &old, &new };

  split(&old, old_text);
  split(&new, new_text);
  walk_hunks(&old, &new, hunks, span_hunk, &s);
  lines_free(&old);
  lines_free(&new);
  return s.spans;
}
