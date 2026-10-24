/*
 * Comparing a file with an older version from git, side by side in one tab: the working file on
 * the left, the old version (read-only, from a temporary file) on the right, each gutter marking
 * what differs from the other. Also File History, which lists a file's commits to compare with.
 */
#pragma once

#include "app.h"

/* compare_revisions(): the staged version (the index) instead of a commit's. */
#define INDEX_REV ":0"

/* The file the comparison commands work on: the current editor's, or for an old version, the
 * working file it is compared with. NULL without an editor. */
const char *comparable_path(App *t);

/* The comparisons ask git in the background: the tab opens once it answers. */

/* path next to its version in the last commit (Ctrl+Alt+D). */
void compare_with_head(App *t, const char *path);

/* path next to its staged version: what is not staged yet. */
void compare_with_index(App *t, const char *path);

/* path next to the file as it is on disk now: the editor's unsaved text against a version written
 * there meanwhile (the save conflict bar's Compare). */
void compare_with_disk(App *t, const char *path);

/* Lists path's commits in the picker; the chosen one is compared with the working file (Ctrl+Alt+H). */
void file_history(App *t, const char *path);

/* Shows a file in two versions side by side: new_rev's on the left, old_rev's on the right. Paths
 * are relative to the work tree top (they differ after a rename); a NULL rev or path is a missing
 * side (the file was added or deleted). */
void compare_revisions(App *t, const char *old_rev, const char *old_rel, const char *new_rev, const char *new_rel);

/* Ends the current tab's comparison: an old version closes, the working file stays (its gutter
 * following HEAD again). */
void stop_comparing(App *t);

/* The same for one tab (the left side of a comparison); nothing when it compares nothing. */
void stop_comparing_tab(Tab *tab);

/* The current tab's comparison shows its unchanged lines, or hides them again (diffgutter.h). */
void toggle_unchanged_lines(App *t);

/* A comparison ends (left: its left side): the two sides stop scrolling each other. Their
 * adjustments may outlive them, and would reach a freed view. */
void unsync_scroll(Tab *left);

/* Deletes the temporary folder of old versions (t->paths.temp), when t-code quits. */
void remove_temp_dir(App *t);
