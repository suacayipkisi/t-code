/*
 * The Source Control panel, the second sidebar page (Ctrl+Shift+G): a commit
 * message with Commit and Push, the Staged Changes and Changes lists, and the last commits.
 */
#pragma once

#include "app.h"

/* Creates the panel. */
GtkWidget *scm_panel(App *t);

/* Fills the panel from the output of "git status --porcelain=v2 --branch -z --untracked-files=all"
 * (size bytes; out NULL: not a repository, or git failed). gitstatus.c calls it with each answer. */
void scm_update(App *t, const char *out, gsize size);

/* Whether the Commits section is open (not folded), and opening or folding it (the session keeps it). */
gboolean scm_commits_shown(void);
void scm_show_commits(gboolean shown);

/* Empties the panel at once (another project was opened); the next git status fills it again. The
 * commit message stays: session_load_project() puts the new project's draft in it. */
void scm_reset(void);

/* The commit message typed but not committed yet (stripped; "" for none), and putting one back (the
 * session keeps it per project until the commit). */
char *scm_message(void);
void scm_set_message(const char *message);

/* Opens the n-th commit of Commits (from 0) in a tab: its message, author, line counts and diff
 * (its right-click menu's "Open Commit Details"; the tests call it). */
void scm_open_commit_details(int n);

/* "Stage Hunk…" / "Unstage Hunk…" (staged): saves the editors first when staging, then lists the
 * file's hunks in the picker, each by its first changed line and its "@@" line; the one picked is
 * applied to the index (its menu; the tests call it). */
void scm_choose_hunk(const char *path, gboolean staged);
