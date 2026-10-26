/*
 * The diff gutter right of the line numbers: green for added lines, blue for
 * changed ones, a red mark where lines were deleted. It compares with the last commit (HEAD,
 * followed as new commits come), or in a comparison with an older version (a commit, a file).
 */
#pragma once

#include "app.h"
#include "window/palette.h"

typedef struct DiffGutter DiffGutter;

/* Adds the gutter to a new editor; it compares with HEAD once the file is loaded. */
void diff_gutter_attach(Tab *tab);

/* The file was loaded (again): its base is read again too. */
void diff_gutter_loaded(Tab *tab);

/* Compares with HEAD, following new commits (the default). */
void diff_gutter_follow_head(Tab *tab);

/* Compares with the file as it is in commit rev. */
void diff_gutter_compare_rev(Tab *tab, const char *rev);

/* Compares with another file (an old version's copy, the working file); NULL: no marks. */
void diff_gutter_compare_file(Tab *tab, const char *path);

/* HEAD moved (a commit, a checkout): the editors following it read their base again. */
void diff_gutter_head_moved(App *t);

/* Pairs two tabs in a side-by-side comparison (new_tab on the left, old_tab on the right) so
 * missing lines open synchronized gap rows (red for deleted lines on the left, black for newly
 * added lines on the right); once both are loaded, the left one goes to its first change. Pass
 * NULL for old_tab to unpair new_tab. */
void diff_gutter_pair(Tab *new_tab, Tab *old_tab);

/* Gap pixels above (*above_px) and below (*below_px) `line` in a side-by-side comparison;
 * returns TRUE when `tab` is the new/working side (red deleted blocks; the old side's are black). */
gboolean diff_gutter_line_gaps(Tab *tab, int line, int *above_px, int *below_px);

/* Of the space below line (diff_gutter_line_gaps), the pixels that only pad a line shorter than
 * the one it faces (word wrap): blank, not a block of missing lines. */
int diff_gutter_line_pad(Tab *tab, int line);

/* Whether line differs from the other side in a side-by-side comparison (its background is tinted;
 * its changed words are colored by a tag). */
gboolean diff_gutter_line_changed(Tab *tab, int line);

/* In a comparison, the unchanged lines folded away right above line (0: none); *row_px: the height
 * of the row standing for them, above the line (part of diff_gutter_line_gaps' above_px). */
int diff_gutter_line_folded(Tab *tab, int line, int *row_px);

/* Whether line is one of them (hidden). */
gboolean diff_gutter_line_hidden(Tab *tab, int line);

/* A comparison (either side's tab) shows its unchanged lines, or hides them again but CONTEXT
 * lines around each change (the default). The cursor going into hidden lines, or a click on the
 * row standing for them, shows them too. */
void diff_gutter_toggle_unchanged(Tab *tab);

/* Called while drawing a compared view: when its text width or word wrap changed since its gaps
 * were made (a long line now wraps to another number of rows), they are made again. */
void diff_gutter_check_width(Tab *tab);

/* The color of line's mark (added, modified, deleted lines), for the strip beside the minimap;
 * FALSE when it has none. */
gboolean diff_gutter_mark(Tab *tab, int line, PalColor *color);

/* How many lines have marks worked out (0 before the first, or without a base). */
int diff_gutter_line_total(Tab *tab);

/* Frees an editor's gutter state. NULL is fine. */
void diff_gutter_free(DiffGutter *gutter);
