/*
 * What changed between two versions of a file, line by line, for the editor's diff gutter (GLib
 * only, so tests/ can run it without a display).
 */
#pragma once

#include <glib.h>

/* A line's mark in the new version. */
typedef enum {
  LINE_SAME,
  LINE_ADDED,
  LINE_MODIFIED,
  LINE_DELETED_ABOVE, /* lines of the old version were removed right before it */
} LineMark;

/* An empty vertical space needed to align two versions side by side, in the unit of the heights
 * given to line_diff_gaps() (lines when none: t-code gives pixels). */
typedef struct {
  int line;       /* 0-based line in the buffer; the space is below it, or above line 0 when above is TRUE */
  int count;      /* for the other side's missing lines (drawn as a block) */
  int pad;        /* right below the line: the line it faces wraps to more rows (left blank) */
  gboolean above; /* TRUE when lines are missing before line 0 */
} DiffGap;

/* A stretch where the versions differ: deleted lines of old from old_start, added lines of new
 * from new_start (either may be 0). */
typedef struct {
  guint old_start, deleted, new_start, added;
} DiffHunk;

/* The DiffHunk list of new compared with old, in line order. The functions below that take one
 * work it out themselves when given NULL: one list for several of them saves doing it again. */
GArray *line_diff_hunks(const char *old, const char *new);

/* The marks (guint8 LineMark, one per line of new) for new compared with old. Lines removed at the
 * very end mark the last line. */
GArray *line_diff(const char *old, const char *new);

/* The DiffGap list (in line order, at most one per line and side) for new (for_old_side == FALSE:
 * gaps where lines were deleted from old) or for old (for_old_side == TRUE: gaps where lines were
 * added in new). old_heights / new_heights: each line's height (pixels, with word wrap; NULL: 1 each);
 * a line both have that is taller on the other side gets the difference as a pad. */
GArray *line_diff_gaps(const char *old, const char *new, const GArray *hunks, const int *old_heights,
                       const int *new_heights, gboolean for_old_side);

/* How many lines text has, counted like the functions above (a last line break ends a line). */
guint line_count(const char *text);

/* What changed in a line, side by side: a whole changed line (end == -1), or the bytes start..end
 * of it that the other version does not have (its changed words). */
typedef struct {
  int line;
  int start, end;
} DiffSpan;

/* For new (for_old_side == FALSE) or old: each changed line (end -1) followed by its changed words.
 * In a stretch of changes the lines are paired in order and compared word by word (words, runs of
 * spaces, other characters); a line without a pair is all changed. */
GArray *line_diff_spans(const char *old, const char *new, const GArray *hunks, gboolean for_old_side);

/* Lines both versions have, count of them from old_line in old and new_line in new: a stretch a
 * comparison can fold away. */
typedef struct {
  guint old_line, new_line, count;
} DiffFold;

/* The DiffFold list (in line order) for hunks, new having new_lines lines: each stretch without
 * changes but its context lines next to a change, when that leaves at least min_lines; the file's
 * last line is never folded (a fold is shown on the line after it). Nothing when nothing changed. */
GArray *line_diff_folds(const GArray *hunks, guint new_lines, guint context, guint min_lines);
